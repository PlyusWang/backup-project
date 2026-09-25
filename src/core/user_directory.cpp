// user_directory.cpp
//
// 见 user_directory.h。

#include "user_directory.h"

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <cstddef>
#include <string>
#include <vector>

namespace backupproject {

namespace {

// 起步 4 KiB，ERANGE 就翻倍，最多到 1 MiB。
//
// 三个约束叠在一起才写成这样：
//   * getpwuid_r / getgrgid_r 的缓冲大小**没有可靠的上限**：sysconf 返回 -1
//     表示"没有提示值"，NSS 后端也可以返回任意大的记录；
//   * 缓冲不够时这两个函数返回 ERANGE，而不是"用户不存在"——必须重试，
//     否则 user:alice 会莫名其妙不命中；
//   * 重试必须有上限，否则一个坏掉的 NSS 能让备份进程无限循环。
constexpr std::size_t kInitialBufferSize = 4096;
constexpr std::size_t kMaxBufferSize = 1024 * 1024;

std::size_t InitialBufferSize(int sysconf_hint) {
  if (sysconf_hint <= 0) {
    return kInitialBufferSize;
  }
  const std::size_t hinted = static_cast<std::size_t>(sysconf_hint);
  return hinted < kInitialBufferSize ? kInitialBufferSize : hinted;
}

}  // namespace

bool LookupUserName(std::uint32_t uid, std::string* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  std::size_t size = InitialBufferSize(::sysconf(_SC_GETPW_R_SIZE_MAX));
  while (size <= kMaxBufferSize) {
    std::vector<char> buffer(size);
    struct passwd entry;
    struct passwd* result = nullptr;
    const int status = ::getpwuid_r(static_cast<uid_t>(uid), &entry,
                                    buffer.data(), buffer.size(), &result);
    if (status == 0) {
      if (result != nullptr && entry.pw_name != nullptr) {
        out->assign(entry.pw_name);
        return true;
      }
      // 查得到、但没有这个 uid：确实不存在。
      return false;
    }
    if (status != ERANGE) {
      return false;
    }
    size *= 2;
  }
  // 一直 ERANGE 到上限：留空，让调用方按"名字未知"处理，而不是当成不存在。
  return false;
}

bool LookupGroupName(std::uint32_t gid, std::string* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  // 注意这里是 _SC_GETGR_R_SIZE_MAX：group 数据库和 passwd 数据库是两回事，
  // 用错 hint 在 NSS 后端上会得到一个偏小甚至为 -1 的起点。
  std::size_t size = InitialBufferSize(::sysconf(_SC_GETGR_R_SIZE_MAX));
  while (size <= kMaxBufferSize) {
    std::vector<char> buffer(size);
    struct group entry;
    struct group* result = nullptr;
    const int status = ::getgrgid_r(static_cast<gid_t>(gid), &entry,
                                    buffer.data(), buffer.size(), &result);
    if (status == 0) {
      if (result != nullptr && entry.gr_name != nullptr) {
        out->assign(entry.gr_name);
        return true;
      }
      return false;
    }
    if (status != ERANGE) {
      return false;
    }
    size *= 2;
  }
  return false;
}

const std::string& UserDirectoryCache::UserName(std::uint32_t uid) {
  const auto found = users_.find(uid);
  if (found != users_.end()) {
    return found->second;
  }
  std::string name;
  LookupUserName(uid, &name);
  return users_.emplace(uid, name).first->second;
}

const std::string& UserDirectoryCache::GroupName(std::uint32_t gid) {
  const auto found = groups_.find(gid);
  if (found != groups_.end()) {
    return found->second;
  }
  std::string name;
  LookupGroupName(gid, &name);
  return groups_.emplace(gid, name).first->second;
}

}  // namespace backupproject

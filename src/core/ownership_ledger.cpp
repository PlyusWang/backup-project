#include "ownership_ledger.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>

#include "app_paths.h"

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

bool IsHex(const std::string& text, std::size_t size) {
  if (text.size() != size) return false;
  for (char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool WriteAll(int fd, const char* data, std::size_t size) {
  std::size_t done = 0;
  while (done < size) {
    const ssize_t written = ::write(fd, data + done, size - done);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    done += static_cast<std::size_t>(written);
  }
  return true;
}

}  // namespace

std::string OwnershipLedgerPath(const std::string& repository_id,
                                const std::string& task_identity) {
  std::string config;
  std::string error;
  if (!AppConfigDirectory(&config, &error)) return std::string();
  const std::string key = repository_id + "|" + task_identity;
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char byte : key) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  char name[80];
  std::snprintf(name, sizeof(name), "/ownership-ledgers/%016llx.ledger",
                static_cast<unsigned long long>(hash));
  return config + name;
}

bool AppendOwnershipRecord(const std::string& ledger_path,
                           const OwnershipRecord& record,
                           std::string* error_message) {
  if (ledger_path.empty() || !IsHex(record.snapshot_id, 32) ||
      !IsHex(record.sha256, 64)) {
    SetError(error_message, "归属记录格式非法");
    return false;
  }
  const std::size_t slash = ledger_path.rfind('/');
  if (slash == std::string::npos) {
    SetError(error_message, "归属账本路径非法");
    return false;
  }
  const std::string directory = ledger_path.substr(0, slash);
  if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
    SetError(error_message, "无法创建归属账本目录");
    return false;
  }
  const int fd =
      ::open(ledger_path.c_str(),
             O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    SetError(error_message, "无法打开归属账本");
    return false;
  }
  if (::fchmod(fd, 0600) != 0 || ::flock(fd, LOCK_EX) != 0) {
    ::close(fd);
    SetError(error_message, "无法锁定归属账本");
    return false;
  }
  const std::string line =
      "BPSOWN1 " + record.snapshot_id + " " + record.sha256 + "\n";
  const bool wrote = WriteAll(fd, line.data(), line.size());
  const bool synced = wrote && ::fsync(fd) == 0;
  ::flock(fd, LOCK_UN);
  ::close(fd);
  if (!synced) {
    SetError(error_message, "归属账本写入失败");
    return false;
  }
  return true;
}

bool ReadOwnershipRecords(const std::string& ledger_path,
                          std::vector<OwnershipRecord>* records,
                          std::string* error_message) {
  if (records == nullptr) {
    SetError(error_message, "归属记录输出指针为空");
    return false;
  }
  records->clear();
  const int fd = ::open(ledger_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message, "无法打开归属账本");
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      (info.st_mode & 0777) != 0600 || info.st_size > 1024 * 1024) {
    ::close(fd);
    SetError(error_message, "归属账本不是 0600 普通文件或过大");
    return false;
  }
  if (::flock(fd, LOCK_SH) != 0) {
    ::close(fd);
    SetError(error_message, "无法锁定归属账本");
    return false;
  }
  std::string content;
  char buffer[4096];
  while (true) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    content.append(buffer, static_cast<std::size_t>(got));
  }
  ::flock(fd, LOCK_UN);
  ::close(fd);
  std::map<std::string, std::string> by_id;
  std::size_t offset = 0;
  while (offset < content.size()) {
    const std::size_t end = content.find('\n', offset);
    if (end == std::string::npos) {
      SetError(error_message, "归属账本最后一行被截断");
      return false;
    }
    const std::string line = content.substr(offset, end - offset);
    offset = end + 1;
    if (line.empty()) continue;
    if (line.rfind("BPSOWN1 ", 0) != 0 || line.size() != 8 + 32 + 1 + 64) {
      SetError(error_message, "归属账本记录格式错误");
      return false;
    }
    const std::string id = line.substr(8, 32);
    const std::string sha = line.substr(41);
    if (!IsHex(id, 32) || !IsHex(sha, 64) || line[40] != ' ') {
      SetError(error_message, "归属账本记录字段非法");
      return false;
    }
    const auto found = by_id.find(id);
    if (found != by_id.end() && found->second != sha) {
      SetError(error_message, "同一快照 id 对应不同摘要");
      return false;
    }
    by_id[id] = sha;
  }
  for (const auto& entry : by_id) {
    records->push_back(OwnershipRecord{entry.first, entry.second});
  }
  return true;
}

}  // namespace backupproject

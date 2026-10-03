// src/network/snapshot_bundle.cpp
//
// BPSNAP1 的实现。格式与信任链的说明见 include/snapshot_bundle.h。

#include "snapshot_bundle.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "backup_catalog.h"
#include "crypto.h"
#include "file_io.h"
#include "incremental_backup.h"

namespace backupproject {
namespace net {
namespace {

constexpr std::size_t kCopyChunkBytes = 256u * 1024u;
constexpr const char kBundleMagic[8] = {'B', 'P', 'S', 'N',
                                        'A', 'P', '1', '\0'};
constexpr std::size_t kSha256Bytes = crypto::kSha256DigestSize;

std::string StrerrorText() { return std::string(std::strerror(errno)); }

bool IsSingleComponentName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  if (name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos ||
      name.find('\0') != std::string::npos) {
    return false;
  }
  // 控制字符一律拒绝：成员名会变成缓存里的文件名、也会写进 .remote-index.tsv
  // 与日志。TAB / LF / CR 会让"一行一个条目"的索引被注入第二行，其他控制字符
  // 只会污染终端与日志。名字是机器生成的，正常只有可打印 ASCII。
  for (const char character : name) {
    const unsigned char value = static_cast<unsigned char>(character);
    if (value < 0x20 || value == 0x7F) {
      return false;
    }
  }
  return name.size() <= kSnapshotBundleMaxNameBytes;
}

void AppendU16(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xFFu));
  out->push_back(static_cast<char>(value & 0xFFu));
}

void AppendU64(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

std::uint16_t LoadU16(const unsigned char* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                    static_cast<std::uint16_t>(data[1]));
}

std::uint64_t LoadU64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint64_t>(data[i]);
  }
  return value;
}

bool WriteAll(int fd, const void* data, std::size_t size,
              std::string* error_message) {
  const char* cursor = static_cast<const char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    const ssize_t written = ::write(fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = "写入 bundle 失败：" + StrerrorText();
      }
      return false;
    }
    if (written == 0) {
      if (error_message != nullptr) {
        *error_message = "写入 bundle 时得到 0 字节";
      }
      return false;
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

bool ReadAll(int fd, void* data, std::size_t size, bool* eof,
             std::string* error_message) {
  if (eof != nullptr) {
    *eof = false;
  }
  char* cursor = static_cast<char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    const ssize_t got = ::read(fd, cursor, remaining);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = "读取 bundle 失败：" + StrerrorText();
      }
      return false;
    }
    if (got == 0) {
      if (eof != nullptr) {
        *eof = true;
      }
      if (error_message != nullptr) {
        *error_message = "bundle 在读到一半时结束";
      }
      return false;
    }
    cursor += got;
    remaining -= static_cast<std::size_t>(got);
  }
  return true;
}

// 打开并确认是普通文件、不跟随符号链接；同时给出大小。
bool OpenRegularFile(const std::string& path, int* fd, std::uint64_t* size,
                     std::string* error_message) {
  const int opened = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (opened < 0) {
    if (error_message != nullptr) {
      *error_message = "无法打开 " + path + "：" + StrerrorText();
    }
    return false;
  }
  struct stat info;
  if (::fstat(opened, &info) != 0) {
    const std::string reason = StrerrorText();
    ::close(opened);
    if (error_message != nullptr) {
      *error_message = "无法检查 " + path + "：" + reason;
    }
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    ::close(opened);
    if (error_message != nullptr) {
      *error_message =
          path + " 不是普通文件（拒绝把符号链接或设备当成快照材料）";
    }
    return false;
  }
  *fd = opened;
  if (size != nullptr) {
    *size = static_cast<std::uint64_t>(info.st_size);
  }
  return true;
}

// 流式读一个文件并算出 SHA-256。
bool HashFile(const std::string& path, std::string* sha256_hex,
              std::uint64_t* size, std::string* error_message) {
  int fd = -1;
  if (!OpenRegularFile(path, &fd, size, error_message)) {
    return false;
  }
  crypto::Sha256 hasher;
  std::vector<char> buffer(kCopyChunkBytes);
  for (;;) {
    const ssize_t got = ::read(fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::string reason = StrerrorText();
      ::close(fd);
      if (error_message != nullptr) {
        *error_message = "读取 " + path + " 失败：" + reason;
      }
      return false;
    }
    if (got == 0) {
      break;
    }
    hasher.Update(buffer.data(), static_cast<std::size_t>(got));
  }
  if (::close(fd) != 0) {
    if (error_message != nullptr) {
      *error_message = "关闭 " + path + " 失败：" + StrerrorText();
    }
    return false;
  }
  unsigned char digest[kSha256Bytes];
  hasher.Final(digest);
  *sha256_hex = crypto::ToHex(digest, sizeof(digest));
  return true;
}

std::string PartPathFor(const std::string& target_path, std::size_t index) {
  return target_path + ".part-" +
         std::to_string(static_cast<long>(::getpid())) + "-" +
         std::to_string(index);
}

// 读 bundle 的成员表（不解包）。
bool ReadBundleIndex(int fd, const std::string& bundle_path,
                     SnapshotBundleInfo* info, std::string* error_message) {
  unsigned char header[kSnapshotBundleHeaderSize];
  bool eof = false;
  if (!ReadAll(fd, header, sizeof(header), &eof, error_message)) {
    return false;
  }
  if (std::memcmp(header, kBundleMagic, sizeof(kBundleMagic)) != 0) {
    if (error_message != nullptr) {
      *error_message = bundle_path + " 不是 BPSNAP1 材料包（magic 不对）";
    }
    return false;
  }
  if (LoadU16(header + 8) != kSnapshotBundleVersion) {
    if (error_message != nullptr) {
      *error_message = bundle_path + " 的 BPSNAP1 版本不被支持";
    }
    return false;
  }
  const std::uint16_t count = LoadU16(header + 10);
  if (count != kSnapshotBundleMemberCount) {
    if (error_message != nullptr) {
      *error_message =
          "材料包的成员数不是 " + std::to_string(kSnapshotBundleMemberCount);
    }
    return false;
  }
  info->members.clear();
  for (std::uint16_t index = 0; index < count; ++index) {
    unsigned char name_length_raw[2];
    if (!ReadAll(fd, name_length_raw, sizeof(name_length_raw), &eof,
                 error_message)) {
      return false;
    }
    const std::size_t name_length = LoadU16(name_length_raw);
    if (name_length == 0 || name_length > kSnapshotBundleMaxNameBytes) {
      if (error_message != nullptr) {
        *error_message = "材料包里的成员名字长度不合法";
      }
      return false;
    }
    std::string name(name_length, '\0');
    if (!ReadAll(fd, &name[0], name_length, &eof, error_message)) {
      return false;
    }
    if (!IsSingleComponentName(name)) {
      if (error_message != nullptr) {
        *error_message = "材料包里的成员名字不是单组件文件名";
      }
      return false;
    }
    unsigned char fixed[8 + kSha256Bytes];
    if (!ReadAll(fd, fixed, sizeof(fixed), &eof, error_message)) {
      return false;
    }
    SnapshotBundleMember member;
    member.name = name;
    member.size = LoadU64(fixed);
    // 写侧明确拒绝空文件（见源文件里 "是空文件，拒绝打包"），读侧必须同样
    // 拒绝：否则"读侧接受、写侧永不产出"的输入会成为一个只存在于解析层的
    // 状态，三件套的成员数与摘要约定跟着松掉（审查轮 F1，用例 RT1.20）。
    if (member.size == 0) {
      if (error_message != nullptr) {
        *error_message = "材料包里的成员长度为 0（写侧从不产出空成员）";
      }
      return false;
    }
    if (member.size > kSnapshotBundleMaxMemberBytes) {
      if (error_message != nullptr) {
        *error_message = "材料包里的成员长度超过上限";
      }
      return false;
    }
    member.sha256 = crypto::ToHex(fixed + 8, kSha256Bytes);
    info->members.push_back(member);
    // 跳过数据区（调用方负责流式复制；这里只索引）。
    if (member.size > 0 &&
        ::lseek(fd, static_cast<off_t>(member.size), SEEK_CUR) < 0) {
      if (error_message != nullptr) {
        *error_message = "无法跳过材料包的成员数据：" + StrerrorText();
      }
      return false;
    }
  }
  info->archive_name =
      info->members.empty() ? std::string() : info->members[0].name;
  return true;
}

// 成员名必须正好是 <archive> / <archive>.manifest / <archive>.identity。
bool MembersMatchArchive(const SnapshotBundleInfo& info,
                         std::string* error_message) {
  if (info.members.size() != kSnapshotBundleMemberCount) {
    return false;
  }
  const std::string& archive = info.members[0].name;
  if (!backupproject::IsManagedBackupFileName(archive)) {
    if (error_message != nullptr) {
      *error_message = "材料包里的归档名不是受管的 .bak 名字";
    }
    return false;
  }
  const std::string manifest = backupproject::SnapshotManifestFileName(archive);
  const std::string identity = backupproject::SnapshotIdentityFileName(archive);
  if (info.members[1].name != manifest || info.members[2].name != identity) {
    if (error_message != nullptr) {
      *error_message = "材料包的三件套名字对不上（应当是 " + archive + " / " +
                       manifest + " / " + identity + "）";
    }
    return false;
  }
  return true;
}

}  // namespace

bool BuildSnapshotBundle(const std::string& repository_directory,
                         const std::string& archive_name,
                         const std::string& bundle_path,
                         SnapshotBundleInfo* info, std::string* error_message) {
  if (info == nullptr || bundle_path.empty()) {
    if (error_message != nullptr) {
      *error_message = "打包材料时参数为空";
    }
    return false;
  }
  *info = SnapshotBundleInfo();
  if (!IsSingleComponentName(archive_name) ||
      !backupproject::IsManagedBackupFileName(archive_name)) {
    if (error_message != nullptr) {
      *error_message = archive_name + " 不是合法的单组件快照名";
    }
    return false;
  }

  struct Source {
    std::string name;
    std::string path;
  };
  const std::string prefix = repository_directory + "/";
  const Source sources[kSnapshotBundleMemberCount] = {
      {archive_name, prefix + archive_name},
      {backupproject::SnapshotManifestFileName(archive_name),
       prefix + backupproject::SnapshotManifestFileName(archive_name)},
      {backupproject::SnapshotIdentityFileName(archive_name),
       prefix + backupproject::SnapshotIdentityFileName(archive_name)},
  };

  // 第一遍：只算摘要与长度（不把任何文件整份读进内存）。
  std::vector<SnapshotBundleMember> members;
  for (const Source& source : sources) {
    SnapshotBundleMember member;
    member.name = source.name;
    if (!HashFile(source.path, &member.sha256, &member.size, error_message)) {
      return false;
    }
    if (member.size == 0) {
      if (error_message != nullptr) {
        *error_message = source.path + " 是空文件，拒绝打包";
      }
      return false;
    }
    if (member.size > kSnapshotBundleMaxMemberBytes) {
      if (error_message != nullptr) {
        *error_message = source.path + " 超过单个成员的长度上限";
      }
      return false;
    }
    members.push_back(member);
  }

  // 第二遍：写包头 + 逐个成员复制数据。
  const std::string part_path = PartPathFor(bundle_path, 0);
  ::unlink(part_path.c_str());
  const int out = ::open(part_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (out < 0) {
    if (error_message != nullptr) {
      *error_message = "无法创建 " + part_path + "：" + StrerrorText();
    }
    return false;
  }

  bool ok = true;
  std::string header;
  header.append(kBundleMagic, sizeof(kBundleMagic));
  AppendU16(&header, kSnapshotBundleVersion);
  AppendU16(&header, static_cast<std::uint16_t>(members.size()));
  if (!WriteAll(out, header.data(), header.size(), error_message)) {
    ok = false;
  }

  std::vector<char> buffer(kCopyChunkBytes);
  for (std::size_t index = 0; ok && index < members.size(); ++index) {
    std::string member_header;
    AppendU16(&member_header,
              static_cast<std::uint16_t>(members[index].name.size()));
    member_header.append(members[index].name);
    AppendU64(&member_header, members[index].size);
    // 摘要按**原始 32 字节**写，不是十六进制文本。
    std::string raw_digest;
    if (!crypto::FromHex(members[index].sha256, &raw_digest) ||
        raw_digest.size() != kSha256Bytes) {
      if (error_message != nullptr) {
        *error_message = "内部错误：成员摘要不是 32 字节";
      }
      ok = false;
      break;
    }
    member_header.append(raw_digest);
    if (!WriteAll(out, member_header.data(), member_header.size(),
                  error_message)) {
      ok = false;
      break;
    }

    int in = -1;
    std::uint64_t source_size = 0;
    if (!OpenRegularFile(sources[index].path, &in, &source_size,
                         error_message)) {
      ok = false;
      break;
    }
    if (source_size != members[index].size) {
      if (error_message != nullptr) {
        *error_message =
            sources[index].path + " 在打包期间被改动（长度变了），已放弃";
      }
      ::close(in);
      ok = false;
      break;
    }
    std::uint64_t remaining = source_size;
    while (remaining > 0) {
      const std::size_t want = static_cast<std::size_t>(
          remaining < buffer.size() ? remaining : buffer.size());
      const ssize_t got = ::read(in, buffer.data(), want);
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (error_message != nullptr) {
          *error_message =
              "读取 " + sources[index].path + " 失败：" + StrerrorText();
        }
        ok = false;
        break;
      }
      if (got == 0) {
        if (error_message != nullptr) {
          *error_message = sources[index].path + " 在打包期间被截短，已放弃";
        }
        ok = false;
        break;
      }
      if (!WriteAll(out, buffer.data(), static_cast<std::size_t>(got),
                    error_message)) {
        ok = false;
        break;
      }
      remaining -= static_cast<std::uint64_t>(got);
    }
    if (::close(in) != 0 && ok) {
      if (error_message != nullptr) {
        *error_message =
            "关闭 " + sources[index].path + " 失败：" + StrerrorText();
      }
      ok = false;
    }
  }

  if (ok && ::fsync(out) != 0) {
    if (error_message != nullptr) {
      *error_message = "fsync " + part_path + " 失败：" + StrerrorText();
    }
    ok = false;
  }
  if (::close(out) != 0 && ok) {
    if (error_message != nullptr) {
      *error_message = "关闭 " + part_path + " 失败：" + StrerrorText();
    }
    ok = false;
  }
  if (!ok) {
    ::unlink(part_path.c_str());
    return false;
  }

  // 原子发布，且**不覆盖**已存在的目标。
  std::string publish_error;
  if (!PublishNoReplace(part_path, bundle_path, &publish_error)) {
    ::unlink(part_path.c_str());
    if (error_message != nullptr) {
      *error_message = "无法发布材料包 " + bundle_path + "：" + publish_error;
    }
    return false;
  }

  struct stat published;
  if (::stat(bundle_path.c_str(), &published) == 0) {
    info->bundle_size = static_cast<std::uint64_t>(published.st_size);
  }
  info->archive_name = archive_name;
  info->members = members;
  return true;
}

bool InspectSnapshotBundle(const std::string& bundle_path,
                           SnapshotBundleInfo* info,
                           std::string* error_message) {
  if (info == nullptr) {
    if (error_message != nullptr) {
      *error_message = "检查材料包时输出指针为空";
    }
    return false;
  }
  *info = SnapshotBundleInfo();
  int fd = -1;
  std::uint64_t size = 0;
  if (!OpenRegularFile(bundle_path, &fd, &size, error_message)) {
    return false;
  }
  const bool ok = ReadBundleIndex(fd, bundle_path, info, error_message);
  ::close(fd);
  if (!ok) {
    return false;
  }
  info->bundle_size = size;
  return MembersMatchArchive(*info, error_message);
}

bool ExtractSnapshotBundle(const std::string& bundle_path,
                           const std::string& target_directory,
                           SnapshotBundleInfo* info,
                           std::string* error_message) {
  if (info == nullptr) {
    if (error_message != nullptr) {
      *error_message = "解包材料时输出指针为空";
    }
    return false;
  }
  *info = SnapshotBundleInfo();
  int fd = -1;
  std::uint64_t bundle_size = 0;
  if (!OpenRegularFile(bundle_path, &fd, &bundle_size, error_message)) {
    return false;
  }
  SnapshotBundleInfo index;
  if (!ReadBundleIndex(fd, bundle_path, &index, error_message) ||
      !MembersMatchArchive(index, error_message)) {
    ::close(fd);
    return false;
  }
  if (::lseek(fd, static_cast<off_t>(kSnapshotBundleHeaderSize), SEEK_SET) <
      0) {
    if (error_message != nullptr) {
      *error_message = "无法回到材料包开头：" + StrerrorText();
    }
    ::close(fd);
    return false;
  }

  std::vector<char> buffer(kCopyChunkBytes);
  std::vector<std::string> part_paths;
  std::vector<std::string> final_paths;

  bool ok = true;
  for (std::size_t i = 0; ok && i < index.members.size(); ++i) {
    // 跳过成员表（成员头），进入数据区。
    unsigned char name_length_raw[2];
    bool eof = false;
    if (!ReadAll(fd, name_length_raw, sizeof(name_length_raw), &eof,
                 error_message)) {
      ok = false;
      break;
    }
    const std::size_t name_length = LoadU16(name_length_raw);
    std::string name(name_length, '\0');
    unsigned char fixed[8 + kSha256Bytes];
    if (name_length == 0 ||
        !ReadAll(fd, &name[0], name_length, &eof, error_message) ||
        !ReadAll(fd, fixed, sizeof(fixed), &eof, error_message)) {
      ok = false;
      break;
    }
    const std::uint64_t data_size = LoadU64(fixed);
    const std::string expected_sha256 = crypto::ToHex(fixed + 8, kSha256Bytes);

    // 第二遍读到的成员头**只用来交叉校验**，路径一律取第一遍已经校验过的
    // 成员表。两次解析之间文件内容是可变的（bundle 是本地文件），如果这里用
    // 刚读到的 name 去拼路径，就等于把"写到哪个文件"的决定权交给能在两次解析
    // 之间改写文件的人——审查轮实测 40 轮里 6-9 轮能把文件写到目标目录之外
    // （用例 RT2.10）。改成索引取值 + 逐项比对之后，改写只会导致失败。
    const SnapshotBundleMember& indexed = index.members[i];
    if (name != indexed.name || data_size != indexed.size ||
        expected_sha256 != indexed.sha256) {
      if (error_message != nullptr) {
        *error_message = "材料包在两次解析之间被改写（成员表与第一次索引不符）";
      }
      ok = false;
      break;
    }
    const std::string final_path = target_directory + "/" + indexed.name;
    const std::string part_path = PartPathFor(final_path, i);
    ::unlink(part_path.c_str());
    const int out =
        ::open(part_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) {
      if (error_message != nullptr) {
        *error_message = "无法创建 " + part_path + "：" + StrerrorText();
      }
      ok = false;
      break;
    }
    crypto::Sha256 hasher;
    std::uint64_t remaining = data_size;
    while (remaining > 0) {
      const std::size_t want = static_cast<std::size_t>(
          remaining < buffer.size() ? remaining : buffer.size());
      if (!ReadAll(fd, buffer.data(), want, &eof, error_message)) {
        ok = false;
        break;
      }
      hasher.Update(buffer.data(), want);
      if (!WriteAll(out, buffer.data(), want, error_message)) {
        ok = false;
        break;
      }
      remaining -= static_cast<std::uint64_t>(want);
    }
    if (ok && ::fsync(out) != 0) {
      if (error_message != nullptr) {
        *error_message = "fsync " + part_path + " 失败：" + StrerrorText();
      }
      ok = false;
    }
    if (::close(out) != 0 && ok) {
      if (error_message != nullptr) {
        *error_message = "关闭 " + part_path + " 失败：" + StrerrorText();
      }
      ok = false;
    }
    if (!ok) {
      ::unlink(part_path.c_str());
      break;
    }
    unsigned char digest[kSha256Bytes];
    hasher.Final(digest);
    const std::string actual_sha256 = crypto::ToHex(digest, sizeof(digest));
    if (actual_sha256 != expected_sha256) {
      if (error_message != nullptr) {
        *error_message =
            "材料包里的 " + name + " 内容校验失败（SHA-256 与包头声明不符）";
      }
      ::unlink(part_path.c_str());
      ok = false;
      break;
    }
    part_paths.push_back(part_path);
    final_paths.push_back(final_path);
  }
  ::close(fd);

  if (!ok) {
    for (const std::string& path : part_paths) {
      ::unlink(path.c_str());
    }
    return false;
  }

  // 全部成员都验证通过之后才发布：任何一步失败都会把本次已经发布的成员撤掉，
  // 绝不留下一套缺件的三件套。
  std::size_t published = 0;
  for (; published < part_paths.size(); ++published) {
    std::string publish_error;
    if (!PublishNoReplace(part_paths[published], final_paths[published],
                          &publish_error)) {
      if (error_message != nullptr) {
        *error_message =
            "无法发布 " + final_paths[published] + "：" + publish_error;
      }
      break;
    }
  }
  if (published != part_paths.size()) {
    for (std::size_t i = 0; i < part_paths.size(); ++i) {
      ::unlink(part_paths[i].c_str());
      if (i < published) {
        ::unlink(final_paths[i].c_str());
      }
    }
    return false;
  }

  index.bundle_size = bundle_size;
  *info = index;
  return true;
}

}  // namespace net
}  // namespace backupproject

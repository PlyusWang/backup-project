// src/network/snapshot_bundle.cpp
//
// BPSNAP1 的实现。格式与信任链的说明见 include/snapshot_bundle.h。

// 职责：把三件套（.bak / .manifest / .identity）装进一个
// BPSNAP1 容器，以及从容器里取出并逐成员校验。
// 它不解释成员内部的字节——归档格式仍然完全由增量引擎拥有。
//
// 数据流：打包时先对三个源文件各做一遍流式摘要（不整份读进内存），
// 再写包头并逐个成员复制，复制的同时对真正写进去的字节再算一次摘要并与第一遍比
// 对。解包时反向：先索引成员表，再按索引把数据写成临时文件并逐个校验，
// 全部通过后才发布。
//
// 安全边界：bundle 是**本地文件且随时可能被改写**，不能当可信输入。
// 所有写入路径只用第一遍已校验过的成员名拼接，两次解析之间被改写只会导致失败；
// 打开源文件用 O_NOFOLLOW 并要求 S_ISREG，发布一律
// NoReplace。
//
// 失败语义：任何一步失败都不留下半成品——临时文件 unlink，
// 已发布的成员回滚，输出参数 info 保持被清空的状态。错误只走
// error_message，不用异常。
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

// 复制缓冲区大小。与传输块同一量级：任何一份归档都不会被整份读进内存，
// 缓冲区只分配一次并在所有成员之间复用。
constexpr std::size_t kCopyChunkBytes = 256u * 1024u;
// magic 含结尾的 NUL，正好 8 字节，因此比较时按整个数组 memcmp。
// 这个字面量就是磁盘格式的一部分，改动等于换协议。
constexpr const char kBundleMagic[8] = {'B', 'P', 'S', 'N',
                                        'A', 'P', '1', '\0'};
constexpr std::size_t kSha256Bytes = crypto::kSha256DigestSize;

// 调用点必须在任何可能改写 errno 的操作**之前**取值：
// 典型写法是先存进局部变量，再 close()、再拼错误消息。否则报出来的会是
// close 的 errno 而不是真正的失败原因。
std::string StrerrorText() { return std::string(std::strerror(errno)); }

// 路径边界的第一道闸：只接受不含路径分隔符、不含 NUL 的单组件名字，
// 且长度不超过 255。成员名后面会被拼成缓存目录里的文件名、写进
// .remote-index.tsv 的一行，因此 '..'
// 逃逸与索引注入都在这里被挡住。
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

// 本地大端原语，与 network_protocol.cpp
// 里那一套是**两份独立实现**：两者格式相同但归属不同的协议，
// 不共享是为了让任一边的格式改动不会静默影响另一边。同样是手工移位，
// 不写结构体。
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

// 循环写满并处理 EINTR。失败时可能已经写出去一部分字节，
// 所以调用方的处理方式是**丢弃整个临时文件**，而不是就地修补：半截
// bundle没有任何可用的恢复语义。
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

// 循环读满。eof 只在“一个字节都没读到就遇到文件结尾”时为 true；
// 读到一半结束属于材料被截断，是错误而不是正常结尾（错误消息会明确写出来）。
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
// O_NOFOLLOW 打开并要求 fstat 出来是普通文件：符号链接、目录、
// 设备、FIFO 全部拒绝，避免把设备当成快照材料读，
// 也避免顺着链接读到仓库之外。
//
// 这里只能保证“打开的这个 fd 是普通文件”；路径本身在之后仍可能被替换，
// 所以后续所有读写都走这个 fd，绝不再按路径重开。失败时内部已把 fd 关掉，
// 调用方不需要清理。
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
// 流式摘要：固定缓冲区循环读，内存占用与文件大小无关。close
// 失败也算失败（出错路径上的 close 反而更值得怀疑），函数只读源文件、
// 不修改任何东西。
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

// 临时文件名 = 目标路径 + .part-<pid>-<index>。带 pid
// 是为了让并发的两个进程（或同一进程的不同快照）不会互相覆盖临时文件；
// 调用点统一先 unlink 再以 O_EXCL 创建，
// 因此“同名残留”只可能来自上一轮崩溃，不会被误用。
std::string PartPathFor(const std::string& target_path, std::size_t index) {
  return target_path + ".part-" +
         std::to_string(static_cast<long>(::getpid())) + "-" +
         std::to_string(index);
}

// 读 bundle 的成员表（不解包）。
// 只读成员表，不碰数据区。校验顺序：magic -> version ->
// 成员数（必须正好 3）-> 每个成员的名字长度、名字、数据长度与摘要。
//
// 每个长度都先比上限再使用：名字长度限制在 1..255，成员长度限制在
// kSnapshotBundleMaxMemberBytes 且**不允许为
// 0**——写侧从不产出空成员，读侧接受它就等于承认一种只能由攻击者构造的状态。
//
// 数据区用 lseek 跳过而不是读进来：一份 16 GiB
// 的成员不应该为了让调用方看到索引而被读一遍。函数返回时 fd
// 的位置停在最后一个成员的数据之后。
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
// 成员名必须严格由第一个成员（归档名）派生：archive、
// archive.manifest、archive.identity，
// 名字对不上就整体拒绝。这样“三件套属于同一份快照”这件事由命名规则保证，
// 而不是靠调用方自觉。
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

// 两遍扫描：第一遍只算长度与摘要（确认三个文件都能读、都不是空文件、
// 都没超上限），第二遍才真正复制。
// 先索引再复制的理由是失败要趁早——源文件缺失或超限时一个字节都不必写。
//
// 发布语义：全程写 <目标>.part-<pid>-0，成功才 fsync +
// NoReplace 改名。目标已存在时明确失败，不覆盖：
// 一份已经上传过的包不能被本地重试悄悄换掉。
//
// 失败路径统一 unlink 临时文件；源文件自始至终以只读方式打开，
// 打包不会修改仓库里的任何东西。
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
  // 先 unlink 再 O_EXCL|0600 创建：0600
  // 保证摘要材料不会在打包期间短暂暴露给同机的其它用户；O_EXCL
  // 让“临时文件已存在”变成硬失败而不是静默复用别人的内容。
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
    // 长度检查是最便宜的一道门，但它只能发现变长或变短；
    // **同长度**的改写要靠下面复制时的第二遍摘要。两道检查缺一不可。
    if (source_size != members[index].size) {
      if (error_message != nullptr) {
        *error_message =
            sources[index].path + " 在打包期间被改动（长度变了），已放弃";
      }
      ::close(in);
      ok = false;
      break;
    }
    // 第二遍**同时**对真正复制进 bundle 的字节再算一次 SHA-256。
    //
    // 第一遍的摘要只证明"索引那一刻"的内容。两遍之间（以及第二遍复制期间的
    // 任意时刻）一次**同长度**的改写不会让下面的长度检查发现任何异常，于是
    // 包头声明的摘要与实际写进去的字节就脱钩了——而这样的包服务端照样收
    // （它只校验整个 blob 的摘要，不解析成员），坏材料要等到将来 restore 才
    // 炸。所以"写进去的字节"必须绑回包头声明：
    //
    //   SHA-256(实际复制进 bundle 的字节) == 第一遍声明的 SHA-256
    //
    // 这是本函数唯一正确的判据：mtime / inode / size 只能提高发现概率，不能
    // 保证任何东西（见 tests/review/bundle_source_mutation.cpp 的 OLD/NEW
    // 判别）。
    crypto::Sha256 copied_hasher;
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
      copied_hasher.Update(buffer.data(), static_cast<std::size_t>(got));
      if (!WriteAll(out, buffer.data(), static_cast<std::size_t>(got),
                    error_message)) {
        ok = false;
        break;
      }
      remaining -= static_cast<std::uint64_t>(got);
    }
    // 复制完声明的长度之后再读 1 个字节：源文件在**打开之后**变长时，多出来
    // 的尾巴既没被复制、也没进摘要，同属"声明与内容脱钩"。0 才是正常。
    // EINTR 只重试，绝不当作 EOF——否则一次信号打断就能让一个超长的文件
    // 被当成"正好复制完"。
    while (ok) {
      char extra = 0;
      const ssize_t tail = ::read(in, &extra, 1);
      if (tail < 0) {
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
      if (tail > 0) {
        if (error_message != nullptr) {
          *error_message = sources[index].path +
                           " 在打包期间变长（复制完之后还有多余字节），已放弃";
        }
        ok = false;
      }
      break;
    }
    if (::close(in) != 0 && ok) {
      if (error_message != nullptr) {
        *error_message =
            "关闭 " + sources[index].path + " 失败：" + StrerrorText();
      }
      ok = false;
    }
    if (ok) {
      unsigned char copied_digest[kSha256Bytes];
      copied_hasher.Final(copied_digest);
      if (crypto::ToHex(copied_digest, sizeof(copied_digest)) !=
          members[index].sha256) {
        if (error_message != nullptr) {
          *error_message = sources[index].path +
                           " 在打包期间内容发生变化（实际复制进材料包的字节与第"
                           "一遍摘要不符），已放弃";
        }
        ok = false;
      }
    }
  }

  // 发布前 fsync：文件名一旦出现就必须有完整内容，
  // 否则崩溃会留下一个“名字在、内容空”的包，而它已经可能被上传。fsync
  // 失败与写失败同等对待，同样放弃整包。
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

// 只读探测：只解析成员表并做命名一致性检查，不写任何文件。失败时 info
// 保持被清空的状态，调用方拿到的永远是一份完整索引而不是半份。
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

// 两阶段提交：先把每个成员写进唯一临时文件、边写边算 SHA-256、
// 长度与摘要都对得上才算通过；**全部成员都通过之后**才逐个
// NoReplace 发布。
//
// 回滚语义：任一成员失败，本次所有临时文件都被删掉；
// 发布阶段失败则连已经发布的成员一起撤回（只删本次创建的文件）。
// 绝不留下一套缺件的三件套——缓存目录里的东西要么验证完整，要么不存在。
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
    // 路径只用**第一遍已校验过的**成员名拼接，因此不存在 ../ 逃逸；发布用
    // NoReplace，目标已存在同名文件时明确失败，不覆盖缓存里已有的内容。
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
    // 边写边算：复制完立刻与包头声明的摘要比较，不符就把临时文件删掉。
    // 摘要比较在数据落盘之后、发布之前，
    // 所以永远不会有一个“已经发布但内容不对”的成员。
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
  // 回滚范围由 published 界定：[0, published)
  // 是本函数已经发布出去的成员，需要连同临时文件一起删；之后的我还在 .part
  // 名下，删临时文件即可。
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

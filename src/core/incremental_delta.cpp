// incremental_delta.cpp
//
// 见 include/incremental_delta.h。这里只做三件事：信封的规范化编解码、
// 把内层 container 包进来（复用现有流水线）、把 envelope 与 payload 的
// 一致性在写侧和读侧都检查一遍。

// ---- 本文件的职责与边界 ----
//
// BKPINC1 增量 delta 的**唯一**格式实现：外层信封（规范化文本）的编解码与
// 全部格式级校验、内层 payload 的抽取与完整性验证。它不决定什么时候做增量、
// 不算 diff、不选 retention —— 那些属于 realtime / catalog 层。
//
// 写侧数据流：changed_entries → RunBackupPipelineFromEntries 产出内层 v2
// container（临时文件）→ 算 payload_sha256 与自引用 snapshot_id → 序列化
// 信封 → 拼 24 字节小端定长头 → 唯一临时文件 fsync + rename 原子发布。
// 读侧反向：ReadDeltaLayout 解释定长头 → ParseDeltaEnvelope 严格解析文本
// → 重算 snapshot_id 自校验；VerifyDeltaPayload 再核对实际字节的 SHA-256。
//
// 关键不变量：
//   * 文件长度必须**正好**等于 24 + envelope_len + payload_len；
//   * removed == tombstones.size()，写侧与读侧用同一条判断；
//   * snapshot_id == 去掉该字段后信封内容的摘要，任何字段被改都能发现；
//   * parent_file_name / tombstones 是不可信输入，在格式层就被判死。
//
// 失败语义：不抛异常，一律 bool + error_message。写侧失败不留半成品（临时
// 文件全部 unlink）；读侧失败表示这份 delta 不可信，调用方必须放弃，
// 而不是挑几个字段降级使用。
//
// 线程与生命周期：无全局可变状态，函数可重入；同一个 fd / 同一个目标路径
// 不允许多线程并发复用，互斥由调用方的 OperationGate 负责。唯一的进程级
// 共享是 getpid()（临时文件命名），只用于避免同名碰撞。
#include "incremental_delta.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "archive_path.h"
#include "backup_catalog.h"
#include "container_format.h"
#include "crypto.h"
#include "source_digest.h"

namespace backupproject {

namespace {

// error_message 允许为 nullptr（自检与测试路径会这么调），所以这里必须判空，
// 而不是让每个失败分支自己写一遍 if。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// strerror 对未知 errno 可能返回 nullptr；退回 "errno <n>"，保证任何失败
// 路径都不会把空字符串当成原因交给用户。
std::string ErrnoText(int error_number) {
  const char* text = ::strerror(error_number);
  return text == nullptr ? std::string("errno ") + std::to_string(error_number)
                         : std::string(text);
}

// 信封是每行 key=value 的文本格式：值里混进 '\n' 会凭空多出一行，混进 '\'
// 会让转义本身产生歧义，'\t' / '\r' 会破坏逐行对比的可读性。只转义这四类
// 字符、其余字节原样输出，序列化结果才与 locale 无关、可逐字节复现。
// 转义只作用于字符串字段：反斜杠、TAB、换行、回车。
std::string EscapeField(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char character : value) {
    switch (character) {
      case '\\':
        out += "\\\\";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += character;
    }
  }
  return out;
}

// EscapeField 的严格逆操作：孤立的反斜杠或未知转义序列一律返回 false，
// 不做尽力恢复。能解析出来的信封必须与写侧逐字节互逆，否则 snapshot_id
// 的自校验就失去意义。
bool UnescapeField(const std::string& text, std::string* value) {
  if (value == nullptr) return false;
  value->clear();
  value->reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (character != '\\') {
      *value += character;
      continue;
    }
    if (index + 1 >= text.size()) return false;
    const char next = text[++index];
    switch (next) {
      case '\\':
        *value += '\\';
        break;
      case 't':
        *value += '\t';
        break;
      case 'n':
        *value += '\n';
        break;
      case 'r':
        *value += '\r';
        break;
      default:
        return false;
    }
  }
  return true;
}

// 严格十进制解析：不 trim、不收正负号、长度上限 20 位。累加过程中就按
// (max - digit) / 10 判溢出，而不是先乘后判 —— 后者在溢出时会静默回绕，
// 得不出越界这个结论。
bool ParseUint64(const std::string& text, std::uint64_t* value) {
  if (value == nullptr || text.empty() || text.size() > 20) return false;
  std::uint64_t result = 0;
  for (char character : text) {
    if (character < '0' || character > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) return false;
    result = result * 10ull + digit;
  }
  *value = result;
  return true;
}

// 先按无符号解析再套符号。只接受 |v| <= INT64_MAX，等价于刻意拒绝
// INT64_MIN：created 是 Unix 秒，用不到那个值，单独为它开分支没有收益。
bool ParseInt64(const std::string& text, std::int64_t* value) {
  if (value == nullptr || text.empty() || text.size() > 20) return false;
  std::string digits = text;
  bool negative = false;
  if (digits[0] == '-') {
    negative = true;
    digits = digits.substr(1);
    if (digits.empty()) return false;
  }
  std::uint64_t magnitude = 0;
  if (!ParseUint64(digits, &magnitude)) return false;
  if (magnitude > 0x7FFFFFFFFFFFFFFFull) return false;
  *value = negative ? -static_cast<std::int64_t>(magnitude)
                    : static_cast<std::int64_t>(magnitude);
  return true;
}

// 每行 key=value；列表键可以出现多次（tombstone / affected_dir）。
void AppendKeyValue(const std::string& key, const std::string& value,
                    std::string* out) {
  *out += key;
  *out += '=';
  *out += EscapeField(value);
  *out += '\n';
}

// 只接受普通文件：调用方要么按 st_size 声明 payload 长度，要么按内容算摘要，
// 而目录 / FIFO / 设备节点没有稳定的“大小”语义，必须在这里挡住。
// 文件大小；失败返回 false。
bool FileSizeOf(const std::string& path, std::uint64_t* size,
                std::string* error_message) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    SetError(error_message, "Cannot stat " + path + ": " + ErrnoText(errno));
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    SetError(error_message, "Not a regular file: " + path);
    return false;
  }
  *size = static_cast<std::uint64_t>(info.st_size);
  return true;
}

// 只是给 ContentDigestOfFile 一个本地名字，让写侧的代码读起来就是“给 payload
// 算摘要”；64 KiB 流式 SHA-256 与小写十六进制的实现不在这里复制一份。
// 流式算一个文件的 SHA-256（读侧校验 payload 用）。
bool DigestOfFile(const std::string& path, std::string* hex,
                  std::string* error_message) {
  return ContentDigestOfFile(path, hex, error_message);
}

// 短写循环：write 可能只写一部分（信号、管道、配额），必须写到 size 耗尽。
// EINTR 重试，其它错误返回 false 并由调用方取 errno。它只保证字节进了内核，
// 不保证落盘 —— 落盘由调用方显式 fsync。
bool WriteAll(int fd, const void* data, std::size_t size) {
  const char* cursor = static_cast<const char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    const ssize_t written = ::write(fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

// 把 from 的内容全部追加到已打开的 to_fd（不 seek，偏移由调用方负责）。
// 64 KiB 分块：摊薄 syscall 开销，又不随 delta 大小增长内存占用。
// 失败时关闭 from_fd；to_fd 归调用方所有，这里不动它 —— 调用方要靠同一条
// 错误路径删掉那个写了一半的临时文件。
bool CopyFileContents(const std::string& from, int to_fd,
                      std::string* error_message) {
  const int from_fd = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (from_fd < 0) {
    SetError(error_message, "Cannot open " + from + ": " + ErrnoText(errno));
    return false;
  }
  std::vector<char> buffer(64u * 1024u);
  for (;;) {
    const ssize_t got = ::read(from_fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string text = ErrnoText(errno);
      ::close(from_fd);
      SetError(error_message, "Cannot read " + from + ": " + text);
      return false;
    }
    if (got == 0) break;
    if (!WriteAll(to_fd, buffer.data(), static_cast<std::size_t>(got))) {
      const std::string text = ErrnoText(errno);
      ::close(from_fd);
      SetError(error_message, "Cannot write delta payload: " + text);
      return false;
    }
  }
  if (::close(from_fd) != 0) {
    SetError(error_message, "Cannot close " + from + ": " + ErrnoText(errno));
    return false;
  }
  return true;
}

// 临时文件必须与目标同目录：rename 的原子性只在同一文件系统内成立。
// pid 后缀避免同机多进程（GUI 与 backupctl）踩到对方留下的半成品。
// 它只防碰撞，不是安全边界：创建时仍然必须带 O_EXCL。
std::string UniqueSiblingPath(const std::string& path, const char* suffix) {
  return path + "." + std::to_string(static_cast<unsigned long>(::getpid())) +
         suffix;
}

}  // namespace

std::string SourceIdentityDigest(const std::string& source_path,
                                 const std::string& repository_identity) {
  // 源身份 = SHA-256("BPSOURCE1\n" + 源路径 + 仓库身份)。把仓库身份也算进去是
  // 必须的：同一个源目录备份到两个仓库时，两条链必须互不相干，否则“路径没变”
  // 会让新仓库的链误认旧仓库的 parent。
  // 领域分隔：前缀不同，两个 identity 永远不会撞在一起。
  return ContentDigestOfBytes("BPSOURCE1\n" + source_path + "\n" +
                              repository_identity);
}

std::string FilterIdentityDigest(
    const std::vector<std::string>& include_rules,
    const std::vector<std::string>& exclude_rules) {
  // 条数也进摘要：["a","b"] 与 ["ab"] 因此不会撞。include / exclude 用
  // 'i:' / 'e:' 前缀区分，并按原顺序计入 —— 规则先后会影响匹配结论。
  std::string text = "BPFILTER1\n";
  text += "include=" + std::to_string(include_rules.size()) + "\n";
  for (const std::string& rule : include_rules) text += "i:" + rule + "\n";
  text += "exclude=" + std::to_string(exclude_rules.size()) + "\n";
  for (const std::string& rule : exclude_rules) text += "e:" + rule + "\n";
  return ContentDigestOfBytes(text);
}

std::string StrategyIdentityDigest(PackMethod pack,
                                   CompressionMethod compression,
                                   EncryptionMethod encryption) {
  // 存枚举数值而不是名字：任何一项策略变了，“如何解码这份 payload”的前提就
  // 不成立，链必须重建 Full baseline；用数值可避免将来改名被误判成策略变化。
  std::string text = "BPSTRATEGY1\n";
  text += std::to_string(static_cast<unsigned>(pack)) + "\n";
  text += std::to_string(static_cast<unsigned>(compression)) + "\n";
  text += std::to_string(static_cast<unsigned>(encryption)) + "\n";
  return ContentDigestOfBytes(text);
}

// 安全边界。parent_file_name 来自**不可信归档**，却会被拼成父快照的真实路径，
// 所以在格式层就逐条判死：空、"." / ".."、含 '/' 或 '\'（路径穿越）、含
// NUL（截断攻击），并要求它是 Catalog 管理的快照名（.bak）。
// 扩展名复用 IsManagedBackupFileName，而不是在这里再写一遍字面量，免得两处
// 规则漂移后出现“Catalog 不认、delta 认”的文件名。
bool IsValidDeltaParentFileName(const std::string& name,
                                std::string* error_message) {
  if (name.empty()) {
    SetError(error_message, "A delta parent file name must not be empty");
    return false;
  }
  if (name == "." || name == "..") {
    SetError(error_message, "Invalid delta parent file name: " + name);
    return false;
  }
  if (name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    SetError(
        error_message,
        "A delta parent file name must be a single path component: " + name);
    return false;
  }
  if (name.find('\0') != std::string::npos) {
    SetError(error_message, "A delta parent file name contains a NUL byte");
    return false;
  }
  // 扩展名规则与 Catalog 管理的备份文件名是同一条：这里复用它的判断，而不是
  // 自己再写一遍 ".bak"。
  if (!IsManagedBackupFileName(name)) {
    SetError(error_message,
             "A delta parent file name must be a managed snapshot name "
             "(ending in .bak): " +
                 name);
    return false;
  }
  return true;
}

// tombstone 是“要从还原结果里删掉的 archive_path”，会被用在真实文件系统上。
// 先复用归档路径的唯一语法实现（长度 / 绝对路径 / 空组件 / "." ".." 组件 /
// 反斜杠 / 盘符 / 结尾 '/' / NUL 都在那里被拒），这里只额外补一条：
// "." 是源根的身份，不是一条可以被删除的路径。
bool IsValidDeltaTombstone(const std::string& path,
                           std::string* error_message) {
  // 先复用归档路径的唯一语法实现：长度、绝对路径、空组件、"."/".." 组件、
  // 反斜杠、盘符、结尾 '/'、NUL 都在那里被拒。
  if (!IsValidArchivePath(path, /*is_first_entry=*/false,
                          /*is_directory=*/false, kMaxArchivePathLength,
                          error_message)) {
    return false;
  }
  // "." 是源根本身：它不是"某一条被删掉的路径"，而是整棵树的身份。
  // IsValidArchivePath 在 is_first_entry = false
  // 时已经拒绝它，这里再明确说一句，
  // 免得将来有人放宽那条规则时把根也一起放进来。
  if (path == ".") {
    SetError(error_message, "A tombstone may not remove the source root");
    return false;
  }
  return true;
}

// 规范化序列化：键序固定、数值十进制、无多余空白，因此同一份内容永远得到同一
// 串字节 —— snapshot_id 的自校验依赖这一点。列表键按元素重复写出。
//
// 键序与键名都是磁盘契约：旧版本按固定键名解析，遇到未知键会**明确拒绝**
// 而不是忽略，所以新增字段不能偷偷追加，只能连 format_version 一起升级。
std::string SerializeDeltaEnvelope(const DeltaEnvelope& envelope) {
  std::string out;
  out += "BPDELTA1\n";
  AppendKeyValue("format_version", std::to_string(envelope.format_version),
                 &out);
  AppendKeyValue("snapshot_id", envelope.snapshot_id, &out);
  AppendKeyValue("parent_file_name", envelope.parent_file_name, &out);
  AppendKeyValue("parent_snapshot_id", envelope.parent_snapshot_id, &out);
  AppendKeyValue("parent_manifest_digest", envelope.parent_manifest_digest,
                 &out);
  AppendKeyValue("base_generation_id", envelope.base_generation_id, &out);
  AppendKeyValue("source_identity", envelope.source_identity, &out);
  AppendKeyValue("filter_identity", envelope.filter_identity, &out);
  AppendKeyValue("strategy_identity", envelope.strategy_identity, &out);
  AppendKeyValue("current_manifest_digest", envelope.current_manifest_digest,
                 &out);
  AppendKeyValue("created", std::to_string(envelope.created_unix_seconds),
                 &out);
  AppendKeyValue("added", std::to_string(envelope.added), &out);
  AppendKeyValue("modified", std::to_string(envelope.modified), &out);
  AppendKeyValue("metadata_changed", std::to_string(envelope.metadata_changed),
                 &out);
  AppendKeyValue("removed", std::to_string(envelope.removed), &out);
  AppendKeyValue("payload_sha256", envelope.payload_sha256, &out);
  for (const std::string& path : envelope.tombstones) {
    AppendKeyValue("tombstone", path, &out);
  }
  for (const std::string& path : envelope.affected_directories) {
    AppendKeyValue("affected_dir", path, &out);
  }
  return out;
}

// snapshot_id 的定义：清空 snapshot_id 字段后对规范化序列化结果取 SHA-256。
// 先清空是为了打破自引用（摘要不能包含自己）；按值拷贝入参，因为调用方往往
// 还要拿原信封继续写盘。读侧用同一个函数重算，任何字段被改都会暴露。
std::string ComputeDeltaSnapshotId(const DeltaEnvelope& envelope) {
  DeltaEnvelope copy = envelope;
  copy.snapshot_id.clear();
  return ContentDigestOfBytes(SerializeDeltaEnvelope(copy));
}

// 严格解析，fail-closed。以下任何一处偏差都返回 false 并把原因写进
// error_message：长度超 kMaxDeltaEnvelopeBytes、头不匹配、行没有 '='、转义
// 非法、未知键、单值键重复或缺失、数值非十进制 / 越界、身份字段不是 SHA-256
// 十六进制形状、removed 与 tombstone 数量不符、parent 自引用、
// parent_file_name 或 tombstone 不满足各自的边界函数。
//
// 先限长再解析：畸形文件不会让这里分配出一个巨大的字符串；列表键各有
// kMaxDeltaTombstones 的上限，避免用一个信封把内存打满。
//
// 宁可拒绝一份“其实只是多了个新字段”的归档，也不去猜它的语义。返回 true 时
// envelope 一定是自洽且字段可信的 —— 这是 Catalog 展示、恢复、retention
// 能直接信任它的前提。
bool ParseDeltaEnvelope(const std::string& text, DeltaEnvelope* envelope,
                        std::string* error_message) {
  if (envelope == nullptr) {
    SetError(error_message, "Delta envelope output must not be null");
    return false;
  }
  *envelope = DeltaEnvelope{};

  if (text.size() > kMaxDeltaEnvelopeBytes) {
    SetError(error_message, "Delta envelope is too large: " +
                                std::to_string(text.size()) + " bytes");
    return false;
  }
  const std::string header = "BPDELTA1\n";
  if (text.compare(0, std::min(header.size(), text.size()), header) != 0) {
    SetError(error_message,
             "Invalid delta envelope: missing or wrong version header");
    return false;
  }

  // 每个单值键必须出现且只出现一次：delta 是机器写的，偏差就是状态坏了。
  std::map<std::string, int> seen;
  std::size_t position = header.size();
  while (position < text.size()) {
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string::npos) {
      SetError(error_message, "Invalid delta envelope: truncated line");
      return false;
    }
    const std::string line = text.substr(position, newline - position);
    position = newline + 1;
    if (line.empty()) continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      SetError(error_message, "Invalid delta envelope: a line has no '='");
      return false;
    }
    const std::string key = line.substr(0, equals);
    std::string value;
    if (!UnescapeField(line.substr(equals + 1), &value)) {
      SetError(error_message,
               "Invalid delta envelope: bad escape in key '" + key + "'");
      return false;
    }

    const bool is_list = key == "tombstone" || key == "affected_dir";
    if (!is_list) {
      const int count = ++seen[key];
      if (count > 1) {
        SetError(error_message,
                 "Invalid delta envelope: duplicate key '" + key + "'");
        return false;
      }
    }

    std::uint64_t number = 0;
    std::int64_t signed_number = 0;
    if (key == "format_version") {
      if (!ParseUint64(value, &number) || number != kDeltaFormatVersion) {
        SetError(error_message,
                 "Invalid delta envelope: unsupported format version '" +
                     value + "'");
        return false;
      }
      envelope->format_version = static_cast<std::uint32_t>(number);
    } else if (key == "snapshot_id") {
      envelope->snapshot_id = value;
    } else if (key == "parent_file_name") {
      envelope->parent_file_name = value;
    } else if (key == "parent_snapshot_id") {
      envelope->parent_snapshot_id = value;
    } else if (key == "parent_manifest_digest") {
      envelope->parent_manifest_digest = value;
    } else if (key == "base_generation_id") {
      envelope->base_generation_id = value;
    } else if (key == "source_identity") {
      envelope->source_identity = value;
    } else if (key == "filter_identity") {
      envelope->filter_identity = value;
    } else if (key == "strategy_identity") {
      envelope->strategy_identity = value;
    } else if (key == "current_manifest_digest") {
      envelope->current_manifest_digest = value;
    } else if (key == "created") {
      if (!ParseInt64(value, &signed_number)) {
        SetError(error_message, "Invalid delta envelope: bad created time");
        return false;
      }
      envelope->created_unix_seconds = signed_number;
    } else if (key == "added") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message, "Invalid delta envelope: bad added count");
        return false;
      }
      envelope->added = number;
    } else if (key == "modified") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message, "Invalid delta envelope: bad modified count");
        return false;
      }
      envelope->modified = number;
    } else if (key == "metadata_changed") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message,
                 "Invalid delta envelope: bad metadata_changed count");
        return false;
      }
      envelope->metadata_changed = number;
    } else if (key == "removed") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message, "Invalid delta envelope: bad removed count");
        return false;
      }
      envelope->removed = number;
    } else if (key == "payload_sha256") {
      envelope->payload_sha256 = value;
    } else if (key == "tombstone") {
      if (envelope->tombstones.size() >= kMaxDeltaTombstones) {
        SetError(error_message, "Invalid delta envelope: too many tombstones");
        return false;
      }
      envelope->tombstones.push_back(value);
    } else if (key == "affected_dir") {
      if (envelope->affected_directories.size() >= kMaxDeltaTombstones) {
        SetError(error_message,
                 "Invalid delta envelope: too many affected directories");
        return false;
      }
      envelope->affected_directories.push_back(value);
    } else {
      SetError(error_message,
               "Invalid delta envelope: unknown key '" + key + "'");
      return false;
    }
  }

  // kRequired 与上面的解析分支必须同步维护：新增字段时两边都要改，否则要么
  // 解析认了却不检查存在性，要么永远报缺键。
  // 单值键一个都不能少。
  static const char* kRequired[] = {"format_version",
                                    "snapshot_id",
                                    "parent_file_name",
                                    "parent_snapshot_id",
                                    "parent_manifest_digest",
                                    "base_generation_id",
                                    "source_identity",
                                    "filter_identity",
                                    "strategy_identity",
                                    "current_manifest_digest",
                                    "created",
                                    "added",
                                    "modified",
                                    "metadata_changed",
                                    "removed",
                                    "payload_sha256"};
  for (const char* key : kRequired) {
    if (seen[key] != 1) {
      SetError(
          error_message,
          std::string("Invalid delta envelope: missing key '") + key + "'");
      return false;
    }
  }
  // 身份与摘要字段必须是 SHA-256 十六进制。形状不对不是“缺字段”，而是文件被
  // 改过或根本不是本工具写的，只能拒。
  if (!IsContentDigest(envelope->snapshot_id) ||
      !IsContentDigest(envelope->parent_snapshot_id) ||
      !IsContentDigest(envelope->parent_manifest_digest) ||
      !IsContentDigest(envelope->base_generation_id) ||
      !IsContentDigest(envelope->source_identity) ||
      !IsContentDigest(envelope->filter_identity) ||
      !IsContentDigest(envelope->strategy_identity) ||
      !IsContentDigest(envelope->current_manifest_digest) ||
      !IsContentDigest(envelope->payload_sha256)) {
    SetError(error_message,
             "Invalid delta envelope: an identity or digest field is not a "
             "SHA-256 hex string");
    return false;
  }
  // 计数与列表必须一致：不一致说明信封被改过（或写侧有 bug），此时“以谁为准”
  // 没有正确答案，拒绝是唯一的确定行为。
  if (envelope->removed != envelope->tombstones.size()) {
    SetError(error_message,
             "Invalid delta envelope: removed count does not match the "
             "tombstone list");
    return false;
  }
  // 自引用必须被拒绝：否则链会出现一个指向自己的节点。
  if (envelope->snapshot_id == envelope->parent_snapshot_id) {
    SetError(error_message,
             "Invalid delta envelope: the snapshot is its own parent");
    return false;
  }
  // 不可信字段的边界：这两个字段会被用在真实文件系统上，格式层就必须把它们
  // 判死。放在这里而不是只放在应用路径，是因为每一个读者（Catalog 列表、
  // 恢复、retention）都要先过这一关。
  if (!IsValidDeltaParentFileName(envelope->parent_file_name, error_message)) {
    return false;
  }
  for (const std::string& tombstone : envelope->tombstones) {
    if (!IsValidDeltaTombstone(tombstone, error_message)) return false;
  }
  return true;
}

// 完整快照的**声明**身份：SHA-256("BPFULL1\n" + 容器 header 里声明的
// payload_sha256 十六进制)。它只读 header、不解密、不读 payload，所以不需要
// 密码；代价是它反映的是“文件自称的身份”。
//
// 边界：展示与快速分类可以，baseline / parent 绑定不行 —— 那条路径必须先用
// VerifyFullSnapshotPayload 证明实际字节与声明一致。
bool FullSnapshotId(const std::string& container_file, std::string* id,
                    std::string* error_message) {
  if (id == nullptr) {
    SetError(error_message, "Snapshot id output must not be null");
    return false;
  }
  id->clear();
  ContainerHeader header;
  if (!InspectContainerFile(container_file, &header, error_message)) {
    return false;
  }
  if (header.payload_sha256.size() != container_v2::kSha256Size) {
    SetError(
        error_message,
        "Cannot derive a snapshot id: the container has no payload digest");
    return false;
  }
  const std::string hex = crypto::ToHex(
      reinterpret_cast<const unsigned char*>(header.payload_sha256.data()),
      header.payload_sha256.size());
  *id = ContentDigestOfBytes("BPFULL1\n" + hex);
  return true;
}

// 任意快照文件的声明身份：delta 取信封里的 snapshot_id（ReadDeltaEnvelope 已
// 做过自校验），完整归档走 FullSnapshotId。未知类型或读失败一律 false，不返回
// “空 id 但成功”这种半真结果。
bool SnapshotIdOfFile(const std::string& path, std::string* id,
                      std::string* error_message) {
  const SnapshotFileKind kind = ClassifySnapshotFile(path, error_message);
  if (kind == SnapshotFileKind::kDelta) {
    DeltaEnvelope envelope;
    if (!ReadDeltaEnvelope(path, &envelope, error_message)) return false;
    if (id == nullptr) {
      SetError(error_message, "Snapshot id output must not be null");
      return false;
    }
    *id = envelope.snapshot_id;
    return true;
  }
  if (kind == SnapshotFileKind::kContainer) {
    return FullSnapshotId(path, id, error_message);
  }
  SetError(error_message, "Unknown snapshot file: " + path);
  return false;
}

// 只比 magic 8 字节，是嗅探而不是授权判断；size < 8 返回 false，绝不越界读。
// 调用方不能仅凭它为 true 就信任文件内容。
bool LooksLikeDelta(const unsigned char* data, std::size_t size) {
  if (data == nullptr || size < kDeltaMagicSize) return false;
  for (std::size_t index = 0; index < kDeltaMagicSize; ++index) {
    if (data[index] != kDeltaMagic[index]) return false;
  }
  return true;
}

// 读文件头 8 字节分流，供 Catalog / retention 快速判断 delta 还是完整归档。
// 打开或读失败都归 kUnknown，但 error_message 保留原因：“陌生文件”与“这个文件
// 读不了”对调用方是两件事。分类结果只是提示，任何真实读取都要重新走对应的
// 解析 + 校验路径。
SnapshotFileKind ClassifySnapshotFile(const std::string& path,
                                      std::string* error_message) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message, "Cannot open " + path + ": " + ErrnoText(errno));
    return SnapshotFileKind::kUnknown;
  }
  unsigned char head[kDeltaMagicSize] = {0};
  const ssize_t got = ::read(fd, head, sizeof(head));
  ::close(fd);
  if (got < 0) {
    SetError(error_message, "Cannot read " + path + ": " + ErrnoText(errno));
    return SnapshotFileKind::kUnknown;
  }
  if (LooksLikeDelta(head, static_cast<std::size_t>(got))) {
    return SnapshotFileKind::kDelta;
  }
  if (LooksLikeContainer(head, static_cast<std::size_t>(got))) {
    return SnapshotFileKind::kContainer;
  }
  return SnapshotFileKind::kUnknown;
}

namespace {

// BKPINC1 的定长头（24 字节，小端），字段顺序就是磁盘布局，不能重排：
//   offset  size  field
//   0       8     magic "BKPINC1\0"
//   8       2     format version (u16)
//   10      2     fixed header size (u16)
//   12      4     envelope length (u32)
//   16      8     payload length (u64)
// 之后紧跟 envelope 文本，再之后是内层 v2 container（也就是 payload）。
// BKPINC1 的 24 字节固定头：解析 + 全部长度层校验。多处以同一套规则读它，
// 所以只有这一份实现（magic / version / header size / envelope 长度上界 /
// payload 长度上界 / 文件长度必须正好等于三段之和）。
struct DeltaLayout {
  // 两个长度都是**已经校验过**的值：非零、在上界内、且与文件长度自洽，
  // 因此调用方可以直接把它们当偏移量用，不需要再判一次。
  std::uint32_t envelope_len = 0;
  std::uint64_t payload_len = 0;
};

// 定长头解析的唯一实现：magic / version / header size / envelope 与 payload
// 长度上界，以及“文件长度必须正好等于 24 + envelope_len + payload_len”。
// 多处以同一套规则读它，重复一份实现就等于制造第二套校验强度。
// 用 pread 读，不改文件偏移；短读按“头被截断”处理，而不是当成空文件。
bool ReadDeltaLayout(int fd, std::uint64_t file_size, DeltaLayout* layout,
                     std::string* error_message) {
  unsigned char header[kDeltaFixedHeaderSize] = {0};
  std::size_t filled = 0;
  while (filled < sizeof(header)) {
    const ssize_t got = ::pread(fd, header + filled, sizeof(header) - filled,
                                static_cast<off_t>(filled));
    if (got < 0) {
      if (errno == EINTR) continue;
      SetError(error_message, "Cannot read delta header: " + ErrnoText(errno));
      return false;
    }
    if (got == 0) break;
    filled += static_cast<std::size_t>(got);
  }
  if (filled < sizeof(header)) {
    SetError(error_message, "Invalid delta: the fixed header is truncated");
    return false;
  }
  if (!LooksLikeDelta(header, sizeof(header))) {
    SetError(error_message,
             "Invalid delta: wrong magic (this file is not a BKPINC1 delta)");
    return false;
  }
  const std::uint16_t version =
      static_cast<std::uint16_t>(header[8] | (header[9] << 8));
  const std::uint16_t fixed_size =
      static_cast<std::uint16_t>(header[10] | (header[11] << 8));
  if (version != kDeltaFormatVersion || fixed_size != kDeltaFixedHeaderSize) {
    SetError(error_message,
             "Invalid delta: unsupported format version or header size");
    return false;
  }
  const std::uint32_t envelope_len =
      static_cast<std::uint32_t>(header[12]) |
      (static_cast<std::uint32_t>(header[13]) << 8) |
      (static_cast<std::uint32_t>(header[14]) << 16) |
      (static_cast<std::uint32_t>(header[15]) << 24);
  std::uint64_t payload_len = 0;
  for (int index = 0; index < 8; ++index) {
    payload_len |= static_cast<std::uint64_t>(header[16 + index])
                   << (8 * index);
  }
  if (envelope_len == 0 || envelope_len > kMaxDeltaEnvelopeBytes) {
    SetError(error_message, "Invalid delta: implausible envelope length");
    return false;
  }
  if (payload_len == 0 || !IsAllowedStreamSize(payload_len)) {
    SetError(error_message, "Invalid delta: implausible payload length");
    return false;
  }
  // 文件长度必须正好等于 header + envelope + payload：多一个字节都不接受。
  const std::uint64_t expected =
      static_cast<std::uint64_t>(kDeltaFixedHeaderSize) + envelope_len +
      payload_len;
  if (file_size != expected) {
    SetError(error_message,
             "Invalid delta: file size does not match the declared envelope "
             "and payload lengths");
    return false;
  }
  layout->envelope_len = envelope_len;
  layout->payload_len = payload_len;
  return true;
}

// 读满 size 字节或失败。返回 false 时 error_message 区分 IO 错误与提前 EOF：
// 后者说明文件在两次读取之间被截断，属于不可信而不是暂时性错误，调用方不应
// 该重试。
// 从 fd 的给定偏移读满一段字节（pread，不改文件偏移）。
bool ReadExactlyAt(int fd, std::uint64_t offset, void* buffer, std::size_t size,
                   std::string* error_message) {
  unsigned char* cursor = static_cast<unsigned char*>(buffer);
  std::size_t filled = 0;
  while (filled < size) {
    const ssize_t got = ::pread(fd, cursor + filled, size - filled,
                                static_cast<off_t>(offset + filled));
    if (got < 0) {
      if (errno == EINTR) continue;
      SetError(error_message, "Cannot read delta bytes: " + ErrnoText(errno));
      return false;
    }
    if (got == 0) {
      SetError(error_message, "Invalid delta: truncated while reading");
      return false;
    }
    filled += static_cast<std::size_t>(got);
  }
  return true;
}

}  // namespace

// 只读信封：不解密、不读 payload、不需要密码 —— Catalog 在没有密码的情况下靠
// 它知道这是 delta、父是谁、能不能恢复。
//
// 检查顺序（每步失败即返回，不继续用半可信数据）：
//   1) 定长头与长度自洽（ReadDeltaLayout，唯一实现）；
//   2) 严格解析信封文本与字段级边界（ParseDeltaEnvelope）；
//   3) 自校验：重算 snapshot_id 必须等于信封里写的那个 —— 这是唯一能发现
//      “字段被逐个篡改”的地方。
//
// 成功时 *envelope 是完整重置过的（入口先赋默认值），失败时不留半份数据。
bool ReadDeltaEnvelope(const std::string& delta_file, DeltaEnvelope* envelope,
                       std::string* error_message) {
  if (envelope == nullptr) {
    SetError(error_message, "Delta envelope output must not be null");
    return false;
  }
  *envelope = DeltaEnvelope{};

  const int fd = ::open(delta_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message,
             "Cannot open delta " + delta_file + ": " + ErrnoText(errno));
    return false;
  }
  // 定长头的规则只有一份实现（ReadDeltaLayout）：magic / version /
  // header size / envelope 上界 / payload 上界 / 文件长度必须正好等于三段之和。
  // 这里再把它拄一遍就会出现第二套校验强度。
  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const std::string text = ErrnoText(errno);
    ::close(fd);
    SetError(error_message, "Cannot stat delta: " + text);
    return false;
  }
  DeltaLayout layout;
  if (!ReadDeltaLayout(fd, static_cast<std::uint64_t>(info.st_size), &layout,
                       error_message)) {
    ::close(fd);
    return false;
  }
  const std::uint32_t envelope_len = layout.envelope_len;
  // ReadDeltaLayout 用 pread（不动文件偏移），而下面读信封走的是顺序 read，
  // 所以必须显式把偏移摆到定长头之后。
  if (::lseek(fd, static_cast<off_t>(kDeltaFixedHeaderSize), SEEK_SET) < 0) {
    const std::string text = ErrnoText(errno);
    ::close(fd);
    SetError(error_message, "Cannot seek to the delta envelope: " + text);
    return false;
  }

  std::string text;
  text.resize(envelope_len);
  std::size_t read_bytes = 0;
  while (read_bytes < text.size()) {
    const ssize_t got = ::read(fd, &text[read_bytes], text.size() - read_bytes);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string error_text = ErrnoText(errno);
      ::close(fd);
      SetError(error_message, "Cannot read delta envelope: " + error_text);
      return false;
    }
    if (got == 0) break;
    read_bytes += static_cast<std::size_t>(got);
  }
  if (::close(fd) != 0) {
    SetError(error_message, "Cannot close delta: " + ErrnoText(errno));
    return false;
  }
  if (read_bytes != text.size()) {
    SetError(error_message, "Invalid delta: the envelope is truncated");
    return false;
  }
  if (!ParseDeltaEnvelope(text, envelope, error_message)) return false;
  // 自校验：snapshot_id 必须真的是这份 envelope 的摘要。
  if (ComputeDeltaSnapshotId(*envelope) != envelope->snapshot_id) {
    SetError(error_message,
             "Invalid delta: the snapshot id does not match the envelope "
             "contents");
    return false;
  }
  return true;
}

// 把内层 container **原样**抽到一个新文件（恢复路径用）。
//
// 目标文件用 O_CREAT|O_EXCL + 0600 创建：已存在就失败，绝不覆盖调用方指定的
// 路径，也不让半个 container 被误当成完整归档；任何一步失败都 unlink 掉它，
// 所以返回 false 时目标文件不存在。
//
// 刻意不 fsync：抽出来的 container 是本次恢复的中间产物，随后的流程立刻消费
// 它；需要持久化的是仓库里的 delta 本身（见 WriteDeltaFile）。它也不验证
// payload 的 SHA-256（那是 VerifyDeltaPayload 的职责）：这里只负责搬字节。
bool ExtractDeltaPayload(const std::string& delta_file,
                         const std::string& container_file,
                         std::string* error_message) {
  const int in_fd = ::open(delta_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (in_fd < 0) {
    SetError(error_message,
             "Cannot open delta " + delta_file + ": " + ErrnoText(errno));
    return false;
  }
  unsigned char header[kDeltaFixedHeaderSize] = {0};
  std::size_t filled = 0;
  while (filled < sizeof(header)) {
    const ssize_t got = ::read(in_fd, header + filled, sizeof(header) - filled);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string text = ErrnoText(errno);
      ::close(in_fd);
      SetError(error_message, "Cannot read delta header: " + text);
      return false;
    }
    if (got == 0) break;
    filled += static_cast<std::size_t>(got);
  }
  if (filled != sizeof(header) || !LooksLikeDelta(header, sizeof(header))) {
    ::close(in_fd);
    SetError(error_message, "Invalid delta: bad header while extracting");
    return false;
  }
  const std::uint32_t envelope_len =
      static_cast<std::uint32_t>(header[12]) |
      (static_cast<std::uint32_t>(header[13]) << 8) |
      (static_cast<std::uint32_t>(header[14]) << 16) |
      (static_cast<std::uint32_t>(header[15]) << 24);
  std::uint64_t payload_len = 0;
  for (int index = 0; index < 8; ++index) {
    payload_len |= static_cast<std::uint64_t>(header[16 + index])
                   << (8 * index);
  }
  if (::lseek(in_fd, static_cast<off_t>(kDeltaFixedHeaderSize + envelope_len),
              SEEK_SET) < 0) {
    const std::string text = ErrnoText(errno);
    ::close(in_fd);
    SetError(error_message, "Cannot seek to the delta payload: " + text);
    return false;
  }

  const int out_fd = ::open(container_file.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (out_fd < 0) {
    const std::string text = ErrnoText(errno);
    ::close(in_fd);
    SetError(error_message, "Cannot create " + container_file + ": " + text);
    return false;
  }
  std::vector<char> buffer(64u * 1024u);
  std::uint64_t remaining = payload_len;
  while (remaining > 0) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    const ssize_t got = ::read(in_fd, buffer.data(), want);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string text = ErrnoText(errno);
      ::close(in_fd);
      ::close(out_fd);
      ::unlink(container_file.c_str());
      SetError(error_message, "Cannot read delta payload: " + text);
      return false;
    }
    if (got == 0) {
      ::close(in_fd);
      ::close(out_fd);
      ::unlink(container_file.c_str());
      SetError(error_message, "Invalid delta: the payload is truncated");
      return false;
    }
    if (!WriteAll(out_fd, buffer.data(), static_cast<std::size_t>(got))) {
      const std::string text = ErrnoText(errno);
      ::close(in_fd);
      ::close(out_fd);
      ::unlink(container_file.c_str());
      SetError(error_message, "Cannot write the extracted payload: " + text);
      return false;
    }
    remaining -= static_cast<std::uint64_t>(got);
  }
  // 两个 fd 都必须尝试关闭：`close(in) != 0 || close(out) != 0` 会在
  // 第一个 close 失败时跳过第二个。诊断只保留第一次失败的 errno。
  int saved_error = 0;
  if (::close(in_fd) != 0) {
    saved_error = errno;
  }
  if (::close(out_fd) != 0 && saved_error == 0) {
    saved_error = errno;
  }
  if (saved_error != 0) {
    ::unlink(container_file.c_str());
    SetError(error_message, "Cannot close during payload extraction: " +
                                ErrnoText(saved_error));
    return false;
  }
  return true;
}

// 只读内层 container 的 header：不抽 payload、不解密、不需要密码。恢复路径用
// 它确认这份 delta 的 payload 没有被加密 —— 加密 delta 的明文外层信封
// （parent / tombstones）不受内层 HMAC 覆盖，接受它等于接受一组未经认证的
// 路径指令，因此 v1 里加密增量在启用侧就被明确拒绝。
//
// 实现上先让 ReadDeltaEnvelope 把外层整个校验一遍，再用 ReadDeltaLayout 取
// 偏移：布局规则只有一份，避免这里出现第二条更弱的检查。
bool InspectDeltaPayloadHeader(const std::string& delta_file,
                               ContainerHeader* header,
                               std::string* error_message) {
  if (header == nullptr) {
    SetError(error_message, "Container header output must not be null");
    return false;
  }
  *header = ContainerHeader{};
  // 布局规则只有一份实现：先让 ReadDeltaEnvelope 把外层整个读一遍
  // （magic / version / 长度自洽 / 信封自校验），再用 ReadDeltaLayout 取偏移。
  DeltaEnvelope envelope;
  if (!ReadDeltaEnvelope(delta_file, &envelope, error_message)) return false;

  const int fd = ::open(delta_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message,
             "Cannot open delta " + delta_file + ": " + ErrnoText(errno));
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const std::string text = ErrnoText(errno);
    ::close(fd);
    SetError(error_message, "Cannot stat delta: " + text);
    return false;
  }
  DeltaLayout layout;
  if (!ReadDeltaLayout(fd, static_cast<std::uint64_t>(info.st_size), &layout,
                       error_message)) {
    ::close(fd);
    return false;
  }
  unsigned char block[container_v2::kHeaderSize] = {0};
  if (!ReadExactlyAt(fd,
                     static_cast<std::uint64_t>(kDeltaFixedHeaderSize) +
                         layout.envelope_len,
                     block, sizeof(block), error_message)) {
    ::close(fd);
    return false;
  }
  if (::close(fd) != 0) {
    SetError(error_message, "Cannot close delta: " + ErrnoText(errno));
    return false;
  }
  return DecodeContainerHeader(block, sizeof(block), header, error_message);
}

// 证明这份 delta 的字节确实属于它自称的身份。三步，缺一不可：
//   1) 布局与信封（含 snapshot_id 自校验）；
//   2) 实际 payload 字节的 SHA-256 == 信封声明的 payload_sha256，就地流式
//      计算，不落临时文件；
//   3) 内层 container 的 header 可解码，且 header 声明的 payload 长度与 delta
//      声明的 payload 长度一致 —— payload 就是那个 container，不多不少。
//
// 不涉及密码：容器自己的 HMAC 仍由恢复路径验证。失败一律 fail-closed，宁可让
// 调用方重做 Full baseline，也不放行未经证实的字节流。
bool VerifyDeltaPayload(const std::string& delta_file,
                        std::string* error_message) {
  // 1) 信封：布局、字段边界、自身摘要（ReadDeltaEnvelope 内部全做）。
  DeltaEnvelope envelope;
  if (!ReadDeltaEnvelope(delta_file, &envelope, error_message)) return false;

  const int fd = ::open(delta_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message,
             "Cannot open delta " + delta_file + ": " + ErrnoText(errno));
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const std::string text = ErrnoText(errno);
    ::close(fd);
    SetError(error_message, "Cannot stat delta: " + text);
    return false;
  }
  DeltaLayout layout;
  if (!ReadDeltaLayout(fd, static_cast<std::uint64_t>(info.st_size), &layout,
                       error_message)) {
    ::close(fd);
    return false;
  }

  // 2) **实际 payload 字节**的 SHA-256 必须等于信封声明的 payload_sha256。
  //    就地流式读，不落任何临时文件：这一步是"身份可信"的前提，不是可选装饰。
  const std::uint64_t payload_offset =
      static_cast<std::uint64_t>(kDeltaFixedHeaderSize) + layout.envelope_len;
  // 分块 pread + 增量哈希：内存占用与 payload 大小无关，也不会为了校验落一个
  // 临时文件 —— 那正是“就地流式”要避免的。
  crypto::Sha256 sha;
  std::vector<unsigned char> buffer(64u * 1024u);
  std::uint64_t offset = 0;
  while (offset < layout.payload_len) {
    const std::uint64_t remaining = layout.payload_len - offset;
    const std::size_t want = static_cast<std::size_t>(
        remaining < buffer.size() ? remaining : buffer.size());
    const ssize_t got = ::pread(fd, buffer.data(), want,
                                static_cast<off_t>(payload_offset + offset));
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string text = ErrnoText(errno);
      ::close(fd);
      SetError(error_message, "Cannot read delta payload: " + text);
      return false;
    }
    if (got == 0) {
      ::close(fd);
      SetError(error_message, "Invalid delta: the payload is truncated");
      return false;
    }
    sha.Update(buffer.data(), static_cast<std::size_t>(got));
    offset += static_cast<std::uint64_t>(got);
  }
  unsigned char digest[crypto::kSha256DigestSize];
  sha.Final(digest);
  const std::string actual = crypto::ToHex(digest, crypto::kSha256DigestSize);
  if (actual != envelope.payload_sha256) {
    ::close(fd);
    SetError(error_message,
             "Delta payload checksum mismatch: the actual payload bytes do not "
             "match the digest declared in the envelope (" +
                 delta_file + ")");
    return false;
  }

  // 3) 内层 container 自己也要自洽：header 可解码，而且它的长度正好等于
  //    这份 payload 的长度（payload 就是那个 container，一个字节都不多不少）。
  unsigned char header_block[container_v2::kHeaderSize] = {0};
  if (!ReadExactlyAt(fd, payload_offset, header_block, sizeof(header_block),
                     error_message)) {
    ::close(fd);
    return false;
  }
  ContainerHeader header;
  if (!DecodeContainerHeader(header_block, sizeof(header_block), &header,
                             error_message)) {
    ::close(fd);
    return false;
  }
  if (static_cast<std::uint64_t>(container_v2::kHeaderSize) +
          header.payload_size !=
      layout.payload_len) {
    ::close(fd);
    SetError(error_message,
             "Invalid delta: the inner container length does not match the "
             "delta payload length");
    return false;
  }
  if (::close(fd) != 0) {
    SetError(error_message, "Cannot close delta: " + ErrnoText(errno));
    return false;
  }
  return true;
}

// 完整快照这一侧的实际字节身份入口：复用 v2 容器自己的完整性规则（header 可
// 解码、文件长度自洽、实际 payload 区的 SHA-256 == header 声明值），成功时把
// **验证过的**摘要以十六进制带出来。
//
// 与 FullSnapshotId 的差别就是这一条：后者读的是声明值，只能用于展示；
// baseline / parent / 恢复链成员的身份必须走这里。加密容器同样适用。
bool VerifyFullSnapshotPayload(const std::string& container_file,
                               std::string* payload_sha256_hex,
                               std::string* error_message) {
  ContainerHeader header;
  if (!VerifyContainerPayloadBytes(container_file, &header, error_message)) {
    return false;
  }
  if (payload_sha256_hex != nullptr) {
    *payload_sha256_hex = crypto::ToHex(
        reinterpret_cast<const unsigned char*>(header.payload_sha256.data()),
        header.payload_sha256.size());
  }
  return true;
}

// 写出 delta 的唯一入口。前置条件（任一不满足即失败，不尽力而为）：
//   * changed_entries.size() <= kMaxDeltaEntries；
//   * options.pack_method == kMyPack：USTAR 表达不了 tombstone 与 parent
//     依赖，与其伪装支持不如明确拒绝（共享校验层会报同一句话）；
//   * envelope.removed == envelope.tombstones.size()；
//   * parent_file_name 与每个 tombstone 都通过读侧那一对边界函数 ——
//     写侧不产出自己随后会拒绝的东西；
//   * changed_entries 第一条必须是源根（"." 目录）：应用完 delta 之后根目录的
//     metadata 也要有人负责。这里不替调用方伪造一条根记录，根的 metadata 必须
//     来自真实扫描。
//
// 执行顺序（失败即 break，清理统一在 do/while 之外做）：
//   1) 内层 container 写到 <target>.<pid>.container，复用完整备份流水线；
//   2) 算 payload_sha256，再算自引用 snapshot_id，序列化信封；
//   3) 拼 24 字节小端定长头；
//   4) <target>.<pid>.part 用 O_EXCL + 0600 写“头 + 信封 + payload”，fsync、
//      close，最后 rename 到目标名。
//
// 原子性：rename 在同一文件系统内原子，读者只会看到完整可用的 delta 或根本
// 没有，不会看到半截文件；失败时临时文件全部 unlink，已存在的目标文件不会被
// 动过。本函数不做互斥，同一目标路径的并发写由调用方的 OperationGate 保证。
bool WriteDeltaFile(const std::string& delta_file,
                    const DeltaEnvelope& envelope,
                    const std::vector<ArchiveEntry>& changed_entries,
                    const BackupOptions& options, std::string* error_message) {
  if (changed_entries.size() > kMaxDeltaEntries) {
    SetError(error_message, "Too many entries for one delta: " +
                                std::to_string(changed_entries.size()));
    return false;
  }
  // 第一版只支持 MyPack：USTAR 表达不了 tombstone 与 parent dependency，
  // 与其"伪装支持"不如明确拒绝。共享校验层会报同一句话。
  if (options.pack_method != PackMethod::kMyPack) {
    SetError(error_message,
             "Incremental deltas require the MyPack pack method; USTAR cannot "
             "express tombstone or parent dependencies");
    return false;
  }
  if (envelope.removed != envelope.tombstones.size()) {
    SetError(error_message,
             "Delta envelope: removed count must equal the tombstone count");
    return false;
  }
  // 写侧过的是与读侧同一对校验：自己不产出自己随后拒绝的东西。
  if (!IsValidDeltaParentFileName(envelope.parent_file_name, error_message)) {
    return false;
  }
  for (const std::string& tombstone : envelope.tombstones) {
    if (!IsValidDeltaTombstone(tombstone, error_message)) return false;
  }
  // payload 复用备份流水线，条目表的约定也就与备份完全一致：第一条是源根。
  // 不替调用方伪造一条根记录——根目录的 metadata 必须来自真实扫描。
  if (changed_entries.empty() || changed_entries.front().archive_path != "." ||
      changed_entries.front().type != EntryType::kDirectory) {
    SetError(error_message,
             "Incremental delta entries must start with the source root");
    return false;
  }

  // 两个临时文件都与目标同目录（rename 的原子性只在同一文件系统内成立）。
  // 先 unlink 是防御性的：上一次崩溃留下的同名残骸不能挡住这一次 —— 下面
  // 创建时带 O_EXCL，不先清掉就会直接失败。
  const std::string temp_container =
      UniqueSiblingPath(delta_file, ".container");
  const std::string temp_delta = UniqueSiblingPath(delta_file, ".part");
  ::unlink(temp_container.c_str());
  ::unlink(temp_delta.c_str());

  bool ok = false;
  do {
    if (!RunBackupPipelineFromEntries(changed_entries, temp_container, options,
                                      error_message)) {
      break;
    }
    std::uint64_t payload_len = 0;
    if (!FileSizeOf(temp_container, &payload_len, error_message)) break;

    DeltaEnvelope filled = envelope;
    if (!DigestOfFile(temp_container, &filled.payload_sha256, error_message)) {
      break;
    }
    filled.snapshot_id = ComputeDeltaSnapshotId(filled);
    const std::string text = SerializeDeltaEnvelope(filled);
    if (text.size() > kMaxDeltaEnvelopeBytes) {
      SetError(error_message, "Delta envelope is too large");
      break;
    }

    unsigned char header[kDeltaFixedHeaderSize] = {0};
    for (std::size_t index = 0; index < kDeltaMagicSize; ++index) {
      header[index] = kDeltaMagic[index];
    }
    header[8] = static_cast<unsigned char>(kDeltaFormatVersion & 0xFF);
    header[9] = static_cast<unsigned char>((kDeltaFormatVersion >> 8) & 0xFF);
    header[10] = static_cast<unsigned char>(kDeltaFixedHeaderSize & 0xFF);
    header[11] =
        static_cast<unsigned char>((kDeltaFixedHeaderSize >> 8) & 0xFF);
    const std::uint32_t envelope_len = static_cast<std::uint32_t>(text.size());
    for (int index = 0; index < 4; ++index) {
      header[12 + index] =
          static_cast<unsigned char>((envelope_len >> (8 * index)) & 0xFF);
    }
    for (int index = 0; index < 8; ++index) {
      header[16 + index] =
          static_cast<unsigned char>((payload_len >> (8 * index)) & 0xFF);
    }

    // 临界区：头、信封、payload 三段全部写入并 fsync 之后才 rename，且 fsync
    // 在 close 之前 —— 保证 rename 可见时内容已经在盘上，而不是只躺在页缓存里
    // （崩溃后 rename 过的文件会变成零长度或带空洞）。
    // 唯一临时文件 + fsync + rename：任何一步失败都不留半成品。
    const int fd = ::open(temp_delta.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      SetError(error_message,
               "Cannot create " + temp_delta + ": " + ErrnoText(errno));
      break;
    }
    if (!WriteAll(fd, header, sizeof(header)) ||
        !WriteAll(fd, text.data(), text.size()) ||
        !CopyFileContents(temp_container, fd, error_message)) {
      ::close(fd);
      ::unlink(temp_delta.c_str());
      if (error_message != nullptr && error_message->empty()) {
        SetError(error_message, "Cannot write the delta file");
      }
      break;
    }
    if (::fsync(fd) != 0) {
      const std::string text_error = ErrnoText(errno);
      ::close(fd);
      ::unlink(temp_delta.c_str());
      SetError(error_message, "Cannot fsync the delta file: " + text_error);
      break;
    }
    if (::close(fd) != 0) {
      ::unlink(temp_delta.c_str());
      SetError(error_message,
               "Cannot close the delta file: " + ErrnoText(errno));
      break;
    }
    if (::rename(temp_delta.c_str(), delta_file.c_str()) != 0) {
      const std::string text_error = ErrnoText(errno);
      ::unlink(temp_delta.c_str());
      SetError(error_message, "Cannot publish the delta file: " + text_error);
      break;
    }
    ok = true;
  } while (false);

  ::unlink(temp_container.c_str());
  // 统一清理点：container 临时文件无论成败都删；part 只在失败时删 —— 成功路径
  // 上它已经被 rename 掉了。这里不看 unlink 的返回值：清理失败不该把一个已经
  // 成功发布的 delta 说成失败。
  if (!ok) ::unlink(temp_delta.c_str());
  return ok;
}

}  // namespace backupproject

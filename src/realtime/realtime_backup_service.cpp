// realtime_backup_service.cpp
//
// 见 include/realtime_backup_service.h。

// 模块职责：把"一次已经稳定的 realtime 触发"落成一次真实备份，并为产出的快照
// 写下可验证的归属。这里只做编排：Full 走 BackupEngine，Incremental 走
// RunIncrementalBackup，淘汰走 BackupCatalog::DeleteSnapshots。
//
// 明确不做：不监听文件系统、不决定何时触发、不自己实现扫描 / 打包 / 压缩 /
// 加密。那些都在 watcher、trigger 与 archive 层。
//
// 数据流：RealtimeEventSummary -> RunRealtimeBackupOnce -> .bak 归档
//   -> LoadVerifiedSnapshotIdentity（只信实际字节）-> <snapshot>.realtime
//   -> RunRealtimeRetention（依赖感知淘汰）。归属只有 per-snapshot marker
//   一个来源，没有中央可变 state 文件，因此不存在"监听目录时状态文件改到
//   自己"的回环。
//
// 不变量：marker 的 snapshot_id 必须等于从归档字节算出的 verified id；一份
//   marker 只属于同名 .bak；archive 先发布，marker 后发布。
//
// 失败边界：全部函数返回 bool + error_message，不抛异常。归档写成而 marker
//   写失败时降级为"成功 + warning"，并放弃破坏性 retention：宁可少删，也不能
//   在归属不可信时删。
//
// 线程：本文件没有跨调用的全局状态（唯一例外是测试开关），也不缓存 fd；
//   并发调用需要调用方自己串行化。
#include "realtime_backup_service.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "backup_engine.h"
#include "backup_option_keys.h"
#include "incremental_delta.h"
#include "source_digest.h"
#include "source_manifest.h"

namespace backupproject {

namespace {

// 统一的错误出口：error_message 允许为 null（有的调用方不关心原因），但所有
// 公开函数都遵守"返回 false 时，若给了 error_message 就填上人能读懂的原因"
// 这条契约，调用方据此决定是重试、降级报 warning，还是中断整次备份。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// strerror 返回进程内静态缓冲，多线程下会被覆盖，所以立刻拷进 std::string，
// 绝不把 const char* 存下来延迟使用。理论上它也可能返回 null，那时退化成
// "errno N"，宁可少信息也不要空指针解引用。
std::string ErrnoText(int error_number) {
  const char* text = ::strerror(error_number);
  return text == nullptr ? std::string("errno ") + std::to_string(error_number)
                         : std::string(text);
}

// 只拼接，不做规范化、不解析 ".."：调用方必须保证 name 已经是单组件名字
// （见 IsManagedBackupFileName），否则这里就是一条路径穿越通道。
std::string JoinPath(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  if (directory.back() == '/') return directory + name;
  return directory + "/" + name;
}

// 测试接缝：让下一次 marker 发布故意失败，用来验证"归档成功 + marker 失败"
// 这条降级路径（成功但带 warning，且不执行破坏性 retention）。进程级变量，
// 只有测试会设置，生产路径永远读到 false。
bool g_marker_write_failure_for_testing = false;

// marker 是逐行的 key=value 文本，换行就是记录分隔符。POSIX 路径可以含换行
// 与反斜杠，写入前必须转义，否则一个带换行的值能把后面几行"伪造"成别的
// 字段。转义表刻意保持最小：反斜杠、\n、\r、\t，其余字节原样透传。
std::string EscapeField(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(character);
    }
  }
  return out;
}

// 转义的反向操作，比写侧严格：不认识的转义序列、结尾孤立的反斜杠一律返回
// false。宽松解码会把坏字节变成另一个合法值，读侧拿到的就不是磁盘上的真实
// 内容了。
bool UnescapeField(const std::string& text, std::string* value) {
  value->clear();
  value->reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '\\') {
      value->push_back(text[index]);
      continue;
    }
    if (index + 1 >= text.size()) return false;
    switch (text[++index]) {
      case '\\':
        value->push_back('\\');
        break;
      case 'n':
        value->push_back('\n');
        break;
      case 'r':
        value->push_back('\r');
        break;
      case 't':
        value->push_back('\t');
        break;
      default:
        return false;
    }
  }
  return true;
}

// 一条记录固定是 key=value 加换行；值先转义再拼接。AppendNumber 只是把整数
// 转成十进制文本走同一条路径，保证写出来的东西读侧能原样解析回来。
void AppendField(std::string* out, const char* key, const std::string& value) {
  *out += key;
  *out += "=";
  *out += EscapeField(value);
  *out += "\n";
}

void AppendNumber(std::string* out, const char* key, std::uint64_t value) {
  AppendField(out, key, std::to_string(value));
}

// 手写十进制解析，不用 strtoull：后者会接受前导空白、'+' 与 "0x" 前缀，
// 还用 ERANGE 而不是返回值表示溢出。marker 里的数字要么全是数字要么拒绝；
// 溢出在乘之前用 (UINT64_MAX - digit) / 10 判断，不依赖回绕行为。
bool ParseUint64(const std::string& text, std::uint64_t* value) {
  if (text.empty() || text.size() > 20) return false;
  std::uint64_t result = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (UINT64_MAX - digit) / 10) return false;
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

// 只接受可选的 '-' 前缀：不接受 '+'、空白，也不接受 -2^63（magnitude 上限
// 取 INT64_MAX）。可解析的范围窄一点没关系，宽一点才是风险。
bool ParseInt64(const std::string& text, std::int64_t* value) {
  if (text.empty()) return false;
  bool negative = false;
  std::string digits = text;
  if (digits[0] == '-') {
    negative = true;
    digits = digits.substr(1);
  }
  std::uint64_t magnitude = 0;
  if (!ParseUint64(digits, &magnitude)) return false;
  if (magnitude > static_cast<std::uint64_t>(INT64_MAX)) return false;
  *value = negative ? -static_cast<std::int64_t>(magnitude)
                    : static_cast<std::int64_t>(magnitude);
  return true;
}

// 读一份 marker 文件。仓库目录是用户可写的，所以按不可信输入对待：O_NOFOLLOW
// 阻止别人用符号链接把读取引到别处；fstat + S_ISREG 确认是普通文件；先按
// kMaxRealtimeMarkerBytes 卡大小上限，避免被超大文件撑爆内存。读取循环处理
// EINTR 与短读，最后要求 fstat 的大小与实际读到的字节数一致，少一个字节就
// 算 truncated —— 半份内容比没有内容更危险。
bool ReadWholeFile(const std::string& path, std::string* text,
                   std::string* error_message) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message, "Cannot open " + path + ": " + ErrnoText(errno));
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    SetError(error_message, "Not a regular file: " + path);
    return false;
  }
  if (static_cast<std::uint64_t>(info.st_size) > kMaxRealtimeMarkerBytes) {
    ::close(fd);
    SetError(error_message, "Marker is too large: " + path);
    return false;
  }
  text->clear();
  text->resize(static_cast<std::size_t>(info.st_size));
  std::size_t filled = 0;
  while (filled < text->size()) {
    const ssize_t got = ::read(fd, &(*text)[filled], text->size() - filled);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string message = ErrnoText(errno);
      ::close(fd);
      SetError(error_message, "Cannot read " + path + ": " + message);
      return false;
    }
    if (got == 0) break;
    filled += static_cast<std::size_t>(got);
  }
  ::close(fd);
  if (filled != text->size()) {
    SetError(error_message, "Marker is truncated: " + path);
    return false;
  }
  return true;
}

}  // namespace

void SetRealtimeMarkerWriteFailureForTesting(bool fail) {
  g_marker_write_failure_for_testing = fail;
}

// marker 与它描述的归档同名，只加后缀：归属关系写进文件名本身，于是"找某份
// 快照的 marker"是一次字符串拼接，而不是去解析某个索引文件。
std::string RealtimeMarkerFileName(const std::string& snapshot_file_name) {
  return snapshot_file_name + kRealtimeMarkerSuffix;
}

// 判断一个目录项是不是本项目的 marker，并回填它描述的 .bak 名字。
// 三层过滤缺一不可：长度必须大于后缀（".realtime" 本身不是 marker）、后缀
// 必须匹配、去掉后缀后必须是受管理的备份文件名。第三条是安全边界：仓库里
// 用户自己放的 notes.realtime / report.realtime / foo.txt.realtime 都不算
// "我们的"，不会被列出，更不会被 retention 删掉。
bool IsRealtimeMarkerFileName(const std::string& file_name,
                              std::string* snapshot_file_name) {
  const std::size_t suffix_len = ::strlen(kRealtimeMarkerSuffix);
  if (file_name.size() <= suffix_len) return false;
  if (file_name.compare(file_name.size() - suffix_len, suffix_len,
                        kRealtimeMarkerSuffix) != 0) {
    return false;
  }
  const std::string base = file_name.substr(0, file_name.size() - suffix_len);
  // 只有"受管理的 .bak 名字 + .realtime"才可能是项目拥有的 marker：
  // notes.realtime / report.realtime / foo.txt.realtime 都不是。
  if (!IsManagedBackupFileName(base)) return false;
  if (snapshot_file_name != nullptr) *snapshot_file_name = base;
  return true;
}

// 序列化结果就是磁盘格式：首行固定 header，其后每行一个 key=value，字段顺序
// 由这里决定并与 ParseRealtimeMarker 的 kRequired 表一一对应。值是文本，所以
// 空值也会写出来（写侧永远写全 17 个 key，读侧要求全在）。
//
// 兼容性约束：解析器对未知 key 直接拒绝，因此这个格式不是前向兼容的。加字段
// 要么同时升级 header 版本号，要么保证旧版本永远不会读到新 marker；只加字段
// 不改版本会让老程序把新 marker 判成坏数据。
std::string SerializeRealtimeMarker(const RealtimeMarker& marker) {
  std::string out = kRealtimeMarkerHeader;
  AppendField(&out, "snapshot_file_name", marker.snapshot_file_name);
  AppendField(&out, "snapshot_id", marker.snapshot_id);
  AppendNumber(&out, "created_time_sec",
               static_cast<std::uint64_t>(marker.created_time_sec));
  AppendField(&out, "job_identity", marker.job_identity);
  AppendField(&out, "source_identity", marker.source_identity);
  AppendField(&out, "filter_identity", marker.filter_identity);
  AppendField(&out, "strategy", BackupStrategyKey(marker.strategy));
  AppendField(&out, "pack", PackMethodKey(marker.pack_method));
  AppendField(&out, "compression",
              CompressionMethodKey(marker.compression_method));
  AppendNumber(&out, "event_count", marker.event_count);
  AppendField(&out, "overflow_recovery", marker.overflow_recovery ? "1" : "0");
  AppendField(&out, "resync_trigger", marker.resync_trigger ? "1" : "0");
  AppendField(&out, "outcome_kind", marker.outcome_kind);
  AppendNumber(&out, "added", marker.added);
  AppendNumber(&out, "removed", marker.removed);
  AppendNumber(&out, "modified", marker.modified);
  AppendNumber(&out, "metadata_changed", marker.metadata_changed);
  return out;
}

// 严格白名单解析，任何一处可疑都返回 false，绝不"尽力解析"：header 必须逐
// 字节匹配；每行必须有 '='；未知 key 拒绝；同一个 key 出现两次也拒绝（否则
// 后一个值会静默覆盖前一个）；kRequired 里的 17 个 key 一个都不能少。
//
// 每个值都做类型校验：摘要必须正好 64 个十六进制字符（IsContentDigest）、
// 枚举 key 必须能被 ParseXxxKey 认出来、布尔只能是 "0"/"1"、计数必须能按
// 无符号十进制解析。
//
// 收尾还有两道语义检查：snapshot_file_name 必须是受管理的备份名（否则会被
// JoinPath 当成路径拼接），outcome_kind 必须是四个已知取值之一。全部通过才
// 算可信；调用方拿到 false 时必须把这份 marker 当不可信数据，既不显示成
// 正常记录，更不能据此做删除。
bool ParseRealtimeMarker(const std::string& text, RealtimeMarker* marker,
                         std::string* error_message) {
  if (marker == nullptr) {
    SetError(error_message, "Marker output must not be null");
    return false;
  }
  *marker = RealtimeMarker{};
  if (text.size() > kMaxRealtimeMarkerBytes) {
    SetError(error_message, "Marker is too large");
    return false;
  }
  const std::string header = kRealtimeMarkerHeader;
  if (text.compare(0, std::min(header.size(), text.size()), header) != 0) {
    SetError(error_message, "Invalid realtime marker: wrong header");
    return false;
  }
  static const char* kRequired[] = {"snapshot_file_name",
                                    "snapshot_id",
                                    "created_time_sec",
                                    "job_identity",
                                    "source_identity",
                                    "filter_identity",
                                    "strategy",
                                    "pack",
                                    "compression",
                                    "event_count",
                                    "overflow_recovery",
                                    "resync_trigger",
                                    "outcome_kind",
                                    "added",
                                    "removed",
                                    "modified",
                                    "metadata_changed"};
  std::map<std::string, int> seen;
  std::size_t position = header.size();
  while (position < text.size()) {
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string::npos) {
      SetError(error_message, "Invalid realtime marker: truncated line");
      return false;
    }
    const std::string line = text.substr(position, newline - position);
    position = newline + 1;
    if (line.empty()) continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      SetError(error_message, "Invalid realtime marker: a line has no '='");
      return false;
    }
    const std::string key = line.substr(0, equals);
    std::string value;
    if (!UnescapeField(line.substr(equals + 1), &value)) {
      SetError(error_message,
               "Invalid realtime marker: bad escape in key '" + key + "'");
      return false;
    }
    if (++seen[key] > 1) {
      SetError(error_message,
               "Invalid realtime marker: duplicate key '" + key + "'");
      return false;
    }

    std::uint64_t number = 0;
    std::int64_t signed_number = 0;
    if (key == "snapshot_file_name") {
      marker->snapshot_file_name = value;
    } else if (key == "snapshot_id") {
      if (!IsContentDigest(value)) {
        SetError(error_message, "Invalid realtime marker: bad snapshot id");
        return false;
      }
      marker->snapshot_id = value;
    } else if (key == "created_time_sec") {
      if (!ParseInt64(value, &signed_number)) {
        SetError(error_message, "Invalid realtime marker: bad created time");
        return false;
      }
      marker->created_time_sec = signed_number;
    } else if (key == "job_identity") {
      if (!IsContentDigest(value)) {
        SetError(error_message, "Invalid realtime marker: bad job identity");
        return false;
      }
      marker->job_identity = value;
    } else if (key == "source_identity") {
      marker->source_identity = value;
    } else if (key == "filter_identity") {
      marker->filter_identity = value;
    } else if (key == "strategy") {
      if (!ParseBackupStrategyKey(value, &marker->strategy)) {
        SetError(error_message,
                 "Invalid realtime marker: unknown strategy '" + value + "'");
        return false;
      }
    } else if (key == "pack") {
      if (!ParsePackMethodKey(value, &marker->pack_method)) {
        SetError(error_message,
                 "Invalid realtime marker: unknown pack '" + value + "'");
        return false;
      }
    } else if (key == "compression") {
      if (!ParseCompressionMethodKey(value, &marker->compression_method)) {
        SetError(
            error_message,
            "Invalid realtime marker: unknown compression '" + value + "'");
        return false;
      }
    } else if (key == "event_count") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message, "Invalid realtime marker: bad event count");
        return false;
      }
      marker->event_count = number;
    } else if (key == "overflow_recovery" || key == "resync_trigger") {
      if (value != "0" && value != "1") {
        SetError(error_message,
                 "Invalid realtime marker: '" + key + "' must be 0 or 1");
        return false;
      }
      const bool flag = value == "1";
      if (key == "overflow_recovery") {
        marker->overflow_recovery = flag;
      } else {
        marker->resync_trigger = flag;
      }
    } else if (key == "outcome_kind") {
      marker->outcome_kind = value;
    } else if (key == "added" || key == "removed" || key == "modified" ||
               key == "metadata_changed") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message,
                 "Invalid realtime marker: bad count for '" + key + "'");
        return false;
      }
      if (key == "added") {
        marker->added = number;
      } else if (key == "removed") {
        marker->removed = number;
      } else if (key == "modified") {
        marker->modified = number;
      } else {
        marker->metadata_changed = number;
      }
    } else {
      SetError(error_message,
               "Invalid realtime marker: unknown key '" + key + "'");
      return false;
    }
  }
  for (const char* key : kRequired) {
    if (seen[key] != 1) {
      SetError(
          error_message,
          std::string("Invalid realtime marker: missing key '") + key + "'");
      return false;
    }
  }
  if (!IsManagedBackupFileName(marker->snapshot_file_name)) {
    SetError(error_message,
             "Invalid realtime marker: snapshot_file_name is not a managed "
             "backup name");
    return false;
  }
  if (marker->outcome_kind != "full" &&
      marker->outcome_kind != "full-baseline" &&
      marker->outcome_kind != "delta" && marker->outcome_kind != "no-changes") {
    SetError(error_message, "Invalid realtime marker: unknown outcome kind '" +
                                marker->outcome_kind + "'");
    return false;
  }
  return true;
}

// 发布 marker，语义是"要么完整可见，要么完全不存在"。
//
// 1) 先序列化，再用自己的 parser 回读一遍：写出去的东西自己都读不回来时
//    宁可不写。一份坏 marker 会让快照在列表里变成"不可验证"，比没有更糟。
// 2) 写同目录临时文件 <path>.tmp（先 unlink 上次可能残留的那份），O_EXCL
//    创建、权限 0600，避免两个写者交错写同一个文件。
// 3) fsync + close 之后才 rename 覆盖目标：rename 在同一文件系统内原子，
//    于是崩溃只会留下 .tmp，不会留下半截 marker。
// 4) 任何一步失败都 unlink 临时文件再返回 false，由调用方降级处理。这里不
//    fsync 父目录：掉电最坏的结果是 marker 消失（归档仍在，只是没人认领），
//    而不是出现一份半截的、看起来可信的 marker。
bool WriteRealtimeMarker(const std::string& repository_directory,
                         const RealtimeMarker& marker,
                         std::string* error_message) {
  if (g_marker_write_failure_for_testing) {
    SetError(error_message,
             "Injected realtime marker publish failure (for testing)");
    return false;
  }
  const std::string text = SerializeRealtimeMarker(marker);
  RealtimeMarker verified;
  std::string parse_error;
  if (!ParseRealtimeMarker(text, &verified, &parse_error)) {
    SetError(error_message,
             "Refusing to write an invalid realtime marker: " + parse_error);
    return false;
  }
  const std::string path = JoinPath(
      repository_directory, RealtimeMarkerFileName(marker.snapshot_file_name));
  const std::string temp = path + ".tmp";
  ::unlink(temp.c_str());
  const int fd =
      ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    SetError(error_message, "Cannot create " + temp + ": " + ErrnoText(errno));
    return false;
  }
  std::size_t written = 0;
  while (written < text.size()) {
    const ssize_t got =
        ::write(fd, text.data() + written, text.size() - written);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string message = ErrnoText(errno);
      ::close(fd);
      ::unlink(temp.c_str());
      SetError(error_message, "Cannot write " + temp + ": " + message);
      return false;
    }
    written += static_cast<std::size_t>(got);
  }
  // fsync 与 close 必须**各自**尝试：`fsync(fd) != 0 || close(fd) != 0` 会在
  // fsync 失败时短路掉 close，每失败一次泄漏一个 fd（ENOSPC/EIO 正是会连续
  // 失败的那种场景）。saved_error 只记第一次失败的原因，后面的清理步骤不得
  // 改写它。
  int saved_error = 0;
  if (::fsync(fd) != 0) {
    saved_error = errno;
  }
  if (::close(fd) != 0 && saved_error == 0) {
    saved_error = errno;
  }
  if (saved_error != 0) {
    const std::string message = ErrnoText(saved_error);
    ::unlink(temp.c_str());
    SetError(error_message, "Cannot flush " + temp + ": " + message);
    return false;
  }
  if (::rename(temp.c_str(), path.c_str()) != 0) {
    const std::string message = ErrnoText(errno);
    ::unlink(temp.c_str());
    SetError(error_message, "Cannot publish " + path + ": " + message);
    return false;
  }
  return true;
}

// 读单份 marker，并确认它确实属于请求的那份快照。两道校验：入参先过
// IsManagedBackupFileName（JoinPath 不做净化，这里是唯一的路径边界），读完
// 再比对 marker 里的 snapshot_file_name —— 把 B 的 marker 复制成 A 的名字，
// 不会让 A 因此变得可信。
bool LoadRealtimeMarker(const std::string& repository_directory,
                        const std::string& snapshot_file_name,
                        RealtimeMarker* marker, std::string* error_message) {
  if (!IsManagedBackupFileName(snapshot_file_name)) {
    SetError(error_message, "Not a managed backup name: " + snapshot_file_name);
    return false;
  }
  const std::string path = JoinPath(repository_directory,
                                    RealtimeMarkerFileName(snapshot_file_name));
  std::string text;
  if (!ReadWholeFile(path, &text, error_message)) return false;
  RealtimeMarker parsed;
  if (!ParseRealtimeMarker(text, &parsed, error_message)) return false;
  if (parsed.snapshot_file_name != snapshot_file_name) {
    SetError(error_message, "The realtime marker belongs to '" +
                                parsed.snapshot_file_name + "'");
    return false;
  }
  *marker = parsed;
  return true;
}

// 列出仓库里全部 realtime 快照，并尽量给出"这一条能不能信"的结论。
//
// 流程：BackupCatalog::List 取受管理的 .bak 全表 -> readdir 并排序，逐个识别
// marker 名 -> LoadRealtimeMarker（严格解析）-> LoadVerifiedSnapshotIdentity
// 从实际归档字节算出 id 与 marker 比对 -> 与 catalog 记录 JOIN 补大小/mtime。
//
// 失败语义是这里最要紧的一点：返回 false 只表示目录级失败（catalog 读不了、
// opendir/readdir 出错）。单份 marker 坏了只是那条记录 verified=false 并带上
// diagnostic，仍然返回给调用方，让上层自己决定"跳过这一份"还是"这轮什么都
// 别删"。
bool ListRealtimeSnapshots(const std::string& repository_directory,
                           std::vector<RealtimeSnapshotRecord>* records,
                           std::string* error_message) {
  if (records == nullptr) {
    SetError(error_message, "Realtime record output must not be null");
    return false;
  }
  records->clear();

  BackupCatalog catalog;
  std::vector<BackupRecord> listed;
  if (!catalog.List(repository_directory, &listed, error_message)) return false;
  std::map<std::string, const BackupRecord*> by_name;
  for (const BackupRecord& record : listed) {
    by_name[record.file_name] = &record;
  }

  DIR* raw = ::opendir(repository_directory.c_str());
  if (raw == nullptr) {
    SetError(error_message, "Cannot open the repository directory " +
                                repository_directory + ": " + ErrnoText(errno));
    return false;
  }
  std::vector<std::string> marker_names;
  while (true) {
    errno = 0;
    struct dirent* item = ::readdir(raw);
    if (item == nullptr) {
      if (errno != 0) {
        const std::string text = ErrnoText(errno);
        ::closedir(raw);
        SetError(error_message,
                 "Cannot read the repository directory: " + text);
        return false;
      }
      break;
    }
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    marker_names.push_back(name);
  }
  ::closedir(raw);
  std::sort(marker_names.begin(), marker_names.end());

  for (const std::string& marker_name : marker_names) {
    std::string base;
    if (!IsRealtimeMarkerFileName(marker_name, &base)) continue;
    RealtimeSnapshotRecord record;
    record.file_name = base;
    record.marker_file_name = marker_name;

    RealtimeMarker marker;
    std::string marker_error;
    if (!LoadRealtimeMarker(repository_directory, base, &marker,
                            &marker_error)) {
      record.diagnostic = marker_error;
      records->push_back(record);
      continue;
    }
    record.snapshot_id = marker.snapshot_id;
    record.job_identity = marker.job_identity;
    record.outcome_kind = marker.outcome_kind;
    record.created_time_sec = marker.created_time_sec;
    record.event_count = marker.event_count;
    record.overflow_recovery = marker.overflow_recovery;
    record.resync_trigger = marker.resync_trigger;
    record.strategy = marker.strategy;
    record.pack_method = marker.pack_method;
    record.compression_method = marker.compression_method;
    record.added = marker.added;
    record.removed = marker.removed;
    record.modified = marker.modified;
    record.metadata_changed = marker.metadata_changed;

    // marker 必须与磁盘上的实际归档一致：这是"只信实际字节"的那道门。
    SnapshotIdentity identity;
    std::string identity_error;
    if (!LoadVerifiedSnapshotIdentity(repository_directory, base, &identity,
                                      nullptr, &identity_error)) {
      record.diagnostic = "the snapshot cannot be verified: " + identity_error;
      records->push_back(record);
      continue;
    }
    if (identity.snapshot_id != marker.snapshot_id) {
      record.diagnostic =
          "the realtime marker does not match the actual archive bytes";
      records->push_back(record);
      continue;
    }
    const auto found = by_name.find(base);
    if (found != by_name.end()) {
      record.archive_size = found->second->archive_size;
      record.archive_mtime_sec = found->second->modified_time_sec;
    }
    record.verified = true;
    records->push_back(record);
  }

  std::sort(records->begin(), records->end(),
            [](const RealtimeSnapshotRecord& left,
               const RealtimeSnapshotRecord& right) {
              if (left.created_time_sec != right.created_time_sec) {
                return left.created_time_sec < right.created_time_sec;
              }
              return left.file_name < right.file_name;
            });
  return true;
}

// 对"当前 job 自己的"实时快照做一次依赖感知淘汰。返回 true 不代表删了东西，
// 只代表这一轮得出了确定结论，结论写在 result 里；返回 false 仅用于 result
// 为空这种编程错误——retention 的任何不顺利都不该把整次备份判成失败。
//
// 每一步的取舍：
//   1) 先列快照。列不出来 -> uncertain + 什么都不删：不删只是留垃圾，误删
//      是不可恢复的。
//   2) 只要有一份 marker 无法验证，整轮 no-op：归属都判断不了就做破坏性操作
//      等于把"不确定"变成"可能删错"。
//   3) 只把 job_identity 与当前配置相同的快照当候选；别的 job（换过源、换过
//      算法）建的快照是别人的东西，不自动淘汰，用户仍可手工删。
//   4) 候选数不超过保留数就直接结束；否则交给 PlanDependencyAwareRetention
//      算 keep_visible / keep_ancestors / remove，删除交给 DeleteSnapshots
//      （descendants-first，先后代后祖先，链不会被删断）。
//   5) remove 为空但不确定标志为 false 是正常结果，不是警告：最新恢复点依赖
//      全部祖先，这一轮本来就没有可删的。
bool RunRealtimeRetention(const std::string& repository_directory,
                          const std::string& job_identity,
                          std::uint32_t retain_count,
                          RealtimeRetentionResult* result,
                          std::string* error_message) {
  if (result == nullptr) {
    SetError(error_message, "Realtime retention result must not be null");
    return false;
  }
  *result = RealtimeRetentionResult{};
  if (error_message != nullptr) error_message->clear();

  std::vector<RealtimeSnapshotRecord> records;
  std::string list_error;
  if (!ListRealtimeSnapshots(repository_directory, &records, &list_error)) {
    result->uncertain = true;
    result->reason = "cannot list realtime snapshots: " + list_error;
    return true;  // 不删 + warning，不把整次备份判成失败
  }

  // 任何一份 marker 坏到无法判断归属 -> 本轮 destructive retention 直接 no-op。
  for (const RealtimeSnapshotRecord& record : records) {
    if (!record.verified) {
      result->uncertain = true;
      result->reason = "a realtime marker cannot be verified (" +
                       record.marker_file_name + "): " + record.diagnostic;
      return true;
    }
  }

  std::vector<std::string> candidates_oldest_first;
  for (const RealtimeSnapshotRecord& record : records) {
    if (record.job_identity != job_identity) continue;  // 旧 job：不自动淘汰
    candidates_oldest_first.push_back(record.file_name);
  }
  if (candidates_oldest_first.size() <= retain_count) {
    result->kept_visible = candidates_oldest_first.size();
    return true;
  }

  RetentionPlan plan;
  std::string plan_error;
  if (!PlanDependencyAwareRetention(repository_directory,
                                    candidates_oldest_first, retain_count,
                                    &plan, &plan_error)) {
    result->uncertain = true;
    result->reason = "cannot plan realtime retention: " + plan_error;
    return true;
  }
  result->kept_visible = plan.keep_visible.size();
  result->kept_ancestors = plan.keep_ancestors.size();
  // 只有**真的不确定**才算 uncertain。
  //
  // 健康的增量链（例如 F0 -> D1 -> D2 -> D3、retain = 1）会出现
  // remove 为空但并非不确定的状态：最新恢复点依赖全部祖先，所以这一轮没有
  // 可删的东西。那是正常的 dependency retention，不是"依赖链读不出来"，
  // 不能报成 warning 让用户以为哪里坏了。
  if (plan.dependency_uncertain) {
    result->uncertain = true;
    result->reason = plan.uncertainty_reason;
    return true;
  }
  if (plan.remove.empty()) {
    // success：deleted = 0，kept_visible / kept_ancestors 保持真实计数，无
    // warning。
    return true;
  }

  BackupCatalog catalog;
  std::vector<std::string> removed;
  std::vector<std::string> diagnostics;
  std::string delete_error;
  if (!catalog.DeleteSnapshots(repository_directory, plan.remove, &removed,
                               &diagnostics, &delete_error)) {
    // 有不在删除集合里的 live descendant，或单个 unlink 失败：链优先。
    result->uncertain = true;
    result->reason = delete_error;
    result->diagnostics = diagnostics;
    result->deleted = removed.size();
    return true;
  }
  result->deleted = removed.size();
  result->diagnostics = diagnostics;
  return true;
}

// 执行一次已经稳定的 realtime 触发。events 只描述事件层面发生了什么（计数、
// 溢出、是否需要 resync），它不会被当成"改了几个文件"——真正的变更统计来自
// 增量引擎返回的 ChangeSummary。
//
// 顺序与失败语义：
//   1) 先 ValidateRealtimeConfig；配置不合法直接 kFailed，不碰仓库。
//   2) 算三个身份摘要：source（哪棵树）、filter（哪套规则）、job（配置组合）。
//      job_identity 是归属键，retention 只淘汰与它相同的快照，所以它必须与
//      watcher/store 用同一套算法算出来，否则会认不出自己的快照。
//   3) 编译 Filter（共享 BuildRealtimeFilter，规则编译只有这一处实现），
//      再 EnsureRepository —— 自动触发可以顺手把仓库布局补齐。
//   4) Full 走 BackupEngine；Incremental 走 RunIncrementalBackup，baseline /
//      delta / no-change 三选一由它决定。no-changes 直接返回：什么都没写，
//      也就没有 marker、没有 retention。
//   5) 写完归档先验证身份再写 marker；marker 失败降级为成功 + warning；
//      最后做 retention。每一步的具体理由见对应代码块上方的注释。
bool RunRealtimeBackupOnce(const RealtimeConfig& config,
                           const std::string& repository_path,
                           const std::string& repository_identity,
                           const RealtimeEventSummary& events,
                           std::int64_t now_sec, RealtimeOutcome* outcome,
                           std::string* error_message) {
  if (outcome == nullptr) {
    SetError(error_message, "Realtime outcome output must not be null");
    return false;
  }
  *outcome = RealtimeOutcome{};
  if (error_message != nullptr) error_message->clear();
  if (!ValidateRealtimeConfig(config, error_message)) {
    outcome->kind = RealtimeOutcome::Kind::kFailed;
    return false;
  }

  const std::string source_identity =
      SourceIdentityDigest(config.source_path, repository_identity);
  const std::string filter_identity =
      FilterIdentityDigest(config.include_rules, config.exclude_rules);
  const std::string job_identity = RealtimeJobIdentityDigest(
      config, repository_identity, config.source_path);

  // 规则编译只有一处实现（共享 Filter::AddRule，见 BuildRealtimeFilter）：
  // ValidateRealtimeConfig 已经在保存 / 启用前编译过一遍，这里是运行时的防御
  // 路径，用的是同一个函数，因此错误逐字一致。
  Filter filter;
  if (!BuildRealtimeFilter(config, &filter, error_message)) {
    outcome->kind = RealtimeOutcome::Kind::kFailed;
    return false;
  }

  BackupCatalog catalog;
  if (!catalog.EnsureRepository(repository_path, error_message)) {
    outcome->kind = RealtimeOutcome::Kind::kFailed;
    return false;
  }

  BackupOptions options;
  options.pack_method = config.pack_method;
  options.compression_method = config.compression_method;
  options.encryption_method = EncryptionMethod::kNone;  // 自动触发不加密

  std::string snapshot_file_name;
  if (config.strategy == BackupStrategy::kFull) {
    // Full：完全走既有 BackupEngine + 既有扫描/打包/压缩流水线。
    std::string archive_path;
    if (!catalog.BuildArchivePath(repository_path, config.source_path, now_sec,
                                  &archive_path, error_message)) {
      outcome->kind = RealtimeOutcome::Kind::kFailed;
      return false;
    }
    snapshot_file_name = archive_path.substr(archive_path.rfind('/') + 1);
    BackupEngine engine;
    if (!engine.Backup(config.source_path, archive_path, filter, options,
                       error_message)) {
      outcome->kind = RealtimeOutcome::Kind::kFailed;
      return false;
    }
    outcome->kind = RealtimeOutcome::Kind::kFullSnapshot;
    // 面向用户的那句话只有一处：CLI 与 GUI 都显示它。
    outcome->summary_text = "创建完整实时快照";
  } else {
    // Incremental：完全走 RunIncrementalBackup（baseline / delta / no-change
    // 三选一由它决定，这里一个字都不复制）。
    std::string archive_path;
    if (!catalog.BuildArchivePath(repository_path, config.source_path, now_sec,
                                  &archive_path, error_message)) {
      outcome->kind = RealtimeOutcome::Kind::kFailed;
      return false;
    }
    snapshot_file_name = archive_path.substr(archive_path.rfind('/') + 1);
    IncrementalOutcome incremental;
    if (!RunIncrementalBackup(
            config.source_path, repository_path, snapshot_file_name,
            repository_identity, filter, options, config.include_rules,
            config.exclude_rules, std::string(), &incremental, error_message)) {
      outcome->kind = RealtimeOutcome::Kind::kFailed;
      return false;
    }
    outcome->changes = incremental.summary;
    switch (incremental.kind) {
      case IncrementalOutcome::Kind::kFullBaseline:
        outcome->kind = RealtimeOutcome::Kind::kFullBaseline;
        outcome->summary_text = "实时增量策略建立了新的完整基线";
        break;
      case IncrementalOutcome::Kind::kDelta:
        outcome->kind = RealtimeOutcome::Kind::kDelta;
        outcome->summary_text = "实时增量快照已创建";
        break;
      case IncrementalOutcome::Kind::kNoChanges:
        outcome->kind = RealtimeOutcome::Kind::kNoChanges;
        outcome->summary_text =
            "检测到文件系统事件，但有效备份集合没有变化；未创建快照";
        return true;  // 什么都没写：marker 与 retention 都不需要
    }
    if (incremental.snapshot_file_name == snapshot_file_name) {
      // 引擎可能已经因为"没有可信基线"而写了完整基线：名字一致。
    }
  }

  // 快照真的写出来了才填名字：调用方（CLI / GUI）要用它去 list / restore。
  outcome->snapshot_file_name = snapshot_file_name;

  // marker 只记录"从归档字节验证过的"身份，不采信 header 或 envelope 的声明值：
  // 声明值可以被写坏，字节不会。验证失败是硬错误（不能把不可信的东西写成
  // realtime 快照），但归档保留 —— 它仍可 list / restore / 手工删除。
  // ---- marker：必须绑定**实际 archive bytes** ----
  SnapshotIdentity identity;
  std::string identity_error;
  if (!LoadVerifiedSnapshotIdentity(repository_path, snapshot_file_name,
                                    &identity, nullptr, &identity_error)) {
    // 归档已经写好了，但身份验证不过：这是硬错误（不能把不可信的东西写成
    // realtime 快照），但**不删**已经产生的归档——它仍可 list / restore /
    // manual delete。
    outcome->diagnostic =
        "the new snapshot could not be verified: " + identity_error;
    outcome->kind = RealtimeOutcome::Kind::kFailed;
    SetError(error_message, outcome->diagnostic);
    return false;
  }

  // 字段来源分三类，别混：snapshot_id 来自刚才的字节验证；created_time_sec /
  // event_count / 溢出与 resync 标志来自本次触发，是"当时发生了什么"的取证；
  // added/removed/modified/metadata_changed 只对增量有意义，Full 时保持 0。
  RealtimeMarker marker;
  marker.snapshot_file_name = snapshot_file_name;
  marker.snapshot_id = identity.snapshot_id;
  marker.created_time_sec = now_sec;
  marker.job_identity = job_identity;
  marker.source_identity = source_identity;
  marker.filter_identity = filter_identity;
  marker.strategy = config.strategy;
  marker.pack_method = config.pack_method;
  marker.compression_method = config.compression_method;
  marker.event_count = events.event_count;
  marker.overflow_recovery = events.overflow;
  marker.resync_trigger = events.resync;
  switch (outcome->kind) {
    case RealtimeOutcome::Kind::kFullSnapshot:
      marker.outcome_kind = "full";
      break;
    case RealtimeOutcome::Kind::kFullBaseline:
      marker.outcome_kind = "full-baseline";
      break;
    case RealtimeOutcome::Kind::kDelta:
      marker.outcome_kind = "delta";
      break;
    default:
      marker.outcome_kind = "no-changes";
      break;
  }
  marker.added = outcome->changes.added;
  marker.removed = outcome->changes.removed;
  marker.modified = outcome->changes.modified;
  marker.metadata_changed = outcome->changes.metadata_changed;

  std::string marker_error;
  if (!WriteRealtimeMarker(repository_path, marker, &marker_error)) {
    // archive 保留；本轮退化成"成功但 ownership 有警告"，而且**不执行**破坏性
    // realtime retention（没有 marker 就没有可信的归属，更不能猜着删）。
    outcome->marker_written = false;
    outcome->marker_warning = true;
    outcome->diagnostic =
        "the snapshot was created, but its realtime marker could not be "
        "written: " +
        marker_error;
    return true;
  }
  outcome->marker_written = true;

  // retention 是尽力而为的收尾：它失败不影响"备份已经成功"这个结论，只把原因
  // 追加进 diagnostic 让用户知道旧版本没清干净。注意它只在 marker 写成功之后
  // 才执行 —— 没有 marker 就没有可信归属，也就不能删任何东西。
  // ---- retention：复用共享的依赖感知计划 + descendants-first 删除 ----
  RealtimeRetentionResult retention;
  std::string retention_error;
  if (RunRealtimeRetention(repository_path, job_identity, config.retain_count,
                           &retention, &retention_error)) {
    outcome->retention_deleted = retention.deleted;
    outcome->retention_kept_ancestors = retention.kept_ancestors;
    outcome->retention_uncertain = retention.uncertain;
    if (retention.uncertain && !retention.reason.empty() &&
        retention.reason != "nothing to remove") {
      if (!outcome->diagnostic.empty()) outcome->diagnostic += " ";
      outcome->diagnostic += "realtime retention warning: " + retention.reason;
    }
  } else if (!retention_error.empty()) {
    if (!outcome->diagnostic.empty()) outcome->diagnostic += " ";
    outcome->diagnostic += "realtime retention failed: " + retention_error;
  }
  return true;
}

}  // namespace backupproject

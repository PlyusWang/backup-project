// schedule_store.cpp
//
// 模块职责：定时备份的**唯一**持久化入口。把 ScheduleConfig、运行状态
// （next_run / last_success / baseline）、受管快照名录与 history 落成一份
// schedule.json；把"上一份成功快照对应的源清单"落成同目录的
// schedule-manifest.dat；并提供两者的原子读写与删除。
//
// 不负责什么：
//   * 不做调度决策（下一轮该不该跑、错过的窗口怎么补）——那是
//     ScheduledBackupService 的职责，本文件只如实存"下一次几点"；
//   * 不解析归档、不读 .bak 的内容，只用单组件文件名引用它们；
//   * 不写第二套产品组合校验，一律委托 backup_option_keys.h 的共享表；
//   * 不猜路径：构造函数只接受调用方算好的路径，不读 HOME/XDG/QSettings。
//
// 数据流（写侧）：调用方改 ScheduleDocument → Save() 做结构校验与各项上界
// 检查 → SerializeScheduleDocument() 手写 JSON →
// WriteFileAtomicallyReplacing() 以 0600 权限做 temp + fsync + rename。
// 读侧反着走：ReadWholeFile() → ParseJson() → 逐字段严格解析 →
// ScheduleDocument。字段集合是双向完全相等的，多一个少一个都报错。
//
// 关键不变量（违反任一条即拒绝写入，绝不落一份读不回来的文件）：
//   * 字符串字段长度 <= kMaxScheduleStringBytes 且不含 NUL 字节；
//   * managed_snapshots.size() <= kMaxRetainCount、
//     history.size() <= kMaxHistoryEntries（读侧上界不得比写侧更严）；
//   * 每个 file_name（含 baseline）都是单组件、以 .bak 结尾的归档名——
//     路径越界的判断只在 BackupCatalog 里，本文件绝不擅自放宽；
//   * trigger 恒为 kScheduled：realtime 有它自己的 store（realtime.json）。
//
// 失败语义：全部走 "bool / 状态码 + 可选的 std::string* error_message"，
// 不抛异常。Load() 用三态区分"文件不在"（kMissing，调用方走默认配置）与
// "文件在但读不懂"（kError，绝不静默回退到默认配置）。Save() 失败时不写盘，
// 磁盘上原有的文件保持原样。
//
// 安全边界：这两个文件里没有密码（计划任务不接受加密），但它们记录了用户的
// 目录结构，所以固定落 0600 文件权限；所有路径只做拼接与 lstat，建目录时
// 不跟随软链接，也绝不把 state 里的名字当成可信路径直接使用。
//
// 线程与生命周期：本类是纯数据 + 路径，构造后路径不再变化（Load/Save 全是
// const），因此多个实例可以并存。但**同一个 schedule.json 上不允许并发写**：
// 调用方必须先通过 lock_file_path() 上的 flock 选出唯一 runner。
// 本文件自己不做任何加锁。

#include "schedule_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "backup_catalog.h"
#include "backup_option_keys.h"
#include "file_io.h"
#include "simple_json.h"

namespace backupproject {
namespace {

// 本文件统一的失败协议：函数返回 false 或错误状态，原因写进可选的
// error_message。所有 *error_message 参数都允许为 nullptr——只要"成没成"
// 的调用方不必先造一个字符串。错误文本一律英文：它会被 CLI、GUI 与日志
// 原样转发，本地化在更外层做，core 里不掺语言判断。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// errno 文本的组装点。调用方必须在**失败当刻**把 errno 作为参数传进来：
// 这里不会再读全局 errno，因为中间的字符串拼接可能已经覆盖了它。
std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

// ASCII 空白。刻意不用 std::isspace：它受 locale 影响，而"数字选项"的解析
// 规则必须是全局一致的。
bool IsAsciiSpace(char character) {
  return character == ' ' || character == '\t' || character == '\n' ||
         character == '\r' || character == '\f' || character == '\v';
}

// std::string 可以合法地装 NUL 字节，而下游任何 c_str() 或系统调用都会在
// 那里截断。长度检查因此必须和这一条同时做，只查 size() 是不够的。
bool ContainsNul(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

// 本文件自己的父目录解析。"a" -> "."、"/a" -> "/"、"/" -> "/"。
//
// 刻意不共用 file_io.h 里那个同名函数：它在"没有 '/' 时返回空串"，语义与
// 这里不同（空串会让 mkdir -p 走到当前目录之外的判断上）。真正共享的是
// 原子的文件替换与目录创建入口，不是这个六行的字符串切分。
std::string ParentDirectoryOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) return std::string(".");
  if (slash == 0) return std::string("/");
  return path.substr(0, slash);
}

// 递归创建目录，等价于 mkdir -p。
//
// 什么时候会真的用到它：用户把 --schedule-file 指到一个还不存在的深层路径
// （首次运行、或者应用配置目录整个被清空过）。此时"保存计划"不该因为父目录
// 不在就失败——那是保存路径的问题，不是用户的输入错误。
//
// 目录权限 0700：里面放的是 schedule.json + manifest，前者已经是 0600，
// 目录没有理由是 0755 让同机器上任何人都能列出来。
// 探测用 lstat 而不是 stat：一个指向别处的软链接必须在这里被判成"不是
// 目录"而拒绝，否则 mkdir -p 会把文件写进链接指向的位置。递归的终止条件
// 就是函数开头那三行（空 / "." / "/"），ParentDirectoryOf 保证每层都更短。
bool MakeDirectories(const std::string& path, std::string* error_message) {
  if (path.empty() || path == "." || path == "/") return true;

  struct stat status;
  if (::lstat(path.c_str(), &status) == 0) {
    if (S_ISDIR(status.st_mode)) return true;
    SetError(error_message,
             "Cannot create the schedule directory: not a directory: " + path);
    return false;
  }
  if (errno != ENOENT) {
    SetError(error_message, Describe(errno, "Failed to inspect", path));
    return false;
  }
  if (!MakeDirectories(ParentDirectoryOf(path), error_message)) return false;
  if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    SetError(error_message,
             Describe(errno, "Failed to create the schedule directory", path));
    return false;
  }
  return true;
}

// 读一个"要么不存在、要么完整读进来"的配置/清单文件。
//
// missing 是独立于返回值的第三态：文件不存在不算错误，调用方据此走"首次
// 运行"的分支；只有"存在但读不了"才返回 false。
//
// 边界处理：
//   * 先用 lstat 挡掉非普通文件（目录、FIFO、软链接）——既避免 open 卡在
//     FIFO 上，也避免顺着链接读到仓库之外；
//   * 上界查两次：lstat 的 st_size 只是读之前的快照，文件完全可能在读取
//     过程中被追加，所以每读一块都再查一次，内存绝不无界增长；
//   * read 返回 0 才是 EOF，EINTR 只重试不算失败；
//   * 每条失败路径都先 close(fd) 再返回，不泄漏描述符。
bool ReadWholeFile(const std::string& path, std::size_t maximum_bytes,
                   std::string* data, bool* missing,
                   std::string* error_message) {
  data->clear();
  *missing = false;

  struct stat status;
  if (::lstat(path.c_str(), &status) != 0) {
    if (errno == ENOENT) {
      *missing = true;
      return true;
    }
    SetError(error_message, Describe(errno, "Failed to inspect", path));
    return false;
  }
  if (!S_ISREG(status.st_mode)) {
    SetError(error_message, "Not a regular file: " + path);
    return false;
  }
  if (static_cast<std::uint64_t>(status.st_size) > maximum_bytes) {
    SetError(error_message, "File is too large to read: " + path + " (" +
                                std::to_string(status.st_size) + " bytes)");
    return false;
  }

  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message, Describe(errno, "Failed to open", path));
    return false;
  }
  std::string contents;
  contents.reserve(static_cast<std::size_t>(status.st_size));
  char buffer[65536];
  while (true) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) continue;
      const int saved_errno = errno;
      ::close(fd);
      SetError(error_message, Describe(saved_errno, "Failed to read", path));
      return false;
    }
    if (count == 0) break;
    contents.append(buffer, static_cast<std::size_t>(count));
    if (contents.size() > maximum_bytes) {
      ::close(fd);
      SetError(error_message, "File is too large to read: " + path);
      return false;
    }
  }
  ::close(fd);
  *data = std::move(contents);
  return true;
}

// 与 BackupCatalog 的规则保持一致：不含 '/' 与 '\\'、不是 "." / ".."、
// 不含 NUL、以 .bak 结尾。Catalog 仍然会在 Resolve / Delete 时再校验一次，
// 这里提前拦是为了"一个坏掉的 state 不会变成一串奇怪的系统调用"。
// size <= 4 让 ".bak" 本身成为非法名字：剥掉后缀之后必须还剩内容，
// 否则 "..bak" 这类名字能靠后缀判断蒙混过去。
bool IsSingleComponentArchiveName(const std::string& file_name) {
  if (file_name.empty()) return false;
  if (file_name == "." || file_name == "..") return false;
  if (file_name.find('/') != std::string::npos) return false;
  if (file_name.find('\\') != std::string::npos) return false;
  if (ContainsNul(file_name)) return false;
  if (file_name.size() <= 4) return false;
  return file_name.compare(file_name.size() - 4, 4, ".bak") == 0;
}

// 写盘与读盘共用同一条有界性判断：长度上界防"一份配置撑爆解析与日志"，
// NUL 检查防"字符串在 c_str() 处被截断成另一个路径"。
bool IsBoundedString(const std::string& value, std::size_t maximum) {
  return value.size() <= maximum && !ContainsNul(value);
}

}  // namespace

// 规则顺序被原样保留：Filter 按加入顺序求值，include 与 exclude 谁覆盖谁
// 是既有产品语义，这里不归一化、不去重、不排序。
// 每条规则都真的走 Filter::AddRule，因此"配置里存的规则"与"CLI 敲进去的
// 规则"是同一套语法、同一套报错，不存在"存得下但解析不了"的中间态。
bool BuildScheduleFilter(const ScheduleConfig& config, Filter* filter,
                         std::string* error_message) {
  if (filter == nullptr) {
    SetError(error_message, "Filter output must not be null");
    return false;
  }
  if (config.include_rules.size() + config.exclude_rules.size() >
      kMaxScheduleRules) {
    SetError(error_message, "Too many schedule filter rules: " +
                                std::to_string(config.include_rules.size() +
                                               config.exclude_rules.size()));
    return false;
  }
  for (const std::string& rule : config.include_rules) {
    if (!filter->AddRule(FilterAction::kInclude, rule, error_message)) {
      return false;
    }
  }
  for (const std::string& rule : config.exclude_rules) {
    if (!filter->AddRule(FilterAction::kExclude, rule, error_message)) {
      return false;
    }
  }
  return true;
}

// 纯结构校验：只回答"这份配置本身合不合法"，不碰文件系统、不看仓库，
// 因此 CLI 的选项校验、GUI 的"保存计划"与调度前的检查可以共用它。
// 顺序是先判作用域再判组合：先回答"这份文件该不该由本 store 执行"，
// 再回答"这个组合能不能跑"，两条错误信息不会互相掩盖。
bool ValidateScheduleConfig(const ScheduleConfig& config,
                            std::string* error_message) {
  // 这份 store 的作用域：它只装 scheduled 触发。Realtime 有它自己的 store
  // （realtime.json）。
  //
  // 为什么这条判断必须存在：共享矩阵里 Realtime × {Full,
  // Incremental} 两格都是"支持"，只问矩阵的话，一份手改成
  // "trigger": "realtime" 的 schedule.json 会被 schedule 路径当成合法配置
  // 执行——那不是"换了个触发方式"，而是"这份文件根本不该被执行"。
  // 所以作用域由 store 自己回答，矩阵继续回答"组合本身能不能跑"。
  if (config.trigger != BackupTrigger::kScheduled) {
    SetError(error_message,
             std::string("This schedule store only holds the scheduled "
                         "trigger, got '") +
                 BackupTriggerText(config.trigger) +
                 "'. Realtime backups use their own store (realtime.json).");
    return false;
  }
  // 组合校验只有一份实现：产品矩阵 + "增量只支持 MyPack" + "计划不接受加密"。
  // 这里不再自己写 if 链——"GUI 能存、CLI 读不了"就是这么来的。
  BackupOptionCombination combination;
  combination.trigger = config.trigger;
  combination.strategy = config.strategy;
  combination.pack_method = config.pack_method;
  combination.compression_method = config.compression_method;
  combination.encryption_method = config.encryption_method;
  if (!IsSupportedBackupOptionCombination(combination)) {
    SetError(error_message,
             UnsupportedBackupOptionCombinationReason(combination));
    return false;
  }
  if (config.interval_minutes < kMinIntervalMinutes ||
      config.interval_minutes > kMaxIntervalMinutes) {
    SetError(error_message, "Schedule interval must be between " +
                                std::to_string(kMinIntervalMinutes) + " and " +
                                std::to_string(kMaxIntervalMinutes) +
                                " minutes, got " +
                                std::to_string(config.interval_minutes));
    return false;
  }
  if (config.retain_count < kMinRetainCount ||
      config.retain_count > kMaxRetainCount) {
    SetError(error_message, "Schedule retain count must be between " +
                                std::to_string(kMinRetainCount) + " and " +
                                std::to_string(kMaxRetainCount) + ", got " +
                                std::to_string(config.retain_count));
    return false;
  }
  // 加密边界（无人值守的计划没有安全的密钥来源）已经在上面那张共享表里
  // 回答过了：这一句不是第二套判断，只是把"哪个字段违规"点出来。
  if (!IsBoundedString(config.source_path, kMaxScheduleStringBytes)) {
    SetError(error_message,
             "Schedule source path is not usable (too long or contains a NUL "
             "byte)");
    return false;
  }
  for (const std::string& rule : config.include_rules) {
    if (!IsBoundedString(rule, kMaxScheduleStringBytes)) {
      SetError(error_message,
               "A schedule include rule is not usable (too long "
               "or contains a NUL byte)");
      return false;
    }
  }
  for (const std::string& rule : config.exclude_rules) {
    if (!IsBoundedString(rule, kMaxScheduleStringBytes)) {
      SetError(error_message,
               "A schedule exclude rule is not usable (too long "
               "or contains a NUL byte)");
      return false;
    }
  }
  // 规则语法也要先过一遍：一份存得下、但 Filter 解析不了的配置，会在启用时
  // 甚至下一次定时触发时才炸——那时用户早已离开设置页，错误来得太晚。
  Filter filter;
  return BuildScheduleFilter(config, &filter, error_message);
}

namespace {

// 一个"必须已经存在、必须是真实目录、且不能是软链接"的路径。
//
// 两条路径（源目录、仓库）用的是同一段判断，报错文案只换主语：绝不写两遍，
// 免得某一天只修好了其中一份。
// 软链接被单独拒绝，而不是"跟随它再判断"：仓库与源目录都是长期配置，
// 跟随链接会让"配置里写的路径"与"实际被写入/备份的路径"不是同一个东西。
bool RequireRealDirectory(const std::string& path, const char* what,
                          std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, std::string(what) + " must be set before enabling");
    return false;
  }
  struct stat status;
  if (::lstat(path.c_str(), &status) != 0) {
    SetError(error_message, Describe(errno, what, path));
    return false;
  }
  if (S_ISLNK(status.st_mode)) {
    SetError(error_message,
             std::string(what) + " must not be a symbolic link: " + path);
    return false;
  }
  if (!S_ISDIR(status.st_mode)) {
    SetError(error_message, std::string(what) + " is not a directory: " + path);
    return false;
  }
  return true;
}

}  // namespace

bool ParseBoundedScheduleNumber(const std::string& text, std::uint32_t minimum,
                                std::uint32_t maximum,
                                const std::string& option, std::uint32_t* value,
                                std::string* error_message) {
  if (value == nullptr) {
    SetError(error_message, "Schedule number output must not be null");
    return false;
  }
  // 前后空白由**这里**统一处理：CLI 把 argv 原样送进来，GUI
  // 把文本框原样送进来， 两边都不做预处理。少了这一条，" 5 " 会在 GUI
  // 被接受、被 CLI 拒绝—— 同一个输入两个前端给不同结论，正是要收掉的那类漂移。
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && IsAsciiSpace(text[begin])) ++begin;
  while (end > begin && IsAsciiSpace(text[end - 1])) --end;
  const std::string trimmed = text.substr(begin, end - begin);
  if (trimmed.empty()) {
    SetError(error_message, option + " needs a number");
    return false;
  }
  // 手写十进制解析而不用 strtoul/from_chars：两者都接受前导正负号，或者按
  // "尽可能长的前缀"截断——"12abc" 会被解析成 12，正是要收掉的那类漂移。
  std::uint64_t result = 0;
  for (const char character : trimmed) {
    if (character < '0' || character > '9') {
      SetError(
          error_message,
          option + " expects a plain non-negative integer, got '" + text + "'");
      return false;
    }
    result = result * 10u + static_cast<std::uint64_t>(character - '0');
    // 边解析边夹：不能等累加到溢出之后再判范围。
    if (result > maximum) {
      SetError(error_message, option + " is out of range: " + text +
                                  " (expected " + std::to_string(minimum) +
                                  ".." + std::to_string(maximum) + ")");
      return false;
    }
  }
  if (result < minimum) {
    SetError(error_message, option + " is out of range: " + text +
                                " (expected " + std::to_string(minimum) + ".." +
                                std::to_string(maximum) + ")");
    return false;
  }
  *value = static_cast<std::uint32_t>(result);
  return true;
}

bool ValidateScheduleForEnable(const ScheduleConfig& config,
                               const std::string& repository_path,
                               std::string* error_message) {
  if (!ValidateScheduleConfig(config, error_message)) return false;

  // 仓库先查：仓库没配好时，"源目录也不对"不是用户现在最需要看到的信息。
  if (repository_path.empty()) {
    SetError(error_message,
             "No backup repository is configured. Set one before enabling the "
             "scheduled backup");
    return false;
  }
  if (!RequireRealDirectory(repository_path, "The backup repository",
                            error_message)) {
    return false;
  }
  return RequireRealDirectory(config.source_path, "Schedule source directory",
                              error_message);
}

// ---- 运行结果的字符串键 ----
//
// 这四个 key 是**磁盘格式的一部分**（写进 schedule.json 的 history[].result），
// 改名等于让历史文件读不回来：只能新增，不能重命名。
// Key 与 Text 分工：Key 进文件、Text 进界面，两者不许互换。
const char* ScheduleRunResultKey(ScheduleRunResult result) {
  switch (result) {
    case ScheduleRunResult::kSuccessCreated:
      return "success_created";
    case ScheduleRunResult::kSkippedNoChanges:
      return "skipped_no_changes";
    case ScheduleRunResult::kFailed:
      return "failed";
    case ScheduleRunResult::kSuccessWithRetentionWarning:
      return "success_with_retention_warning";
  }
  return "failed";
}

const char* ScheduleRunResultText(ScheduleRunResult result) {
  switch (result) {
    case ScheduleRunResult::kSuccessCreated:
      return "Created a new full snapshot";
    case ScheduleRunResult::kSkippedNoChanges:
      return "Skipped: no changes since the last snapshot";
    case ScheduleRunResult::kFailed:
      return "Failed";
    case ScheduleRunResult::kSuccessWithRetentionWarning:
      return "Snapshot created, but retention could not delete an old one";
  }
  return "Failed";
}

// 未知 key 一律返回 false，由调用方报错；不做"不认识就当 failed"的兜底——
// 静默归类会把一份格式错误的文件变成一条看起来正常的历史记录。
bool ParseScheduleRunResultKey(const std::string& key,
                               ScheduleRunResult* result) {
  if (result == nullptr) return false;
  if (key == "success_created") {
    *result = ScheduleRunResult::kSuccessCreated;
    return true;
  }
  if (key == "skipped_no_changes") {
    *result = ScheduleRunResult::kSkippedNoChanges;
    return true;
  }
  if (key == "failed") {
    *result = ScheduleRunResult::kFailed;
    return true;
  }

  if (key == "success_with_retention_warning") {
    *result = ScheduleRunResult::kSuccessWithRetentionWarning;
    return true;
  }
  return false;
}

// ---- 序列化 ----
//
// 手写 JSON 而不是引入第三方库：字段集合很小，而这里的顺序就是磁盘布局，
// 需要完全可控——同一个 ScheduleDocument 必须永远序列化成同一串字节，
// 否则"配置没改"也会被外层的文本比对判成改动。
// 缩进固定 2 空格、每字段独占一行：这文件既要给人看，也要给 diff 看。

namespace {

// 下面这组 Append*Field 的 last 参数决定行尾是逗号还是换行：JSON 不允许
// 尾随逗号，而"谁是最后一个字段"只有调用点知道，所以由调用方显式传。
// 缩进由调用方先写一次（字段可以不在行首时用它），函数只负责字段本身。
void AppendIndent(std::string* out, int depth) {
  out->append(static_cast<std::size_t>(depth) * 2, ' ');
}

void AppendNumberField(std::string* out, const char* name, std::uint64_t value,
                       bool last) {
  *out += '"';
  *out += name;
  *out += "\": ";
  *out += std::to_string(value);
  *out += last ? "\n" : ",\n";
}

void AppendSignedField(std::string* out, const char* name, std::int64_t value,
                       bool last) {
  *out += '"';
  *out += name;
  *out += "\": ";
  *out += std::to_string(value);
  *out += last ? "\n" : ",\n";
}

void AppendStringField(std::string* out, const char* name,
                       const std::string& value, bool last) {
  *out += '"';
  *out += name;
  *out += "\": ";
  WriteJsonString(out, value);
  *out += last ? "\n" : ",\n";
}

void AppendBoolField(std::string* out, const char* name, bool value,
                     bool last) {
  *out += '"';
  *out += name;
  *out += "\": ";
  *out += value ? "true" : "false";
  *out += last ? "\n" : ",\n";
}

// 空数组写成单行 "[]"，非空才展开多行：既让 diff 稳定，也让"没有规则"
// 与"有规则"在文件里一眼可分。
void AppendStringArrayField(std::string* out, const char* name,
                            const std::vector<std::string>& values, int depth,
                            bool last) {
  AppendIndent(out, depth);
  *out += '"';
  *out += name;
  *out += "\": [";
  if (values.empty()) {
    *out += last ? "]\n" : "],\n";
    return;
  }
  *out += '\n';
  for (std::size_t index = 0; index < values.size(); ++index) {
    AppendIndent(out, depth + 1);
    WriteJsonString(out, values[index]);
    *out += (index + 1 == values.size()) ? "\n" : ",\n";
  }
  AppendIndent(out, depth);
  *out += last ? "]\n" : "],\n";
}

// 四个计数字段恒定写出（0 也写）。读侧因此可以要求字段完全相等：
// "少一个字段"与"这个值是 0"必须是两件不同的事。
void AppendChangeFields(std::string* out, const ChangeSummary& changes,
                        int depth) {
  AppendIndent(out, depth);
  AppendNumberField(out, "added", changes.added, false);
  AppendIndent(out, depth);
  AppendNumberField(out, "removed", changes.removed, false);
  AppendIndent(out, depth);
  AppendNumberField(out, "modified", changes.modified, false);
  AppendIndent(out, depth);
  AppendNumberField(out, "metadata_changed", changes.metadata_changed, false);
}

// 名录项的字段顺序同样是磁盘布局：只允许在末尾追加字段并提升 version，
// 不允许重排、改名或省略。
void AppendSnapshotField(std::string* out,
                         const ScheduledSnapshotRecord& record, int depth,
                         bool last) {
  AppendIndent(out, depth);
  out->append("{\n");
  AppendIndent(out, depth + 1);
  AppendStringField(out, "file_name", record.file_name, false);
  AppendIndent(out, depth + 1);
  AppendSignedField(out, "created_time_sec", record.created_time_sec, false);
  AppendChangeFields(out, record.changes, depth + 1);
  AppendIndent(out, depth + 1);
  AppendNumberField(out, "entry_count", record.entry_count, false);
  AppendIndent(out, depth + 1);
  AppendNumberField(out, "archive_size", record.archive_size, false);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "pack", PackMethodKey(record.pack_method), false);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "compression",
                    CompressionMethodKey(record.compression_method), true);
  AppendIndent(out, depth);
  *out += last ? "}\n" : "},\n";
}

// diagnostic 是这里唯一一段自由文本，由 WriteJsonString 负责转义，
// 因此换行与引号不会破坏 JSON 结构；它的上界与"不许含密码"由写侧保证。
void AppendHistoryField(std::string* out, const ScheduleHistoryEntry& entry,
                        int depth, bool last) {
  AppendIndent(out, depth);
  out->append("{\n");
  AppendIndent(out, depth + 1);
  AppendSignedField(out, "scheduled_at_sec", entry.scheduled_at_sec, false);
  AppendIndent(out, depth + 1);
  AppendSignedField(out, "started_at_sec", entry.started_at_sec, false);
  AppendIndent(out, depth + 1);
  AppendSignedField(out, "finished_at_sec", entry.finished_at_sec, false);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "result", ScheduleRunResultKey(entry.result), false);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "archive_file_name", entry.archive_file_name, false);
  AppendChangeFields(out, entry.changes, depth + 1);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "diagnostic", entry.diagnostic, true);
  AppendIndent(out, depth);
  *out += last ? "}\n" : "},\n";
}

}  // namespace

// 唯一的序列化入口。三条硬规则：
//   * version 恒为 1 且是第一个字段——读侧先看版本，再决定其余字段怎么解；
//   * config / state 下的字段集合与读侧的 k*Fields 列表必须逐字对应，
//     增删字段要两边同时改，否则自己写的文件自己读不回来；
//   * managed_snapshots / history 即使为空也写出来（"[]"），
//     不靠"字段缺失即空数组"这种隐式约定。
// 输出以换行结尾："文件末尾有没有换行"不构成版本差异。
std::string SerializeScheduleDocument(const ScheduleDocument& document) {
  std::string out;
  out += "{\n";
  AppendIndent(&out, 1);
  AppendNumberField(&out, "version", 1, false);

  AppendIndent(&out, 1);
  out += "\"config\": {\n";
  AppendIndent(&out, 2);
  AppendBoolField(&out, "enabled", document.config.enabled, false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "trigger", BackupTriggerKey(document.config.trigger),
                    false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "strategy",
                    BackupStrategyKey(document.config.strategy), false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "source_path", document.config.source_path, false);
  AppendIndent(&out, 2);
  AppendNumberField(&out, "interval_minutes", document.config.interval_minutes,
                    false);
  AppendIndent(&out, 2);
  AppendNumberField(&out, "retain_count", document.config.retain_count, false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "pack", PackMethodKey(document.config.pack_method),
                    false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "compression",
                    CompressionMethodKey(document.config.compression_method),
                    false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "encryption",
                    EncryptionMethodKey(document.config.encryption_method),
                    false);
  AppendStringArrayField(&out, "include_rules", document.config.include_rules,
                         2, false);
  AppendStringArrayField(&out, "exclude_rules", document.config.exclude_rules,
                         2, true);
  AppendIndent(&out, 1);
  out += "},\n";

  AppendIndent(&out, 1);
  out += "\"state\": {\n";
  AppendIndent(&out, 2);
  AppendSignedField(&out, "next_run_time_sec", document.state.next_run_time_sec,
                    false);
  AppendIndent(&out, 2);
  AppendSignedField(&out, "last_success_time_sec",
                    document.state.last_success_time_sec, false);
  AppendIndent(&out, 2);
  AppendNumberField(&out, "last_manifest_entry_count",
                    document.state.last_manifest_entry_count, false);

  // baseline 三件套总是写出来（空串也写）。读侧把它们当成可选字段，
  // 所以旧版本写出的、没有这三行的 schedule.json 仍然读得进来。
  AppendIndent(&out, 2);
  AppendStringField(&out, "baseline_snapshot_file_name",
                    document.state.baseline.snapshot_file_name, false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "baseline_repository_identity",
                    document.state.baseline.repository_identity, false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "baseline_source_path",
                    document.state.baseline.source_path, false);

  AppendIndent(&out, 2);
  out += "\"managed_snapshots\": [";
  if (document.state.managed_snapshots.empty()) {
    out += "],\n";
  } else {
    out += '\n';
    for (std::size_t index = 0; index < document.state.managed_snapshots.size();
         ++index) {
      AppendSnapshotField(&out, document.state.managed_snapshots[index], 3,
                          index + 1 == document.state.managed_snapshots.size());
    }
    AppendIndent(&out, 2);
    out += "],\n";
  }

  AppendIndent(&out, 2);
  out += "\"history\": [";
  if (document.state.history.empty()) {
    out += "]\n";
  } else {
    out += '\n';
    for (std::size_t index = 0; index < document.state.history.size();
         ++index) {
      AppendHistoryField(&out, document.state.history[index], 3,
                         index + 1 == document.state.history.size());
    }
    AppendIndent(&out, 2);
    out += "]\n";
  }

  AppendIndent(&out, 1);
  out += "}\n";
  out += "}\n";
  return out;
}

// ---- 解析 ----
//
// 字段集合是"完全相等"的：少一个报 missing，多一个报 unknown。
// 现场手改出来的、我们不认识的字段不会被静默忽略。
//
// 另外两条贯穿整个解析层的规则：
//   * 解析只看文本、不碰文件系统——所以 schedule show 能把一份非法配置原样
//     展示出来，而不是先被环境（目录不存在、仓库没配）挡住；
//   * 任何失败都让 *document 保持默认值：调用方拿到的一定是"全有或全无"，
//     不存在半份文件内容 + 半份默认值的混合体。

namespace {

// 这几张字段表就是本文件的 schema，字段名必须与 SerializeScheduleDocument
// 写出的名字逐字一致；RequireExactFields 用它们同时判 missing 与 unknown。
// 唯一的兼容口子是 kStateOptionalFields（历史版本追加的三个 baseline 字段），
// 而且它只减不增：以后新增字段应提升 version，不要再开第二个可选列表。
const std::vector<const char*> kRootFields = {"version", "config", "state"};
const std::vector<const char*> kConfigFields = {
    "enabled",          "trigger",       "strategy",     "source_path",
    "interval_minutes", "retain_count",  "pack",         "compression",
    "encryption",       "include_rules", "exclude_rules"};
const std::vector<const char*> kStateFields = {
    "next_run_time_sec", "last_success_time_sec", "last_manifest_entry_count",
    "managed_snapshots", "history"};
// baseline 是后来追加的字段。旧版写出的 schedule.json 没有这
// 三个字段，它必须仍然读得进来（缺 baseline = 不知道 manifest 属于哪份快照
// = 下一轮重建基线快照，语义上恰好就是安全的那个默认）。但"不在这两个列表
// 里的 key"照样报 unknown，"kStateFields 里少一个"照样报 missing。
const std::vector<const char*> kStateOptionalFields = {
    "baseline_snapshot_file_name", "baseline_repository_identity",
    "baseline_source_path"};
const std::vector<const char*> kSnapshotFields = {
    "file_name", "created_time_sec", "added",       "removed",
    "modified",  "metadata_changed", "entry_count", "archive_size",
    "pack",      "compression"};
const std::vector<const char*> kHistoryFields = {
    "scheduled_at_sec",  "started_at_sec", "finished_at_sec", "result",
    "archive_file_name", "added",          "removed",         "modified",
    "metadata_changed",  "diagnostic"};

// 解析错误的统一形状 "Invalid schedule store: <位置>: field '<key>' ..."。
// 位置用 state.managed_snapshots[3] 这类路径式写法，用户能直接定位到字段。
bool BadField(const std::string& what, const char* key,
              const std::string& detail, std::string* error_message) {
  SetError(error_message, "Invalid schedule store: " + what + ": field '" +
                              key + "' " + detail);
  return false;
}

// 可选字符串字段：完全不出现就保持默认（空串），出现了就必须是合法字符串。
// 只有 kStateOptionalFields 里的字段会走这里。
// 可选的只是"允许缺失"，不是"允许不合法"：出现了就按完整规则校验。
bool RequireOptionalString(const JsonValue& object, const char* key,
                           const std::string& what, std::string* out,
                           std::string* error_message) {
  if (object.Find(key) == nullptr) return true;
  if (!RequireString(object, key, what, out, error_message)) return false;
  if (!IsBoundedString(*out, kMaxScheduleStringBytes)) {
    return BadField(what, key, "is unusable (too long or contains a NUL byte)",
                    error_message);
  }
  return true;
}

// 四个计数各有上界（kMaxManifestEntries），但刻意**不**交叉校验
// added+removed+modified 是否等于名录项的 entry_count：两者由不同的扫描
// 路径产出，强行相等会让扫描口径一改，历史文件就集体读不回来。
// 这里只保证每个数值本身可用。
bool ParseChangeFields(const JsonValue& object, const std::string& what,
                       ChangeSummary* changes, std::string* error_message) {
  if (!RequireUint64(object, "added", what, kMaxManifestEntries,
                     &changes->added, error_message)) {
    return false;
  }
  if (!RequireUint64(object, "removed", what, kMaxManifestEntries,
                     &changes->removed, error_message)) {
    return false;
  }
  if (!RequireUint64(object, "modified", what, kMaxManifestEntries,
                     &changes->modified, error_message)) {
    return false;
  }
  return RequireUint64(object, "metadata_changed", what, kMaxManifestEntries,
                       &changes->metadata_changed, error_message);
}

// config 段：先要求字段集合完全相等，再逐字段解析。枚举一律走 Parse*Key
// （精确匹配字符串），不做大小写折叠、不接受别名；未知取值直接报错而不是
// 回落到默认值——静默回落会把一份写错的配置变成一份"看着正常、行为不同"
// 的计划，用户永远不会发现。
bool ParseConfig(const JsonValue& root, ScheduleConfig* config,
                 std::string* error_message) {
  const JsonValue* object = nullptr;
  if (!RequireObject(root.Find("config"), "config", &object, error_message)) {
    return false;
  }
  if (!RequireExactFields(*object, kConfigFields, "config", error_message)) {
    return false;
  }
  if (!RequireBool(*object, "enabled", "config", &config->enabled,
                   error_message)) {
    return false;
  }

  std::string trigger_key;
  if (!RequireString(*object, "trigger", "config", &trigger_key,
                     error_message)) {
    return false;
  }
  if (!ParseBackupTriggerKey(trigger_key, &config->trigger)) {
    return BadField("config", "trigger",
                    "is not a known trigger: '" + trigger_key + "'",
                    error_message);
  }
  std::string strategy_key;
  if (!RequireString(*object, "strategy", "config", &strategy_key,
                     error_message)) {
    return false;
  }
  if (!ParseBackupStrategyKey(strategy_key, &config->strategy)) {
    return BadField("config", "strategy",
                    "is not a known strategy: '" + strategy_key + "'",
                    error_message);
  }
  if (!RequireString(*object, "source_path", "config", &config->source_path,
                     error_message)) {
    return false;
  }
  if (!RequireUint32(*object, "interval_minutes", "config", kMinIntervalMinutes,
                     kMaxIntervalMinutes, &config->interval_minutes,
                     error_message)) {
    return false;
  }
  if (!RequireUint32(*object, "retain_count", "config", kMinRetainCount,
                     kMaxRetainCount, &config->retain_count, error_message)) {
    return false;
  }

  std::string pack_key;
  if (!RequireString(*object, "pack", "config", &pack_key, error_message)) {
    return false;
  }
  if (!ParsePackMethodKey(pack_key, &config->pack_method)) {
    return BadField("config", "pack",
                    "is not a known pack method: '" + pack_key + "'",
                    error_message);
  }
  std::string compression_key;
  if (!RequireString(*object, "compression", "config", &compression_key,
                     error_message)) {
    return false;
  }
  if (!ParseCompressionMethodKey(compression_key,
                                 &config->compression_method)) {
    return BadField(
        "config", "compression",
        "is not a known compression method: '" + compression_key + "'",
        error_message);
  }
  std::string encryption_key;
  if (!RequireString(*object, "encryption", "config", &encryption_key,
                     error_message)) {
    return false;
  }
  // 这里只解析，不做"必须等于 none"的产品判断——那是 ValidateScheduleConfig
  // 的职责。分开之后 schedule show 还能把一份非法配置原样显示出来。
  if (!ParseEncryptionMethodKey(encryption_key, &config->encryption_method)) {
    return BadField(
        "config", "encryption",
        "is not a known encryption method: '" + encryption_key + "'",
        error_message);
  }

  // 规则条数与单条长度在这里再查一遍：Save() 侧查过，但读进来的文件可能
  // 根本没过过 Save()（手改、别的版本写的），读侧不能假设它守规矩。
  if (!RequireStringArray(*object, "include_rules", "config",
                          &config->include_rules, error_message)) {
    return false;
  }
  if (!RequireStringArray(*object, "exclude_rules", "config",
                          &config->exclude_rules, error_message)) {
    return false;
  }
  if (config->include_rules.size() + config->exclude_rules.size() >
      kMaxScheduleRules) {
    return BadField("config", "include_rules",
                    "contains too many rules (limit " +
                        std::to_string(kMaxScheduleRules) + ")",
                    error_message);
  }
  for (const std::string& rule : config->include_rules) {
    if (!IsBoundedString(rule, kMaxScheduleStringBytes)) {
      return BadField("config", "include_rules",
                      "contains an unusable rule (too long or with a NUL byte)",
                      error_message);
    }
  }
  for (const std::string& rule : config->exclude_rules) {
    if (!IsBoundedString(rule, kMaxScheduleStringBytes)) {
      return BadField("config", "exclude_rules",
                      "contains an unusable rule (too long or with a NUL byte)",
                      error_message);
    }
  }
  if (!IsBoundedString(config->source_path, kMaxScheduleStringBytes)) {
    return BadField("config", "source_path",
                    "is unusable (too long or contains a NUL byte)",
                    error_message);
  }
  return true;
}

// 名录项：file_name 必须是"单组件 + .bak"的归档名，因为它随后会被交给
// BackupCatalog 去 Resolve/Delete——本文件绝不放行带 '/'、'\' 或 ".." 的值。
// entry_count / archive_size 的上界取 1<<62 而不是 uint64 的最大值：
// 这两个数还会参与界面上的加法与格式化，留出余量，避免在别处回绕。
bool ParseSnapshot(const JsonValue& value, const std::string& what,
                   ScheduledSnapshotRecord* record,
                   std::string* error_message) {
  if (!value.is_object()) {
    SetError(error_message,
             "Invalid schedule store: " + what + ": must be an object");
    return false;
  }
  if (!RequireExactFields(value, kSnapshotFields, what, error_message)) {
    return false;
  }
  if (!RequireString(value, "file_name", what, &record->file_name,
                     error_message)) {
    return false;
  }
  if (!IsSingleComponentArchiveName(record->file_name)) {
    return BadField(
        what, "file_name",
        "must be a single .bak file name: '" + record->file_name + "'",
        error_message);
  }
  if (!RequireInt64(value, "created_time_sec", what, &record->created_time_sec,
                    error_message)) {
    return false;
  }
  if (!ParseChangeFields(value, what, &record->changes, error_message)) {
    return false;
  }
  if (!RequireUint64(value, "entry_count", what, 1ull << 62,
                     &record->entry_count, error_message)) {
    return false;
  }
  if (!RequireUint64(value, "archive_size", what, 1ull << 62,
                     &record->archive_size, error_message)) {
    return false;
  }
  std::string pack_key;
  if (!RequireString(value, "pack", what, &pack_key, error_message)) {
    return false;
  }
  if (!ParsePackMethodKey(pack_key, &record->pack_method)) {
    return BadField(what, "pack",
                    "is not a known pack method: '" + pack_key + "'",
                    error_message);
  }
  std::string compression_key;
  if (!RequireString(value, "compression", what, &compression_key,
                     error_message)) {
    return false;
  }
  if (!ParseCompressionMethodKey(compression_key,
                                 &record->compression_method)) {
    return BadField(
        what, "compression",
        "is not a known compression method: '" + compression_key + "'",
        error_message);
  }
  return true;
}

// history 项：archive_file_name 允许为空（失败的那一轮没有产物），但非空时
// 同样必须是可管理的归档名。diagnostic 是有界的自由文本，写它的调用方
// 负责不把密码放进去——那是 schedule_store.h 里写死的硬约束。
bool ParseHistoryEntry(const JsonValue& value, const std::string& what,
                       ScheduleHistoryEntry* entry,
                       std::string* error_message) {
  if (!value.is_object()) {
    SetError(error_message,
             "Invalid schedule store: " + what + ": must be an object");
    return false;
  }
  if (!RequireExactFields(value, kHistoryFields, what, error_message)) {
    return false;
  }
  if (!RequireInt64(value, "scheduled_at_sec", what, &entry->scheduled_at_sec,
                    error_message)) {
    return false;
  }
  if (!RequireInt64(value, "started_at_sec", what, &entry->started_at_sec,
                    error_message)) {
    return false;
  }
  if (!RequireInt64(value, "finished_at_sec", what, &entry->finished_at_sec,
                    error_message)) {
    return false;
  }
  std::string result_key;
  if (!RequireString(value, "result", what, &result_key, error_message)) {
    return false;
  }
  if (!ParseScheduleRunResultKey(result_key, &entry->result)) {
    return BadField(what, "result",
                    "is not a known run result: '" + result_key + "'",
                    error_message);
  }
  if (!RequireString(value, "archive_file_name", what,
                     &entry->archive_file_name, error_message)) {
    return false;
  }
  if (!entry->archive_file_name.empty() &&
      !IsSingleComponentArchiveName(entry->archive_file_name)) {
    return BadField(what, "archive_file_name",
                    "must be empty or a single .bak file name: '" +
                        entry->archive_file_name + "'",
                    error_message);
  }
  if (!ParseChangeFields(value, what, &entry->changes, error_message)) {
    return false;
  }
  if (!RequireString(value, "diagnostic", what, &entry->diagnostic,
                     error_message)) {
    return false;
  }
  if (!IsBoundedString(entry->diagnostic, kMaxScheduleStringBytes)) {
    return BadField(what, "diagnostic",
                    "is unusable (too long or contains a NUL byte)",
                    error_message);
  }
  return true;
}

// state 段：三个数值字段必填，baseline 三件套可选（缺失 = 没记录过基线）。
// 两个数组的上界与 Save() 侧一致（kMaxRetainCount / kMaxHistoryEntries）：
// 读侧一旦比写侧更严，就会出现"自己写的文件自己读不回来"，
// 而 Save() 的上界又与调用方真正会产出的长度绑定（见 schedule_store.h）。
bool ParseState(const JsonValue& root, ScheduleState* state,
                std::string* error_message) {
  const JsonValue* object = nullptr;
  if (!RequireObject(root.Find("state"), "state", &object, error_message)) {
    return false;
  }
  if (!RequireExactFields(*object, kStateFields, kStateOptionalFields, "state",
                          error_message)) {
    return false;
  }
  if (!RequireInt64(*object, "next_run_time_sec", "state",
                    &state->next_run_time_sec, error_message)) {
    return false;
  }
  if (!RequireInt64(*object, "last_success_time_sec", "state",
                    &state->last_success_time_sec, error_message)) {
    return false;
  }
  if (!RequireUint64(*object, "last_manifest_entry_count", "state",
                     kMaxManifestEntries, &state->last_manifest_entry_count,
                     error_message)) {
    return false;
  }

  // baseline 三件套：全部可选，缺失就是空串（= 没有记录过 baseline）。
  // file_name 存在时必须是合法的单组件归档名——它是从仓库里 Resolve 出来的
  // 依据，绝不允许出现带 '/' 或 ".." 的东西。
  if (!RequireOptionalString(*object, "baseline_snapshot_file_name", "state",
                             &state->baseline.snapshot_file_name,
                             error_message)) {
    return false;
  }
  if (!state->baseline.snapshot_file_name.empty() &&
      !IsSingleComponentArchiveName(state->baseline.snapshot_file_name)) {
    return BadField("state", "baseline_snapshot_file_name",
                    "is not a plain .bak file name", error_message);
  }
  if (!RequireOptionalString(*object, "baseline_repository_identity", "state",
                             &state->baseline.repository_identity,
                             error_message)) {
    return false;
  }
  if (!RequireOptionalString(*object, "baseline_source_path", "state",
                             &state->baseline.source_path, error_message)) {
    return false;
  }

  const JsonValue* snapshots = nullptr;
  if (!RequireArray(*object, "managed_snapshots", "state", &snapshots,
                    error_message)) {
    return false;
  }
  if (snapshots->array.size() > kMaxRetainCount) {
    return BadField("state", "managed_snapshots",
                    "holds more records than the maximum retain count (" +
                        std::to_string(kMaxRetainCount) + ")",
                    error_message);
  }
  state->managed_snapshots.clear();
  state->managed_snapshots.reserve(snapshots->array.size());
  for (std::size_t index = 0; index < snapshots->array.size(); ++index) {
    ScheduledSnapshotRecord record;
    const std::string what =
        "state.managed_snapshots[" + std::to_string(index) + "]";
    if (!ParseSnapshot(snapshots->array[index], what, &record, error_message)) {
      return false;
    }
    state->managed_snapshots.push_back(std::move(record));
  }

  const JsonValue* history = nullptr;
  if (!RequireArray(*object, "history", "state", &history, error_message)) {
    return false;
  }
  if (history->array.size() > kMaxHistoryEntries) {
    return BadField("state", "history",
                    "holds more entries than the bound (" +
                        std::to_string(kMaxHistoryEntries) + ")",
                    error_message);
  }
  state->history.clear();
  state->history.reserve(history->array.size());
  for (std::size_t index = 0; index < history->array.size(); ++index) {
    ScheduleHistoryEntry entry;
    const std::string what = "state.history[" + std::to_string(index) + "]";
    if (!ParseHistoryEntry(history->array[index], what, &entry,
                           error_message)) {
      return false;
    }
    state->history.push_back(std::move(entry));
  }
  return true;
}

// 版本闸门放在最前面：version 不认识就直接失败，绝不做"尽力解析"。
// 未来版本写出的文件里，我们读不懂的字段很可能改变语义，猜着用比拒绝危险。
// 解析在局部 loaded 上完成，全部成功后才 move 进 *document，避免半成品。
bool ParseDocument(const std::string& text, ScheduleDocument* document,
                   std::string* error_message) {
  JsonValue root;
  if (!ParseJson(text, &root, error_message)) return false;
  const JsonValue* object = nullptr;
  if (!RequireObject(&root, "the schedule store", &object, error_message)) {
    return false;
  }
  if (!RequireExactFields(*object, kRootFields, "the schedule store",
                          error_message)) {
    return false;
  }
  std::int64_t version = 0;
  if (!RequireInt64(*object, "version", "the schedule store", &version,
                    error_message)) {
    return false;
  }
  if (version != 1) {
    SetError(error_message,
             "Unsupported schedule store version: " + std::to_string(version));
    return false;
  }
  ScheduleDocument loaded;
  if (!ParseConfig(*object, &loaded.config, error_message)) return false;
  if (!ParseState(*object, &loaded.state, error_message)) return false;
  *document = std::move(loaded);
  return true;
}

}  // namespace

// ---- ScheduleStore ----

// 构造函数只保存路径，不做任何 I/O：对象可以自由拷贝、放进容器，
// 真正的读写全部发生在 Load/Save/LoadManifest/SaveManifest 里，且都是 const。
ScheduleStore::ScheduleStore(std::string schedule_file_path)
    : schedule_file_path_(std::move(schedule_file_path)) {}

// manifest 的路径由 schedule.json 派生：先剥掉末尾的 ".json" 再拼
// "-manifest.dat"。派生规则只有这一处，所以"配置放哪"与"清单放哪"
// 不可能分叉。副作用是 "a.json" 与 "a" 会派生出同一个 manifest 路径，
// 调用方不该把这两个路径当成两份互不相干的计划。
std::string ScheduleStore::manifest_file_path() const {
  std::string base = schedule_file_path_;
  const std::string suffix = ".json";
  if (base.size() > suffix.size() &&
      base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
    base.erase(base.size() - suffix.size());
  }
  return base + "-manifest.dat";
}

// 锁文件与数据文件分开：flock 需要一个长期稳定的 fd，而不是数据文件本身
// （原子替换会把数据文件的 inode 换掉，锁也就跟着丢了）。
// 这个文件只有 flock 语义，内容永远为空，也不属于任何快照的副文件。
std::string ScheduleStore::lock_file_path() const {
  return schedule_file_path_ + ".lock";
}

// 读配置。三种结果各有明确语义：
//   kLoaded  —— *document 是文件内容的忠实映射；
//   kMissing —— 文件不存在，*document 保持默认值（disabled、每小时、留 12）；
//   kError   —— 文件在但不可用：*document 同样是默认值，但调用方**必须**把
//               error_message 报出去，不许假装这是首次运行。
// 进函数先复位 *document：失败路径上不会留下上一次调用的残留。
ScheduleLoadStatus ScheduleStore::Load(ScheduleDocument* document,
                                       std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (document == nullptr) {
    SetError(error_message, "Schedule document output must not be null");
    return ScheduleLoadStatus::kError;
  }
  *document = ScheduleDocument{};
  // 空路径必须在碰文件系统之前拦掉：否则 lstat("") 给出的错误跟"配置还不存在"
  // 完全不是一回事，调用方也就没法正确分流。
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot load schedule: schedule file path is empty");
    return ScheduleLoadStatus::kError;
  }

  std::string text;
  bool missing = false;
  if (!ReadWholeFile(schedule_file_path_, kMaxScheduleFileBytes, &text,
                     &missing, error_message)) {
    return ScheduleLoadStatus::kError;
  }
  if (missing) return ScheduleLoadStatus::kMissing;

  ScheduleDocument loaded;
  if (!ParseDocument(text, &loaded, error_message)) {
    return ScheduleLoadStatus::kError;
  }
  *document = std::move(loaded);
  return ScheduleLoadStatus::kLoaded;
}

// 保存配置。步骤顺序是刻意的：
//   1. 结构校验（ValidateScheduleConfig）——一份"写下去就读不回来"的配置
//      比一次明确的失败糟得多；
//   2. 名录 / history / baseline 的上界与命名检查——这些字段不在
//      ValidateScheduleConfig 的职责里（它只管 config），必须单独把住；
//   3. 建父目录，覆盖"首次运行"与"应用配置目录被清空过"两种情况；
//   4. 原子替换：临时文件 + fsync + rename，权限固定 0600。
// 失败语义：任何一步失败都不写盘，磁盘上原有的文件保持原样。
bool ScheduleStore::Save(const ScheduleDocument& document,
                         std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot save schedule: schedule file path is empty");
    return false;
  }
  // 保存前先做结构校验：一份"写下去就读不回来"的配置，比一次明确的失败糟得多。
  if (!ValidateScheduleConfig(document.config, error_message)) return false;
  if (document.state.managed_snapshots.size() > kMaxRetainCount) {
    SetError(error_message,
             "Cannot save schedule: managed snapshot list exceeds " +
                 std::to_string(kMaxRetainCount) + " records");
    return false;
  }
  if (document.state.history.size() > kMaxHistoryEntries) {
    SetError(error_message, "Cannot save schedule: history exceeds " +
                                std::to_string(kMaxHistoryEntries) +
                                " entries");
    return false;
  }
  for (const ScheduledSnapshotRecord& record :
       document.state.managed_snapshots) {
    if (!IsSingleComponentArchiveName(record.file_name)) {
      SetError(error_message,
               "Cannot save schedule: managed snapshot record has an invalid "
               "file name: '" +
                   record.file_name + "'");
      return false;
    }
  }
  // baseline 指向的也必须是一个合法的单组件归档名。空串是合法的：那表示
  // "还没有 baseline"，不是错误。
  if (!document.state.baseline.snapshot_file_name.empty() &&
      !IsSingleComponentArchiveName(
          document.state.baseline.snapshot_file_name)) {
    SetError(error_message,
             "Cannot save schedule: the baseline snapshot has an invalid "
             "file name: '" +
                 document.state.baseline.snapshot_file_name + "'");
    return false;
  }
  if (!IsBoundedString(document.state.baseline.repository_identity,
                       kMaxScheduleStringBytes) ||
      !IsBoundedString(document.state.baseline.source_path,
                       kMaxScheduleStringBytes)) {
    SetError(error_message,
             "Cannot save schedule: the baseline record holds an unusable "
             "path (too long or contains a NUL byte)");
    return false;
  }

  // 父目录不存在就先建出来：首次运行、或者应用配置目录被清空过时，
  // "保存计划"不该因为路径还不存在而失败。
  if (!MakeDirectories(ParentDirectoryOf(schedule_file_path_), error_message)) {
    return false;
  }
  // 原子替换的实现在 file_io.cpp：临时文件名由 mkstemp 生成（唯一 + O_EXCL），
  // 因此既不会跟随别人预放的符号链接，也不会截断别人预放的文件。ConfigManager
  // 用的是同一个函数——两份各自演化的临时文件写法迟早会有一份漏掉某条边界。
  return WriteFileAtomicallyReplacing(
      schedule_file_path_, SerializeScheduleDocument(document), error_message);
}

// 两个方向都是纯字段搬运，不做任何规范化或补全：这里是"我以为 baseline
// 是谁"与"文件自己说属于谁"之间唯一的桥。任何一边偷偷填空，都会让
// SameBaselineBinding 的"空即不一致"结论失效。
ManifestBinding BindingOf(const ScheduleBaseline& baseline) {
  ManifestBinding binding;
  binding.snapshot_file_name = baseline.snapshot_file_name;
  binding.repository_identity = baseline.repository_identity;
  binding.source_path = baseline.source_path;
  return binding;
}

ScheduleBaseline BaselineOf(const ManifestBinding& binding) {
  ScheduleBaseline baseline;
  baseline.snapshot_file_name = binding.snapshot_file_name;
  baseline.repository_identity = binding.repository_identity;
  baseline.source_path = binding.source_path;
  return baseline;
}

// 逐字段比较而不是比较序列化后的整体文本：字段顺序与书写形式不该参与
// 归属判断，只有三个语义值都相同才算同一对。
bool SameBaselineBinding(const ScheduleBaseline& baseline,
                         const ManifestBinding& binding) {
  // version 1 的 manifest 没有归属信息。空 binding 与空 baseline 都是
  // "不知道属于谁"，一律判为不一致——这里绝不猜。
  if (binding.empty()) return false;
  if (baseline.snapshot_file_name.empty()) return false;
  return baseline.snapshot_file_name == binding.snapshot_file_name &&
         baseline.repository_identity == binding.repository_identity &&
         baseline.source_path == binding.source_path;
}

// 读源清单。kMissing 同时覆盖"从来没写过"与"文件被删了"——两者都等于
// **没有可信基线**，调用方据此重建完整基线快照，而不是假装没有变化。
// kError（文件在但坏了）同样不可信，区别只在于调用方要把 error_message
// 记进诊断，不能假装无事发生。
ScheduleStore::ManifestLoadStatus ScheduleStore::LoadManifest(
    std::vector<ManifestEntry>* entries, ManifestBinding* binding,
    std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (entries == nullptr) {
    SetError(error_message, "Manifest output must not be null");
    return ManifestLoadStatus::kError;
  }
  if (binding == nullptr) {
    SetError(error_message, "Manifest binding output must not be null");
    return ManifestLoadStatus::kError;
  }
  entries->clear();
  *binding = ManifestBinding{};
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot load source manifest: schedule file path is empty");
    return ManifestLoadStatus::kError;
  }

  std::string text;
  bool missing = false;
  if (!ReadWholeFile(manifest_file_path(), kMaxManifestBytes, &text, &missing,
                     error_message)) {
    return ManifestLoadStatus::kError;
  }
  if (missing) return ManifestLoadStatus::kMissing;
  if (!ParseManifest(text, entries, binding, error_message)) {
    return ManifestLoadStatus::kError;
  }
  // 第二层校验：binding 里那份快照名还必须是一个**本仓库能管理的归档名**。
  // 结构检查（单组件、无 NUL、有界）在 ParseManifest 里已经做过一次，
  // 这里补上归档命名规则这一条——两份 manifest 规则各自守自己的边界，
  // 谁也不替谁放宽。
  if (!binding->empty() &&
      !IsSingleComponentArchiveName(binding->snapshot_file_name)) {
    SetError(
        error_message,
        "Invalid source manifest: the baseline snapshot file name is not a "
        "manageable archive name: '" +
            binding->snapshot_file_name + "'");
    return ManifestLoadStatus::kError;
  }
  return ManifestLoadStatus::kLoaded;
}

// 写源清单。归属（快照名 + 仓库 identity + 源路径）必须完整且合法，否则
// 拒绝写入：一份"不知道自己属于谁"的 manifest 会被读侧判成不可信，
// 落盘只是占地方，还会让下一轮误以为"仓库里有清单可查"。
// 序列化、字节上界、原子替换三层依次把关，最后一步才碰磁盘。
bool ScheduleStore::SaveManifest(const std::vector<ManifestEntry>& entries,
                                 const ManifestBinding& binding,
                                 std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot save source manifest: schedule file path is empty");
    return false;
  }
  if (entries.size() > kMaxManifestEntries) {
    SetError(error_message,
             "Cannot save source manifest: " + std::to_string(entries.size()) +
                 " entries exceeds the bound");
    return false;
  }
  // 归属必须完整，而且必须是一个 Catalog 认得出来的归档名。
  if (!IsSingleComponentArchiveName(binding.snapshot_file_name)) {
    SetError(error_message,
             "Cannot save source manifest: the baseline snapshot file name is "
             "not a manageable archive name: '" +
                 binding.snapshot_file_name + "'");
    return false;
  }
  if (!IsBoundedString(binding.repository_identity, kMaxScheduleStringBytes) ||
      !IsBoundedString(binding.source_path, kMaxScheduleStringBytes) ||
      binding.repository_identity.empty() || binding.source_path.empty()) {
    SetError(error_message,
             "Cannot save source manifest: the baseline binding holds an "
             "unusable repository or source path");
    return false;
  }

  const std::string text = SerializeManifest(entries, binding);
  if (text.empty()) {
    SetError(error_message,
             "Cannot save source manifest: the baseline binding is incomplete");
    return false;
  }
  if (text.size() > kMaxManifestBytes) {
    SetError(error_message,
             "Cannot save source manifest: " + std::to_string(text.size()) +
                 " bytes exceeds the bound");
    return false;
  }
  return WriteFileAtomicallyReplacing(manifest_file_path(), text,
                                      error_message);
}

// 删除源清单（例如重建基线之前先作废旧清单）。语义是幂等的：文件本来就
// 不存在（ENOENT）也算成功——"让这个文件不存在"这个后置条件已经满足。
// 这里不做目录 fsync：最坏情况是崩溃后旧清单还在，而它带着完整 binding，
// 读侧与当前 baseline 一比就会发现对不上，结论仍然是"没有可信基线"。
bool ScheduleStore::RemoveManifest(std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot remove source manifest: schedule file path is empty");
    return false;
  }
  const std::string path = manifest_file_path();
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    SetError(error_message, Describe(errno, "Failed to remove", path));
    return false;
  }
  return true;
}

}  // namespace backupproject

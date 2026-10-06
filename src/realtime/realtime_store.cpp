// realtime_store.cpp
//
// 见 include/realtime_store.h。
// 模块职责：realtime 配置的磁盘读写与加载期校验——把 RealtimeConfig 序列化成
// 一份人可读、可 diff 的 JSON，再把它安全地读回来。
//
// 边界（不负责什么）：不做文件事件监听、不做 debounce 计时、不做备份调度；
// 也不决定“哪些选项组合合法”——那份答案的唯一来源是 backup_option_keys.h
// 里的共享真值表。
//
// 数据流：Save(config) -> 校验 -> 拼 JSON 文本 -> 原子替换落盘；
// Load(&config) -> 读整份文件 -> ParseJson -> 逐字段强类型取值 -> 校验 ->
// 调用方拿到一份通过产品级校验的配置（或者一个明确的失败原因）。
//
// 失败语义：Load 用 RealtimeLoadStatus 三态区分“没有配置文件”与“配置文件坏
// 了”：前者可以走默认值，后者必须让用户看到原因。Save 失败时不改动原文件。
//
// 不变量：写出去的字节必须能被本文件的 Load 原样读回；长度与取值在写之前、
// 读之后各校验一次；任何一步不过都整体失败，绝不部分接受。
//
// 安全边界：磁盘上的文件按不可信输入对待——O_NOFOLLOW 打开、必须是普通文件、
// 有长度上限、逐字段强类型解析、解析后再跑一次完整校验。

#include "realtime_store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "backup_catalog.h"
#include "backup_option_keys.h"
#include "file_io.h"
#include "incremental_delta.h"
#include "simple_json.h"
#include "source_digest.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

std::string ErrnoText(int error_number) {
  const char* text = ::strerror(error_number);
  return text == nullptr ? std::string("errno ") + std::to_string(error_number)
                         : std::string(text);
}

// 所有来自磁盘或命令行的字符串都要过这一关：长度上限挡住内存放大，NUL 检查
// 挡住“std::string 里藏着 C 字符串看不见的后半截”这类语义分叉。
bool IsBoundedString(const std::string& value) {
  if (value.size() > kMaxRealtimeStringBytes) return false;
  return value.find('\0') == std::string::npos;
}

// 去掉尾部 '/'（根目录本身除外）。只处理分隔符，不解析 "." / ".."：那是
// canonicalize 的活，而 canonicalize 可能把路径带到别处去。
std::string StripTrailingSlashes(const std::string& path) {
  if (path.empty()) return path;
  std::string out = path;
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

// 存在时用 realpath 归一化；不存在时退化成"去掉尾部斜杠的绝对路径"。
// 两种情况下都不做任何猜测。
std::string CanonicalOrAbsolute(const std::string& path) {
  if (path.empty()) return path;
  char resolved[PATH_MAX];
  if (::realpath(path.c_str(), resolved) != nullptr) {
    return StripTrailingSlashes(std::string(resolved));
  }
  return StripTrailingSlashes(path);
}

// 路径包含判定必须按“组件边界”比较，不能拿字符串前缀：/data 与 /data2 不是
// 祖孙关系。root == "/" 单独处理，因为空前缀会让任何绝对路径都命中。
bool IsSameOrDescendant(const std::string& candidate, const std::string& root) {
  if (candidate == root) return true;
  if (root == "/") return !candidate.empty() && candidate[0] == '/';
  if (candidate.size() <= root.size()) return false;
  if (candidate.compare(0, root.size(), root) != 0) return false;
  return candidate[root.size()] == '/';
}

// 用 lstat 而不是 stat：调用方要能区分“不是目录”和“是个指向别处的符号链接”，
// 后者在 realtime 里一律拒绝。失败时 error_message 带上 errno 文本。
bool LstatIsDirectory(const std::string& path, bool* is_directory,
                      bool* is_symlink, std::string* error_message) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    SetError(error_message, "Cannot inspect " + path + ": " + ErrnoText(errno));
    return false;
  }
  *is_directory = S_ISDIR(info.st_mode);
  *is_symlink = S_ISLNK(info.st_mode);
  return true;
}

// 把规则数组折叠成一个字符串，专供 identity 摘要使用；分隔符必须是 '\n'，
// 否则 ["ab"] 与 ["a","b"] 会折叠出同一个摘要。
std::string KeyOfRuleList(const std::vector<std::string>& rules) {
  std::string out;
  for (const std::string& rule : rules) {
    out += rule;
    out.push_back('\n');
  }
  return out;
}

}  // namespace

// 判定源目录与仓库目录的关系：相等、谁在谁里面、还是无关。这是 realtime 的
// 硬边界：重叠意味着“备份写出的文件又触发下一次备份”。
// 两个路径都不要求存在：realpath 成功就用归一化结果，失败就退回“去掉尾部斜杠
// 的路径”，所以 kUnknown 只有“路径为空”一种来源——调用方必须当失败处理，
// 不能当成“没重叠”继续跑。
RealtimePathOverlap ClassifyPathOverlap(const std::string& source_path,
                                        const std::string& repository_path,
                                        std::string* detail) {
  if (detail != nullptr) detail->clear();
  if (source_path.empty() || repository_path.empty()) {
    if (detail != nullptr) *detail = "one of the two paths is empty";
    return RealtimePathOverlap::kUnknown;
  }
  const std::string source = CanonicalOrAbsolute(source_path);
  const std::string repository = CanonicalOrAbsolute(repository_path);
  if (source == repository) {
    if (detail != nullptr) *detail = source;
    return RealtimePathOverlap::kEqual;
  }
  if (IsSameOrDescendant(repository, source)) {
    if (detail != nullptr) *detail = repository + " is inside " + source;
    return RealtimePathOverlap::kRepositoryInsideSource;
  }
  if (IsSameOrDescendant(source, repository)) {
    if (detail != nullptr) *detail = source + " is inside " + repository;
    return RealtimePathOverlap::kSourceInsideRepository;
  }
  return RealtimePathOverlap::kNone;
}

// 用产品自己的 Filter 编译规则，顺序固定为“先 include 再 exclude”；优先级语义
// 完全由 Filter 决定，这里不复制一份（两份规则迟早会分叉）。
// 任一规则编译失败就整体失败：半个 Filter 不能拿去做备份。
bool BuildRealtimeFilter(const RealtimeConfig& config, Filter* filter,
                         std::string* error_message) {
  if (filter == nullptr) {
    SetError(error_message, "Realtime filter output must not be null");
    return false;
  }
  // 顺序与手动 / 计划一致：先 include 再 exclude；Filter 内部沿用它自己的
  // 优先级规则（exclude 优先），这里不复制任何语义。
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

// 这是 realtime 配置唯一的语义校验点：Save、Load、enable 都调它，能走通的配置
// 在三条路径上含义一致。校验范围：版本、trigger、路径长度、debounce 与
// max_wait 的区间与相互关系、retain_count、规则条数与规则能否编译、策略组合
// 是否存在。它不访问文件系统，所以 disabled 状态下允许保存一个暂时不存在的
// source（enable 前的那道完整校验才要求目录真的存在）。失败时 error_message
// 是给用户看的原因，调用方不要吞掉它。
bool ValidateRealtimeConfig(const RealtimeConfig& config,
                            std::string* error_message) {
  if (config.version != kRealtimeConfigVersion) {
    SetError(error_message, "Unsupported realtime config version: " +
                                std::to_string(config.version));
    return false;
  }
  // trigger 必须是 realtime：这份配置文件只描述实时任务，别把计划任务的配置
  // 塞进来（那会让“这一轮是谁触发的”变成需要猜的字段）。
  if (config.trigger != BackupTrigger::kRealtime) {
    SetError(error_message, "A realtime config must have trigger = realtime");
    return false;
  }
  if (!IsBoundedString(config.source_path)) {
    SetError(error_message,
             "Realtime source path is not usable (too long or contains NUL)");
    return false;
  }
  if (config.debounce_ms < kMinRealtimeDebounceMs ||
      config.debounce_ms > kMaxRealtimeDebounceMs) {
    SetError(error_message,
             "Realtime debounce must be between " +
                 std::to_string(kMinRealtimeDebounceMs) + " and " +
                 std::to_string(kMaxRealtimeDebounceMs) + " ms, got " +
                 std::to_string(config.debounce_ms));
    return false;
  }
  if (config.max_wait_ms < kMinRealtimeMaxWaitMs ||
      config.max_wait_ms > kMaxRealtimeMaxWaitMs) {
    SetError(error_message,
             "Realtime max wait must be between " +
                 std::to_string(kMinRealtimeMaxWaitMs) + " and " +
                 std::to_string(kMaxRealtimeMaxWaitMs) + " ms, got " +
                 std::to_string(config.max_wait_ms));
    return false;
  }
  // 两个窗口的关系也要校验：max_wait 是“最多攒多久”，比 debounce 还短的话
  // 配置在语义上自相矛盾——引擎侧只能二选一，不如在这里拒绝。
  if (config.max_wait_ms < config.debounce_ms) {
    SetError(error_message,
             "Realtime max wait must not be smaller than the debounce window");
    return false;
  }
  if (config.retain_count < kMinRealtimeRetainCount ||
      config.retain_count > kMaxRealtimeRetainCount) {
    SetError(error_message, "Realtime retain count must be between " +
                                std::to_string(kMinRealtimeRetainCount) +
                                " and " +
                                std::to_string(kMaxRealtimeRetainCount) +
                                ", got " + std::to_string(config.retain_count));
    return false;
  }
  if (config.include_rules.size() + config.exclude_rules.size() >
      kMaxRealtimeRules) {
    SetError(error_message, "Too many realtime filter rules: " +
                                std::to_string(config.include_rules.size() +
                                               config.exclude_rules.size()));
    return false;
  }
  for (const std::string& rule : config.include_rules) {
    if (!IsBoundedString(rule)) {
      SetError(error_message, "Realtime include rule is not usable");
      return false;
    }
  }
  for (const std::string& rule : config.exclude_rules) {
    if (!IsBoundedString(rule)) {
      SetError(error_message, "Realtime exclude rule is not usable");
      return false;
    }
  }
  // 规则必须真的编译一遍。只查长度与 NUL 的话，
  // `realtime set --include nonsense:xx` 会保存成功、enable 成功，直到第一次
  // 真正触发才在引擎里失败——那正是"先存进去、运行时才炸"。
  // 语法裁决只有一处：Filter::AddRule（与 ValidateScheduleConfig
  // 同一个编译器）。
  Filter filter;
  if (!BuildRealtimeFilter(config, &filter, error_message)) return false;
  // 策略 / 算法组合只有一份答案来源。Realtime 不需要自己的第二张表。
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
  return true;
}

// enable 的门槛比 Save 高：Save 只保证“这份配置本身合法”，enable 还要保证
// “这台机器现在真的跑得起来”：源目录存在、不是符号链接、真的是目录，仓库
// 能被建起来且不是符号链接，两者路径不重叠。任何一项不过都不写配置文件，
// 也不会留下半个状态。
// 输出 repository_identity 是给 identity 摘要用的逻辑身份，与本地路径无关。
bool ValidateRealtimeForEnable(const RealtimeConfig& config,
                               const std::string& repository_path,
                               std::string* error_message,
                               std::string* repository_identity) {
  if (!ValidateRealtimeConfig(config, error_message)) return false;
  if (config.source_path.empty()) {
    SetError(error_message, "Realtime source directory must not be empty");
    return false;
  }
  bool is_directory = false;
  bool is_symlink = false;
  if (!LstatIsDirectory(config.source_path, &is_directory, &is_symlink,
                        error_message)) {
    return false;
  }
  if (is_symlink) {
    SetError(error_message, "Realtime source must not be a symbolic link: " +
                                config.source_path);
    return false;
  }
  if (!is_directory) {
    SetError(error_message,
             "Realtime source must be a directory: " + config.source_path);
    return false;
  }

  // 用产品的目录管理接口确保仓库布局存在：它会补齐缺失的目录并校验既有布局，
  // 所以“仓库目录还不存在”在这里是允许的。
  BackupCatalog catalog;
  std::string normalized_repository;
  if (!catalog.EnsureRepository(repository_path, error_message)) {
    return false;
  }
  if (!LstatIsDirectory(repository_path, &is_directory, &is_symlink,
                        error_message)) {
    return false;
  }
  if (is_symlink || !is_directory) {
    SetError(error_message,
             "Realtime repository must be a real directory (not a symlink): " +
                 repository_path);
    return false;
  }

  // 硬边界：源与仓库绝不重叠，从根上消灭"备份产生的 .bak 又触发自己"。
  std::string overlap_detail;
  const RealtimePathOverlap overlap =
      ClassifyPathOverlap(config.source_path, repository_path, &overlap_detail);
  switch (overlap) {
    case RealtimePathOverlap::kNone:
      break;
    case RealtimePathOverlap::kEqual:
      SetError(
          error_message,
          "Realtime source and repository must not be the same directory: " +
              overlap_detail);
      return false;
    case RealtimePathOverlap::kRepositoryInsideSource:
      SetError(error_message,
               "The realtime repository must not live inside the source "
               "directory: " +
                   overlap_detail);
      return false;
    case RealtimePathOverlap::kSourceInsideRepository:
      SetError(error_message,
               "The realtime source must not live inside the repository: " +
                   overlap_detail);
      return false;
    case RealtimePathOverlap::kUnknown:
      SetError(error_message,
               "Cannot compare the realtime source and repository paths");
      return false;
  }

  if (repository_identity != nullptr) {
    *repository_identity = RepositoryIdentity(repository_path);
  }
  return true;
}

// file_path_ 是对象的全部状态：不做内存缓存，每次 Load 都重新读盘，因此多个
// 实例/多个进程之间不会读到彼此的陈旧副本。
RealtimeStore::RealtimeStore(std::string file_path)
    : file_path_(std::move(file_path)) {}

namespace {

// 一次性读进整份文件，任何一步不过都返回 false：O_NOFOLLOW 拒绝符号链接
// （否则读的来源可能被换掉），fstat + S_ISREG 要求普通文件，长度要在上限内。
// 按 st_size 预分配后循环读，EINTR 重试；读到 EOF 但字节数不足，按“文件被
// 截断”处理，而不是把半份内容交给调用方——半份 JSON 的解析错误信息会误导人。
// close 失败同样算失败：它可能意味着数据其实没落到盘上。
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
  if (static_cast<std::uint64_t>(info.st_size) > kMaxRealtimeFileBytes) {
    ::close(fd);
    SetError(error_message, "Realtime config file is too large: " + path);
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
  if (::close(fd) != 0) {
    SetError(error_message, "Cannot close " + path + ": " + ErrnoText(errno));
    return false;
  }
  if (filled != text->size()) {
    SetError(error_message, "Realtime config is truncated: " + path);
    return false;
  }
  return true;
}

// 字段取值辅助：字段必须存在、类型必须完全正确，不做任何隐式转换（字符串 "1"
// 不是数字，0/1 不是 bool）。调用方前面已经用 RequireExactFields 确认过字段集
// 合，所以这里的“缺失”等同于文件被改坏，直接失败。
bool RealtimeRequireString(const JsonValue& object, const char* key,
                           const std::string& what, std::string* value,
                           std::string* error_message) {
  const JsonValue* field = object.Find(key);
  if (field == nullptr || !field->is_string()) {
    SetError(error_message, what + ": '" + key + "' must be a string");
    return false;
  }
  if (!IsBoundedString(field->text)) {
    SetError(error_message, what + ": '" + key + "' is not usable");
    return false;
  }
  *value = field->text;
  return true;
}

// bool 不接受 0/1 或 "true" 这类近似写法：配置文件由本程序写出，出现近似值
// 就说明它被手改过（或者来自不认识的版本）。
bool RealtimeRequireBool(const JsonValue& object, const char* key,
                         const std::string& what, bool* value,
                         std::string* error_message) {
  const JsonValue* field = object.Find(key);
  if (field == nullptr || !field->is_bool()) {
    SetError(error_message, what + ": '" + key + "' must be a boolean");
    return false;
  }
  *value = field->boolean;
  return true;
}

// 上界由调用方显式给出，避免同一字段的上限在两处各写一份；负值一律拒绝，
// 数字类型本身由共享 parser 保证是整数（它不接受小数点与指数）。
bool RealtimeRequireUint(const JsonValue& object, const char* key,
                         const std::string& what, std::uint32_t maximum,
                         std::uint32_t* value, std::string* error_message) {
  const JsonValue* field = object.Find(key);
  if (field == nullptr || !field->is_number()) {
    SetError(error_message, what + ": '" + key + "' must be an integer");
    return false;
  }
  if (field->number < 0 ||
      static_cast<std::uint64_t>(field->number) > maximum) {
    SetError(error_message, what + ": '" + key + "' is out of range");
    return false;
  }
  *value = static_cast<std::uint32_t>(field->number);
  return true;
}

// 规则数组：先清空再填，因为调用方可能在同一个 config 上重复 Load。
// 条目数上限在解析时就挡住，免得一个超大数组先被完整收进内存再被拒绝；
// 每条必须是长度受限、无 NUL 的字符串，元素类型不符即整体失败。
bool RealtimeRequireRuleList(const JsonValue& object, const char* key,
                             const std::string& what,
                             std::vector<std::string>* rules,
                             std::string* error_message) {
  const JsonValue* field = object.Find(key);
  if (field == nullptr || !field->is_array()) {
    SetError(error_message, what + ": '" + key + "' must be an array");
    return false;
  }
  rules->clear();
  if (field->array.size() > kMaxRealtimeRules) {
    SetError(error_message, what + ": '" + key + "' has too many entries");
    return false;
  }
  for (const JsonValue& item : field->array) {
    if (!item.is_string() || !IsBoundedString(item.text)) {
      SetError(error_message,
               what + ": '" + key + "' must contain only usable strings");
      return false;
    }
    rules->push_back(item.text);
  }
  return true;
}

// JSON 字符串体（不含两端的引号）。
//
// 这是 RealtimeStore 写侧**唯一**的转义实现：source_path 与两个规则数组都用它。
// 要求只有一条，但很硬：Save() 写出去的东西 Load() 必须读得回来。共享 parser
// （simple_json.cpp）明确拒绝 raw < 0x20，所以：
//
//   * `" \\ \b \f \n \r \t` 用 JSON 标准简写；
//   * 其余 C0（0x00..0x1F）用 parser 同样支持的 `\u00XX`；
//   * 非控制字节原样写出（项目字符串是 UTF-8 bytes，不做 Unicode 编码器）。
//
// 之前 include/exclude 规则只处理了 \\ 与 \" 与 \n，一个带 \t / \r / \b / \f
// 的合法规则会写出 parser 拒绝的 raw 控制字符 —— "写得出、读不回"。
void AppendJsonStringBody(std::string* out, const std::string& value) {
  static const char kHex[] = "0123456789abcdef";
  for (const unsigned char character : value) {
    switch (character) {
      case '\\':
        *out += "\\\\";
        continue;
      case '"':
        *out += "\\\"";
        continue;
      case '\b':
        *out += "\\b";
        continue;
      case '\f':
        *out += "\\f";
        continue;
      case '\n':
        *out += "\\n";
        continue;
      case '\r':
        *out += "\\r";
        continue;
      case '\t':
        *out += "\\t";
        continue;
      default:
        break;
    }
    if (character < 0x20) {
      *out += "\\u00";
      out->push_back(kHex[(character >> 4) & 0x0F]);
      out->push_back(kHex[character & 0x0F]);
      continue;
    }
    out->push_back(static_cast<char>(character));
  }
}

// 手写 JSON 的写侧小工具。AppendStringField 只写“缩进 + "key": "value"”，结尾
// 逗号留给调用方：这样在末尾追加字段时不用回头改上一行（手写 JSON 最容易
// 出错的地方就是逗号）。AppendRules 的 last 参数把“最后一行不加逗号”显式化。
void AppendStringField(std::string* out, const char* key,
                       const std::string& value) {
  *out += "    \"";
  *out += key;
  *out += "\": \"";
  AppendJsonStringBody(out, value);
  *out += "\"";
}

void AppendRules(std::string* out, const char* key,
                 const std::vector<std::string>& rules, bool last) {
  *out += "    \"";
  *out += key;
  *out += "\": [";
  for (std::size_t index = 0; index < rules.size(); ++index) {
    if (index > 0) *out += ", ";
    *out += "\"";
    AppendJsonStringBody(out, rules[index]);
    *out += "\"";
  }
  *out += "]";
  if (!last) *out += ",";
  *out += "\n";
}

}  // namespace

// 三态返回，调用方必须按语义分开处理：
//   kMissing  文件不存在——可以走默认配置；
//   kLoaded   文件存在、解析通过、且已过完 ValidateRealtimeConfig；
//   kError    文件存在但读不进来、解析失败或校验不过——绝不能当成默认值。
//
// 先用 lstat 探一次存在性，才能把 ENOENT 与其它错误分开（EACCES、ELOOP 都
// 是真错误）；文件随后仍按不可信输入打开，所以这里的存在性检查只是分类，
// 不构成“已确认安全”。
RealtimeLoadStatus RealtimeStore::Load(RealtimeConfig* config,
                                       std::string* error_message) const {
  if (config == nullptr) {
    SetError(error_message, "Realtime config output must not be null");
    return RealtimeLoadStatus::kError;
  }
  if (error_message != nullptr) error_message->clear();
  *config = RealtimeConfig{};

  struct stat info;
  if (::lstat(file_path_.c_str(), &info) != 0) {
    if (errno == ENOENT) return RealtimeLoadStatus::kMissing;
    SetError(error_message,
             "Cannot inspect " + file_path_ + ": " + ErrnoText(errno));
    return RealtimeLoadStatus::kError;
  }

  std::string text;
  if (!ReadWholeFile(file_path_, &text, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  JsonValue root;
  std::string json_error;
  if (!ParseJson(text, &root, &json_error)) {
    SetError(error_message, "Invalid realtime config: " + json_error);
    return RealtimeLoadStatus::kError;
  }
  // 字段集合必须精确匹配：少一个字段、多一个未知字段都拒绝。
  // 未知字段通常意味着文件来自更新的版本或被人手改过；静默忽略它，用户会以为
  // 那个字段生效了。
  if (!RequireExactFields(
          root,
          {"version", "enabled", "trigger", "source_path", "debounce_ms",
           "max_wait_ms", "retain_count", "strategy", "pack", "compression",
           "encryption", "include_rules", "exclude_rules"},
          "realtime config", error_message)) {
    return RealtimeLoadStatus::kError;
  }

  std::uint32_t number = 0;
  std::string text_value;
  const std::string what = "realtime config";
  if (!RealtimeRequireUint(root, "version", what, 1000, &number,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  config->version = number;
  if (!RealtimeRequireBool(root, "enabled", what, &config->enabled,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "trigger", what, &text_value,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParseBackupTriggerKey(text_value, &config->trigger) ||
      config->trigger != BackupTrigger::kRealtime) {
    SetError(error_message, "realtime config: unknown trigger '" + text_value +
                                "' (expected realtime)");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "source_path", what, &config->source_path,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireUint(root, "debounce_ms", what, 60000000u, &number,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  config->debounce_ms = number;
  if (!RealtimeRequireUint(root, "max_wait_ms", what, 60000000u, &number,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  config->max_wait_ms = number;
  if (!RealtimeRequireUint(root, "retain_count", what, 1000000u, &number,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  config->retain_count = number;
  if (!RealtimeRequireString(root, "strategy", what, &text_value,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParseBackupStrategyKey(text_value, &config->strategy)) {
    SetError(error_message,
             "realtime config: unknown strategy '" + text_value + "'");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "pack", what, &text_value, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParsePackMethodKey(text_value, &config->pack_method)) {
    SetError(error_message,
             "realtime config: unknown pack method '" + text_value + "'");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "compression", what, &text_value,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParseCompressionMethodKey(text_value, &config->compression_method)) {
    SetError(error_message, "realtime config: unknown compression method '" +
                                text_value + "'");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "encryption", what, &text_value,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParseEncryptionMethodKey(text_value, &config->encryption_method)) {
    SetError(error_message,
             "realtime config: unknown encryption method '" + text_value + "'");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireRuleList(root, "include_rules", what,
                               &config->include_rules, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireRuleList(root, "exclude_rules", what,
                               &config->exclude_rules, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  // 结构解析通过 ≠ 语义合法：文件可能被人改成一个“类型对、取值非法”的组合，
  // （Save 也调用它），因此“存进去的”和“读回来的”受同一套约束。
  if (!ValidateRealtimeConfig(*config, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  return RealtimeLoadStatus::kLoaded;
}

// 先校验再落盘：写出去的每一个字段都已经过 ValidateRealtimeConfig，所以磁盘上
// 不可能出现一份“看起来能读、跑起来才发现不行”的配置。
// 文本按固定顺序拼、4 空格缩进：它是人可读、可 diff 的文件，字段顺序属于可读性
// 契约，不要随意重排（字符串与规则数组共用同一套转义实现）。
bool RealtimeStore::Save(const RealtimeConfig& config,
                         std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (!ValidateRealtimeConfig(config, error_message)) return false;

  // 磁盘布局（realtime.json，UTF-8，一行一个字段，顺序固定）：
  //   version / enabled / trigger / source_path / debounce_ms / max_wait_ms
  //   / retain_count / strategy / pack / compression / encryption
  //   / include_rules / exclude_rules
  std::string out = "{\n";
  out += "    \"version\": " + std::to_string(config.version) + ",\n";
  out += std::string("    \"enabled\": ") +
         (config.enabled ? "true" : "false") + ",\n";
  out += "    \"trigger\": \"" + std::string(BackupTriggerKey(config.trigger)) +
         "\",\n";
  AppendStringField(&out, "source_path", config.source_path);
  out += ",\n";
  out += "    \"debounce_ms\": " + std::to_string(config.debounce_ms) + ",\n";
  out += "    \"max_wait_ms\": " + std::to_string(config.max_wait_ms) + ",\n";
  out += "    \"retain_count\": " + std::to_string(config.retain_count) + ",\n";
  out += "    \"strategy\": \"" +
         std::string(BackupStrategyKey(config.strategy)) + "\",\n";
  out += "    \"pack\": \"" + std::string(PackMethodKey(config.pack_method)) +
         "\",\n";
  out += "    \"compression\": \"" +
         std::string(CompressionMethodKey(config.compression_method)) + "\",\n";
  out += "    \"encryption\": \"" +
         std::string(EncryptionMethodKey(config.encryption_method)) + "\",\n";
  AppendRules(&out, "include_rules", config.include_rules, false);
  // exclude_rules 必须是最后一行：AppendRules 的 last=true 决定不写尾逗号。
  AppendRules(&out, "exclude_rules", config.exclude_rules, true);
  out += "}\n";

  // 发布走共享的原子替换写入（src/core/file_io.cpp）：唯一临时文件 +
  // fsync + rename + 父目录 fsync。以前这里自己写了一套（固定的 <file>.tmp、
  // 不 fsync 父目录），而 schedule_store 当时就警告过：两份各自演化的写法
  // 迟早会有一份漏掉某条边界。
  if (!WriteFileAtomicallyReplacing(file_path_, out, error_message)) {
    return false;
  }
  return true;
}

// job identity = “这套 realtime 配置在语义上还是不是同一件事”。
// 因此只收语义字段：源身份、仓库身份、过滤规则、策略与流水线；enabled 与
// debounce / max_wait / retain_count 这些“调参”不进摘要——否则改一下去抖就
// 会让历史和 retention 认不出这是同一个 job。
// canonical 串带 "BPREALTIMEJOB1" 版本前缀：摘要格式一改就换前缀，避免新旧
// 摘要被当成同一种东西；每行以 '\n' 结尾、字段名带 '='，拼接不会有歧义。
std::string RealtimeJobIdentityDigest(const RealtimeConfig& config,
                                      const std::string& repository_identity,
                                      const std::string& source_path) {
  std::string canonical = "BPREALTIMEJOB1\n";
  canonical += "trigger=realtime\n";
  canonical +=
      "source=" + SourceIdentityDigest(source_path, repository_identity) + "\n";
  canonical += "repository=" + repository_identity + "\n";
  canonical +=
      "filter=" +
      FilterIdentityDigest(config.include_rules, config.exclude_rules) + "\n";
  canonical +=
      "strategy=" + std::string(BackupStrategyKey(config.strategy)) + "\n";
  canonical +=
      "pipeline=" +
      StrategyIdentityDigest(config.pack_method, config.compression_method,
                             config.encryption_method) +
      "\n";
  canonical += "include=" + KeyOfRuleList(config.include_rules) + "\n";
  canonical += "exclude=" + KeyOfRuleList(config.exclude_rules) + "\n";
  return ContentDigestOfBytes(canonical);
}

}  // namespace backupproject

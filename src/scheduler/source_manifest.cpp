// source_manifest.cpp
// 源树“内容身份”快照的读写与差分。三条边界先说清楚：
//   * 不做备份、不写归档：只回答“这一轮的源树与上一轮比变了没有”；
//   * 不做增量存储：结论只有“要不要再建一份完整快照”；
//   * 不读用户文件内容——只有强化版（v3）为普通文件算 SHA-256。
//
// 数据流：ScanSourceTree（备份自己用的扫描器 + 同一个 Filter）-> ArchiveEntry
// -> ManifestFromScannedEntries -> ManifestEntry -> SerializeManifestV3 文本
// -> 调用方原子写盘；反向则是文本 -> ParseManifest -> ManifestEntry -> 与
// 当前扫描结果 DiffManifests -> ChangeSummary。
//
// 磁盘布局是行式文本、TAB 分隔，第一行是版本头：
//   BPMANIFEST3 <count>\t<snapshot>\t<repository>\t<source>\n
//   <12 个 v2 字段>\t<转义后的 content_digest>\n   （每个条目一行）
// 转义只覆盖反斜杠与 \t \n \r：Linux 路径里这四种字节都合法，不转义就会
// 把一个字段切成两半，或者让“反斜杠加 t”与 TAB 产生二义性。
//
// 不变量：条目数与头行声明一致、archive_path 唯一且是相对路径、字段数固定、
// 所有数字都做过范围检查、binding 要么完整合法要么整份 manifest 不写不认。
//
// 失败边界：全部返回 bool + error_message，不抛异常；解析是 fail-closed 的，
// 任何一处不规范都判整份文件损坏。容错地猜出一个半可信的基线，会让增量链上
// 所有后代一起继承这个错误——宁可多建一份完整快照，绝不错误跳过。

#include "source_manifest.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "source_digest.h"
#include "tree_scanner.h"

namespace backupproject {
namespace {

// error_message 是可选出参：调用方不关心文本时传 nullptr。所有失败路径都只
// 经过这里写一次文本，因此“报错文案”与“返回 false”不会脱节。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// ---- 数字解析：不抛异常，逐位检查溢出 ----
//
// 刻意不用 std::stoull：它会抛 std::out_of_range，"解析失败"与"内部错误"
// 就分不开了。这里全部返回 bool。

// 语法是唯一的规范形式：[0] | [1-9][0-9]*，且不超过 20 位（UINT64_MAX 的
// 位数）。长度上限先挡一次，逐位累加时再用 (UINT64_MAX - digit) / 10 判溢出。
bool ParseUnsigned(const std::string& text, std::uint64_t* value) {
  if (text.empty() || text.size() > 20) return false;
  // 拒绝前导零：manifest 由本模块自己写，规范形式就是唯一的合法形式。
  if (text.size() > 1 && text[0] == '0') return false;
  std::uint64_t result = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (UINT64_MAX - digit) / 10u) return false;
    result = result * 10u + digit;
  }
  *value = result;
  return true;
}

// 与 ParseUnsigned 共用同一套数字语法，只是多一个可选的负号。
// INT64_MIN 的绝对值比 INT64_MAX 大 1，无法先转成正数再取负，所以负数分支
// 单独用 kNegativeLimit 判断并特判 INT64_MIN。
// 已知的宽松点："-0" 会被解析成 0，也就是接受了一个非规范写法（值相同，
// 因此不影响正确性，但打破了“只认规范形式”的严格性）。
bool ParseSigned(const std::string& text, std::int64_t* value) {
  if (text.empty()) return false;
  bool negative = false;
  std::string digits = text;
  if (digits[0] == '-') {
    negative = true;
    digits.erase(0, 1);
  }
  std::uint64_t magnitude = 0;
  if (!ParseUnsigned(digits, &magnitude)) return false;
  constexpr std::uint64_t kPositiveLimit = 9223372036854775807ull;
  constexpr std::uint64_t kNegativeLimit = 9223372036854775808ull;
  if (negative) {
    if (magnitude > kNegativeLimit) return false;
    if (magnitude == kNegativeLimit) {
      *value = INT64_MIN;
    } else {
      *value = -static_cast<std::int64_t>(magnitude);
    }
    return true;
  }
  if (magnitude > kPositiveLimit) return false;
  *value = static_cast<std::int64_t>(magnitude);
  return true;
}

// ---- 字符串字段转义 ----
//
// 只转义反斜杠、TAB、换行、回车：路径里出现 TAB 或换行在 Linux 上是合法的，
// 不转义就会把一个字段切成两半；不转义反斜杠则会产生二义性（"\t" 到底是
// 一个 TAB 还是一个反斜杠加 t）。

// 转义后保证：结果里不会再出现 TAB、LF、CR，因此“一行一条记录、一个字段
// 一段”这个结构不会被字段内容破坏；其它字节（含 UTF-8 多字节序列）原样透传，
// 不做任何编码转换——manifest 是字节透明的，不替用户的文件名做规范化。
std::string EscapeField(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\t':
        escaped += "\\t";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      default:
        escaped.push_back(character);
        break;
    }
  }
  return escaped;
}

// 严格反向：未知的转义序列（例如 \x）直接判失败，而不是原样保留。
// 接受未知序列会让同一份文本有多种解码结果，破坏“规范形式唯一”这条前提，
// 而唯一性正是 DiffManifests 与 ManifestDigest 可复现的基础。
// 失败时 *value 里可能有部分内容，调用方必须在返回 true 之后才使用它。
bool UnescapeField(const std::string& text, std::string* value) {
  value->clear();
  value->reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (character != '\\') {
      value->push_back(character);
      continue;
    }
    if (index + 1 >= text.size()) return false;
    const char escaped = text[++index];
    switch (escaped) {
      case '\\':
        value->push_back('\\');
        break;
      case 't':
        value->push_back('\t');
        break;
      case 'n':
        value->push_back('\n');
        break;
      case 'r':
        value->push_back('\r');
        break;
      default:
        return false;
    }
  }
  return true;
}

// 按 TAB 切分。字段数上界用来让病态输入（一行里上百万个 TAB）也能被拒绝，
// 但检查点在循环里，最后一次 push 不再判，所以实际放行的是最多 65 个字段。
// 这一点无害：调用方要求字段数恰好 12 或 13，多出来的那一个必然被判错。
bool SplitFields(const std::string& line, std::vector<std::string>* fields) {
  fields->clear();
  std::size_t start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields->push_back(line.substr(start));
      return true;
    }
    fields->push_back(line.substr(start, tab - start));
    start = tab + 1;
    if (fields->size() > 64) return false;  // 字段数上界，防病态输入
  }
}

// archive_path 的语法检查。这里是**不可信输入**的入口：文本可能来自磁盘上
// 被改过的 manifest。拒绝绝对路径、NUL、"./" 前缀与结尾 '/'，是为了让同一个
// 逻辑路径只有一种写法——它同时是去重键和 diff 的排序键，多种写法会让这两
// 件事都出错。注意这里只做结构检查：archive_path 由扫描器产生，
// 因此“不含 .. 组件”这条更强的不变量并没有在这里被验证。
bool IsValidArchivePath(const std::string& path) {
  if (path.empty()) return false;
  if (path[0] == '/') return false;
  if (path.find('\0') != std::string::npos) return false;
  if (path.size() >= 2 && path.compare(0, 2, "./") == 0) return false;
  if (path.back() == '/') return false;
  return true;
}

// 排序与比较统一走这一个谓词：std::string 的 < 是逐字节的（无符号 memcmp
// 语义），因此结果与 locale、文件系统返回顺序都无关——摘要的可复现性依赖它。
bool LessByArchivePath(const ManifestEntry& left, const ManifestEntry& right) {
  return left.archive_path < right.archive_path;
}

// binding 里那份快照名字的**结构**检查：必须是一个单组件文件名——不带路径
// 分隔符、不是 "." / ".."、不含 NUL。".bak 后缀"这条更具体的归档命名规则属于
// BackupCatalog / ScheduleStore 那一层，由 ScheduleStore::LoadManifest 再校验
// 一次；两层各守自己的规则，谁也不替谁放宽。
// 反斜杠也一并拒绝：它在 Linux 上不是分隔符，但 manifest 可能被拿到别的
// 平台上处理，这里不做平台猜测，直接判不合法。
bool IsPlainSingleComponentName(const std::string& name) {
  if (name.empty()) return false;
  if (name == "." || name == "..") return false;
  if (name.find('/') != std::string::npos) return false;
  if (name.find('\\') != std::string::npos) return false;
  if (name.find('\0') != std::string::npos) return false;
  return true;
}

// binding 的完整校验：三个字段都非空、都不含 NUL、长度各自有上界，快照名
// 还必须是单组件文件名。这些条件成立时，调用方才能把 snapshot_file_name
// 安全地 join 到仓库目录去检查那份快照是否真的存在。
// 校验顺序固定、错误文案固定：它们有测试按文案断言。上界与 schedule.json
// 的字符串上界同量级，避免一行头就把内存放大。
bool IsValidManifestBinding(const ManifestBinding& binding,
                            std::string* error_message) {
  if (binding.snapshot_file_name.empty()) {
    SetError(error_message,
             "Invalid source manifest: the baseline snapshot file name is "
             "empty");
    return false;
  }
  if (binding.repository_identity.empty()) {
    SetError(error_message,
             "Invalid source manifest: the baseline repository identity is "
             "empty");
    return false;
  }
  if (binding.source_path.empty()) {
    SetError(error_message,
             "Invalid source manifest: the baseline source path is empty");
    return false;
  }
  if (binding.snapshot_file_name.size() > kMaxManifestBindingBytes ||
      binding.repository_identity.size() > kMaxManifestBindingBytes ||
      binding.source_path.size() > kMaxManifestBindingBytes) {
    SetError(error_message,
             "Invalid source manifest: a baseline binding field is too long");
    return false;
  }
  if (binding.repository_identity.find('\0') != std::string::npos ||
      binding.source_path.find('\0') != std::string::npos) {
    SetError(error_message,
             "Invalid source manifest: a baseline binding field contains a NUL "
             "byte");
    return false;
  }
  if (!IsPlainSingleComponentName(binding.snapshot_file_name)) {
    SetError(error_message,
             "Invalid source manifest: the baseline snapshot file name is not "
             "a single path component");
    return false;
  }
  return true;
}

// 条目正文。v1 与 v2 的正文格式完全一样，只有头行不同。
// v1/v2 的条目行：12 个 TAB 分隔字段，顺序就是磁盘布局，不能重排——旧版本按
// 固定字段序号读取。新增字段只能追加到末尾并提升版本号（v3 就是这么做的）。
// 数字字段一律十进制无前导零、无符号，负数只可能出现在 mtime_sec。
void AppendManifestEntries(const std::vector<ManifestEntry>& entries,
                           std::string* out) {
  for (const ManifestEntry& entry : entries) {
    *out += std::to_string(static_cast<unsigned>(entry.type));
    *out += '\t';
    *out += std::to_string(entry.size);
    *out += '\t';
    *out += std::to_string(entry.mtime_sec);
    *out += '\t';
    *out += std::to_string(entry.mtime_nsec);
    *out += '\t';
    *out += std::to_string(entry.mode);
    *out += '\t';
    *out += std::to_string(entry.uid);
    *out += '\t';
    *out += std::to_string(entry.gid);
    *out += '\t';
    *out += std::to_string(entry.dev_major);
    *out += '\t';
    *out += std::to_string(entry.dev_minor);
    *out += '\t';
    *out += std::to_string(entry.hardlink_degree);
    *out += '\t';
    *out += EscapeField(entry.archive_path);
    *out += '\t';
    *out += EscapeField(entry.link_target);
    *out += '\n';
  }
}

// version 3 的条目行：v2 的 12 个字段之后追加内容摘要（第 13 个字段）。
// 摘要本身是十六进制，转义只是让"字段"这个概念保持统一。
// v3 的条目行 = v2 的 12 个字段 + 第 13 个字段 content_digest（64 个小写
// 十六进制字符）。前面的字段顺序与含义与 v2 逐字节相同，因此 v2 的读取器
// 至少能读出前 12 个字段。
void AppendManifestEntriesV3(const std::vector<ManifestEntry>& entries,
                             std::string* out) {
  for (const ManifestEntry& entry : entries) {
    *out += std::to_string(static_cast<unsigned>(entry.type));
    *out += '\t';
    *out += std::to_string(entry.size);
    *out += '\t';
    *out += std::to_string(entry.mtime_sec);
    *out += '\t';
    *out += std::to_string(entry.mtime_nsec);
    *out += '\t';
    *out += std::to_string(entry.mode);
    *out += '\t';
    *out += std::to_string(entry.uid);
    *out += '\t';
    *out += std::to_string(entry.gid);
    *out += '\t';
    *out += std::to_string(entry.dev_major);
    *out += '\t';
    *out += std::to_string(entry.dev_minor);
    *out += '\t';
    *out += std::to_string(entry.hardlink_degree);
    *out += '\t';
    *out += EscapeField(entry.archive_path);
    *out += '\t';
    *out += EscapeField(entry.link_target);
    *out += '\t';
    *out += EscapeField(entry.content_digest);
    *out += '\n';
  }
}

// 把一次 ScanSourceTree 的结果映射成 manifest 条目。
// source_path 一并带上：强化版要拿它去读正文算摘要。
// 把扫描结果映射成 manifest 条目，字段一一对应地搬运。
// hardlink_degree 需要先统计：它是“有多少条条目把本路径当作 leader”
// （link_target == 本路径的条目数），必须扫完一遍才知道，所以分两趟。
// source_path 一并带上，但**不参与序列化**：它只用来读正文算摘要，写进
// manifest 等于把源目录的绝对路径留在磁盘上。
void ManifestFromScannedEntries(const std::vector<ArchiveEntry>& scanned,
                                std::vector<ManifestEntry>* entries) {
  std::unordered_map<std::string, std::uint32_t> hardlink_degree;
  for (const ArchiveEntry& entry : scanned) {
    if (entry.type != EntryType::kHardLink) continue;
    ++hardlink_degree[entry.link_target];
  }

  entries->clear();
  entries->reserve(scanned.size());
  for (const ArchiveEntry& scanned_entry : scanned) {
    ManifestEntry entry;
    entry.archive_path = scanned_entry.archive_path;
    entry.source_path = scanned_entry.source_path;
    entry.type = scanned_entry.type;
    entry.size = scanned_entry.size;
    entry.mtime_sec = scanned_entry.mtime_sec;
    entry.mtime_nsec = scanned_entry.mtime_nsec;
    entry.mode = scanned_entry.mode;
    entry.uid = scanned_entry.uid;
    entry.gid = scanned_entry.gid;
    entry.link_target = scanned_entry.link_target;
    entry.dev_major = scanned_entry.dev_major;
    entry.dev_minor = scanned_entry.dev_minor;
    const auto found = hardlink_degree.find(entry.archive_path);
    entry.hardlink_degree = found == hardlink_degree.end() ? 0u : found->second;
    entries->push_back(std::move(entry));
  }
}

}  // namespace

// 四个桶互斥，所以总数可以直接相加；调用方用它判断“这一轮到底有没有变化”。
std::uint64_t ChangeSummaryTotal(const ChangeSummary& summary) {
  return summary.added + summary.removed + summary.modified +
         summary.metadata_changed;
}

// 元数据版：只 lstat、不读正文，因此便宜但存在已知盲区（same-size +
// same-mtime 的 in-place rewrite 看不出来）。
// 复用备份自己的扫描器：集合、类型判定、filter 剪枝、socket 规则都只有一份，
// “manifest 看到的集合”与“备份实际写入的集合”因此不可能漂移。
// 失败时 *entries 保持为空：清空在入口做，填充只在扫描与条数检查都通过之后。
bool BuildSourceManifest(const std::string& source_directory,
                         const Filter* filter,
                         std::vector<ManifestEntry>* entries,
                         std::string* error_message) {
  if (entries == nullptr) {
    SetError(error_message, "Manifest output must not be null");
    return false;
  }
  entries->clear();

  std::vector<ArchiveEntry> scanned;
  // 复用备份自己的扫描器：集合、类型判定、filter 剪枝、socket 规则都只有一份。
  if (!ScanSourceTree(source_directory, filter, &scanned, error_message)) {
    return false;
  }
  if (scanned.size() > kMaxManifestEntries) {
    SetError(error_message, "Source manifest is too large: " +
                                std::to_string(scanned.size()) + " entries");
    return false;
  }

  ManifestFromScannedEntries(scanned, entries);
  return true;
}

// 强化版：集合与元数据版完全一致，额外为每个普通文件读一遍正文算 SHA-256，
// 为每个软链接算目标字节的 SHA-256。代价是 O(源体积) 的读盘。
// 第一版刻意全量哈希，不做 size+mtime 缓存：缓存的失效判断本身就是
// correctness 问题，而这里漏掉一次变化会被增量链的所有后代继承。
bool BuildStrongSourceManifest(const std::string& source_directory,
                               const Filter* filter,
                               std::vector<ManifestEntry>* entries,
                               std::string* error_message) {
  if (entries == nullptr) {
    SetError(error_message, "Manifest output must not be null");
    return false;
  }
  entries->clear();

  std::vector<ArchiveEntry> scanned;
  if (!ScanSourceTree(source_directory, filter, &scanned, error_message)) {
    return false;
  }
  if (scanned.size() > kMaxManifestEntries) {
    SetError(error_message, "Source manifest is too large: " +
                                std::to_string(scanned.size()) + " entries");
    return false;
  }
  ManifestFromScannedEntries(scanned, entries);

  // 全量哈希。第一版不做任何 size+mtime 摘要缓存：缓存的失效判断本身就是
  // correctness 问题，而"漏掉一次变化"在增量链上是会被后代继承的错误。
  // 只有普通文件与软链接需要内容身份：目录、FIFO、设备、socket 的身份由类型
  // 加元数据字段唯一确定，再算一遍摘要只是重复。
  // 摘要失败（读不了、不是普通文件、被换成符号链接）一律让整次构建失败，
  // 绝不用空摘要冒充成功。
  for (ManifestEntry& entry : *entries) {
    if (entry.type == EntryType::kRegularFile) {
      std::string digest;
      if (!ContentDigestOfFile(entry.source_path, &digest, error_message)) {
        return false;
      }
      entry.content_digest = std::move(digest);
    } else if (entry.type == EntryType::kSymlink) {
      // 软链接没有"正文"，它的内容就是目标字符串的字节。
      entry.content_digest = ContentDigestOfBytes(entry.link_target);
    }
  }

  // 读正文期间源不许变。变了就整次失败：否则 manifest 会把"读到的内容"与
  // "扫描时记下的元数据"拼成一个从未真实存在过的版本，而增量链会把它当成
  // 一个可信的祖先。
  // 复查用 lstat 而不是 stat：哈希之后路径被换成符号链接也必须被发现，
  // 而 S_ISREG 正是在这一步挡掉它的。比较 size 与 mtime（秒 + 纳秒）三项，
  // 任何一项不符都说明“读到的内容”与“记录的元数据”不是同一个瞬间的状态。
  for (const ManifestEntry& entry : *entries) {
    if (entry.type != EntryType::kRegularFile) continue;
    struct stat info;
    if (::lstat(entry.source_path.c_str(), &info) != 0) {
      SetError(error_message,
               "Source entry disappeared while hashing: " + entry.source_path);
      return false;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
    const std::int64_t mtime_sec =
        static_cast<std::int64_t>(info.st_mtim.tv_sec);
    const std::uint32_t mtime_nsec =
        static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
    if (!S_ISREG(info.st_mode) || size != entry.size ||
        mtime_sec != entry.mtime_sec || mtime_nsec != entry.mtime_nsec) {
      SetError(error_message,
               "Source changed while hashing: " + entry.source_path);
      return false;
    }
  }
  return true;
}

// 这是“能不能当增量基线”的唯一判据：普通文件与软链接都必须有合法摘要。
// 空 manifest 会 vacuous 地返回 true，所以调用方还要自己处理“一条都没有”
// 的情形，不能只看这一个布尔值。
bool HasContentDigests(const std::vector<ManifestEntry>& entries) {
  for (const ManifestEntry& entry : entries) {
    if (entry.type != EntryType::kRegularFile &&
        entry.type != EntryType::kSymlink) {
      continue;
    }
    if (!IsContentDigest(entry.content_digest)) return false;
  }
  return true;
}

// 两份 manifest 的变化摘要。分类规则互斥且有序：只在一侧、类型变了、
// 内容变了、元数据变了，命中即停，因此总数等于各桶之和，不会重复计数。
// 目录的 mtime 刻意不参与比较（见头文件里那条已知盲区），hardlink 条目也
// 只比 link_target，理由同样是“别重复计数”。
// 失败只可能来自空指针参数；正常路径恒返回 true 且 *summary 已清零。
bool DiffManifests(const std::vector<ManifestEntry>& previous,
                   const std::vector<ManifestEntry>& current,
                   ChangeSummary* summary,
                   std::vector<std::string>* changed_paths,
                   std::string* error_message) {
  if (summary == nullptr) {
    SetError(error_message, "Change summary output must not be null");
    return false;
  }
  *summary = ChangeSummary{};
  if (changed_paths != nullptr) changed_paths->clear();

  // 先按 archive_path 排序再做归并式单遍扫描，输入顺序因此不影响结果——
  // manifest 里的顺序取决于目录遍历顺序，那不是契约。
  // 存指针而不是复制 ManifestEntry：每条带两个 std::string，复制几十万条
  // 既费内存又费时间，而这里的比较是只读的。
  std::vector<const ManifestEntry*> left;
  std::vector<const ManifestEntry*> right;
  left.reserve(previous.size());
  right.reserve(current.size());
  for (const ManifestEntry& entry : previous) left.push_back(&entry);
  for (const ManifestEntry& entry : current) right.push_back(&entry);
  const auto less = [](const ManifestEntry* a, const ManifestEntry* b) {
    return LessByArchivePath(*a, *b);
  };
  std::sort(left.begin(), left.end(), less);
  std::sort(right.begin(), right.end(), less);

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.size() || j < right.size()) {
    if (j >= right.size() ||
        (i < left.size() && left[i]->archive_path < right[j]->archive_path)) {
      ++summary->removed;
      if (changed_paths != nullptr)
        changed_paths->push_back(left[i]->archive_path);
      ++i;
      continue;
    }
    if (i >= left.size() || right[j]->archive_path < left[i]->archive_path) {
      ++summary->added;
      if (changed_paths != nullptr)
        changed_paths->push_back(right[j]->archive_path);
      ++j;
      continue;
    }

    const ManifestEntry& old_entry = *left[i];
    const ManifestEntry& new_entry = *right[j];
    // 同一个路径两侧都有，按**新**条目的类型分派比较：不同类型的“同一性”由
    // 不同字段决定——普通文件看 size + mtime（有摘要时再看摘要），链接看
    // target， 设备看 major/minor，目录与 FIFO
    // 没有可比字段（只可能落到元数据桶）。 is_modified 与 is_metadata_changed
    // 是互斥的：前者为真时不再判后者。
    bool is_modified = false;
    bool is_metadata_changed = false;

    if (old_entry.type != new_entry.type) {
      is_modified = true;
    } else {
      switch (new_entry.type) {
        case EntryType::kRegularFile:
          if (old_entry.size != new_entry.size ||
              old_entry.mtime_sec != new_entry.mtime_sec ||
              old_entry.mtime_nsec != new_entry.mtime_nsec) {
            is_modified = true;
          } else if (!old_entry.content_digest.empty() ||
                     !new_entry.content_digest.empty()) {
            // 元数据一致时，内容身份才说话：same-size + same-mtime 的人为
            // in-place rewrite 只有摘要看得出来。两边都有摘要时比摘要；
            // 只有一边有（拿 v3 与 v2 比，例如刚升级完）时无法证明相等，
            // 按变化处理——宁可多重一份完整快照，绝不错误跳过。
            if (old_entry.content_digest != new_entry.content_digest) {
              is_modified = true;
            }
          }
          break;
        case EntryType::kSymlink:
        case EntryType::kHardLink:
          if (old_entry.link_target != new_entry.link_target)
            is_modified = true;
          break;
        case EntryType::kCharDevice:
        case EntryType::kBlockDevice:
          if (old_entry.dev_major != new_entry.dev_major ||
              old_entry.dev_minor != new_entry.dev_minor) {
            is_modified = true;
          }
          break;
        case EntryType::kDirectory:
        case EntryType::kFifo:
        case EntryType::kSocket:
          break;
      }

      if (!is_modified) {
        if (new_entry.type == EntryType::kHardLink) {
          // hardlink 条目的内容身份只有 link_target；inode 上的其它字段由
          // leader 那条记录负责，这里再看一遍就是重复计数。
          is_metadata_changed = false;
        } else if (old_entry.mode != new_entry.mode ||
                   old_entry.uid != new_entry.uid ||
                   old_entry.gid != new_entry.gid) {
          is_metadata_changed = true;
        } else if (new_entry.type != EntryType::kRegularFile &&
                   new_entry.type != EntryType::kDirectory &&
                   (old_entry.mtime_sec != new_entry.mtime_sec ||
                    old_entry.mtime_nsec != new_entry.mtime_nsec)) {
          // FIFO / 软链接 / 设备的 mtime 变化统一算 metadata_changed。
          // 目录刻意不在此列：子项的任何增删都会顺带改掉父目录的 mtime，
          // 把它算进来会让"新增一个被 filter 排除的文件"也触发一次完整快照，
          // 而实际备份集合并没有变。代价是单独 touch 目录看不出来——已知盲区。
          is_metadata_changed = true;
          // leader 自己的元数据一字未变、只是多/少了一个指向同一 inode
          // 的硬链接时， 只有这个计数会动；它因此是 hardlink
          // 身份里唯一需要单独比较的字段。
        } else if (old_entry.hardlink_degree != new_entry.hardlink_degree) {
          is_metadata_changed = true;
        }
      }
    }

    // 计数与路径是同一个判定的两个视图：只要落进某个桶就一定会 push 路径，
    // 调用方不会看到“计数 3 条、路径只有 2 条”这种自相矛盾的结果。
    if (is_modified) {
      ++summary->modified;
      if (changed_paths != nullptr)
        changed_paths->push_back(new_entry.archive_path);
    } else if (is_metadata_changed) {
      ++summary->metadata_changed;
      if (changed_paths != nullptr)
        changed_paths->push_back(new_entry.archive_path);
    }
    ++i;
    ++j;
  }
  return true;
}

// 返回空串表示“拒绝序列化”，不是“内容为空”（空 manifest 至少有一行头）。
// 调用方必须把空串当错误处理，绝不能把空串写进磁盘冒充一份 manifest。
std::string SerializeManifest(const std::vector<ManifestEntry>& entries,
                              const ManifestBinding& binding) {
  // 归属不完整就什么都不写。调用方必须在写盘前拿到一个明确的失败，
  // 而不是一份"看起来正常、其实不知道属于谁"的 manifest。
  std::string binding_error;
  if (!IsValidManifestBinding(binding, &binding_error)) return std::string();

  std::string out;
  out += "BPMANIFEST2 ";
  out += std::to_string(entries.size());
  out += '\t';
  out += EscapeField(binding.snapshot_file_name);
  out += '\t';
  out += EscapeField(binding.repository_identity);
  out += '\t';
  out += EscapeField(binding.source_path);
  out += '\n';
  AppendManifestEntries(entries, &out);
  return out;
}

std::string SerializeManifestV3(const std::vector<ManifestEntry>& entries,
                                const ManifestBinding& binding) {
  std::string binding_error;
  if (!IsValidManifestBinding(binding, &binding_error)) return std::string();

  // 自称 v3 就必须带齐内容身份：普通文件与软链接缺摘要时什么都不写。
  // 写出半份 v3 等于给了增量链一个看起来可用、实际无法校验的基线。
  for (const ManifestEntry& entry : entries) {
    if (entry.type != EntryType::kRegularFile &&
        entry.type != EntryType::kSymlink) {
      continue;
    }
    if (!IsContentDigest(entry.content_digest)) return std::string();
  }

  std::string out;
  out += "BPMANIFEST3 ";
  out += std::to_string(entries.size());
  out += '\t';
  out += EscapeField(binding.snapshot_file_name);
  out += '\t';
  out += EscapeField(binding.repository_identity);
  out += '\t';
  out += EscapeField(binding.source_path);
  out += '\n';
  AppendManifestEntriesV3(entries, &out);
  return out;
}

// manifest 自身的摘要，覆盖“规范化的 v3 正文”：条目先按 archive_path 排序再
// 序列化，同一个源状态无论遍历细节如何，摘要都可复现。
// 它刻意不含 binding：binding 说的是“这份 manifest 属于哪一份快照”，
// 不是源的内容身份。摘要本身不再进 manifest，只用于比对与日志。
std::string ManifestDigest(const std::vector<ManifestEntry>& entries) {
  // 先按 archive_path 排序：摘要必须只取决于"源是什么样"，不取决于遍历
  // 恰好以什么顺序产出条目。排序后的顺序就是规范顺序。
  std::vector<const ManifestEntry*> ordered;
  ordered.reserve(entries.size());
  for (const ManifestEntry& entry : entries) ordered.push_back(&entry);
  std::sort(ordered.begin(), ordered.end(),
            [](const ManifestEntry* left, const ManifestEntry* right) {
              return LessByArchivePath(*left, *right);
            });
  std::vector<ManifestEntry> canonical;
  canonical.reserve(ordered.size());
  for (const ManifestEntry* entry : ordered) canonical.push_back(*entry);

  std::string text;
  text += "BPMANIFEST3 ";
  text += std::to_string(canonical.size());
  text += '\n';
  AppendManifestEntriesV3(canonical, &text);
  return ContentDigestOfBytes(text);
}

// v1 写出只服务于兼容性与迁移测试：头行没有 binding，读回来必然是不可信
// 基线。生产路径一律用带 binding 的 SerializeManifest / SerializeManifestV3。
std::string SerializeManifestV1(const std::vector<ManifestEntry>& entries) {
  std::string out;
  out += "BPMANIFEST1 ";
  out += std::to_string(entries.size());
  out += '\n';
  AppendManifestEntries(entries, &out);
  return out;
}

// 严格解析器：头必须完全匹配、字段数固定、数字全部做范围检查、条数必须与
// 正文一致、末尾不许有多余字节。任何偏差都判整份文件损坏，不“尽力猜”。
// 失败时 *entries 与 *binding 都保持清空/空值，调用方据此安全地走重建流程。
bool ParseManifest(const std::string& text, std::vector<ManifestEntry>* entries,
                   ManifestBinding* binding, std::string* error_message) {
  if (entries == nullptr) {
    SetError(error_message, "Manifest output must not be null");
    return false;
  }
  if (binding == nullptr) {
    SetError(error_message, "Manifest binding output must not be null");
    return false;
  }
  entries->clear();
  *binding = ManifestBinding{};

  // 先卡总字节数再解析：整份文本在内存里被反复扫描，没有上限就等于让一个坏
  // 文件决定进程的内存占用。条目数与单行长度在上限之外还有各自的上界。
  if (text.size() > kMaxManifestBytes) {
    SetError(error_message, "Source manifest is too large: " +
                                std::to_string(text.size()) + " bytes");
    return false;
  }

  // 两个版本头都要认。v1 只是"读得出来"——它的 binding 会留空，调用方据此
  // 判定这是不可信基线，走重建。升级语义因此是单向安全的。
  const std::string header_v3 = "BPMANIFEST3 ";
  const std::string header_v2 = "BPMANIFEST2 ";
  const std::string header_v1 = "BPMANIFEST1 ";
  // is_version_2 的含义是"头行带 binding"，v3 同样带，只是条目多一个字段。
  bool is_version_2 = false;
  bool is_version_3 = false;
  std::size_t header_size = 0;
  // compare(0, min(header.size(), text.size()), ...) 让短文本（甚至空串）也走
  // 同一条比较路径，不需要先判长度、也不会抛异常。
  // is_version_2 的含义是“头行带 binding”，v3 同样带，所以 v3 会把两个标志
  // 都置真；条目字段数才用 is_version_3 单独区分。
  if (text.compare(0, std::min(header_v3.size(), text.size()), header_v3) ==
      0) {
    is_version_3 = true;
    is_version_2 = true;
    header_size = header_v3.size();
  } else if (text.compare(0, std::min(header_v2.size(), text.size()),
                          header_v2) == 0) {
    is_version_2 = true;
    header_size = header_v2.size();
  } else if (text.compare(0, std::min(header_v1.size(), text.size()),
                          header_v1) == 0) {
    header_size = header_v1.size();
  } else {
    SetError(error_message,
             "Invalid source manifest: missing or wrong version header");
    return false;
  }
  const std::size_t first_newline = text.find('\n');
  if (first_newline == std::string::npos) {
    SetError(error_message, "Invalid source manifest: truncated header");
    return false;
  }
  const std::string header_rest =
      text.substr(header_size, first_newline - header_size);
  if (header_rest.size() > kMaxManifestLineBytes) {
    SetError(error_message,
             "Invalid source manifest: the header line is too long");
    return false;
  }

  std::uint64_t declared_count = 0;
  if (is_version_2) {
    // 头行结构固定：<count>\t<snapshot>\t<repository>\t<source>，正好四个
    // 字段。少一个、多一个、多一个 TAB 都算坏文件——manifest 是机器写的，
    // 不规范的字节只能说明状态坏了。
    std::vector<std::string> header_fields;
    if (!SplitFields(header_rest, &header_fields) ||
        header_fields.size() != 4) {
      SetError(error_message,
               "Invalid source manifest: a version 2 header must have exactly "
               "four fields");
      return false;
    }
    if (!ParseUnsigned(header_fields[0], &declared_count) ||
        declared_count > kMaxManifestEntries) {
      SetError(error_message, "Invalid source manifest: bad entry count '" +
                                  header_fields[0] + "'");
      return false;
    }
    ManifestBinding parsed_binding;
    if (!UnescapeField(header_fields[1], &parsed_binding.snapshot_file_name) ||
        !UnescapeField(header_fields[2], &parsed_binding.repository_identity) ||
        !UnescapeField(header_fields[3], &parsed_binding.source_path)) {
      SetError(error_message,
               "Invalid source manifest: bad baseline binding escape");
      return false;
    }
    if (!IsValidManifestBinding(parsed_binding, error_message)) return false;
    *binding = std::move(parsed_binding);
  } else {
    if (!ParseUnsigned(header_rest, &declared_count) ||
        declared_count > kMaxManifestEntries) {
      SetError(error_message, "Invalid source manifest: bad entry count '" +
                                  header_rest + "'");
      return false;
    }
  }

  // 循环次数以头行声明的条数为准，而不是“读到没有行”：声明与正文必须一致，
  // 多一条少一条都算坏文件。declared_count 已在头行解析时被卡到
  // kMaxManifestEntries 以内，所以这里的 reserve 不会被一行坏数字放大。
  entries->reserve(static_cast<std::size_t>(declared_count));
  // 重复路径必须在这里挡住：它会让 diff 把同一条路径算两次，也让“按路径查找”
  // 的调用方产生歧义。去重用的是反转义后的 archive_path，即逻辑路径。
  std::unordered_set<std::string> seen_paths;
  std::size_t position = first_newline + 1;
  for (std::uint64_t index = 0; index < declared_count; ++index) {
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string::npos) {
      SetError(error_message,
               "Invalid source manifest: fewer entries than the header "
               "declares");
      return false;
    }
    const std::string line = text.substr(position, newline - position);
    position = newline + 1;
    if (line.size() > kMaxManifestLineBytes) {
      SetError(error_message,
               "Invalid source manifest: entry line is too long");
      return false;
    }

    std::vector<std::string> fields;
    // 字段数由版本决定，且要求**恰好相等**：多一个 TAB、少一个字段都说明这份
    // 文本不是本模块写出来的，不能按“前 N 个字段有效”来宽容处理。
    const std::size_t expected_fields = is_version_3 ? 13u : 12u;
    if (!SplitFields(line, &fields) || fields.size() != expected_fields) {
      SetError(error_message,
               "Invalid source manifest: an entry does not have " +
                   std::to_string(expected_fields) + " fields");
      return false;
    }

    std::uint64_t type_id = 0;
    if (!ParseUnsigned(fields[0], &type_id) || type_id < 1 || type_id > 7) {
      SetError(error_message,
               "Invalid source manifest: bad entry type '" + fields[0] + "'");
      return false;
    }

    ManifestEntry entry;
    entry.type = static_cast<EntryType>(type_id);

    // 范围本身就是格式的一部分：size ≤ 2^62（给后续加法留出余量）、mtime_nsec
    // < 1e9、mode ≤ 07777、uid/gid/dev ≤ 2^32-1、hardlink_degree ≤ 条目上限。
    // 这些上界同时也是内存与算术安全的前提，缺一条都要重新审一遍解析器。
    if (!ParseUnsigned(fields[1], &entry.size) || entry.size > (1ull << 62)) {
      SetError(error_message,
               "Invalid source manifest: bad size '" + fields[1] + "'");
      return false;
    }
    if (!ParseSigned(fields[2], &entry.mtime_sec)) {
      SetError(error_message, "Invalid source manifest: bad mtime seconds '" +
                                  fields[2] + "'");
      return false;
    }
    std::uint64_t mtime_nsec = 0;
    if (!ParseUnsigned(fields[3], &mtime_nsec) || mtime_nsec >= 1000000000ull) {
      SetError(
          error_message,
          "Invalid source manifest: bad mtime nanoseconds '" + fields[3] + "'");
      return false;
    }
    entry.mtime_nsec = static_cast<std::uint32_t>(mtime_nsec);

    std::uint64_t mode = 0;
    if (!ParseUnsigned(fields[4], &mode) || mode > 07777ull) {
      SetError(error_message,
               "Invalid source manifest: bad mode '" + fields[4] + "'");
      return false;
    }
    entry.mode = static_cast<std::uint32_t>(mode);

    std::uint64_t uid = 0;
    std::uint64_t gid = 0;
    if (!ParseUnsigned(fields[5], &uid) || uid > 0xFFFFFFFFull) {
      SetError(error_message,
               "Invalid source manifest: bad uid '" + fields[5] + "'");
      return false;
    }
    if (!ParseUnsigned(fields[6], &gid) || gid > 0xFFFFFFFFull) {
      SetError(error_message,
               "Invalid source manifest: bad gid '" + fields[6] + "'");
      return false;
    }
    entry.uid = static_cast<std::uint32_t>(uid);
    entry.gid = static_cast<std::uint32_t>(gid);

    std::uint64_t dev_major = 0;
    std::uint64_t dev_minor = 0;
    if (!ParseUnsigned(fields[7], &dev_major) || dev_major > 0xFFFFFFFFull) {
      SetError(error_message,
               "Invalid source manifest: bad device major '" + fields[7] + "'");
      return false;
    }
    if (!ParseUnsigned(fields[8], &dev_minor) || dev_minor > 0xFFFFFFFFull) {
      SetError(error_message,
               "Invalid source manifest: bad device minor '" + fields[8] + "'");
      return false;
    }
    entry.dev_major = static_cast<std::uint32_t>(dev_major);
    entry.dev_minor = static_cast<std::uint32_t>(dev_minor);

    std::uint64_t degree = 0;
    if (!ParseUnsigned(fields[9], &degree) || degree > kMaxManifestEntries) {
      SetError(error_message, "Invalid source manifest: bad hardlink degree '" +
                                  fields[9] + "'");
      return false;
    }
    entry.hardlink_degree = static_cast<std::uint32_t>(degree);

    // archive_path 是唯一参与集合语义的字段：先反转义，再按“归档路径”的语法
    // 校验（相对、无 NUL、无 "./" 前缀、不以 '/' 结尾）。它是去重键，也是 diff
    // 的排序键，因此这一步失败必须整份拒绝，而不是跳过这一条。
    if (!UnescapeField(fields[10], &entry.archive_path) ||
        !IsValidArchivePath(entry.archive_path)) {
      SetError(error_message,
               "Invalid source manifest: bad archive path in an entry");
      return false;
    }
    if (!UnescapeField(fields[11], &entry.link_target)) {
      SetError(error_message,
               "Invalid source manifest: bad link target escape");
      return false;
    }
    if (is_version_3) {
      if (!UnescapeField(fields[12], &entry.content_digest)) {
        SetError(error_message,
                 "Invalid source manifest: bad content digest escape");
        return false;
      }
      // v3 自称带内容身份，就必须真的带：普通文件与软链接缺摘要说明这份
      // manifest 不能当增量基线；反过来，其它类型带摘要说明写入方不懂格式。
      if (entry.type == EntryType::kRegularFile ||
          entry.type == EntryType::kSymlink) {
        if (!IsContentDigest(entry.content_digest)) {
          SetError(error_message,
                   "Invalid source manifest: a version 3 entry is missing its "
                   "content digest");
          return false;
        }
      } else if (!entry.content_digest.empty()) {
        SetError(error_message,
                 "Invalid source manifest: this entry type must not carry a "
                 "content digest");
        return false;
      }
    }
    if (!seen_paths.insert(entry.archive_path).second) {
      SetError(error_message, "Invalid source manifest: duplicate path '" +
                                  entry.archive_path + "'");
      return false;
    }
    entries->push_back(std::move(entry));
  }

  // 末尾不许有多余字节。“多出来的内容被忽略”会让一份被追加过数据的文件看
  // 起来完全正常，而 manifest 是机器写的，出现偏差就说明状态已经坏了。
  if (position != text.size()) {
    SetError(error_message,
             "Invalid source manifest: unexpected data after the last entry");
    return false;
  }
  return true;
}

}  // namespace backupproject

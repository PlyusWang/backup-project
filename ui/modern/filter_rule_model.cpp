// filter_rule_model.cpp
#include "filter_rule_model.h"

#include <sys/stat.h>

#include <QDateTime>
#include <QtConcurrent>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "user_directory.h"

namespace backup_modern {
namespace {

namespace bp = backupproject;

// ---- 表单取值 <-> 枚举 ------------------------------------------------------
//
// 这里只做"界面用的一组稳定键"与枚举之间的翻译，不含任何匹配或语法裁决：
// 生成的草稿一律交给 FilterRuleBuilder 序列化、由 Filter::AddRule 最终裁决。

// type 的 7 个取值，字符串与 DSL 逐字一致（见 docs/filter_usage.md 12.3）。
const char* TypeText(bp::RuleTypeValue type) {
  switch (type) {
    case bp::RuleTypeValue::kFile:
      return "file";
    case bp::RuleTypeValue::kFolder:
      return "folder";
    case bp::RuleTypeValue::kSymlink:
      return "symlink";
    case bp::RuleTypeValue::kFifo:
      return "fifo";
    case bp::RuleTypeValue::kCharDevice:
      return "char";
    case bp::RuleTypeValue::kBlockDevice:
      return "block";
    case bp::RuleTypeValue::kSocket:
      return "socket";
  }
  return "file";
}

// 表单文本 -> type。未知取值返回 false，由调用方报错：以后核心再添类型而界面
// 没跟上时，会明确失败，而不是静默按"普通文件"生成一条看起来正常的规则。
bool TypeFromText(const QString& text, bp::RuleTypeValue* type) {
  if (text == "file") {
    *type = bp::RuleTypeValue::kFile;
  } else if (text == "folder") {
    *type = bp::RuleTypeValue::kFolder;
  } else if (text == "symlink") {
    *type = bp::RuleTypeValue::kSymlink;
  } else if (text == "fifo") {
    *type = bp::RuleTypeValue::kFifo;
  } else if (text == "char") {
    *type = bp::RuleTypeValue::kCharDevice;
  } else if (text == "block") {
    *type = bp::RuleTypeValue::kBlockDevice;
  } else if (text == "socket") {
    *type = bp::RuleTypeValue::kSocket;
  } else {
    return false;
  }
  return true;
}

// 明细 / 摘要里的比较运算符文本。
const char* CompareText(bp::RuleSizeCompare compare) {
  switch (compare) {
    case bp::RuleSizeCompare::kLess:
      return "<";
    case bp::RuleSizeCompare::kLessEqual:
      return "<=";
    case bp::RuleSizeCompare::kGreater:
      return ">";
    case bp::RuleSizeCompare::kGreaterEqual:
      return ">=";
    case bp::RuleSizeCompare::kRange:
      return "..";
    case bp::RuleSizeCompare::kEqual:
      return "=";
  }
  return "=";
}

// uid / gid 的比较运算符用独立的一组键（eq/lt/le/gt/ge/range），不复用 size 的
// 符号键：size 的表单键就是符号本身，已经定型。空值按"等于"处理，与
// FilterClauseDraft 的默认值一致。
bool IdCompareFromText(const QString& text, bp::RuleSizeCompare* compare) {
  if (text.isEmpty() || text == "eq") {
    *compare = bp::RuleSizeCompare::kEqual;
  } else if (text == "lt") {
    *compare = bp::RuleSizeCompare::kLess;
  } else if (text == "le") {
    *compare = bp::RuleSizeCompare::kLessEqual;
  } else if (text == "gt") {
    *compare = bp::RuleSizeCompare::kGreater;
  } else if (text == "ge") {
    *compare = bp::RuleSizeCompare::kGreaterEqual;
  } else if (text == "range") {
    *compare = bp::RuleSizeCompare::kRange;
  } else {
    return false;
  }
  return true;
}

// uid / gid 的表单值是十进制文本。这里只做"文本 -> uint32"的转换：超范围或非
// 数字明确报错，绝不截断——把 uid:4294967296 截成 uid:0（root）会静默变成一条
// 完全不同的规则；真正的语法裁决仍然在 Filter::AddRule 里。
bool IdValueFromForm(const QVariantMap& form, const QString& key,
                     std::uint32_t* value, QString* error_message) {
  const QString text = form.value(key).toString();
  bool ok = false;
  const std::uint32_t parsed = text.trimmed().toUInt(&ok);
  if (!ok) {
    if (error_message != nullptr) {
      *error_message = key +
                       QStringLiteral(" 必须是 0..4294967295 的整数（当前是 ") +
                       text + QStringLiteral("）");
    }
    return false;
  }
  *value = parsed;
  return true;
}

// mtime 的紧凑明细：5 种形态都写清楚，规则列表里不用再猜。
QString MtimeDetail(const bp::FilterClauseDraft& clause) {
  switch (clause.mtime_kind) {
    case bp::RuleMtimeKind::kToday:
      return QStringLiteral("mtime = 今天");
    case bp::RuleMtimeKind::kYesterday:
      return QStringLiteral("mtime = 昨天");
    case bp::RuleMtimeKind::kLastDays:
      return QStringLiteral("mtime = 最近 ") +
             QString::number(clause.days_back) + QStringLiteral(" 天");
    case bp::RuleMtimeKind::kDay:
      return QStringLiteral("mtime = ") +
             QString::fromStdString(clause.date_low);
    case bp::RuleMtimeKind::kDayRange:
      return QStringLiteral("mtime = ") +
             QString::fromStdString(clause.date_low) + QStringLiteral("..") +
             QString::fromStdString(clause.date_high);
  }
  return QStringLiteral("mtime");
}

// uid / gid 的紧凑明细：等于写成 "uid = 1000"，其余带运算符。
QString IdDetail(const char* field, std::uint32_t low, std::uint32_t high,
                 bp::RuleSizeCompare compare) {
  const QString name = QString::fromLatin1(field);
  if (compare == bp::RuleSizeCompare::kRange) {
    return name + QStringLiteral(" = ") + QString::number(low) +
           QStringLiteral("..") + QString::number(high);
  }
  if (compare == bp::RuleSizeCompare::kEqual) {
    return name + QStringLiteral(" = ") + QString::number(low);
  }
  return name + QStringLiteral(" ") +
         QString::fromLatin1(CompareText(compare)) + QStringLiteral(" ") +
         QString::number(low);
}

// mtime 的类型键（today/yesterday/last_days/day/day_range）-> 枚举。
// 未知取值明确失败：核心以后再加时间形态时，界面没跟上会报错而不是静默落成"今天"。
bool MtimeKindFromText(const QString& text, bp::RuleMtimeKind* kind) {
  if (text == "today") {
    *kind = bp::RuleMtimeKind::kToday;
  } else if (text == "yesterday") {
    *kind = bp::RuleMtimeKind::kYesterday;
  } else if (text == "last_days") {
    *kind = bp::RuleMtimeKind::kLastDays;
  } else if (text == "day") {
    *kind = bp::RuleMtimeKind::kDay;
  } else if (text == "day_range") {
    *kind = bp::RuleMtimeKind::kDayRange;
  } else {
    return false;
  }
  return true;
}

// "最近 N 天"的天数：这里只做"文本 -> int"的转换；天数必须大于 0、不能超过
// 核心的上限，这些裁决全部在 FilterRuleBuilder 与 Filter::AddRule 里。
bool DaysBackFromForm(const QVariantMap& form, const QString& key, int* value,
                      QString* error_message) {
  const QString text = form.value(key).toString();
  bool ok = false;
  const int parsed = text.trimmed().toInt(&ok);
  if (!ok) {
    if (error_message != nullptr) {
      *error_message = QStringLiteral("mtime 的天数必须是整数（当前是 ") +
                       text + QStringLiteral("）");
    }
    return false;
  }
  *value = parsed;
  return true;
}

// 给界面用的紧凑明细：field = value（不含动作）。纯展示，不参与匹配。
QString ClauseDetail(const bp::FilterClauseDraft& clause) {
  switch (clause.field) {
    case bp::RuleField::kName:
    case bp::RuleField::kPath:
    case bp::RuleField::kStem: {
      const char* field = bp::RuleFieldName(clause.field);
      return QString::fromLatin1(field) + QStringLiteral(" = ") +
             QString::fromStdString(clause.pattern);
    }
    case bp::RuleField::kExt: {
      QStringList exts;
      for (const std::string& raw : clause.extensions) {
        QString e = QString::fromStdString(raw).trimmed();
        while (e.startsWith(QLatin1Char('.'))) e.remove(0, 1);
        if (!e.isEmpty()) exts << e;
      }
      return QStringLiteral("ext = ") + exts.join(QStringLiteral(";"));
    }
    case bp::RuleField::kType:
      return QStringLiteral("type = ") +
             QString::fromLatin1(TypeText(clause.type));
    case bp::RuleField::kSize: {
      const char* unit = clause.unit == bp::RuleSizeUnit::kByte   ? ""
                         : clause.unit == bp::RuleSizeUnit::kKilo ? " KB"
                         : clause.unit == bp::RuleSizeUnit::kMega ? " MB"
                                                                  : " GB";
      const QString low =
          QString::number(clause.size_low) + QString::fromLatin1(unit);
      if (clause.compare == bp::RuleSizeCompare::kRange) {
        return QStringLiteral("size = ") + low + QStringLiteral(" .. ") +
               QString::number(clause.size_high) + QString::fromLatin1(unit);
      }
      const char* op = clause.compare == bp::RuleSizeCompare::kLess ? "<"
                       : clause.compare == bp::RuleSizeCompare::kLessEqual
                           ? "<="
                       : clause.compare == bp::RuleSizeCompare::kGreater ? ">"
                                                                         : ">=";
      return QStringLiteral("size ") + QString::fromLatin1(op) +
             QStringLiteral(" ") + low;
    }
    case bp::RuleField::kMtime:
      return MtimeDetail(clause);
    case bp::RuleField::kUid:
      return IdDetail("uid", clause.uid, clause.uid_high, clause.uid_compare);
    case bp::RuleField::kGid:
      return IdDetail("gid", clause.gid, clause.gid_high, clause.gid_compare);
    case bp::RuleField::kUser:
      return QStringLiteral("user = ") + QString::fromStdString(clause.user);
    case bp::RuleField::kGroup:
      return QStringLiteral("group = ") + QString::fromStdString(clause.group);
  }
  return QString();
}

QString FormatSize(std::uint64_t bytes) {
  if (bytes < 1024) return QString::number(bytes) + " B";
  if (bytes < 1024 * 1024) return QString::number(bytes / 1024) + " KB";
  return QString::number(bytes / (1024 * 1024)) + " MB";
}

bp::RuleSizeUnit UnitFromText(const QString& text) {
  if (text == "B") return bp::RuleSizeUnit::kByte;
  if (text == "MB") return bp::RuleSizeUnit::kMega;
  if (text == "GB") return bp::RuleSizeUnit::kGiga;
  return bp::RuleSizeUnit::kKilo;
}

bp::RuleSizeCompare CompareFromText(const QString& text) {
  if (text == "<") return bp::RuleSizeCompare::kLess;
  if (text == "<=") return bp::RuleSizeCompare::kLessEqual;
  if (text == ">") return bp::RuleSizeCompare::kGreater;
  if (text == "..") return bp::RuleSizeCompare::kRange;
  return bp::RuleSizeCompare::kGreaterEqual;
}

// 预览条目的类型名。socket 在归档格式里没有对应表示（见 tree_scanner.h），
// 标签直接把后果写出来，而不是让它看起来像一个能备份的条目。
QString PreviewTypeLabel(bp::EntryType type) {
  switch (type) {
    case bp::EntryType::kDirectory:
      return QStringLiteral("目录");
    case bp::EntryType::kRegularFile:
      return QStringLiteral("普通文件");
    case bp::EntryType::kSymlink:
      return QStringLiteral("符号链接");
    case bp::EntryType::kHardLink:
      return QStringLiteral("硬链接");
    case bp::EntryType::kFifo:
      return QStringLiteral("FIFO");
    case bp::EntryType::kCharDevice:
      return QStringLiteral("字符设备");
    case bp::EntryType::kBlockDevice:
      return QStringLiteral("块设备");
    case bp::EntryType::kSocket:
      return QStringLiteral("socket（不支持归档）");
  }
  return QStringLiteral("未知类型");
}

// lstat 的 st_mode -> EntryType，判定与 src/core/tree_scanner.cpp 的 FactsOf
// 同一套：预览看到的类型必须和真实扫描一致。
bool TypeFromStat(const struct stat& info, bp::EntryType* type) {
  if (S_ISDIR(info.st_mode)) {
    *type = bp::EntryType::kDirectory;
  } else if (S_ISREG(info.st_mode)) {
    *type = bp::EntryType::kRegularFile;
  } else if (S_ISLNK(info.st_mode)) {
    *type = bp::EntryType::kSymlink;
  } else if (S_ISFIFO(info.st_mode)) {
    *type = bp::EntryType::kFifo;
  } else if (S_ISCHR(info.st_mode)) {
    *type = bp::EntryType::kCharDevice;
  } else if (S_ISBLK(info.st_mode)) {
    *type = bp::EntryType::kBlockDevice;
  } else if (S_ISSOCK(info.st_mode)) {
    *type = bp::EntryType::kSocket;
  } else {
    return false;
  }
  return true;
}

// uid / gid -> 名字走共享实现（backupproject::UserDirectoryCache）：预览和
// 真实扫描（tree_scanner.cpp）必须给出同一份元数据。各写一份时，group 用错
// sysconf hint 这类问题会让 user: / group: 规则在预览里命中、真实备份却漏掉。
// 解析失败留空，Filter 对空名字一律视为不匹配（见 include/filter.h）。
//
// 不直接把扫描交给 ScanSourceTree 的原因：那个入口遇到"没有被排除的 socket"会
// 整次失败，而预览要做的恰恰是把这类条目列出来并提示后果。
bp::Filter BuildFilterFromDrafts(
    const std::vector<bp::FilterRuleDraft>& drafts) {
  bp::Filter filter;
  for (const bp::FilterRuleDraft& draft : drafts) {
    std::string dsl;
    if (!bp::ToDsl(draft, &dsl, nullptr)) continue;
    std::string error;
    filter.AddRule(draft.action, dsl, &error);
  }
  return filter;
}

}  // namespace

FilterRuleModel::FilterRuleModel(BackupController* controller, QObject* parent)
    : QObject(parent), controller_(controller) {
  connect(&watcher_, &QFutureWatcher<PreviewOutcome>::finished, this, [this]() {
    const PreviewOutcome outcome = watcher_.result();
    preview_busy_ = false;
    // 扫描期间又来了请求：丢掉这次（已过期的）结果，立刻用最新的 source +
    // drafts 重新扫描，中间不展示旧结果。这样界面最终显示的必然对应"当前 source
    // + 当前 rules"。
    if (preview_pending_) {
      preview_pending_ = false;
      const QString next_source = pending_source_;
      const std::vector<backupproject::FilterRuleDraft> next_drafts =
          pending_drafts_;
      StartScan(next_source, next_drafts);
      return;
    }
    if (!outcome.error.isEmpty()) {
      SetError(outcome.error);
    } else {
      preview_items_ = outcome.items;
      preview_truncated_ = outcome.truncated;
      preview_source_ = outcome.source_path;
    }
    emit previewChanged();
  });
}

QString FilterRuleModel::summaryText() const {
  QStringList parts;
  for (const bp::FilterRuleDraft& draft : drafts_) {
    parts << QString::fromStdString(bp::Summarize(draft));
  }
  if (parts.isEmpty())
    return QStringLiteral(
        "未设置过滤规则：将备份源目录中的全部文件与目录结构。");
  return parts.join(QStringLiteral("；"));
}

QString FilterRuleModel::dslText() const {
  QStringList lines;
  for (const bp::FilterRuleDraft& draft : drafts_) {
    std::string dsl;
    if (!bp::ToDsl(draft, &dsl, nullptr)) continue;
    const QString action = draft.action == bp::FilterAction::kInclude
                               ? QStringLiteral("include")
                               : QStringLiteral("exclude");
    lines << action + " " + QString::fromStdString(dsl);
  }
  return lines.join(QStringLiteral("\n"));
}

bool FilterRuleModel::DraftFromForm(const QVariantMap& form,
                                    bp::FilterRuleDraft* draft,
                                    QString* error) const {
  const auto fail = [error](const QString& text) {
    if (error != nullptr) *error = text;
    return false;
  };

  const QString action = form.value(QStringLiteral("action")).toString();
  const QString field = form.value(QStringLiteral("field")).toString();
  draft->action = action == QStringLiteral("exclude")
                      ? bp::FilterAction::kExclude
                      : bp::FilterAction::kInclude;

  // 表单字段名 -> RuleField。这一步只做名字映射；未知字段名直接失败，
  // 不会落进别的分支生成一条不相干的规则。
  bp::RuleField rule_field = bp::RuleField::kName;
  if (field == QStringLiteral("name")) {
    rule_field = bp::RuleField::kName;
  } else if (field == QStringLiteral("path")) {
    rule_field = bp::RuleField::kPath;
  } else if (field == QStringLiteral("stem")) {
    rule_field = bp::RuleField::kStem;
  } else if (field == QStringLiteral("ext")) {
    rule_field = bp::RuleField::kExt;
  } else if (field == QStringLiteral("type")) {
    rule_field = bp::RuleField::kType;
  } else if (field == QStringLiteral("size")) {
    rule_field = bp::RuleField::kSize;
  } else if (field == QStringLiteral("uid")) {
    rule_field = bp::RuleField::kUid;
  } else if (field == QStringLiteral("gid")) {
    rule_field = bp::RuleField::kGid;
  } else if (field == QStringLiteral("user")) {
    rule_field = bp::RuleField::kUser;
  } else if (field == QStringLiteral("group")) {
    rule_field = bp::RuleField::kGroup;
  } else if (field == QStringLiteral("mtime")) {
    // mtime 是已知字段，只是表单里还没有对应控件：照常映射成 RuleField，
    // 让下面的 switch 给出"还没有表单控件"这个准确原因。
    rule_field = bp::RuleField::kMtime;
  } else {
    return fail(QStringLiteral("未知字段：") + field);
  }

  bp::FilterClauseDraft clause;
  clause.field = rule_field;
  switch (rule_field) {
    case bp::RuleField::kName:
    case bp::RuleField::kPath:
    case bp::RuleField::kStem:
      clause.pattern =
          form.value(QStringLiteral("pattern")).toString().toStdString();
      break;
    case bp::RuleField::kExt: {
      const QString raw = form.value(QStringLiteral("extensions")).toString();
      const QStringList pieces =
          raw.split(QLatin1Char(';'), Qt::SkipEmptyParts);
      for (const QString& piece : pieces) {
        clause.extensions.push_back(piece.toStdString());
      }
      break;
    }
    case bp::RuleField::kType: {
      const QString text = form.value(QStringLiteral("type")).toString();
      if (!TypeFromText(text, &clause.type)) {
        return fail(QStringLiteral("未知的 type 取值：") + text);
      }
      break;
    }
    case bp::RuleField::kSize:
      clause.compare =
          CompareFromText(form.value(QStringLiteral("compare")).toString());
      clause.unit = UnitFromText(form.value(QStringLiteral("unit")).toString());
      clause.size_low = form.value(QStringLiteral("sizeLow")).toULongLong();
      clause.size_high = form.value(QStringLiteral("sizeHigh")).toULongLong();
      break;
    case bp::RuleField::kUid:
    case bp::RuleField::kGid: {
      const bool is_uid = rule_field == bp::RuleField::kUid;
      const QString prefix =
          is_uid ? QStringLiteral("uid") : QStringLiteral("gid");
      bp::RuleSizeCompare compare = bp::RuleSizeCompare::kEqual;
      const QString compare_text =
          form.value(prefix + QStringLiteral("_compare")).toString();
      if (!IdCompareFromText(compare_text, &compare)) {
        return fail(QStringLiteral("未知的 ") + prefix +
                    QStringLiteral(" 比较运算符：") + compare_text);
      }
      std::uint32_t low = 0;
      std::uint32_t high = 0;
      if (!IdValueFromForm(form, prefix, &low, error)) {
        return false;
      }
      // 上界只在区间里用：不是区间时表单一侧的残留值不该拦住这条规则。
      if (compare == bp::RuleSizeCompare::kRange &&
          !IdValueFromForm(form, prefix + QStringLiteral("_high"), &high,
                           error)) {
        return false;
      }
      if (is_uid) {
        clause.uid = low;
        clause.uid_high = high;
        clause.uid_compare = compare;
      } else {
        clause.gid = low;
        clause.gid_high = high;
        clause.gid_compare = compare;
      }
      break;
    }
    case bp::RuleField::kUser:
      // 名字是精确匹配、大小写敏感，这里不做 trim 之类的清洗。
      clause.user = form.value(QStringLiteral("user")).toString().toStdString();
      break;
    case bp::RuleField::kGroup:
      clause.group =
          form.value(QStringLiteral("group")).toString().toStdString();
      break;
    case bp::RuleField::kMtime: {
      const QString kind = form.value(QStringLiteral("mtime_kind")).toString();
      bp::RuleMtimeKind mtime_kind = bp::RuleMtimeKind::kToday;
      if (!MtimeKindFromText(kind, &mtime_kind)) {
        return fail(QStringLiteral("未知的 mtime 类型：") + kind);
      }
      clause.mtime_kind = mtime_kind;
      // 日期原样透传：格式、区间方向是否合法由 builder 与 Filter::AddRule
      // 裁决， 界面里没有第二套日期解析。
      clause.date_low =
          form.value(QStringLiteral("date_low")).toString().toStdString();
      clause.date_high =
          form.value(QStringLiteral("date_high")).toString().toStdString();
      if (mtime_kind == bp::RuleMtimeKind::kLastDays &&
          !DaysBackFromForm(form, QStringLiteral("days_back"),
                            &clause.days_back, error)) {
        return false;
      }
      break;
    }
    default:
      // 以后 RuleField 再添取值而这里忘了补分支时，会带着字段名明确失败。
      return fail(QStringLiteral("未知字段：") + field);
  }
  draft->clauses.clear();
  draft->clauses.push_back(clause);
  std::string message;
  if (!bp::ValidateRule(*draft, &message)) {
    if (error != nullptr) *error = QString::fromStdString(message);
    return false;
  }
  return true;
}

QString FilterRuleModel::dslForForm(const QVariantMap& form) const {
  bp::FilterRuleDraft draft;
  QString error;
  if (!DraftFromForm(form, &draft, &error)) return QString();
  std::string dsl;
  if (!bp::ToDsl(draft, &dsl, nullptr)) return QString();
  return QString::fromStdString(dsl);
}

QString FilterRuleModel::summaryForForm(const QVariantMap& form) const {
  bp::FilterRuleDraft draft;
  QString error;
  if (!DraftFromForm(form, &draft, &error)) return QString();
  return QString::fromStdString(bp::Summarize(draft));
}

QString FilterRuleModel::validateForm(const QVariantMap& form) const {
  bp::FilterRuleDraft draft;
  QString error;
  if (!DraftFromForm(form, &draft, &error)) return error;
  return QString();
}

bool FilterRuleModel::addRule(const QVariantMap& form) {
  bp::FilterRuleDraft draft;
  QString error;
  if (!DraftFromForm(form, &draft, &error)) {
    SetError(error);
    return false;
  }
  drafts_.push_back(draft);
  SyncController();
  RebuildRules();
  clearError();
  emit rulesChanged();
  return true;
}

void FilterRuleModel::removeRule(int index) {
  if (index < 0 || index >= static_cast<int>(drafts_.size())) return;
  drafts_.erase(drafts_.begin() + index);
  SyncController();
  RebuildRules();
  emit rulesChanged();
}

void FilterRuleModel::moveRule(int index, int delta) {
  const int target = index + delta;
  if (index < 0 || index >= static_cast<int>(drafts_.size())) return;
  if (target < 0 || target >= static_cast<int>(drafts_.size())) return;
  std::swap(drafts_[index], drafts_[target]);
  SyncController();
  RebuildRules();
  emit rulesChanged();
}

void FilterRuleModel::clearRules() {
  drafts_.clear();
  SyncController();
  RebuildRules();
  emit rulesChanged();
}

QString FilterRuleModel::cliArguments() const {
  QStringList parts;
  for (const std::string& arg : bp::CliArguments(drafts_)) {
    const QString text = QString::fromStdString(arg);
    parts << (text.startsWith(QStringLiteral("--"))
                  ? text
                  : QStringLiteral("'") + text + QStringLiteral("'"));
  }
  return parts.join(QStringLiteral(" "));
}

void FilterRuleModel::clearError() {
  if (last_error_.isEmpty()) return;
  last_error_.clear();
  emit lastErrorChanged();
}

void FilterRuleModel::SetError(const QString& message) {
  if (last_error_ == message) return;
  last_error_ = message;
  emit lastErrorChanged();
}

// 规则列表是唯一来源，控制器只是它的投影：每次改动都整体重放一遍，
// 这样两边的顺序与内容不可能不一致，也不需要索引映射。
void FilterRuleModel::SyncController() {
  if (controller_ == nullptr) return;
  controller_->clearFilterRules();
  for (const bp::FilterRuleDraft& draft : drafts_) {
    std::string dsl;
    if (!bp::ToDsl(draft, &dsl, nullptr)) continue;
    controller_->addFilterRule(draft.action == bp::FilterAction::kInclude
                                   ? QStringLiteral("include")
                                   : QStringLiteral("exclude"),
                               QString::fromStdString(dsl));
  }
}

void FilterRuleModel::RebuildRules() {
  rules_.clear();
  for (const bp::FilterRuleDraft& draft : drafts_) {
    std::string dsl;
    bp::ToDsl(draft, &dsl, nullptr);
    QVariantMap item;
    item.insert(QStringLiteral("action"),
                draft.action == bp::FilterAction::kInclude
                    ? QStringLiteral("include")
                    : QStringLiteral("exclude"));
    QString detail;
    for (const bp::FilterClauseDraft& clause : draft.clauses) {
      if (!detail.isEmpty()) detail += QStringLiteral("，且 ");
      detail += ClauseDetail(clause);
    }
    item.insert(QStringLiteral("detail"), detail);
    item.insert(QStringLiteral("summary"),
                QString::fromStdString(bp::Summarize(draft)));
    item.insert(QStringLiteral("dsl"), QString::fromStdString(dsl));
    rules_.push_back(item);
  }
}

void FilterRuleModel::requestPreview(const QString& source_path,
                                     const QString& restore_path) {
  if (source_path.isEmpty()) {
    SetError(restore_path.isEmpty()
                 ? QStringLiteral("请先填写源目录，再刷新预览。")
                 : QStringLiteral("恢复不重新筛选，无需预览。"));
    return;
  }
  clearError();
  // 记下最新一次请求；正在扫描时不排队第二次，等当前这次结束立刻用最新参数重扫。
  pending_source_ = source_path;
  pending_drafts_ = drafts_;
  if (preview_busy_) {
    preview_pending_ = true;
    return;
  }
  StartScan(pending_source_, pending_drafts_);
}

void FilterRuleModel::StartScan(
    const QString& source_path,
    const std::vector<backupproject::FilterRuleDraft>& drafts) {
  preview_busy_ = true;
  preview_pending_ = false;
  emit previewChanged();
  watcher_.setFuture(QtConcurrent::run([source_path, drafts]() {
    return ScanPreview(source_path, drafts, kPreviewLimit);
  }));
}

FilterRuleModel::PreviewOutcome FilterRuleModel::ScanPreview(
    const QString& source_path, const std::vector<bp::FilterRuleDraft>& drafts,
    int limit) {
  namespace fs = std::filesystem;
  PreviewOutcome outcome;
  outcome.source_path = source_path;
  const fs::path root(source_path.toStdString());
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    outcome.error = QStringLiteral("源目录不存在或不是目录：") + source_path;
    return outcome;
  }
  const bp::Filter filter = BuildFilterFromDrafts(drafts);
  bp::UserDirectoryCache names;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    if (static_cast<int>(outcome.items.size()) >= limit) {
      outcome.truncated = true;
      break;
    }
    const fs::path& path = it->path();
    // lstat：软链接不会被跟随，预览看到的就是条目自己；mtime / uid / gid 也都
    // 取自链接本身，口径与真实扫描（tree_scanner）一致。
    struct stat info;
    if (::lstat(path.c_str(), &info) != 0) continue;
    bp::EntryType type = bp::EntryType::kRegularFile;
    if (!TypeFromStat(info, &type)) continue;

    bp::FilterEntry fe;
    fe.archive_path = path.lexically_relative(root).generic_string();
    fe.name = path.filename().string();
    fe.is_directory = type == bp::EntryType::kDirectory;
    fe.type = type;
    fe.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    fe.uid = static_cast<std::uint32_t>(info.st_uid);
    fe.gid = static_cast<std::uint32_t>(info.st_gid);
    if (type == bp::EntryType::kRegularFile) {
      fe.size = static_cast<std::uint64_t>(info.st_size);
    }
    // 与 tree_scanner 一致：所有类型（含软链接）都解析属主 / 属组名字。
    // uid / gid 来自 lstat，属于链接自己，解析名字不 follow。
    fe.user_name = names.UserName(fe.uid);
    fe.group_name = names.GroupName(fe.gid);

    // 归属判定全部问真实 Filter：GUI 里没有第二套匹配逻辑。
    bool included = false;
    QString tag;
    if (type == bp::EntryType::kDirectory) {
      if (filter.ShouldPruneDirectory(fe)) {
        // 命中 exclude 的目录整棵剪掉：子树里的 socket 也不再是问题。
        tag = QStringLiteral("目录被排除（整棵剪掉）");
        it.disable_recursion_pending();
      } else {
        included = true;
        tag = QStringLiteral("目录（保留结构）");
      }
    } else if (type == bp::EntryType::kSocket) {
      // socket 不作为可恢复备份：只有明确写了 exclude 才会被跳过，
      // 否则真实备份会整次失败（见 tree_scanner.h 的失败语义）。
      if (filter.ShouldSkipSpecialEntry(fe)) {
        tag = PreviewTypeLabel(type) + QStringLiteral(" · 被规则排除");
      } else {
        tag = QStringLiteral("不支持的 socket（会导致备份失败）");
      }
    } else {
      // 普通文件与软链接 / FIFO / 设备走同一条 include/exclude 判定。
      included = filter.ShouldIncludeFile(fe);
      tag =
          PreviewTypeLabel(type) + (included ? QStringLiteral(" · 进入归档")
                                             : QStringLiteral(" · 被规则排除"));
    }

    QVariantMap item;
    item.insert(QStringLiteral("path"),
                QString::fromStdString(fe.archive_path));
    item.insert(QStringLiteral("isDirectory"), fe.is_directory);
    item.insert(QStringLiteral("size"), type == bp::EntryType::kRegularFile
                                            ? FormatSize(fe.size)
                                            : QString());
    item.insert(
        QStringLiteral("mtime"),
        fe.mtime_sec > 0
            ? QDateTime::fromSecsSinceEpoch(static_cast<qint64>(fe.mtime_sec))
                  .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
            : QString());
    item.insert(QStringLiteral("included"), included);
    item.insert(QStringLiteral("tag"), tag);
    outcome.items.push_back(item);
  }
  return outcome;
}

}  // namespace backup_modern

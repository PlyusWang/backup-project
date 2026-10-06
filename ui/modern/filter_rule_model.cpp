// filter_rule_model.cpp
// 职责：过滤规则页的视图模型。它是"规则草稿列表"的唯一来源，对外提供三类
// 东西：
//   * rules_：QML 直接绑定的展示行（动作、条件摘要、明细、DSL 文本）；
//   * 表单翻译：DraftFromForm / dslForForm / summaryForForm / validateForm，
//     把 QML 的表单 map 翻成 backupproject::FilterRuleDraft；
//   * 预览：把源目录加上当前草稿交给共享核心跑一遍，结果摊平成 QVariantMap。
//
// 职责边界：本文不定义筛选语义，也不实现匹配或扫描。规则文本由
// FilterRuleBuilder 序列化、由 Filter::AddRule 裁决；预览由
// backupproject::PreviewBackupSelection 完成（CLI 预览用的是同一个函数），
// 所以"GUI 预览与 CLI 预览给出不同集合"在结构上不可能发生。
//
// 数据流：
//   QML 表单 -> DraftFromForm -> drafts_（唯一来源）
//            -> SyncController / RebuildRules -> 控制器 + 展示行
//            -> requestPreview -> QtConcurrent 线程 -> ScanPreview
//            -> watcher_.finished -> preview_items_ -> previewChanged
//
// 不变量：
//   * drafts_ 是唯一来源，控制器与 rules_ 都是它的投影：任何改动都整体重放
//     （见 SyncController），既没有索引映射，也不会出现两边顺序不同；
//   * setRules 要么整体成功、要么一条都不改（先在副本上校验完再替换）；
//   * last_error_ 为空表示现在没有错误；失败路径必须同时清掉上一份预览结果，
//     绝不把已经过期的结果摆在错误信息旁边。
//
// 线程与生命周期：drafts_ / rules_ / preview_* 只由 GUI 线程访问；后台线程
// 只拿到 source_path 与 drafts 的值拷贝，不接触任何成员；结果经
// QFutureWatcher 回到 GUI 线程之后才写入。controller_ 是可空的弱引用（对象
// 不属于本类），生命周期由外部保证。
//
// 失败语义：对外方法都不抛异常。表单类方法失败时返回空串或空 map，并把原因
// 写进出参 error；setRules / addRule 返回 false，并通过 lastError 属性把中文
// 原因送到界面。
#include "filter_rule_model.h"

#include <QDateTime>
#include <QtConcurrent>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "backup_preview.h"
#include "format_bytes.h"

namespace backup_modern {
namespace {

namespace bp = backupproject;

// ---- 表单取值 <-> 枚举 ------------------------------------------------------
//
// 这里只做"界面用的一组稳定键"与枚举之间的翻译，不含任何匹配或语法裁决：
// 生成的草稿一律交给 FilterRuleBuilder 序列化、由 Filter::AddRule 最终裁决。

// type 的 7 个取值，字符串与 DSL 逐字一致（见 docs/filter_usage.md 12.3）。
// type 枚举 -> 稳定键。键同时是 DSL 取值与 QML 下拉框的 key，改动它等于
// 改动规则语法：已保存的规则会解析失败，因此这里只做翻译，不做归一化。
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
// 稳定键 -> type 枚举。未知取值返回 false 而不是退回 kFile：界面用一个
// 看不懂的值生成一条看似正常的"普通文件"规则，比直接报错危险得多。
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
// 数值比较 -> 显示文本。这些文本同时是 size 表单的键（kSizeCompares 直接
// 用它们当 key），所以不能为了好看改成"<="以外的写法。
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
// uid / gid 的比较键 -> 枚举，未知取值返回 false（见 IdValueFromForm 的
// fail-closed 说明）。空值按"等于"处理，与 FilterClauseDraft 的默认值一致。
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

// size 的数值按**文本**解析。
//
// 为什么不用 QVariant 的 toULongLong：QML 侧一旦先 parseInt，像
// "99999999999999999999" 这样的输入就已经变成一个浮点数了，溢出发生在到达
// C++ 之前，谁都来不及拒绝它——最后生成一条与用户所写完全不同的规则。
// 这里逐字符累加并**在乘之前**夹住上界（result > (max - digit) / 10），
// 所以"非法"与"溢出"都是明确的失败，不会回绕。
// 前置条件：form 里的 key 是用户在输入框里敲的原文，本函数负责 trim、
// 判空与逐字符转数字。
// 后置条件：成功时写 *value；失败时只写 *error_message，不动 *value。
// 溢出判定与乘前夹紧已在下面说明，这里是整个表单里唯一解析 size 数字的地方。
bool SizeValueFromForm(const QVariantMap& form, const QString& key,
                       std::uint64_t* value, QString* error_message) {
  const QString text = form.value(key).toString().trimmed();
  if (text.isEmpty()) {
    if (error_message != nullptr) {
      *error_message = QStringLiteral("文件大小必须填一个数字。");
    }
    return false;
  }
  std::uint64_t result = 0;
  for (const QChar character : text) {
    if (character < QLatin1Char('0') || character > QLatin1Char('9')) {
      if (error_message != nullptr) {
        *error_message = QStringLiteral("文件大小必须是十进制整数（当前是 ") +
                         text + QStringLiteral("）");
      }
      return false;
    }
    const std::uint64_t digit =
        static_cast<std::uint64_t>(character.unicode() - '0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      if (error_message != nullptr) {
        *error_message = QStringLiteral("文件大小超出可表示范围（当前是 ") +
                         text + QStringLiteral("）");
      }
      return false;
    }
    result = result * 10u + digit;
  }
  *value = result;
  return true;
}

// uid / gid 的表单值是十进制文本。这里只做"文本 -> uint32"的转换：超范围或非
// 数字明确报错，绝不截断——把 uid:4294967296 截成 uid:0（root）会静默变成一条
// 完全不同的规则；真正的语法裁决仍然在 Filter::AddRule 里。
// 失败契约与 SizeValueFromForm 相同。区别是这里借用 QString::toUInt：非数字、
// 负号与超出 uint32 的值都会让它把 ok 置为 false，正是 uid / gid 要的语义。
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
// 未知取值明确失败：核心以后再加时间形态时，
// 界面没跟上会报错而不是静默落成"今天"。
// mtime 类型键 -> 枚举，未知取值返回 false（采用 fail-closed，理由同
// TypeFromText）：核心以后新增时间形态而界面没跟上时会明确报错，而不是
// 静默落成"今天"。
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
// "最近 N 天"的 N 只做整数转换；N 必须大于 0、不能超过核心的上限，这些裁决
// 全部留给 FilterRuleBuilder 与 Filter::AddRule，界面不复制一遍业务规则。
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
// 每条子句一行的紧凑明细，纯展示、不参与匹配。它只复述草稿里的字段值，
// 不去查询真实文件，所以列表滚动时没有 I/O，也不会因为源目录变化而变样。
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
      if (clause.compare == bp::RuleSizeCompare::kEqual) {
        return QStringLiteral("size = ") + low;
      }
      return QStringLiteral("size ") +
             QString::fromLatin1(CompareText(clause.compare)) +
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

// 预览里的文件大小也走全产品唯一的格式化规则：以前这里是整数截断的
// “KB / MB”，而实际除的是 1024。
// 预览里的大小也走全产品唯一的格式化函数：以前这里是整数截断的 KB/MB，
// 而实际除的是 1024，同一份数据在预览和归档列表上显示成两个值。
QString FormatSize(std::uint64_t bytes) {
  return QString::fromStdString(backupproject::FormatByteSize(bytes));
}

// 单位键 -> 枚举。注意这里会默默兜底：不认识的键
// 一律变成 KB。它安全的前提是调用方只喂 kUnits 表里的取值；如果哪天键来自
// 持久化数据，这里必须改成返回 bool，否则规则会悄悄换一个数量级。
bp::RuleSizeUnit UnitFromText(const QString& text) {
  if (text == "B") return bp::RuleSizeUnit::kByte;
  if (text == "MB") return bp::RuleSizeUnit::kMega;
  if (text == "GB") return bp::RuleSizeUnit::kGiga;
  return bp::RuleSizeUnit::kKilo;
}

// 运算符键 -> 枚举，同样带兜底（默认 kGreaterEqual，即 QML 下拉框的默认项）。
// 兜底的安全性同样依赖调用方只传 kSizeCompares 里的键。
bp::RuleSizeCompare CompareFromText(const QString& text) {
  if (text == "<") return bp::RuleSizeCompare::kLess;
  if (text == "<=") return bp::RuleSizeCompare::kLessEqual;
  if (text == ">") return bp::RuleSizeCompare::kGreater;
  if (text == "..") return bp::RuleSizeCompare::kRange;
  // "等于" 在 size 上由 builder 展开成 a..a（核心的 size 没有单独的 "="），
  // 在 uid / gid 上就是裸数字。两种展开都在 builder 里，这里只记住用户选了它。
  if (text == "=") return bp::RuleSizeCompare::kEqual;
  return bp::RuleSizeCompare::kGreaterEqual;
}

// 预览条目的类型名。socket 在归档格式里没有对应表示（见 tree_scanner.h），
// 标签直接把后果写出来，而不是让它看起来像一个能备份的条目。
// 预览行的类型名。硬链接单独成类：它由扫描层为去重生成，并不是源目录里
// 真实存在的条目类型，把它并进"普通文件"会让预览与实际归档内容对不上。
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

// 把共享核心给出的判定翻成界面文案。判定本身（included / 剪枝 / 排除）全部
// 来自 backupproject::PreviewBackupSelection，这里只负责措辞。
//
// 只有三种取值，因为遍历的判定就是三选一：没有被排除的 socket 不让遍历继续
// 走下去（真实 Backup 也会在它上面失败），所以它不是某一行的标签，而是整次
// 预览的失败原因——见 FilterRuleModel::ScanPreview 的错误分支。
// 判定 -> 界面文案。这里只做措辞，判定本身（进入归档 / 目录被剪 / 被排除）
// 来自核心，界面不重新推算一遍。
// 枚举里没有"socket 被跳过"这一项：未排除的 socket 会让整次预览失败，
// 所以它是错误分支，而不是某一行上的标签。
QString PreviewTag(const bp::PreviewItem& item) {
  switch (item.disposition) {
    case bp::PreviewDisposition::kDirectoryPruned:
      return QStringLiteral("目录被排除（整棵剪掉）");
    case bp::PreviewDisposition::kIncluded:
      return item.is_directory
                 ? QStringLiteral("目录（保留结构）")
                 : PreviewTypeLabel(item.type) + QStringLiteral(" · 进入归档");
    case bp::PreviewDisposition::kExcludedByRule:
      break;
  }
  return PreviewTypeLabel(item.type) + QStringLiteral(" · 被规则排除");
}

}  // namespace

// 构造：只建立 watcher 与 finished 之间的这一次连接，不主动扫描——预览永远
// 由用户操作触发。回调必定在 GUI 线程执行（QFutureWatcher 的契约），因此
// 下面直接读写成员，不需要加锁。
FilterRuleModel::FilterRuleModel(BackupController* controller, QObject* parent)
    : QObject(parent), controller_(controller) {
  // 结果到达后先复位 busy，再决定是"补跑最新一次"还是"落地这次结果"。
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
    last_error_kind_ = outcome.error_kind;
    if (!outcome.error.isEmpty()) {
      SetError(outcome.error);
      // 失败时不留上一份成功的列表：那是一份**不再成立**的结果（源目录可能已经
      // 变了、规则可能已经改了），把它摆在错误信息旁边只会让人以为"大部分还是
      // 好的"。预览的答案就是那句错误。
      preview_items_.clear();
      preview_truncated_ = false;
      preview_total_ = 0;
      preview_included_ = 0;
      preview_source_ = outcome.source_path;
    } else {
      preview_items_ = outcome.items;
      preview_truncated_ = outcome.truncated;
      preview_total_ = outcome.total;
      preview_included_ = outcome.included;
      preview_source_ = outcome.source_path;
    }
    emit previewChanged();
  });
}

// 面板顶部的规则汇总：所有草稿拼成一句句"包含/排除 ..."。没有任何规则时
// 明确说明"将备份全部内容"，而不是留一片空白让人猜。
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

// 当前全部规则的 DSL 原文，一行一条，供用户复制或排查。序列化失败的草稿
// 直接跳过——能显示出来的每一行都是真的能被核心接受的。
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

// QML 表单 -> 草稿。这是界面侧唯一的输入关口，因此所有翻译都在这里收口：
// 字段名、type、单位、运算符、mtime 形态、uid/gid 数值，任何一项不认识都
// 返回 false 并给出中文原因，绝不退回默认值生成一条"看起来对"的规则。
//
// 副作用：只写 *draft 与 *error，不碰任何成员，因此可以在 QML 的属性绑定
// 里被反复调用（预览文本就是这么更新的）。
// 最后一步会调用 ValidateRule 让真实核心复核一遍，界面层不自己宣布合法。
bool FilterRuleModel::DraftFromForm(const QVariantMap& form,
                                    bp::FilterRuleDraft* draft,
                                    QString* error) const {
  const auto fail = [error](const QString& text) {
    if (error != nullptr) *error = text;
    return false;
  };

  // action 只有 "exclude" 是排除，其余（含空）都按包含处理：QML 下拉框的
  // 默认项就是包含，这里不做隐式反转。
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
      // 数值按文本收：解析与范围裁决都在这里，QML 不先 parseInt。
      if (!SizeValueFromForm(form, QStringLiteral("sizeLowText"),
                             &clause.size_low, error)) {
        return false;
      }
      if (clause.compare == bp::RuleSizeCompare::kRange &&
          !SizeValueFromForm(form, QStringLiteral("sizeHighText"),
                             &clause.size_high, error)) {
        return false;
      }
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
  // 表单一次只描述一个子句；多子句规则只能从高级入口进来。先清空再放入，
  // 这样调用方复用同一个 draft 时不会把上一次的子句带进来。
  draft->clauses.clear();
  draft->clauses.push_back(clause);
  std::string message;
  if (!bp::ValidateRule(*draft, &message)) {
    if (error != nullptr) *error = QString::fromStdString(message);
    return false;
  }
  return true;
}

// 六个下拉框的全部取值都在这里产出，QML 侧不硬编码任何选项文本。
// 这里也刻意不做缓存：选项是常量表，重建一次的开销远小于维护失效逻辑。
QVariantMap FilterRuleModel::editorOptions() const {
  // ---- 条件类型 ----
  //
  // 顺序 = 普通用户的使用频率，不是 RuleField 的枚举顺序：先"按名字/扩展名挑
  // 文件"，再"按大小/时间/属性挑"。user / group
  // 排在最后：它们需要知道属主是谁， 日常用不到。
  //
  // 每一项都必须能真的生成 DSL 并被 Filter::AddRule 接受——这张表与
  // FilterRuleBuilder::RuleFieldLabel / RuleFieldHint 是同一份定义。
  struct FieldRow {
    const char* key;
    bp::RuleField field;
  };
  const FieldRow kFieldRows[] = {
      {"ext", bp::RuleField::kExt},     {"name", bp::RuleField::kName},
      {"path", bp::RuleField::kPath},   {"stem", bp::RuleField::kStem},
      {"type", bp::RuleField::kType},   {"size", bp::RuleField::kSize},
      {"uid", bp::RuleField::kUid},     {"gid", bp::RuleField::kGid},
      {"mtime", bp::RuleField::kMtime}, {"user", bp::RuleField::kUser},
      {"group", bp::RuleField::kGroup},
  };
  QVariantList fields;
  for (const FieldRow& row : kFieldRows) {
    QVariantMap option;
    option.insert(QStringLiteral("key"), QString::fromLatin1(row.key));
    option.insert(QStringLiteral("label"),
                  QString::fromUtf8(bp::RuleFieldLabel(row.field)));
    option.insert(QStringLiteral("hint"),
                  QString::fromUtf8(bp::RuleFieldHint(row.field)));
    option.insert(QStringLiteral("dslKey"),
                  QString::fromLatin1(bp::RuleFieldName(row.field)));
    fields.push_back(option);
  }

  // ---- type 的 7 个取值 ----
  const bp::RuleTypeValue kTypes[] = {
      bp::RuleTypeValue::kFile,       bp::RuleTypeValue::kFolder,
      bp::RuleTypeValue::kSymlink,    bp::RuleTypeValue::kFifo,
      bp::RuleTypeValue::kCharDevice, bp::RuleTypeValue::kBlockDevice,
      bp::RuleTypeValue::kSocket,
  };
  QVariantList types;
  for (const bp::RuleTypeValue type : kTypes) {
    QVariantMap option;
    option.insert(QStringLiteral("key"), QString::fromLatin1(TypeText(type)));
    option.insert(QStringLiteral("label"),
                  QString::fromUtf8(bp::TypeValueLabel(type)));
    types.push_back(option);
  }

  // ---- size 的比较方式 ----
  //
  // 顺序刻意做成"日常先用的在上"：小于 / 小于等于 / 等于 / 大于等于 / 大于，
  // 区间放在最后。键就是表单键，也是 size 的 DSL 运算符（"等于"由 builder
  // 展开成 a..a，核心的 size 没有单独的 "="）。
  const bp::RuleSizeCompare kSizeCompares[] = {
      bp::RuleSizeCompare::kLess,    bp::RuleSizeCompare::kLessEqual,
      bp::RuleSizeCompare::kEqual,   bp::RuleSizeCompare::kGreaterEqual,
      bp::RuleSizeCompare::kGreater, bp::RuleSizeCompare::kRange,
  };
  QVariantList size_compares;
  for (const bp::RuleSizeCompare compare : kSizeCompares) {
    QVariantMap option;
    option.insert(QStringLiteral("key"),
                  QString::fromLatin1(CompareText(compare)));
    option.insert(QStringLiteral("label"),
                  QString::fromUtf8(bp::SizeCompareLabel(compare)));
    size_compares.push_back(option);
  }

  // ---- uid / gid 的比较方式（独立的一组键，不复用符号键）----
  struct IdRow {
    const char* key;
    bp::RuleSizeCompare compare;
  };
  const IdRow kIdRows[] = {
      {"eq", bp::RuleSizeCompare::kEqual},
      {"lt", bp::RuleSizeCompare::kLess},
      {"le", bp::RuleSizeCompare::kLessEqual},
      {"gt", bp::RuleSizeCompare::kGreater},
      {"ge", bp::RuleSizeCompare::kGreaterEqual},
      {"range", bp::RuleSizeCompare::kRange},
  };
  QVariantList id_compares;
  for (const IdRow& row : kIdRows) {
    QVariantMap option;
    option.insert(QStringLiteral("key"), QString::fromLatin1(row.key));
    option.insert(QStringLiteral("label"),
                  QString::fromUtf8(bp::SizeCompareLabel(row.compare)));
    id_compares.push_back(option);
  }

  // ---- 大小单位（1024 进制，与 builder 的 RuleSizeUnit 一一对应）----
  const bp::RuleSizeUnit kUnits[] = {
      bp::RuleSizeUnit::kByte,
      bp::RuleSizeUnit::kKilo,
      bp::RuleSizeUnit::kMega,
      bp::RuleSizeUnit::kGiga,
  };
  QVariantList units;
  for (const bp::RuleSizeUnit unit : kUnits) {
    QVariantMap option;
    option.insert(QStringLiteral("key"),
                  QString::fromLatin1(bp::SizeUnitLabel(unit)));
    option.insert(QStringLiteral("label"),
                  QString::fromUtf8(bp::SizeUnitLabel(unit)));
    units.push_back(option);
  }

  // ---- 修改时间的 5 种形态 ----
  struct MtimeRow {
    const char* key;
    bp::RuleMtimeKind kind;
  };
  const MtimeRow kMtimeRows[] = {
      {"today", bp::RuleMtimeKind::kToday},
      {"yesterday", bp::RuleMtimeKind::kYesterday},
      {"last_days", bp::RuleMtimeKind::kLastDays},
      {"day", bp::RuleMtimeKind::kDay},
      {"day_range", bp::RuleMtimeKind::kDayRange},
  };
  QVariantList mtime_kinds;
  for (const MtimeRow& row : kMtimeRows) {
    QVariantMap option;
    option.insert(QStringLiteral("key"), QString::fromLatin1(row.key));
    option.insert(QStringLiteral("label"),
                  QString::fromUtf8(bp::MtimeKindLabel(row.kind)));
    mtime_kinds.push_back(option);
  }

  // 表结构与 QML 的绑定一一对应；新增一个字段 / 类型 / 单位时只改上面六个
  // 数组即可，六个下拉框会一起更新。
  QVariantMap options;
  options.insert(QStringLiteral("fields"), fields);
  options.insert(QStringLiteral("types"), types);
  options.insert(QStringLiteral("sizeCompares"), size_compares);
  options.insert(QStringLiteral("idCompares"), id_compares);
  options.insert(QStringLiteral("sizeUnits"), units);
  options.insert(QStringLiteral("mtimeKinds"), mtime_kinds);
  return options;
}

// 从保存下来的 include / exclude 文本列表重建草稿（打开设置页、应用预设时
// 用）。每一条都按 raw_dsl 处理：存下来的就是 DSL 原文，不再反过来猜它当初
// 是哪个表单字段填出来的。
bool FilterRuleModel::setRules(const QStringList& include_rules,
                               const QStringList& exclude_rules) {
  // 先在**副本**上全部校验通过，再整体替换：半份新规则比旧规则更糟——
  // 用户看到的列表会既不是他保存的那份，也不是他刚填的那份。
  std::vector<bp::FilterRuleDraft> next;
  next.reserve(
      static_cast<std::size_t>(include_rules.size() + exclude_rules.size()));
  const auto append = [&next](const QStringList& rules,
                              bp::FilterAction action) -> QString {
    for (const QString& text : rules) {
      bp::FilterRuleDraft draft;
      draft.action = action;
      draft.raw_dsl = text.toStdString();
      std::string error;
      if (!bp::ValidateRule(draft, &error)) {
        return QString::fromStdString(error);
      }
      next.push_back(draft);
    }
    return QString();
  };
  const QString include_error =
      append(include_rules, bp::FilterAction::kInclude);
  if (!include_error.isEmpty()) {
    SetError(include_error);
    return false;
  }
  const QString exclude_error =
      append(exclude_rules, bp::FilterAction::kExclude);
  if (!exclude_error.isEmpty()) {
    SetError(exclude_error);
    return false;
  }
  drafts_ = next;
  SyncController();
  RebuildRules();
  clearError();
  emit rulesChanged();
  return true;
}

// 取出某一动作下的全部 DSL 文本（保存设置时用）。除 "exclude" 以外的
// action 都按 include 处理，与 DraftFromForm 的默认值规则保持一致。
QStringList FilterRuleModel::rulesForAction(const QString& action) const {
  const bool want_include = action != QStringLiteral("exclude");
  QStringList texts;
  for (const bp::FilterRuleDraft& draft : drafts_) {
    const bool is_include = draft.action == bp::FilterAction::kInclude;
    if (is_include != want_include) continue;
    std::string dsl;
    if (!bp::ToDsl(draft, &dsl, nullptr)) continue;
    texts << QString::fromStdString(dsl);
  }
  return texts;
}

// 表单实时预览用：非法草稿返回空串，由 QML 决定"还没填完"怎么显示。
QString FilterRuleModel::dslForForm(const QVariantMap& form) const {
  bp::FilterRuleDraft draft;
  QString error;
  if (!DraftFromForm(form, &draft, &error)) return QString();
  std::string dsl;
  if (!bp::ToDsl(draft, &dsl, nullptr)) return QString();
  return QString::fromStdString(dsl);
}

// 表单实时预览的人话版本，与 dslForForm 走同一条草稿构造路径。
QString FilterRuleModel::summaryForForm(const QVariantMap& form) const {
  bp::FilterRuleDraft draft;
  QString error;
  if (!DraftFromForm(form, &draft, &error)) return QString();
  return QString::fromStdString(bp::Summarize(draft));
}

// 只回答"能不能加"：合法返回空串，否则返回中文原因（可直接显示在表单下方）。
QString FilterRuleModel::validateForm(const QVariantMap& form) const {
  bp::FilterRuleDraft draft;
  QString error;
  if (!DraftFromForm(form, &draft, &error)) return error;
  return QString();
}

// 追加一条表单规则。顺序是"先翻译、再入库、最后同步投影"：翻译失败时
// drafts_ 一个字节都没变，不需要回滚。
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

// 追加一条手写 DSL 规则。与 addRule 的差别是不经过表单翻译，直接校验原文；
// 校验走的是真实 Filter，因此高级入口与表单入口的合法性标准完全一致。
bool FilterRuleModel::addAdvancedRule(const QString& action,
                                      const QString& dsl) {
  const bp::FilterAction filter_action = action == QStringLiteral("exclude")
                                             ? bp::FilterAction::kExclude
                                             : bp::FilterAction::kInclude;
  const QString reason = validateDsl(action, dsl);
  if (!reason.isEmpty()) {
    SetError(reason);
    return false;
  }
  bp::FilterRuleDraft draft;
  draft.action = filter_action;
  draft.raw_dsl = dsl.toStdString();
  drafts_.push_back(draft);
  SyncController();
  RebuildRules();
  clearError();
  emit rulesChanged();
  return true;
}

// 校验一段手写 DSL，不改变任何状态。单独开一个 const 方法是为了让"边打字
// 边校验"和"确认添加"走同一条判定路径，不会出现能校验通过却添加失败。
QString FilterRuleModel::validateDsl(const QString& action,
                                     const QString& dsl) const {
  const bp::FilterAction filter_action = action == QStringLiteral("exclude")
                                             ? bp::FilterAction::kExclude
                                             : bp::FilterAction::kInclude;
  bp::FilterRuleDraft draft;
  draft.action = filter_action;
  draft.raw_dsl = dsl.toStdString();
  std::string error;
  if (!bp::ValidateRule(draft, &error)) return QString::fromStdString(error);
  return QString();
}

// 越界索引返回 false 而不是抛异常：QML 侧的索引来自列表选中项，删除与
// 选中之间存在天然的时序窗口，这里必须能容忍过期索引。
bool FilterRuleModel::isAdvancedRule(int index) const {
  if (index < 0 || index >= static_cast<int>(drafts_.size())) return false;
  return !drafts_[static_cast<std::size_t>(index)].raw_dsl.empty();
}

// 删除一条规则。越界索引是静默无操作：调用方可能持有已经失效的选中项。
void FilterRuleModel::removeRule(int index) {
  if (index < 0 || index >= static_cast<int>(drafts_.size())) return;
  drafts_.erase(drafts_.begin() + index);
  SyncController();
  RebuildRules();
  emit rulesChanged();
}

// 上移 / 下移一条规则，delta 通常是 -1 或 +1。
// index 与 target 都必须落在范围内才交换：只做一次 std::swap，不做旋转，
// 因此越界时宁可什么都不做，也不会把别的规则挪位。
void FilterRuleModel::moveRule(int index, int delta) {
  const int target = index + delta;
  if (index < 0 || index >= static_cast<int>(drafts_.size())) return;
  if (target < 0 || target >= static_cast<int>(drafts_.size())) return;
  std::swap(drafts_[index], drafts_[target]);
  SyncController();
  RebuildRules();
  emit rulesChanged();
}

// 清空全部规则，等价于"备份所有内容"。同样要同步控制器与展示行，否则界面
// 已经空了、控制器里还留着旧规则。
void FilterRuleModel::clearRules() {
  drafts_.clear();
  SyncController();
  RebuildRules();
  emit rulesChanged();
}

// 拼出可以直接粘贴到 shell 的参数串。值一律加单引号、选项名不加：规则里
// 常有 '*'、'?'、空格（name:my report.txt），不加引号会被 shell 吃掉。
// 这里只做展示，不负责转义单引号本身——含单引号的规则粘出去要用户自己改，
// 与其在界面里做半套 shell 转义，不如让文本保持所见即所得。
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

// 清空错误。内容没变时不发信号：QML 的属性绑定会被无意义地重算一遍。
void FilterRuleModel::clearError() {
  if (last_error_.isEmpty()) return;
  last_error_.clear();
  emit lastErrorChanged();
}

// 设置错误，同样做了去重。注意它只记录，不清预览结果——清理由调用方决定：
// 表单校验失败时保留上一次预览是合理的，扫描失败时才必须清。
void FilterRuleModel::SetError(const QString& message) {
  if (last_error_ == message) return;
  last_error_ = message;
  emit lastErrorChanged();
}

// 规则列表是唯一来源，控制器只是它的投影：每次改动都整体重放一遍，
// 这样两边的顺序与内容不可能不一致，也不需要索引映射。
// 控制器只认 DSL 文本、不认草稿，所以这里做一次整体重放：先清空再按顺序
// 重新添加，顺序与 drafts_ 逐条对应。
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

// 把 drafts_ 投影成 QML 绑定的 rules_。整表重建而不是增量更新：规则数量是
// 个位数，重建的代价可以忽略，换来的是"界面永远等于 drafts_"这条不变量。
void FilterRuleModel::RebuildRules() {
  rules_.clear();
  for (const bp::FilterRuleDraft& draft : drafts_) {
    std::string dsl;
    bp::ToDsl(draft, &dsl, nullptr);
    QVariantMap item;
    const bool is_include = draft.action == bp::FilterAction::kInclude;
    item.insert(QStringLiteral("action"), is_include
                                              ? QStringLiteral("include")
                                              : QStringLiteral("exclude"));
    // 主行是给人看的人话："包含 · 文件扩展名：cpp、h"。动作名与条件名都来自
    // 共享 builder 的中文表，界面不再自己拼一套术语（也就不会出现"一处叫
    // Include、一处叫包含规则"）。
    item.insert(QStringLiteral("actionLabel"),
                is_include ? QStringLiteral("包含") : QStringLiteral("排除"));
    item.insert(QStringLiteral("conditionLabel"),
                QString::fromStdString(bp::SummarizeShort(draft)));
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

// 预览入口。恢复模式（restore_path 非空）不重新筛选，直接给出说明性错误：
// 恢复的是已经归档的内容，再筛一遍只会误导用户以为恢复会重新选文件。
void FilterRuleModel::requestPreview(const QString& source_path,
                                     const QString& restore_path) {
  if (source_path.isEmpty()) {
    SetError(restore_path.isEmpty()
                 ? QStringLiteral("请先填写源目录，再刷新预览。")
                 : QStringLiteral("恢复不重新筛选，无需预览。"));
    return;
  }
  clearError();
  // 记下最新一次请求；正在扫描时不排队第二次，
  // 等当前这次结束立刻用最新参数重扫。
  pending_source_ = source_path;
  pending_drafts_ = drafts_;
  // 正在扫描时只记下"还有新请求"，不排队：用户连点刷新时真正想要的是最后
  // 那一次的结果，中间态没有展示价值。
  if (preview_busy_) {
    preview_pending_ = true;
    return;
  }
  StartScan(pending_source_, pending_drafts_);
}

// 启动一次后台扫描。走到这里 preview_pending_ 一定已经复位，busy 由本函数
// 置位，因此"busy 为真但没有人会复位它"的状态不存在。
void FilterRuleModel::StartScan(
    const QString& source_path,
    const std::vector<backupproject::FilterRuleDraft>& drafts) {
  preview_busy_ = true;
  preview_pending_ = false;
  emit previewChanged();
  // 值捕获是刻意的：后台线程只拿到字符串与草稿副本，不捕获 this，所以窗口
  // 销毁、模型析构都不会与扫描线程打架；结果经 watcher_ 回到 GUI 线程。
  watcher_.setFuture(QtConcurrent::run([source_path, drafts]() {
    return ScanPreview(source_path, drafts, kPreviewLimit);
  }));
}

// 扫描本身在共享核心 backupproject::PreviewBackupSelection 里（CLI 预览用的是
// 同一个函数），这里只把结果翻成 QML 能绑定的 QVariantMap。所以"GUI 预览与
// CLI 预览看到不同的集合"在结构上不可能发生——两边是同一次调用。
// 扫描线程里的全部工作。本函数是静态的、不访问任何成员，因此可以安全地在
// 后台线程运行；它唯一的输入是路径与草稿副本，唯一的输出是纯数据的结构体。
// limit 为 0 时退回核心的默认条目上限，避免把"不限制"解释成"扫描全部"。
FilterRuleModel::PreviewOutcome FilterRuleModel::ScanPreview(
    const QString& source_path, const std::vector<bp::FilterRuleDraft>& drafts,
    int limit) {
  PreviewOutcome outcome;
  outcome.source_path = source_path;
  const std::size_t window =
      limit > 0 ? static_cast<std::size_t>(limit) : bp::kPreviewEntryLimit;
  const bp::PreviewResult result =
      bp::PreviewBackupSelection(source_path.toStdString(), drafts, window);
  if (!result.error.empty()) {
    outcome.error_kind = result.error_kind;
    if (result.error_kind == bp::PreviewErrorKind::kSourceUnusable) {
      // 核心只报事实，中文措辞留在界面这一层。
      outcome.error = QStringLiteral("源目录不存在或不是目录：") + source_path;
    } else {
      // 其余失败一律转述核心原文，一个字都不改：
      //   * kRuleRejected / kScanFailed / kSelectionBlocked 的诊断来自共享
      //     核心，翻译只会让它和 CLI、和真实 Backup 说出来的话不一样；
      //   * kSelectionBlocked 的那句话就是"备份会怎么失败"，界面里最该原样
      //     看到的就是它。
      outcome.error = QString::fromStdString(result.error);
    }
    return outcome;
  }
  outcome.truncated = result.truncated;
  outcome.total = static_cast<int>(result.total_entries);
  outcome.included = static_cast<int>(result.included_count);
  outcome.items.reserve(static_cast<int>(result.items.size()));
  for (const bp::PreviewItem& preview : result.items) {
    QVariantMap item;
    item.insert(QStringLiteral("path"),
                QString::fromStdString(preview.archive_path));
    item.insert(QStringLiteral("isDirectory"), preview.is_directory);
    item.insert(QStringLiteral("size"),
                preview.type == bp::EntryType::kRegularFile
                    ? FormatSize(preview.size)
                    : QString());
    item.insert(QStringLiteral("mtime"),
                preview.mtime_sec > 0
                    ? QDateTime::fromSecsSinceEpoch(
                          static_cast<qint64>(preview.mtime_sec))
                          .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                    : QString());
    item.insert(QStringLiteral("included"), preview.included);
    item.insert(QStringLiteral("tag"), PreviewTag(preview));
    outcome.items.push_back(item);
  }
  return outcome;
}

}  // namespace backup_modern

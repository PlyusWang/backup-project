// filter_rule_model.cpp
#include "filter_rule_model.h"

#include <QDateTime>
#include <QtConcurrent>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

namespace backup_modern {
namespace {

namespace bp = backupproject;

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

// 预览扫描在后台线程执行：先按当前草稿构造真实 Filter，再逐条问它。
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
    return QStringLiteral("没有规则：按 PR #8 行为备份全部内容。");
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
  const QString action = form.value(QStringLiteral("action")).toString();
  const QString field = form.value(QStringLiteral("field")).toString();
  draft->action = action == QStringLiteral("exclude")
                      ? bp::FilterAction::kExclude
                      : bp::FilterAction::kInclude;
  bp::FilterClauseDraft clause;
  if (field == QStringLiteral("name") || field == QStringLiteral("path") ||
      field == QStringLiteral("stem")) {
    clause.field =
        field == QStringLiteral("path")
            ? bp::RuleField::kPath
            : (field == QStringLiteral("stem") ? bp::RuleField::kStem
                                               : bp::RuleField::kName);
    clause.pattern =
        form.value(QStringLiteral("pattern")).toString().toStdString();
  } else if (field == QStringLiteral("ext")) {
    clause.field = bp::RuleField::kExt;
    const QString raw = form.value(QStringLiteral("extensions")).toString();
    const QStringList pieces = raw.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& piece : pieces)
      clause.extensions.push_back(piece.toStdString());
  } else if (field == QStringLiteral("type")) {
    clause.field = bp::RuleField::kType;
    clause.type = form.value(QStringLiteral("type")).toString() ==
                          QStringLiteral("folder")
                      ? bp::RuleTypeValue::kFolder
                      : bp::RuleTypeValue::kFile;
  } else if (field == QStringLiteral("size")) {
    clause.field = bp::RuleField::kSize;
    clause.compare =
        CompareFromText(form.value(QStringLiteral("compare")).toString());
    clause.unit = UnitFromText(form.value(QStringLiteral("unit")).toString());
    clause.size_low = form.value(QStringLiteral("sizeLow")).toULongLong();
    clause.size_high = form.value(QStringLiteral("sizeHigh")).toULongLong();
  } else {
    if (error != nullptr) *error = QStringLiteral("未知字段：") + field;
    return false;
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
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    if (static_cast<int>(outcome.items.size()) >= limit) {
      outcome.truncated = true;
      break;
    }
    const fs::directory_entry& entry = *it;
    const fs::path& path = entry.path();
    std::error_code status_ec;
    const fs::file_status status = entry.symlink_status(status_ec);
    if (status_ec) continue;

    const bool is_dir = fs::is_directory(status);
    const bool is_regular = fs::is_regular_file(status);
    bp::FilterEntry fe;
    fe.archive_path = path.lexically_relative(root).generic_string();
    fe.name = path.filename().string();
    fe.is_directory = is_dir;
    if (is_regular) {
      std::error_code size_ec;
      fe.size = fs::file_size(path, size_ec);
    }
    std::error_code time_ec;
    const fs::file_time_type file_time = fs::last_write_time(path, time_ec);
    if (!time_ec) {
      const auto sys_time =
          std::chrono::time_point_cast<std::chrono::system_clock::duration>(
              file_time - fs::file_time_type::clock::now() +
              std::chrono::system_clock::now());
      fe.mtime_sec = std::chrono::system_clock::to_time_t(sys_time);
    }

    bool included = false;
    QString tag;
    if (is_dir) {
      if (filter.ShouldPruneDirectory(fe)) {
        included = false;
        tag = QStringLiteral("目录被排除（整棵剪掉）");
        it.disable_recursion_pending();
      } else {
        included = true;
        tag = QStringLiteral("目录（保留结构）");
      }
    } else if (is_regular) {
      included = filter.ShouldIncludeFile(fe);
      tag =
          included ? QStringLiteral("进入归档") : QStringLiteral("被规则排除");
    } else {
      const bool skipped = filter.ShouldSkipSpecialEntry(fe);
      included = false;
      tag = skipped ? QStringLiteral("特殊文件（已被规则排除）")
                    : QStringLiteral("特殊文件（会导致备份失败）");
    }

    QVariantMap item;
    item.insert(QStringLiteral("path"),
                QString::fromStdString(fe.archive_path));
    item.insert(QStringLiteral("isDirectory"), is_dir);
    item.insert(QStringLiteral("size"),
                is_regular ? FormatSize(fe.size) : QString());
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

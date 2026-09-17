// filter_rule_model.h
//
// 可视化规则编辑器的 Qt 桥。
//
// 它只负责：把 QML 表单原样转成 FilterRuleDraft、调用 FilterRuleBuilder 做
// 序列化/摘要/表单校验、把最终 DSL 交给 BackupController（进而交给真实的
// Filter::AddRule 与 BackupEngine）、以及在后台线程做预览扫描。
//
// 它不定义 DSL、不做 glob 匹配、不判断后缀或大小 —— 这些全部在
// backupproject::Filter 里，GUI 只有一条语义来源。

#ifndef BACKUP_PROJECT_UI_MODERN_FILTER_RULE_MODEL_H_
#define BACKUP_PROJECT_UI_MODERN_FILTER_RULE_MODEL_H_

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

#include "backup_controller.h"
#include "filter_rule_builder.h"

namespace backup_modern {

class FilterRuleModel : public QObject {
  Q_OBJECT
  // rules_ 是给列表显示的镜像：[{action, summary, dsl}]
  Q_PROPERTY(QVariantList rules READ rules NOTIFY rulesChanged)
  Q_PROPERTY(QString summaryText READ summaryText NOTIFY rulesChanged)
  Q_PROPERTY(QString dslText READ dslText NOTIFY rulesChanged)
  Q_PROPERTY(QVariantList previewItems READ previewItems NOTIFY previewChanged)
  Q_PROPERTY(bool previewBusy READ previewBusy NOTIFY previewChanged)
  Q_PROPERTY(bool previewTruncated READ previewTruncated NOTIFY previewChanged)
  Q_PROPERTY(int previewShown READ previewShown NOTIFY previewChanged)
  Q_PROPERTY(int previewLimit READ previewLimit CONSTANT)
  Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

 public:
  explicit FilterRuleModel(BackupController* controller,
                           QObject* parent = nullptr);

  QVariantList rules() const { return rules_; }
  QString summaryText() const;
  QString dslText() const;
  QVariantList previewItems() const { return preview_items_; }
  bool previewBusy() const { return preview_busy_; }
  bool previewTruncated() const { return preview_truncated_; }
  int previewShown() const { return static_cast<int>(preview_items_.size()); }
  int previewLimit() const { return kPreviewLimit; }
  QString lastError() const { return last_error_; }

  // 表单 -> DSL / 摘要（只读，用来做实时预览，不改动规则列表）
  Q_INVOKABLE QString dslForForm(const QVariantMap& form) const;
  Q_INVOKABLE QString summaryForForm(const QVariantMap& form) const;
  // 表单校验：返回空串表示合法，否则返回具体原因（交给界面标红）
  Q_INVOKABLE QString validateForm(const QVariantMap& form) const;

  Q_INVOKABLE bool addRule(const QVariantMap& form);
  Q_INVOKABLE void removeRule(int index);
  Q_INVOKABLE void moveRule(int index, int delta);
  Q_INVOKABLE void clearRules();
  Q_INVOKABLE QString cliArguments() const;
  Q_INVOKABLE void clearError();
  // 后台扫描源目录并标注 Included / Excluded；结果只用于展示，
  // 真正备份时由 BackupEngine 重新走完整 Filter 流程。
  Q_INVOKABLE void requestPreview(const QString& source_path,
                                  const QString& restore_path);

 signals:
  void rulesChanged();
  void previewChanged();
  void lastErrorChanged();

 private:
  struct PreviewOutcome {
    QVariantList items;
    bool truncated = false;
    QString error;
  };
  static PreviewOutcome ScanPreview(
      const QString& source_path,
      const std::vector<backupproject::FilterRuleDraft>& drafts, int limit);
  bool DraftFromForm(const QVariantMap& form,
                     backupproject::FilterRuleDraft* draft,
                     QString* error) const;
  void SyncController();
  void RebuildRules();
  void SetError(const QString& message);

  static constexpr int kPreviewLimit = 300;

  BackupController* controller_ = nullptr;
  std::vector<backupproject::FilterRuleDraft> drafts_;
  QVariantList rules_;
  QVariantList preview_items_;
  bool preview_busy_ = false;
  bool preview_truncated_ = false;
  QString last_error_;
  QFutureWatcher<PreviewOutcome> watcher_;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_FILTER_RULE_MODEL_H_

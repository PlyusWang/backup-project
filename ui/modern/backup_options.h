// ui/modern/backup_options.h
//
// 三种备份方式共用的选项目录与校验入口。
//
// 为什么需要它：打包 / 压缩 / 加密的显示名、合法组合与拒绝原因以前分散在
// 三个 QML 页面与各自的控制器里。同一个“增量 + AES”在三个页面可能得到三句
// 不同的话，甚至一个页面放行、另一个页面拒绝。
//
// 这里只做两件事：
//   * 给出三组下拉的显示文案（与 backup_option_keys.h 的 key 一一对应）；
//   * 把用户选择交给共享真值表 IsSupportedBackupOptionCombination，
//     并把英文原因翻译成一句可展示的中文。
// 真正的密钥校验、口令读取、归档行为仍全部在共享核心里。

#ifndef BACKUP_PROJECT_UI_MODERN_BACKUP_OPTIONS_H_
#define BACKUP_PROJECT_UI_MODERN_BACKUP_OPTIONS_H_

#include <QObject>
#include <QString>
#include <QStringList>

namespace backup_modern {

class BackupOptionsCatalog : public QObject {
  Q_OBJECT
  Q_PROPERTY(QStringList triggerLabels READ triggerLabels CONSTANT)
  Q_PROPERTY(QStringList strategyLabels READ strategyLabels CONSTANT)
  Q_PROPERTY(QStringList packLabels READ packLabels CONSTANT)
  Q_PROPERTY(QStringList compressionLabels READ compressionLabels CONSTANT)
  Q_PROPERTY(QStringList encryptionLabels READ encryptionLabels CONSTANT)

 public:
  explicit BackupOptionsCatalog(QObject* parent = nullptr);

  QStringList triggerLabels() const;
  QStringList strategyLabels() const;
  QStringList packLabels() const;
  QStringList compressionLabels() const;
  QStringList encryptionLabels() const;

  // 返回空串表示这组选项可用；否则是一句中文原因。
  // credentialConfigured 只在 scheduled / realtime + 加密时有意义。
  Q_INVOKABLE QString validate(const QString& trigger,
                               const QString& strategy, const QString& pack,
                               const QString& compression,
                               const QString& encryption,
                               bool credentialConfigured) const;

  // 是否需要“仅本人可读的口令文件”。manual 永远返回 false。
  Q_INVOKABLE bool needsCredentialFile(const QString& trigger,
                                       const QString& encryption) const;

  // 增量策略的能力边界说明（随策略变化）。
  Q_INVOKABLE QString strategyNote(const QString& strategy) const;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_BACKUP_OPTIONS_H_

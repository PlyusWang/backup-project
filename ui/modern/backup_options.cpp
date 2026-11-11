// ui/modern/backup_options.cpp

#include "backup_options.h"

#include <string>

#include "backup_mode.h"
#include "backup_option_keys.h"
#include "incremental_backup.h"

namespace backup_modern {
namespace {

using backupproject::BackupOptionCombination;
using backupproject::BackupStrategy;
using backupproject::BackupTrigger;
using backupproject::CompressionMethod;
using backupproject::EncryptionMethod;
using backupproject::PackMethod;

}  // namespace

BackupOptionsCatalog::BackupOptionsCatalog(QObject* parent) : QObject(parent) {}

QStringList BackupOptionsCatalog::triggerLabels() const {
  return {QStringLiteral("手动"), QStringLiteral("定时"),
          QStringLiteral("实时")};
}

QStringList BackupOptionsCatalog::strategyLabels() const {
  return {QStringLiteral("完整备份"), QStringLiteral("增量备份")};
}

QStringList BackupOptionsCatalog::packLabels() const {
  return {QStringLiteral("MyPack（推荐）"), QStringLiteral("USTAR（标准格式）"),
          QStringLiteral("Fast USTAR（高吞吐）")};
}

QStringList BackupOptionsCatalog::compressionLabels() const {
  return {QStringLiteral("不压缩"), QStringLiteral("Huffman"),
          QStringLiteral("Zstd")};
}

QStringList BackupOptionsCatalog::encryptionLabels() const {
  return {QStringLiteral("不加密"), QStringLiteral("AES-256-CTR + HMAC-SHA256"),
          QStringLiteral("DES-CBC + HMAC-SHA256（教学用）")};
}

bool BackupOptionsCatalog::needsCredentialFile(
    const QString& trigger, const QString& encryption) const {
  const bool unattended = trigger == QStringLiteral("scheduled") ||
                          trigger == QStringLiteral("realtime");
  return unattended && encryption != QStringLiteral("none");
}

QString BackupOptionsCatalog::strategyNote(const QString& strategy) const {
  if (strategy == QStringLiteral("incremental")) {
    return QStringLiteral(
        "第一次（或基线不可信时）建立完整基线，之后只写变化部分；"
        "没有变化就不创建新快照。支持 MyPack、USTAR 和 Fast USTAR 打包格式。");
  }
  return QStringLiteral("每次备份都生成一份完整、自包含的归档。");
}

QString BackupOptionsCatalog::validate(const QString& trigger,
                                       const QString& strategy,
                                       const QString& pack,
                                       const QString& compression,
                                       const QString& encryption,
                                       bool credential_configured) const {
  BackupTrigger parsed_trigger = BackupTrigger::kManual;
  BackupStrategy parsed_strategy = BackupStrategy::kFull;
  PackMethod parsed_pack = PackMethod::kMyPack;
  CompressionMethod parsed_compression = CompressionMethod::kNone;
  EncryptionMethod parsed_encryption = EncryptionMethod::kNone;
  if (!backupproject::ParseBackupTriggerKey(trigger.toStdString(),
                                            &parsed_trigger) ||
      !backupproject::ParseBackupStrategyKey(strategy.toStdString(),
                                             &parsed_strategy) ||
      !backupproject::ParsePackMethodKey(pack.toStdString(), &parsed_pack) ||
      !backupproject::ParseCompressionMethodKey(compression.toStdString(),
                                                &parsed_compression) ||
      !backupproject::ParseEncryptionMethodKey(encryption.toStdString(),
                                               &parsed_encryption)) {
    return QStringLiteral("选项组合无法识别，请重新选择。");
  }
  BackupOptionCombination combination;
  combination.trigger = parsed_trigger;
  combination.strategy = parsed_strategy;
  combination.pack_method = parsed_pack;
  combination.compression_method = parsed_compression;
  combination.encryption_method = parsed_encryption;
  combination.unattended_credential_available = credential_configured;
  if (backupproject::IsSupportedBackupOptionCombination(combination)) {
    return QString();
  }
  // 翻译成用户能理解的一句原因。判定本身仍然只来自共享真值表。
  if (parsed_strategy == BackupStrategy::kIncremental &&
      !backupproject::IsSupportedIncrementalPack(parsed_pack)) {
    return QStringLiteral("请选择 MyPack、USTAR 或 Fast USTAR 打包格式。");
  }
  const bool unattended = parsed_trigger == BackupTrigger::kScheduled ||
                          parsed_trigger == BackupTrigger::kRealtime;
  if (unattended && parsed_encryption != EncryptionMethod::kNone &&
      !credential_configured) {
    return QStringLiteral(
        "请先输入并确认归档密码，或在高级设置中选择口令文件。");
  }
  return QStringLiteral("当前选项组合不可用。");
}

}  // namespace backup_modern

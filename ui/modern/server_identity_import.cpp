// server_identity_import.cpp

#include "server_identity_import.h"

#include <QFile>

#include "bpcert.h"
#include "trusted_root_store.h"

namespace backup_modern {

bool ServerIdentityImport::importCertificate(const QString& roots_path,
                                             const QString& certificate_path) {
  server_id_.clear();
  const auto fail = [this](const QString& message) {
    message_ = message;
    emit resultChanged();
    return false;
  };
  if (roots_path.trimmed().isEmpty()) {
    return fail(QStringLiteral("请先选择管理员提供的可信根文件。"));
  }
  QFile roots(roots_path);
  if (!roots.open(QIODevice::ReadOnly) || roots.size() > 65536) {
    return fail(QStringLiteral("无法读取可信根文件，请重新选择。"));
  }
  backupproject::crypto::TrustedRootStore store;
  std::string error;
  const QByteArray root_bytes = roots.read(65537);
  if (root_bytes.size() > 65536) {
    return fail(QStringLiteral("可信根文件过大，请重新选择。"));
  }
  if (!backupproject::crypto::TrustedRootStore::LoadFromText(
          root_bytes.toStdString(), &store, &error)) {
    return fail(QStringLiteral("可信根文件无效，请向服务器管理员获取。"));
  }
  QFile file(certificate_path);
  if (!file.open(QIODevice::ReadOnly)) {
    return fail(QStringLiteral("无法读取服务器身份文件，请重新选择。"));
  }
  const QByteArray bytes =
      file.read(backupproject::crypto::kBpcert1MaxCertificateSize + 1);
  if (bytes.size() > static_cast<qsizetype>(
                         backupproject::crypto::kBpcert1MaxCertificateSize)) {
    return fail(QStringLiteral("服务器身份文件过大，请选择 BPCERT1 证书。"));
  }
  const std::string raw = bytes.toStdString();
  std::string issuer;
  if (store.VerifyCertificate(raw, &issuer, &error) !=
      backupproject::crypto::Bpcert1Error::kOk) {
    return fail(QString::fromStdString(error));
  }
  backupproject::crypto::Bpcert1 certificate;
  if (backupproject::crypto::Bpcert1Parse(raw, &certificate) !=
      backupproject::crypto::Bpcert1Error::kOk) {
    return fail(QStringLiteral("服务器身份文件无效。"));
  }
  backupproject::crypto::Bpcert1Error validity;
  if (!backupproject::crypto::Bpcert1CheckValidity(
          certificate, backupproject::crypto::Bpcert1NowUnixSeconds(),
          &validity, &error)) {
    return fail(QString::fromStdString(error));
  }
  server_id_ = QString::fromStdString(certificate.server_id);
  message_ = QStringLiteral("已验证服务器身份：%1").arg(server_id_);
  emit resultChanged();
  return true;
}

}  // namespace backup_modern

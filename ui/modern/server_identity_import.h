// server_identity_import.h
//
// 服务器身份导入复用 BPSEC2 的证书验证；可信根由用户独立选择。

#ifndef BACKUP_PROJECT_UI_MODERN_SERVER_IDENTITY_IMPORT_H_
#define BACKUP_PROJECT_UI_MODERN_SERVER_IDENTITY_IMPORT_H_

#include <QObject>
#include <QString>

namespace backup_modern {

class ServerIdentityImport : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString serverId READ serverId NOTIFY resultChanged)
  Q_PROPERTY(QString message READ message NOTIFY resultChanged)

 public:
  explicit ServerIdentityImport(QObject* parent = nullptr) : QObject(parent) {}

  QString serverId() const { return server_id_; }
  QString message() const { return message_; }
  Q_INVOKABLE bool importCertificate(const QString& roots_path,
                                     const QString& certificate_path);

 signals:
  void resultChanged();

 private:
  QString server_id_;
  QString message_;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_SERVER_IDENTITY_IMPORT_H_

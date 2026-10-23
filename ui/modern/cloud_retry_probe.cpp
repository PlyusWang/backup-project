#include "cloud_retry_probe.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <set>

#include "backup_engine.h"
#include "cloud_retention.h"
#include "cloud_task.h"
#include "ownership_ledger.h"
#include "realtime_backup_service.h"
#include "remote_backup_client.h"
#include "remote_controller.h"
#include "schedule_store.h"
#include "scheduled_backup_service.h"
#include "snapshot_bundle.h"

namespace backup_modern {
namespace {

int g_failures = 0;
void Check(bool ok, const QString& what, const QString& detail = QString()) {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.toUtf8().constData(),
              detail.isEmpty() ? "" : " -- ", detail.toUtf8().constData());
  if (!ok) ++g_failures;
}

quint16 FreePort() {
  QTcpServer probe;
  if (!probe.listen(QHostAddress::LocalHost, 0)) return 0;
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

bool WaitPort(quint16 port, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < timeout_ms) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (socket.waitForConnected(100)) return true;
    QThread::msleep(50);
  }
  return false;
}

QString PinFromKeygen(const QString& text) {
  const QRegularExpression re(QStringLiteral("sha256:[0-9a-f]{64}"));
  const auto match = re.match(text);
  return match.hasMatch() ? match.captured(0) : QString();
}

bool WriteSecret(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  file.write("BACKUP_TOKEN_SECRET=0123456789abcdef0123456789abcdef\n");
  file.close();
  return QFile::setPermissions(
      path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QString ProxyScript() {
  return QStringLiteral(R"PY(
import socket, sys, threading
listen_port = int(sys.argv[1]); target_port = int(sys.argv[2])
server = socket.socket(); server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', listen_port)); server.listen(32)
first = True
def pipe(a, b):
    try:
        while True:
            data = a.recv(65536)
            if not data: break
            b.sendall(data)
    except Exception:
        pass
    finally:
        try: a.close()
        except Exception: pass
        try: b.close()
        except Exception: pass
while True:
    client, _ = server.accept()
    if first:
        first = False
        client.close()
        continue
    upstream = socket.create_connection(('127.0.0.1', target_port))
    threading.Thread(target=pipe, args=(client, upstream), daemon=True).start()
    threading.Thread(target=pipe, args=(upstream, client), daemon=True).start()
)PY");
}

}  // namespace

int RunCloudRetryProbe(const QString& work_root) {
  const QString dir =
      work_root.isEmpty()
          ? QDir::tempPath() + QStringLiteral("/cloud-retry-probe-") +
                QString::number(QCoreApplication::applicationPid()) + "-" +
                QString::number(QDateTime::currentMSecsSinceEpoch())
          : work_root;
  QDir().mkpath(dir);
  const QString key_path = dir + "/server.key";
  const QString secret_path = dir + "/secrets.env";
  const QString server_program =
      QCoreApplication::applicationDirPath() + "/backup-server";
  const QString keygen_program =
      QCoreApplication::applicationDirPath() + "/backup-server-keygen";
  const QString cli_program =
      QCoreApplication::applicationDirPath() + "/backupctl";
  if (!WriteSecret(secret_path)) {
    Check(false, "retry fixture secret");
    return 1;
  }
  QProcess keygen;
  keygen.start(keygen_program, {QStringLiteral("--output"), key_path});
  keygen.waitForFinished(30000);
  const QString keygen_text = QString::fromUtf8(keygen.readAllStandardOutput() +
                                                keygen.readAllStandardError());
  const QString pin = PinFromKeygen(keygen_text);
  if (pin.isEmpty()) {
    Check(false, "retry fixture keygen", keygen_text.left(200));
    return 1;
  }
  const quint16 server_port = FreePort();
  const quint16 proxy_port = FreePort();
  if (server_port == 0 || proxy_port == 0) return 1;
  QProcess server;
  server.start(server_program,
               {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"),
                QStringLiteral("--port"), QString::number(server_port),
                QStringLiteral("--root"), dir + "/root", QStringLiteral("--db"),
                dir + "/db", QStringLiteral("--secret-file"), secret_path,
                QStringLiteral("--transport-key-file"), key_path,
                QStringLiteral("--log-file"), dir + "/server.log",
                QStringLiteral("--quiet")});
  Check(server.waitForStarted(10000) && WaitPort(server_port, 15000),
        "retry fixture server started");
  auto start_proxy = [&]() {
    QProcess* proxy = new QProcess();
    proxy->start(QStringLiteral("python3"),
                 {QStringLiteral("-u"), QStringLiteral("-c"), ProxyScript(),
                  QString::number(proxy_port), QString::number(server_port)});
    if (!proxy->waitForStarted(5000) || !WaitPort(proxy_port, 5000)) {
      Check(false, "retry fixture proxy started");
    }
    return proxy;
  };
  QProcess* proxy = start_proxy();

  const std::string username = "retry_probe_user";
  const std::string password = "retry-probe-password";
  backupproject::net::RemoteEndpoint direct;
  direct.host = "127.0.0.1";
  direct.port = server_port;
  direct.server_key_pin = pin.toStdString();
  backupproject::net::RemoteArchiveClient client;
  std::string error;
  const bool registered = client.Connect(direct, &error) &&
                          client.Register(username, password, &error) &&
                          client.Login(username, password, &error);
  Check(registered, "retry fixture account", QString::fromStdString(error));

  auto write_creds = [&](quint16 port, const std::string& pass) {
    const QString path = dir + "/creds.env";
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return path;
    file.write(QStringLiteral("host=127.0.0.1\nport=%1\nusername=%2\n"
                              "password=%3\nidentity_mode=pin\npin=%4\n")
                   .arg(port)
                   .arg(QString::fromStdString(username))
                   .arg(QString::fromStdString(pass))
                   .arg(pin)
                   .toUtf8());
    file.close();
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return path;
  };

  const QString source = dir + "/source";
  QDir().mkpath(source);
  QFile source_file(source + "/data.txt");
  if (source_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    source_file.write("retry-payload\n");
  }
  source_file.close();

  const QString creds_proxy = write_creds(proxy_port, password);
  const QString schedule_path = dir + "/schedule.json";
  backupproject::ScheduleStore store(schedule_path.toStdString());
  backupproject::ScheduleDocument document;
  document.config.enabled = true;
  document.config.trigger = backupproject::BackupTrigger::kScheduled;
  document.config.strategy = backupproject::BackupStrategy::kFull;
  document.config.source_path = source.toStdString();
  document.config.interval_minutes = 60;
  document.config.retain_count = 4;
  document.config.pack_method = backupproject::PackMethod::kMyPack;
  document.config.compression_method = backupproject::CompressionMethod::kNone;
  document.config.repository_type = "cloud";
  document.config.cloud_credentials_file = creds_proxy.toStdString();
  {
    backupproject::CloudCredentials creds;
    std::string cred_error;
    backupproject::LoadCloudCredentialsFile(creds_proxy.toStdString(), &creds,
                                            &cred_error);
    document.config.cloud_repository_id =
        backupproject::CloudRepositoryId(creds);
  }
  std::string save_error;
  Check(store.Save(document, &save_error), "retry fixture schedule saved",
        QString::fromStdString(save_error));
  backupproject::ScheduledBackupService schedule_service(std::string(), &store);
  backupproject::ScheduleEvaluationResult result;
  std::string schedule_error;
  QElapsedTimer scheduled_timer;
  scheduled_timer.start();
  const bool scheduled_ok = schedule_service.EvaluateNow(
      static_cast<std::int64_t>(::time(nullptr)), &result, &schedule_error);
  Check(scheduled_ok &&
            result.status ==
                backupproject::ScheduleEvaluationStatus::kCreatedSnapshot,
        "D 定时：第一次网络失败、第二次成功",
        QString::fromStdString(schedule_error) +
            " elapsed=" + QString::number(scheduled_timer.elapsed()));
  std::vector<backupproject::net::RemoteSnapshotInfo> snapshots;
  const bool listed = client.List(&snapshots, &error);
  Check(listed && snapshots.size() == 1, "D 定时：服务端只有一份正确快照",
        QString::number(snapshots.size()));
  QString snapshot_id;
  QString archive_name;
  if (!snapshots.empty()) {
    snapshot_id = QString::fromStdString(snapshots[0].snapshot_id);
    archive_name = QString::fromStdString(snapshots[0].display_name);
  }
  const QString restore_dir = dir + "/restore";
  QDir().mkpath(restore_dir);
  const QString downloaded = dir + "/downloaded.bak";
  backupproject::net::RemoteSnapshotInfo downloaded_info;
  const bool downloaded_ok =
      !snapshot_id.isEmpty() &&
      client.DownloadArchiveFile(snapshot_id.toStdString(),
                                 downloaded.toStdString(), true,
                                 backupproject::net::RemoteProgressCallback(),
                                 &downloaded_info, &error);
  const QString extracted = dir + "/extracted";
  QDir().mkpath(extracted);
  backupproject::net::SnapshotBundleInfo bundle_info;
  std::string extract_error;
  const bool extracted_ok =
      downloaded_ok && backupproject::net::ExtractSnapshotBundle(
                           downloaded.toStdString(), extracted.toStdString(),
                           &bundle_info, &extract_error);
  backupproject::BackupEngine engine;
  std::string restore_error;
  const bool restore_ok =
      extracted_ok &&
      engine.Restore((extracted + QString::fromStdString("/") +
                      QString::fromStdString(bundle_info.archive_name))
                         .toStdString(),
                     restore_dir.toStdString(), &restore_error);
  QFile restored(restore_dir + "/data.txt");
  Check(restore_ok && restored.open(QIODevice::ReadOnly) &&
            restored.readAll() == QByteArray("retry-payload\n"),
        "D 定时：恢复内容一致",
        QString::fromStdString(extract_error + " / " + restore_error));

  // 实时：换一个同样只失败第一次的代理。
  proxy->kill();
  proxy->waitForFinished(3000);
  const quint16 proxy2_port = FreePort();
  QProcess proxy2;
  proxy2.start(QStringLiteral("python3"),
               {QStringLiteral("-u"), QStringLiteral("-c"), ProxyScript(),
                QString::number(proxy2_port), QString::number(server_port)});
  QProcess* proxy_holder = &proxy2;
  Check(proxy2.waitForStarted(5000) && WaitPort(proxy2_port, 5000),
        "retry fixture realtime proxy");
  const QString creds_rt = write_creds(proxy2_port, password);
  backupproject::RealtimeConfig realtime;
  realtime.enabled = true;
  realtime.trigger = backupproject::BackupTrigger::kRealtime;
  realtime.strategy = backupproject::BackupStrategy::kFull;
  realtime.source_path = source.toStdString();
  realtime.pack_method = backupproject::PackMethod::kMyPack;
  realtime.compression_method = backupproject::CompressionMethod::kNone;
  realtime.retain_count = 4;
  realtime.repository_type = "cloud";
  realtime.cloud_credentials_file = creds_rt.toStdString();
  {
    backupproject::CloudCredentials creds;
    std::string cred_error;
    backupproject::LoadCloudCredentialsFile(creds_rt.toStdString(), &creds,
                                            &cred_error);
    realtime.cloud_repository_id = backupproject::CloudRepositoryId(creds);
  }
  backupproject::RealtimeEventSummary events;
  events.generation = 1;
  events.event_count = 1;
  backupproject::RealtimeOutcome realtime_outcome;
  std::string realtime_error;
  const bool realtime_ok = backupproject::RunRealtimeBackupOnce(
      realtime, std::string(), std::string(), events,
      static_cast<std::int64_t>(::time(nullptr)), &realtime_outcome,
      &realtime_error);
  Check(
      realtime_ok && (realtime_outcome.kind ==
                          backupproject::RealtimeOutcome::Kind::kFullSnapshot ||
                      realtime_outcome.kind ==
                          backupproject::RealtimeOutcome::Kind::kDelta),
      "D 实时：第一次网络失败、第二次成功",
      QString::fromStdString(realtime_error));
  const bool realtime_listed = client.List(&snapshots, &error);
  Check(realtime_listed && snapshots.size() == 2, "D 实时：服务端快照数正确",
        QString::number(snapshots.size()));

  // 永久错误：错误密码不得重试 3 次。
  const QString bad_creds = write_creds(server_port, "definitely-wrong");
  backupproject::ScheduleDocument bad_doc = document;
  bad_doc.config.cloud_credentials_file = bad_creds.toStdString();
  const QString bad_path = dir + "/bad-schedule.json";
  backupproject::ScheduleStore bad_store(bad_path.toStdString());
  bad_store.Save(bad_doc, &save_error);
  backupproject::ScheduledBackupService bad_service(std::string(), &bad_store);
  QElapsedTimer bad_timer;
  bad_timer.start();
  const bool bad_ok = bad_service.EvaluateNow(
      static_cast<std::int64_t>(::time(nullptr)), &result, &schedule_error);
  Check(bad_ok &&
            result.status == backupproject::ScheduleEvaluationStatus::kFailed &&
            bad_timer.elapsed() < 2500,
        "D 永久错误：认证失败快速失败不重试",
        "elapsed=" + QString::number(bad_timer.elapsed()));

  // A：超过 32 个版本 + 重启后账本驱动的淘汰。
  {
    const QString direct_creds = write_creds(server_port, password);
    backupproject::CloudCredentials stress_creds;
    std::string stress_error;
    backupproject::LoadCloudCredentialsFile(direct_creds.toStdString(),
                                            &stress_creds, &stress_error);
    std::vector<std::string> owned_ids;
    std::vector<std::string> owned_shas;
    for (int i = 0; i < 40; ++i) {
      QFile file(source + "/stress.txt");
      if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QByteArray(64, 'a' + (i % 26)));
        file.write(QString::number(i).toUtf8());
      }
      file.close();
      backupproject::CloudTaskRequest task;
      task.credentials = stress_creds;
      task.expected_repository_id =
          backupproject::CloudRepositoryId(stress_creds);
      task.source_directory = source.toStdString();
      task.allow_incremental = false;
      backupproject::CloudTaskOutcome outcome;
      std::string run_error;
      if (!backupproject::RunCloudTask(task, &outcome, &run_error)) {
        Check(false, "A 40 版本创建", QString::fromStdString(run_error));
        return 1;
      }
      owned_ids.push_back(outcome.snapshot_id);
      owned_shas.push_back(outcome.sha256);
    }
    backupproject::CloudRetentionRequest retention;
    retention.credentials = stress_creds;
    retention.expected_repository_id =
        backupproject::CloudRepositoryId(stress_creds);
    retention.retain_count = 36;
    retention.owned_snapshot_ids = owned_ids;
    retention.owned_sha256 = owned_shas;
    backupproject::CloudRetentionResult result;
    Check(backupproject::RunCloudRetention(retention, &result, &stress_error) &&
              result.deleted.size() == 4,
          "A >32 版本：保留 36、删除 4", QString::fromStdString(stress_error));

    const QString ledger = dir + "/stress.ledger";
    for (std::size_t i = 0; i < owned_ids.size(); ++i) {
      backupproject::AppendOwnershipRecord(
          ledger.toStdString(),
          backupproject::OwnershipRecord{owned_ids[i], owned_shas[i]},
          &stress_error);
    }
    std::vector<backupproject::OwnershipRecord> persisted;
    Check(backupproject::ReadOwnershipRecords(ledger.toStdString(), &persisted,
                                              &stress_error) &&
              persisted.size() == 40,
          "A 重启前账本保存 40 条");
    backupproject::CloudRetentionRequest reloaded;
    reloaded.credentials = stress_creds;
    reloaded.expected_repository_id =
        backupproject::CloudRepositoryId(stress_creds);
    reloaded.retain_count = 32;
    for (const auto& record : persisted) {
      reloaded.owned_snapshot_ids.push_back(record.snapshot_id);
      reloaded.owned_sha256.push_back(record.sha256);
    }
    backupproject::CloudRetentionResult reloaded_result;
    Check(backupproject::RunCloudRetention(reloaded, &reloaded_result,
                                           &stress_error) &&
              reloaded_result.deleted.size() == 4,
          "A 重启后按持久账本保留 32、删除 4",
          QString::fromStdString(stress_error));
  }

  proxy_holder->kill();
  proxy_holder->waitForFinished(3000);
  server.kill();
  server.waitForFinished(5000);
  std::printf("cloud-retry-probe failures=%d\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace backup_modern

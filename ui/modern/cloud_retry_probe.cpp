#include "cloud_retry_probe.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
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
listen_port = int(sys.argv[1]); target_port = int(sys.argv[2]); events_path = sys.argv[3]
fail_first = (sys.argv[4] if len(sys.argv) > 4 else '1') == '1'
events = open(events_path, 'a', buffering=1)
def event(text):
    events.write(text + '\n')
server = socket.socket(); server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', listen_port)); server.listen(32)
event('READY')
first = fail_first
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
        event('FAILED')
        client.close()
        continue
    event('FORWARD')
    upstream = socket.create_connection(('127.0.0.1', target_port))
    threading.Thread(target=pipe, args=(client, upstream), daemon=True).start()
    threading.Thread(target=pipe, args=(upstream, client), daemon=True).start()
)PY");
}

}  // namespace

int RunCloudRetryProbe(const QString& work_root) {
  std::printf(
      "[isolation] QtAppConfig=%s\n",
      QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
          .toUtf8()
          .constData());
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
    const QString events_path = dir + "/proxy1.events";
    proxy->start(QStringLiteral("python3"),
                 {QStringLiteral("-u"), QStringLiteral("-c"), ProxyScript(),
                  QString::number(proxy_port), QString::number(server_port),
                  events_path, QStringLiteral("1")});
    bool ready = false;
    for (int i = 0; i < 100 && !ready; ++i) {
      QThread::msleep(50);
      QFile events(events_path);
      if (events.open(QIODevice::ReadOnly)) {
        ready = events.readAll().contains("READY");
      }
    }
    Check(proxy->waitForStarted(5000) && ready, "retry fixture proxy READY");
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
  QFile schedule_events(dir + "/proxy1.events");
  schedule_events.open(QIODevice::ReadOnly);
  const QString schedule_proxy_out =
      QString::fromUtf8(schedule_events.readAll());
  Check(schedule_proxy_out.count(QStringLiteral("FAILED")) == 1 &&
            schedule_proxy_out.count(QStringLiteral("FORWARD")) >= 1,
        "G2 定时：代理观测到 1 次注入失败、至少 1 次转发", schedule_proxy_out);
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
  const QString proxy2_events = dir + "/proxy2.events";
  proxy2.start(QStringLiteral("python3"),
               {QStringLiteral("-u"), QStringLiteral("-c"), ProxyScript(),
                QString::number(proxy2_port), QString::number(server_port),
                proxy2_events, QStringLiteral("1")});
  QProcess* proxy_holder = &proxy2;
  bool proxy2_ready = false;
  for (int i = 0; i < 100 && !proxy2_ready; ++i) {
    QThread::msleep(50);
    QFile events(proxy2_events);
    if (events.open(QIODevice::ReadOnly)) {
      proxy2_ready = events.readAll().contains("READY");
    }
  }
  Check(proxy2.waitForStarted(5000) && proxy2_ready, "PROXY_READY realtime");
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
  QFile rt_events(proxy2_events);
  rt_events.open(QIODevice::ReadOnly);
  const QString rt_text = QString::fromUtf8(rt_events.readAll());
  Check(rt_text.count(QStringLiteral("FAILED")) == 1 &&
            rt_text.count(QStringLiteral("FORWARD")) >= 1,
        "G2 实时：代理观测到 1 次注入失败、至少 1 次转发", rt_text);

  // G3 负向控制：代理不注入失败时，“必须出现一次 FAILED”的断言必须失败。
  {
    const quint16 proxy3_port = FreePort();
    const QString proxy3_events = dir + "/proxy3.events";
    QProcess proxy3;
    proxy3.start(QStringLiteral("python3"),
                 {QStringLiteral("-u"), QStringLiteral("-c"), ProxyScript(),
                  QString::number(proxy3_port), QString::number(server_port),
                  proxy3_events, QStringLiteral("0")});
    bool ready3 = false;
    for (int i = 0; i < 100 && !ready3; ++i) {
      QThread::msleep(50);
      QFile events(proxy3_events);
      if (events.open(QIODevice::ReadOnly))
        ready3 = events.readAll().contains("READY");
    }
    Check(proxy3.waitForStarted(5000) && ready3, "G3 负向代理 READY");
    const QString creds3 = write_creds(proxy3_port, password);
    backupproject::RealtimeConfig negative = realtime;
    negative.cloud_credentials_file = creds3.toStdString();
    backupproject::CloudCredentials nc;
    std::string nce;
    backupproject::LoadCloudCredentialsFile(creds3.toStdString(), &nc, &nce);
    negative.cloud_repository_id = backupproject::CloudRepositoryId(nc);
    backupproject::RealtimeEventSummary events;
    events.generation = 99;
    events.event_count = 1;
    backupproject::RealtimeOutcome out;
    std::string nerr;
    const bool negative_ok = backupproject::RunRealtimeBackupOnce(
        negative, std::string(), std::string(), events,
        static_cast<std::int64_t>(::time(nullptr)), &out, &nerr);
    QFile ne(proxy3_events);
    ne.open(QIODevice::ReadOnly);
    const QString nt = QString::fromUtf8(ne.readAll());
    const int negative_failures = nt.count(QStringLiteral("FAILED"));
    const int negative_forwards = nt.count(QStringLiteral("FORWARD"));
    Check(negative_ok && negative_failures == 0 && negative_forwards >= 1,
          "G3 负向控制：无注入时 FAILED 计数为 0（正向断言会失败）",
          "failed=" + QString::number(negative_failures) +
              " forward=" + QString::number(negative_forwards));
    proxy3.kill();
    proxy3.waitForFinished(3000);
  }

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

  // A：真实 ScheduledBackupService 生产链：40 次执行 > 32 历史，
  // 账本持久化，服务重建后继续淘汰，最后恢复保留版本。
  {
    const QString direct_creds = write_creds(server_port, password);
    document.config.cloud_credentials_file = direct_creds.toStdString();
    backupproject::CloudCredentials direct_value;
    std::string direct_error;
    backupproject::LoadCloudCredentialsFile(direct_creds.toStdString(),
                                            &direct_value, &direct_error);
    document.config.cloud_repository_id =
        backupproject::CloudRepositoryId(direct_value);
    QFile::remove(QString::fromStdString(backupproject::OwnershipLedgerPath(
        document.config.cloud_repository_id, store.schedule_file_path())));
    document.config.retain_count = 40;
    std::string save_error;
    Check(store.Save(document, &save_error), "A 生产链配置保存",
          QString::fromStdString(save_error));
    bool created = true;
    for (int i = 0; i < 40; ++i) {
      QFile file(source + "/data.txt");
      if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QByteArray("production-") + QByteArray::number(i) + "\n");
      }
      file.close();
      backupproject::ScheduleEvaluationResult result;
      std::string run_error;
      if (!schedule_service.EvaluateNow(
              static_cast<std::int64_t>(::time(nullptr)), &result,
              &run_error) ||
          result.status !=
              backupproject::ScheduleEvaluationStatus::kCreatedSnapshot) {
        created = false;
        Check(false, "A 生产链第 " + QString::number(i + 1) + " 次执行",
              QString::fromStdString(run_error) + " status=" +
                  QString::number(static_cast<int>(result.status)) +
                  " diag=" + QString::fromStdString(result.diagnostic));
        break;
      }
    }
    Check(created, "A 生产链实际执行 40 次定时备份");
    backupproject::ScheduleDocument after_runs;
    std::string reload_error;
    const bool reloaded = store.Load(&after_runs, &reload_error) ==
                          backupproject::ScheduleLoadStatus::kLoaded;
    Check(reloaded && after_runs.state.history.size() == 32,
          "A 执行器 history 最多 32 条",
          QString::number(after_runs.state.history.size()));
    const std::string ledger_path = backupproject::OwnershipLedgerPath(
        document.config.cloud_repository_id, store.schedule_file_path());
    std::vector<backupproject::OwnershipRecord> records;
    std::fprintf(stderr, "[A] ledger=%s repo=%s\n", ledger_path.c_str(),
                 document.config.cloud_repository_id.c_str());
    const bool ledger_read_ok = backupproject::ReadOwnershipRecords(
        ledger_path, &records, &reload_error);
    const std::size_t ledger_count = records.size();
    Check(ledger_read_ok && ledger_count == 40, "A 执行器长期账本保存 40 条",
          QString::number(ledger_count));

    document.config.retain_count = 36;
    store.Save(document, &save_error);
    backupproject::ScheduleEvaluationResult keep36;
    std::string keep36_error;
    const bool keep36_ok = schedule_service.EvaluateNow(
        static_cast<std::int64_t>(::time(nullptr)), &keep36, &keep36_error);
    std::vector<backupproject::OwnershipRecord> records36;
    std::string records36_error;
    const bool records36_ok = backupproject::ReadOwnershipRecords(
        ledger_path, &records36, &records36_error);
    std::vector<backupproject::net::RemoteSnapshotInfo> after36;
    client.List(&after36, &error);
    std::size_t owned36 = 0;
    for (const auto& snap : after36) {
      for (const auto& record : records36) {
        if (snap.snapshot_id == record.snapshot_id) ++owned36;
      }
    }
    Check(
        keep36_ok &&
            keep36.status != backupproject::ScheduleEvaluationStatus::kFailed &&
            records36_ok && owned36 == 36,
        "A 生产链按账本保留 36",
        QString::number(owned36) +
            " err=" + QString::fromStdString(keep36_error));

    // 重建服务实例与 store，模拟进程重启后的磁盘恢复。
    backupproject::ScheduleStore reloaded_store(schedule_path.toStdString());
    backupproject::ScheduleDocument reloaded_doc;
    std::string reload_error2;
    reloaded_store.Load(&reloaded_doc, &reload_error2);
    reloaded_doc.config.retain_count = 32;
    reloaded_store.Save(reloaded_doc, &save_error);
    backupproject::ScheduledBackupService rebuilt_service(std::string(),
                                                          &reloaded_store);
    backupproject::ScheduleEvaluationResult keep32;
    std::string keep32_error;
    const bool keep32_ok = rebuilt_service.EvaluateNow(
        static_cast<std::int64_t>(::time(nullptr)), &keep32, &keep32_error);
    std::vector<backupproject::OwnershipRecord> records32;
    std::string records32_error;
    const bool records32_ok = backupproject::ReadOwnershipRecords(
        ledger_path, &records32, &records32_error);
    std::vector<backupproject::net::RemoteSnapshotInfo> after32;
    client.List(&after32, &error);
    std::size_t owned32 = 0;
    QString latest_id;
    for (const auto& snap : after32) {
      for (const auto& record : records32) {
        if (snap.snapshot_id == record.snapshot_id) {
          ++owned32;
          latest_id = QString::fromStdString(snap.snapshot_id);
        }
      }
    }
    Check(
        keep32_ok &&
            keep32.status != backupproject::ScheduleEvaluationStatus::kFailed &&
            records32_ok && owned32 == 32,
        "A 服务重建后按持久账本保留 32",
        QString::number(owned32) +
            " err=" + QString::fromStdString(keep32_error));

    const QString restore40 = dir + "/restore40";
    QDir().mkpath(restore40);
    const QString downloaded40 = dir + "/downloaded40.bak";
    backupproject::net::RemoteSnapshotInfo downloaded_info;
    const bool downloaded_ok =
        !latest_id.isEmpty() &&
        client.DownloadArchiveFile(latest_id.toStdString(),
                                   downloaded40.toStdString(), true,
                                   backupproject::net::RemoteProgressCallback(),
                                   &downloaded_info, &error);
    const QString extracted40 = dir + "/extracted40";
    QDir().mkpath(extracted40);
    backupproject::net::SnapshotBundleInfo bundle_info;
    std::string extract_error;
    const bool extracted_ok =
        downloaded_ok &&
        backupproject::net::ExtractSnapshotBundle(downloaded40.toStdString(),
                                                  extracted40.toStdString(),
                                                  &bundle_info, &extract_error);
    backupproject::BackupEngine engine;
    std::string restore_error;
    const bool restored40 =
        extracted_ok &&
        engine.Restore((extracted40 + QString::fromStdString("/") +
                        QString::fromStdString(bundle_info.archive_name))
                           .toStdString(),
                       restore40.toStdString(), &restore_error);
    QFile restored_file(restore40 + "/data.txt");
    Check(restored40 && restored_file.open(QIODevice::ReadOnly) &&
              restored_file.readAll() == QByteArray("production-39\n"),
          "A 淘汰后保留版本恢复内容一致",
          QString::fromStdString(extract_error + " / " + restore_error));
  }

  // G4：名称复用后的真实删除 + 账本损坏后 fail-closed。
  {
    const QString direct_creds = write_creds(server_port, password);
    backupproject::CloudCredentials creds;
    std::string creds_error;
    backupproject::LoadCloudCredentialsFile(direct_creds.toStdString(), &creds,
                                            &creds_error);
    auto run_full = [&](const QString& text,
                        backupproject::CloudTaskOutcome* out) {
      QFile file(source + "/reuse-delete.txt");
      if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(text.toUtf8());
      }
      file.close();
      backupproject::CloudTaskRequest task;
      task.credentials = creds;
      task.expected_repository_id = backupproject::CloudRepositoryId(creds);
      task.source_directory = source.toStdString();
      task.allow_incremental = false;
      std::string run_error;
      return backupproject::RunCloudTask(task, out, &run_error);
    };
    backupproject::CloudTaskOutcome a;
    backupproject::CloudTaskOutcome w;
    backupproject::CloudTaskOutcome z;
    const bool created = run_full(QStringLiteral("A"), &a) &&
                         run_full(QStringLiteral("W"), &w) &&
                         run_full(QStringLiteral("Z"), &z);
    const bool deleted_a = created && client.Delete(a.snapshot_id, &error);
    const QString replacement = dir + "/reuse-replacement.bak";
    QFile replacement_file(replacement);
    if (replacement_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      replacement_file.write(QByteArray(4096, 'b'));
    }
    replacement_file.close();
    backupproject::net::RemoteSnapshotInfo b;
    const bool reuploaded =
        deleted_a &&
        client.UploadArchiveFile(replacement.toStdString(), a.archive_name,
                                 backupproject::net::RemoteProgressCallback(),
                                 &b, &error);
    backupproject::CloudRetentionRequest retention;
    retention.credentials = creds;
    retention.expected_repository_id = backupproject::CloudRepositoryId(creds);
    retention.retain_count = 1;
    retention.owned_snapshot_ids = {a.snapshot_id, w.snapshot_id,
                                    z.snapshot_id};
    retention.owned_sha256 = {a.sha256, w.sha256, z.sha256};
    retention.dry_run = false;
    backupproject::CloudRetentionResult result;
    std::string retention_error;
    const bool deleted_w = reuploaded &&
                           backupproject::RunCloudRetention(retention, &result,
                                                            &retention_error) &&
                           result.deleted.size() == 1;
    std::vector<backupproject::net::RemoteSnapshotInfo> after;
    client.List(&after, &error);
    bool b_present = false;
    bool w_present = false;
    bool z_present = false;
    QString b_sha;
    for (const auto& snap : after) {
      if (snap.snapshot_id == b.snapshot_id) {
        b_present = true;
        b_sha = QString::fromStdString(snap.sha256);
      }
      if (snap.snapshot_id == w.snapshot_id) w_present = true;
      if (snap.snapshot_id == z.snapshot_id) z_present = true;
    }
    const QString downloaded_b = dir + "/reuse-b.bak";
    backupproject::net::RemoteSnapshotInfo downloaded_b_info;
    const bool b_downloaded =
        b_present && client.DownloadArchiveFile(
                         b.snapshot_id, downloaded_b.toStdString(), true,
                         backupproject::net::RemoteProgressCallback(),
                         &downloaded_b_info, &error);
    QFile b_file(downloaded_b);
    const bool b_ok = b_downloaded && b_file.open(QIODevice::ReadOnly) &&
                      b_file.size() == 4096;
    Check(deleted_w && b_present && !w_present && z_present && b_ok &&
              b_sha == QString::fromStdString(b.sha256),
          "G4 名称复用真实删除：Y 保留、Z 保留、W 删除",
          QString::fromStdString(retention_error));

    // 账本损坏：保留合法记录后追加坏行，下一次执行必须停止淘汰。
    const std::string ledger = backupproject::OwnershipLedgerPath(
        document.config.cloud_repository_id, store.schedule_file_path());
    std::vector<backupproject::OwnershipRecord> before_corrupt;
    std::string corrupt_error;
    Check(backupproject::ReadOwnershipRecords(ledger, &before_corrupt,
                                              &corrupt_error) &&
              !before_corrupt.empty(),
          "G4 损坏前账本可读", QString::fromStdString(corrupt_error));
    {
      QFile corrupt(QString::fromStdString(ledger));
      if (corrupt.open(QIODevice::WriteOnly | QIODevice::Append)) {
        corrupt.write("BROKEN LINE\n");
      }
    }
    std::vector<backupproject::net::RemoteSnapshotInfo> before_snaps;
    client.List(&before_snaps, &error);
    document.config.retain_count = 1;
    store.Save(document, &save_error);
    backupproject::ScheduleEvaluationResult corrupt_result;
    std::string corrupt_run_error;
    const bool corrupt_ok =
        schedule_service.EvaluateNow(static_cast<std::int64_t>(::time(nullptr)),
                                     &corrupt_result, &corrupt_run_error);
    std::vector<backupproject::net::RemoteSnapshotInfo> after_corrupt;
    client.List(&after_corrupt, &error);
    std::size_t preserved = 0;
    for (const auto& before : before_snaps) {
      for (const auto& after_snap : after_corrupt) {
        if (before.snapshot_id == after_snap.snapshot_id) {
          ++preserved;
          break;
        }
      }
    }
    Check(corrupt_ok &&
              corrupt_result.diagnostic.find("cloud retention stopped") !=
                  std::string::npos &&
              preserved == before_snaps.size(),
          "G4 账本读错误 fail-closed：不删除任何旧快照",
          QString::fromStdString(corrupt_result.diagnostic));
  }

  // P1：账本写入失败时跳过自动淘汰；逐 snapshot_id + sha256 验证旧对象保留。
  {
    const QString cpath = write_creds(server_port, password);
    backupproject::CloudCredentials c;
    std::string ce;
    backupproject::LoadCloudCredentialsFile(cpath.toStdString(), &c, &ce);
    const QString sp = dir + "/wf-schedule.json";
    backupproject::ScheduleStore st(sp.toStdString());
    backupproject::ScheduleDocument doc;
    doc.config.enabled = true;
    doc.config.source_path = source.toStdString();
    doc.config.retain_count = 2;
    doc.config.pack_method = backupproject::PackMethod::kMyPack;
    doc.config.repository_type = "cloud";
    doc.config.cloud_credentials_file = cpath.toStdString();
    doc.config.cloud_repository_id = backupproject::CloudRepositoryId(c);
    std::string e;
    st.Save(doc, &e);
    backupproject::ScheduledBackupService svc(std::string(), &st);
    const std::string lp = backupproject::OwnershipLedgerPath(
        doc.config.cloud_repository_id, st.schedule_file_path());
    backupproject::ScheduleEvaluationResult r;
    auto run = [&](const QByteArray& text) {
      QFile f(source + "/data.txt");
      if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text);
      f.close();
      return svc.EvaluateNow(static_cast<std::int64_t>(::time(nullptr)), &r,
                             &e);
    };
    auto list_ok =
        [&](std::vector<backupproject::net::RemoteSnapshotInfo>* out) {
          return client.List(out, &error);
        };
    auto id_sha_map =
        [](const std::vector<backupproject::net::RemoteSnapshotInfo>& snaps) {
          std::map<std::string, std::string> m;
          for (const auto& s : snaps) m[s.snapshot_id] = s.sha256;
          return m;
        };
    auto all_preserved =
        [](const std::map<std::string, std::string>& before_map,
           const std::vector<backupproject::net::RemoteSnapshotInfo>& after) {
          std::map<std::string, std::string> after_map;
          for (const auto& s : after) after_map[s.snapshot_id] = s.sha256;
          for (const auto& kv : before_map) {
            const auto it = after_map.find(kv.first);
            if (it == after_map.end() || it->second != kv.second) return false;
          }
          return true;
        };
    const bool first_ok =
        run("wf-first") &&
        r.status == backupproject::ScheduleEvaluationStatus::kCreatedSnapshot;
    Check(first_ok, "P1 前置第一次备份", QString::fromStdString(e));
    const bool second_ok =
        run("wf-second") &&
        r.status == backupproject::ScheduleEvaluationStatus::kCreatedSnapshot;
    Check(second_ok, "P1 前置第二次备份", QString::fromStdString(e));

    std::vector<backupproject::net::RemoteSnapshotInfo> before;
    const bool list_before = list_ok(&before);
    Check(list_before, "P1 故障前 List 成功", QString::fromStdString(error));
    const std::map<std::string, std::string> before_map = id_sha_map(before);

    const QString saved = QString::fromStdString(lp) + ".saved";
    QFile::remove(saved);
    const bool blocked = QFile::rename(QString::fromStdString(lp), saved) &&
                         QDir().mkpath(QString::fromStdString(lp));
    Check(blocked, "P1 阻塞账本路径");
    const bool failed_run =
        blocked && run("wf-failure") &&
        r.status == backupproject::ScheduleEvaluationStatus::kCreatedSnapshot &&
        r.diagnostic.find("ownership ledger not saved") != std::string::npos;
    Check(failed_run, "P1 账本写失败且备份本身成功",
          QString::fromStdString(r.diagnostic));

    std::vector<backupproject::net::RemoteSnapshotInfo> after;
    const bool list_after = list_ok(&after);
    Check(list_after, "P1 故障后 List 成功", QString::fromStdString(error));
    Check(list_before && list_after && all_preserved(before_map, after),
          "P1 旧快照逐 ID/SHA 全部保留");
    std::map<std::string, std::string> after_map = id_sha_map(after);
    std::vector<std::string> new_ids;
    for (const auto& s : after) {
      if (before_map.find(s.snapshot_id) == before_map.end()) {
        new_ids.push_back(s.snapshot_id);
      }
    }
    Check(new_ids.size() == 1 && !after_map[new_ids[0]].empty(),
          "P1 故障期间只创建一个新快照", QString::number(new_ids.size()));

    std::vector<backupproject::net::RemoteSnapshotInfo> after_negative = after;
    if (!after_negative.empty()) after_negative.erase(after_negative.begin());
    Check(!all_preserved(before_map, after_negative),
          "P1 负向控制：从 after 移除一个旧快照后断言必须失败");

    QDir(QString::fromStdString(lp)).removeRecursively();
    const bool restored = QFile::rename(saved, QString::fromStdString(lp));
    const bool recovery_ok =
        restored && run("wf-recovery") &&
        r.status == backupproject::ScheduleEvaluationStatus::kCreatedSnapshot;
    std::vector<backupproject::OwnershipRecord> records;
    const bool ledger_ok =
        recovery_ok && backupproject::ReadOwnershipRecords(lp, &records, &e);
    Check(
        ledger_ok && records.size() == 3 &&
            r.diagnostic.find("cloud retention deleted 1") != std::string::npos,
        "P1 故障解除后恢复记账并淘汰",
        QString::fromStdString(r.diagnostic + " / " + e));
  }

  proxy_holder->kill();
  proxy_holder->waitForFinished(3000);
  server.kill();
  server.waitForFinished(5000);
  std::printf("cloud-retry-probe failures=%d\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace backup_modern

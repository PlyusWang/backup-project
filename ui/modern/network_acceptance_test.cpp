// Real, isolated loopback BPSEC2 and OpenSSH acceptance. The companion script
// owns the fixture server/sshd; this test owns only its RemoteController
// tunnel.

#include "network_acceptance_test.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUuid>
#include <QVariantMap>
#include <cstdio>

#include "backup_catalog.h"
#include "incremental_backup.h"
#include "remote_controller.h"
#include "ssh_tunnel_manager.h"

namespace {

QByteArray Read(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool Write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray Digest(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return QByteArray();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&file) ? hash.result() : QByteArray();
}

void Pump(int milliseconds) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < milliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
}

void DismissError(QQuickWindow* window) {
  QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &press);
  QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &release);
  Pump(50);
}

QQuickItem* FindItem(QQuickItem* parent, const char* name) {
  if (parent->objectName() == QLatin1String(name)) return parent;
  for (QQuickItem* child : parent->childItems()) {
    QQuickItem* found = FindItem(child, name);
    if (found != nullptr) return found;
  }
  return nullptr;
}

// Scroll the actual product page. No progress value, model or visibility is
// changed by the harness; grabWindow renders the controller's live state.
bool RevealRemoteItem(QQuickWindow* window, const char* name) {
  QQuickItem* scroll = FindItem(window->contentItem(), "remotePageScroll");
  QQuickItem* item = FindItem(window->contentItem(), name);
  if (scroll == nullptr || item == nullptr) return false;
  QQuickItem* flick = scroll->property("contentItem").value<QQuickItem*>();
  if (flick == nullptr) return false;
  const qreal top = item->mapToItem(flick, QPointF()).y();
  const qreal current = flick->property("contentY").toReal();
  const qreal maximum = qMax(
      qreal(0), flick->property("contentHeight").toReal() - flick->height());
  flick->setProperty(
      "contentY",
      qBound(qreal(0), current + top - flick->height() / 2, maximum));
  const qreal visible_top = item->mapToItem(flick, QPointF()).y();
  return item->isVisible() && visible_top >= 0 &&
         visible_top + item->height() <= flick->height();
}

}  // namespace

int RunNetworkAcceptanceTest(QQuickWindow* window,
                             backup_modern::RemoteController* remote,
                             const QString& fixture_directory,
                             const QString& output_directory) {
  int passed = 0;
  int failed = 0;
  const auto check = [&](bool ok, const char* name) {
    std::printf("NETWORK %s=%s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
      std::fprintf(stderr, "NETWORK diagnostic kind=%s detail=%s status=%s\n",
                   qPrintable(remote->lastErrorKindForTest()),
                   qPrintable(remote->lastDetailForTest()),
                   qPrintable(remote->statusMessage()));
    }
    std::fflush(stdout);
    ok ? ++passed : ++failed;
    return ok;
  };
  const QDir fixture(fixture_directory);
  const QString port =
      QString::fromUtf8(Read(fixture.filePath("backup.port"))).trimmed();
  const QString ssh_target =
      QString::fromUtf8(Read(fixture.filePath("ssh.target"))).trimmed();
  const QString roots = fixture.filePath("root.key.pub");
  const QString wrong_roots = fixture.filePath("wrong-root.key.pub");
  const QString wrapper = fixture.filePath("fixture-ssh");
  const QString server_id = QStringLiteral("acceptance-loopback-server");
  const QString host = QStringLiteral("127.0.0.1");
  bool valid_port = false;
  const int port_number = port.toInt(&valid_port);
  if (!check(QFileInfo(fixture_directory).isDir() && valid_port &&
                 port_number > 1024 && port_number <= 65535 &&
                 ssh_target.endsWith(QStringLiteral("@127.0.0.1")) &&
                 QFileInfo(wrapper).isExecutable() && QFile::exists(roots) &&
                 QFile::exists(wrong_roots) && QDir().mkpath(output_directory),
             "ISOLATED_LOOPBACK_FIXTURE"))
    return 1;
  std::printf("NETWORK platform=%s endpoint=127.0.0.1:%d\n",
              qPrintable(QGuiApplication::platformName()), port_number);
  const auto completed = [&](bool accepted) {
    return accepted && remote->waitForIdle(180000) &&
           remote->lastErrorKindForTest() == QStringLiteral("none");
  };
  const auto capture = [&](const QString& name) {
    Pump(150);
    const QImage image = window->grabWindow();
    return !image.isNull() &&
           image.save(output_directory + QLatin1Char('/') + name);
  };
  const auto capture_session = [&](const QString& name,
                                   const QString& username) {
    Pump(100);
    if (!RevealRemoteItem(window, "remoteAccountStateText")) return false;
    Pump(100);
    QQuickItem* state =
        FindItem(window->contentItem(), "remoteAccountStateText");
    QQuickItem* account = FindItem(window->contentItem(), "remoteAccountText");
    QQuickItem* scroll = FindItem(window->contentItem(), "remotePageScroll");
    QQuickItem* flick =
        scroll == nullptr
            ? nullptr
            : scroll->property("contentItem").value<QQuickItem*>();
    const auto in_view = [&](QQuickItem* item) {
      if (item == nullptr || flick == nullptr || !item->isVisible())
        return false;
      const qreal top = item->mapToItem(flick, QPointF()).y();
      return top >= 0 && top + item->height() <= flick->height();
    };
    return remote->authenticated() && in_view(state) && in_view(account) &&
           state->property("text").toString() ==
               QStringLiteral("状态：已登录") &&
           account->property("text").toString() ==
               QStringLiteral("当前账户：") + username &&
           capture(name);
  };
  const auto certificate_feedback = [&](const QString& kind,
                                        const QString& summary,
                                        const QString& suggestion) {
    Pump(100);
    QObject* dialog =
        window->findChild<QObject*>(QStringLiteral("userErrorDialog"));
    QQuickItem* content =
        dialog == nullptr
            ? nullptr
            : dialog->property("contentItem").value<QQuickItem*>();
    QQuickItem* summary_item =
        content == nullptr ? nullptr : FindItem(content, "userErrorSummary");
    QQuickItem* suggestion_item =
        content == nullptr ? nullptr : FindItem(content, "userErrorSuggestion");
    QQuickItem* form = FindItem(window->contentItem(), "remoteLoginError");
    const QString form_message = remote->loginError();
    return dialog != nullptr && dialog->property("visible").toBool() &&
           remote->lastErrorKindForTest() == kind && summary_item != nullptr &&
           suggestion_item != nullptr && summary_item->isVisible() &&
           suggestion_item->isVisible() &&
           summary_item->property("text").toString() == summary &&
           suggestion_item->property("text").toString() == suggestion &&
           form != nullptr && form->isVisible() &&
           form->property("text").toString() == form_message &&
           form_message == summary + suggestion &&
           !form_message.contains(QStringLiteral("BPSEC1")) &&
           !form_message.contains(QStringLiteral("路径")) &&
           !form_message.contains(QStringLiteral("权限")) &&
           !form_message.contains(QStringLiteral("空间"));
  };
  QObject* management =
      window->findChild<QObject*>(QStringLiteral("managementPage"));
  QObject* page = window->findChild<QObject*>(QStringLiteral("remotePage"));
  window->setProperty("currentPageKey", QStringLiteral("management"));
  if (management != nullptr)
    management->setProperty("currentTab", QStringLiteral("cloud"));
  const auto show_configuration = [&](const QString& mode,
                                      const QString& shown_roots,
                                      const QString& shown_id) {
    if (page == nullptr) return;
    page->setProperty("draftServerKind", QStringLiteral("custom"));
    page->setProperty("draftConnectionMode", mode);
    page->setProperty("draftAuthMode", QStringLiteral("certificate"));
    page->setProperty("draftHost", host);
    page->setProperty("draftPort", port);
    page->setProperty("draftTrustedRoots", shown_roots);
    page->setProperty("draftExpectedServerId", shown_id);
    page->setProperty("draftSshHost", ssh_target);
    page->setProperty("draftSshLocalPort", QString());
  };

  remote->logoutLocal();
  remote->stopTunnel();
  check(remote->setServerKind(QStringLiteral("custom")) &&
            remote->setConnectionMode(QStringLiteral("direct")) &&
            remote->setAuthMode(QStringLiteral("certificate")),
        "DIRECT_CERTIFICATE_CONFIGURATION");
  remote->setCertificateSettings(roots, server_id);
  show_configuration(QStringLiteral("direct"), roots, server_id);
  const QString account_secret =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString direct_user = QStringLiteral("network-direct");
  check(completed(remote->registerAccount(host, port, direct_user,
                                          account_secret, account_secret)),
        "DIRECT_BPSEC2_REGISTER");
  remote->logoutLocal();
  if (!check(
          completed(remote->login(host, port, direct_user, account_secret)) &&
              remote->authenticated() && !remote->tunnelReady() &&
              remote->tunnelPidForTest() == 0,
          "DIRECT_BPSEC2_LOGIN_WITHOUT_SSH"))
    return 1;
  if (page != nullptr) page->setProperty("draftUser", direct_user);
  check(capture_session(
            QStringLiteral("17-network-direct-certificate-session.png"),
            direct_user),
        "DIRECT_SESSION_SCREENSHOT");

  const auto round_trip = [&](const QString& prefix) {
    const QString source = fixture.filePath(prefix + QStringLiteral("-source"));
    const QString destination =
        fixture.filePath(prefix + QStringLiteral("-restore"));
    const bool monitor_upload = prefix == QStringLiteral("direct");
    QByteArray stable(monitor_upload ? 32 * 1024 * 1024 : 256 * 1024, '\0');
    for (int index = 0; index < stable.size(); ++index) {
      stable[index] = static_cast<char>((index * 53 + index / 251) & 255);
    }
    check(QDir().mkpath(source) &&
              Write(source + QStringLiteral("/stable.bin"), stable) &&
              Write(source + QStringLiteral("/说明.txt"), "before\n") &&
              Write(source + QStringLiteral("/empty"), QByteArray()),
          qPrintable(prefix.toUpper() + QStringLiteral("_SYNTHETIC_SOURCE")));
    int intermediate_updates = 0;
    bool progress_screenshot = false;
    bool capturing_progress = false;
    qint64 screenshot_done = 0;
    qint64 screenshot_total = 0;
    const auto progress_connection = QObject::connect(
        remote, &backup_modern::RemoteController::progressChanged, window,
        [&]() {
          if (!monitor_upload || capturing_progress || !remote->busy() ||
              !remote->transferActive() ||
              remote->transferPhaseText() != QStringLiteral("正在上传"))
            return;
          const qint64 done = remote->bytesDone();
          const qint64 total = remote->bytesTotal();
          if (done <= 0 || total <= done) return;
          ++intermediate_updates;
          if (intermediate_updates == 1) {
            std::printf(
                "NETWORK upload_intermediate done=%lld total=%lld "
                "percent=%.3f\n",
                static_cast<long long>(done), static_cast<long long>(total),
                100.0 * static_cast<double>(done) / total);
          }
          // Capture a visibly nonzero percentage, without waiting or changing
          // the producer. Reentrant notifications while grabbing are ignored.
          if (progress_screenshot || static_cast<double>(done) / total < 0.01)
            return;
          capturing_progress = true;
          const bool visible = RevealRemoteItem(window, "remoteProgressRow");
          const QImage frame = visible ? window->grabWindow() : QImage();
          const qint64 after_done = remote->bytesDone();
          const qint64 after_total = remote->bytesTotal();
          if (!frame.isNull() && remote->transferActive() && after_done > 0 &&
              after_done < after_total &&
              frame.save(output_directory +
                         QStringLiteral("/21-cloud-upload-progress.png"))) {
            progress_screenshot = true;
            screenshot_done = after_done;
            screenshot_total = after_total;
            QQuickItem* ratio_text =
                FindItem(window->contentItem(), "remoteProgressRatioText");
            std::printf(
                "NETWORK upload_screenshot before_done=%lld before_total=%lld "
                "after_done=%lld after_total=%lld percent=%.3f displayed=%s\n",
                static_cast<long long>(done), static_cast<long long>(total),
                static_cast<long long>(after_done),
                static_cast<long long>(after_total),
                100.0 * static_cast<double>(after_done) / after_total,
                ratio_text == nullptr
                    ? "missing"
                    : qPrintable(ratio_text->property("text").toString()));
          }
          capturing_progress = false;
          std::fflush(stdout);
        });
    const bool full_ok = completed(remote->backupRemote(source, false));
    QObject::disconnect(progress_connection);
    const QString full_id = remote->lastBackupSnapshotIdForTest();
    const qint64 full_bytes = remote->lastBackupUploadedBytesForTest();
    check(full_ok && !full_id.isEmpty() &&
              !remote->lastBackupProducedDeltaForTest(),
          qPrintable(prefix.toUpper() + QStringLiteral("_FULL_BACKUP")));
    if (monitor_upload) {
      check(intermediate_updates >= 1,
            "DIRECT_REAL_INTERMEDIATE_UPLOAD_PROGRESS");
      check(progress_screenshot && screenshot_done > 0 &&
                screenshot_done < screenshot_total,
            "DIRECT_UPLOAD_PROGRESS_SCREENSHOT");
      const bool result_visible =
          RevealRemoteItem(window, "remoteBackupSummary");
      check(full_ok && result_visible &&
                capture(QStringLiteral("22-cloud-upload-complete.png")),
            "DIRECT_UPLOAD_COMPLETE_SCREENSHOT");
      std::printf(
          "NETWORK upload_progress_summary intermediate_updates=%d "
          "screenshot_done=%lld screenshot_total=%lld "
          "full_uploaded_bytes=%lld\n",
          intermediate_updates, static_cast<long long>(screenshot_done),
          static_cast<long long>(screenshot_total),
          static_cast<long long>(full_bytes));
    }
    check(Write(source + QStringLiteral("/说明.txt"), "after\n") &&
              Write(source + QStringLiteral("/added.txt"), "new\n"),
          qPrintable(prefix.toUpper() + QStringLiteral("_SMALL_CHANGE")));
    const bool delta_ok = completed(remote->backupRemote(source, true));
    const QString delta_id = remote->lastBackupSnapshotIdForTest();
    check(
        delta_ok && remote->lastBackupProducedDeltaForTest() &&
            !delta_id.isEmpty() && delta_id != full_id &&
            remote->lastBackupUploadedBytesForTest() > 0 &&
            remote->lastBackupUploadedBytesForTest() < full_bytes / 2,
        qPrintable(prefix.toUpper() + QStringLiteral("_PHYSICAL_INCREMENTAL")));
    const bool restore_ok =
        completed(remote->restoreSnapshot(delta_id, destination));
    bool hashes_match = restore_ok &&
                        remote->lastRestoreChainLengthForTest() >= 2 &&
                        remote->lastRestoreDeltaCountForTest() >= 1;
    for (const QString& file :
         {QStringLiteral("stable.bin"), QStringLiteral("说明.txt"),
          QStringLiteral("added.txt"), QStringLiteral("empty")}) {
      const QByteArray expected = Digest(source + QLatin1Char('/') + file);
      hashes_match = hashes_match && !expected.isEmpty() &&
                     expected == Digest(destination + QLatin1Char('/') + file);
    }
    check(hashes_match, qPrintable(prefix.toUpper() +
                                   QStringLiteral("_RESTORE_CHAIN_SHA256")));
  };
  round_trip(QStringLiteral("direct"));
  const auto encrypted_cloud_round_trip = [&]() {
    const QString source = fixture.filePath(QStringLiteral("cloud-aes-source"));
    const QString repository =
        fixture.filePath(QStringLiteral("cloud-aes-repository"));
    const QString destination =
        fixture.filePath(QStringLiteral("cloud-aes-restore"));
    const QString full_name = QStringLiteral("cloud-aes-full.bak");
    const QString delta_name = QStringLiteral("cloud-aes-delta.bak");
    const QString archive_password =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QByteArray stable(256 * 1024, '\0');
    for (int index = 0; index < stable.size(); ++index) {
      stable[index] = static_cast<char>((index * 29 + index / 251) & 255);
    }
    if (!check(QDir().mkpath(source) && QDir().mkpath(repository) &&
                   Write(source + QStringLiteral("/stable.bin"), stable) &&
                   Write(source + QStringLiteral("/说明.txt"), "before\n") &&
                   Write(source + QStringLiteral("/empty"), QByteArray()),
               "AES_SYNTHETIC_LOCAL_REPOSITORY"))
      return;
    backupproject::BackupOptions options;
    options.encryption_method =
        backupproject::EncryptionMethod::kAes256CtrHmacSha256;
    options.password = archive_password.toStdString();
    backupproject::IncrementalOutcome outcome;
    std::string core_error;
    const std::string repository_id =
        backupproject::RepositoryIdentity(repository.toStdString());
    const auto core_diagnostic = [&]() {
      std::fprintf(stderr, "NETWORK encrypted_core_diagnostic=%s\n",
                   core_error.c_str());
    };
    if (!check(
            backupproject::RunManagedFullBackup(
                source.toStdString(), repository.toStdString(),
                full_name.toStdString(), repository_id, backupproject::Filter(),
                options, {}, {}, &outcome, &core_error),
            "AES_MANAGED_FULL_BASELINE")) {
      core_diagnostic();
      return;
    }
    if (!check(Write(source + QStringLiteral("/说明.txt"), "after\n") &&
                   Write(source + QStringLiteral("/added.txt"), "new\n"),
               "AES_CORE_SMALL_CHANGE"))
      return;
    const QString delta_path = QDir(repository).filePath(delta_name);
    backupproject::DeltaEnvelope envelope;
    backupproject::ContainerHeader payload_header;
    if (!check(
            backupproject::RunIncrementalBackup(
                source.toStdString(), repository.toStdString(),
                delta_name.toStdString(), repository_id,
                backupproject::Filter(), options, {}, {},
                full_name.toStdString(), &outcome, &core_error) &&
                outcome.kind ==
                    backupproject::IncrementalOutcome::Kind::kDelta &&
                backupproject::ReadDeltaEnvelope(delta_path.toStdString(),
                                                 &envelope, &core_error) &&
                envelope.format_version ==
                    backupproject::kAuthenticatedDeltaFormatVersion &&
                !envelope.envelope_hmac.empty() &&
                envelope.parent_file_name == full_name.toStdString() &&
                backupproject::InspectDeltaPayloadHeader(
                    delta_path.toStdString(), &payload_header, &core_error) &&
                payload_header.encryption_method ==
                    static_cast<std::uint8_t>(backupproject::EncryptionMethod::
                                                  kAes256CtrHmacSha256) &&
                QFileInfo(delta_path).size() > 0 &&
                QFileInfo(delta_path).size() <
                    QFileInfo(QDir(repository).filePath(full_name)).size() / 2,
            "AES_AUTHENTICATED_LOCAL_DELTA")) {
      core_diagnostic();
      return;
    }
    const int cloud_count_before = remote->snapshotCountForTest();
    remote->setLocalRepositoryPath(repository);
    if (!check(completed(remote->copyLocalArchiveToCloud(delta_name)) &&
                   completed(remote->refreshList()) &&
                   remote->snapshotCountForTest() == cloud_count_before + 2,
               "AES_COPIES_DEPENDENCY_CHAIN_TO_CLOUD"))
      return;
    QString full_id;
    QVariantMap delta_row;
    for (const QVariant& row : remote->snapshots()) {
      const QVariantMap record = row.toMap();
      if (record.value(QStringLiteral("name")).toString() == full_name)
        full_id = record.value(QStringLiteral("id")).toString();
      if (record.value(QStringLiteral("name")).toString() == delta_name)
        delta_row = record;
    }
    const QString delta_id = delta_row.value(QStringLiteral("id")).toString();
    if (!check(!full_id.isEmpty() && !delta_id.isEmpty() &&
                   delta_row.value(QStringLiteral("kind")).toString() ==
                       QStringLiteral("incremental") &&
                   delta_row.value(QStringLiteral("generation")).toInt() == 1 &&
                   delta_row.value(QStringLiteral("parentId")).toString() ==
                       full_id,
               "AES_CLOUD_DELTA_PARENT"))
      return;
    bool matches = completed(remote->restoreSnapshot(delta_id, destination,
                                                     archive_password)) &&
                   remote->lastRestoreChainLengthForTest() == 2 &&
                   remote->lastRestoreDeltaCountForTest() == 1;
    for (const QString& name :
         {QStringLiteral("stable.bin"), QStringLiteral("说明.txt"),
          QStringLiteral("added.txt"), QStringLiteral("empty")}) {
      const QByteArray expected = Digest(source + QLatin1Char('/') + name);
      matches = matches && !expected.isEmpty() &&
                expected == Digest(destination + QLatin1Char('/') + name);
    }
    check(matches, "AES_CLOUD_PASSWORD_CHAIN_RESTORE_SHA256");
    const QString wrong_destination =
        fixture.filePath(QStringLiteral("cloud-aes-wrong-password"));
    const bool wrong_accepted = remote->restoreSnapshot(
        delta_id, wrong_destination, archive_password + QLatin1Char('!'));
    const bool wrong_idle = wrong_accepted && remote->waitForIdle(180000);
    check(
        wrong_idle &&
            remote->lastErrorKindForTest() != QStringLiteral("none") &&
            remote->lastDetailForTest().contains(
                QStringLiteral("Authentication failed"), Qt::CaseInsensitive) &&
            !QFileInfo::exists(wrong_destination),
        "AES_CLOUD_WRONG_PASSWORD_NO_PUBLISH");
    DismissError(window);
  };
  encrypted_cloud_round_trip();
  window->setProperty("currentPageKey", QStringLiteral("home"));
  if (management != nullptr)
    management->setProperty("currentTab", QStringLiteral("local"));
  Pump(60);
  remote->logoutLocal();
  Pump(60);
  window->setProperty("currentPageKey", QStringLiteral("management"));
  Pump(60);
  if (management != nullptr)
    management->setProperty("currentTab", QStringLiteral("cloud"));
  Pump(60);
  check(!remote->authenticated() && remote->snapshotCountForTest() == 0,
        "HIDDEN_SNAPSHOT_LIST_CLEAR_AND_TAB_SWITCH");

  // Negative identity checks must fail after a genuine request, before login.
  remote->setCertificateSettings(roots, QStringLiteral("wrong-server-id"));
  bool accepted = remote->login(host, port, direct_user, account_secret);
  bool idle = accepted && remote->waitForIdle(180000);
  check(
      idle && !remote->authenticated() &&
          remote->lastErrorKindForTest() != QStringLiteral("none") &&
          (remote->lastDetailForTest().contains(QStringLiteral("server_id")) ||
           remote->lastErrorKindForTest().contains(
               QStringLiteral("server-id"))),
      "WRONG_SERVER_ID_REJECTED");
  show_configuration(QStringLiteral("direct"), roots,
                     QStringLiteral("wrong-server-id"));
  check(
      certificate_feedback(
          QStringLiteral("server-certificate-wrong-server-id"),
          QStringLiteral("服务器身份与预期不一致，连接已停止。"),
          QStringLiteral(
              "请向服务器管理员核对服务器身份文件和预期的服务器标识后重试。")),
      "WRONG_SERVER_ID_DIALOG_AND_FORM");
  check(capture(QStringLiteral("19-network-wrong-server-id.png")),
        "WRONG_SERVER_ID_SCREENSHOT");
  DismissError(window);
  remote->logoutLocal();
  remote->setCertificateSettings(wrong_roots, server_id);
  accepted = remote->login(host, port, direct_user, account_secret);
  idle = accepted && remote->waitForIdle(180000);
  check(idle && !remote->authenticated() &&
            remote->lastErrorKindForTest() != QStringLiteral("none") &&
            remote->lastErrorKindForTest() != QStringLiteral("auth"),
        "UNTRUSTED_ROOT_REJECTED");
  show_configuration(QStringLiteral("direct"), wrong_roots, server_id);
  check(certificate_feedback(
            QStringLiteral("server-certificate-untrusted"),
            QStringLiteral("服务器证书的签发者不在可信根列表中，连接已停止。"),
            QStringLiteral(
                "请向服务器管理员确认可信根文件，并从可信渠道导入后重试。")),
        "UNTRUSTED_ROOT_DIALOG_AND_FORM");
  check(capture(QStringLiteral("20-network-untrusted-root.png")),
        "UNTRUSTED_ROOT_SCREENSHOT");
  DismissError(window);
  remote->logoutLocal();

  remote->setCertificateSettings(roots, server_id);
  remote->setConnectionMode(QStringLiteral("ssh"));
  remote->setSshProgram(wrapper);
  remote->setSshHost(ssh_target);
  remote->setSshLocalPort(QString());
  show_configuration(QStringLiteral("ssh"), roots, server_id);
  const QString ssh_user = QStringLiteral("network-ssh");
  const bool ssh_registered = completed(remote->registerAccount(
      host, port, ssh_user, account_secret, account_secret));
  check(ssh_registered && remote->tunnelReady() && remote->tunnelOwnedByApp() &&
            !remote->tunnelExternalReuse() && remote->tunnelPidForTest() > 0 &&
            remote->authMode() == QStringLiteral("certificate"),
        "REAL_SSH_BPSEC2_REGISTER");
  remote->logoutLocal();
  if (check(completed(remote->login(host, port, ssh_user, account_secret)) &&
                remote->authenticated() && remote->tunnelReady(),
            "REAL_SSH_BPSEC2_LOGIN")) {
    if (page != nullptr) page->setProperty("draftUser", ssh_user);
    check(capture_session(
              QStringLiteral("18-network-real-ssh-certificate-session.png"),
              ssh_user),
          "REAL_SSH_SESSION_SCREENSHOT");
    round_trip(QStringLiteral("ssh"));
  }
  const qint64 owned_pid = remote->tunnelPidForTest();
  const QString endpoint = remote->tunnelLocalEndpointText();
  const int local_port = endpoint.section(QLatin1Char(':'), -1).toInt();
  remote->logoutLocal();
  remote->stopTunnel();
  Pump(100);
  QString port_error;
  check(owned_pid > 0 && !remote->tunnelReady() &&
            remote->tunnelPidForTest() == 0 &&
            !QFileInfo::exists(QStringLiteral("/proc/%1").arg(owned_pid)) &&
            local_port > 1024 &&
            !backup_modern::SshTunnelManager::IsLoopbackPortOpen(
                local_port, 200, &port_error),
        "OWNED_SSH_PROCESS_AND_LISTENER_RECLAIMED");
  check(capture(QStringLiteral("21-network-ssh-stopped.png")),
        "SSH_STOPPED_SCREENSHOT");
  std::printf("NETWORK passed=%d failed=%d\n", passed, failed);
  return failed == 0 ? 0 : 1;
}

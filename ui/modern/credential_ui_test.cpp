// credential_ui_test.cpp
//
// 真实 QML 输入与合成服务器验证会话密码、连接复用及清空行为。
#include "credential_ui_test.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QProcess>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVariantMap>
#include <cstdio>
#include <functional>

#include "backup_catalog.h"
#include "backup_controller.h"
#include "backup_credentials.h"
#include "cloud_task.h"
#include "incremental_restore.h"
#include "realtime_controller.h"
#include "remote_controller.h"
#include "schedule_controller.h"
#include "secure_transport.h"
#include "server_identity_import.h"
#include "x25519.h"

namespace {

void Settle() {
  QEventLoop loop;
  QTimer::singleShot(60, &loop, &QEventLoop::quit);
  loop.exec();
}

QQuickItem* FindItem(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name) return parent;
  for (QQuickItem* child : parent->childItems()) {
    QQuickItem* found = FindItem(child, name);
    if (found != nullptr) return found;
  }
  return nullptr;
}

bool Reveal(QQuickWindow* window, QQuickItem* item,
            const QString& scroll_name) {
  if (item == nullptr) return false;
  if (scroll_name.isEmpty()) return item->isVisible() && item->isEnabled();
  QQuickItem* scroll = FindItem(window->contentItem(), scroll_name);
  if (scroll == nullptr) return false;
  QQuickItem* flick = scroll->property("contentItem").value<QQuickItem*>();
  if (flick == nullptr) return false;
  const qreal current = flick->property("contentY").toReal();
  const qreal top = item->mapToItem(flick, QPointF()).y();
  const qreal maximum = qMax(
      qreal(0), flick->property("contentHeight").toReal() - flick->height());
  flick->setProperty("contentY",
                     qBound(qreal(0), current + top - 160, maximum));
  Settle();
  return item->isVisible() && item->isEnabled();
}

bool Click(QQuickWindow* window, QQuickItem* item, const QString& scroll_name) {
  if (!Reveal(window, item, scroll_name)) return false;
  const QPointF position =
      item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
  const QPointF global = window->mapToGlobal(position.toPoint());
  QMouseEvent press(QEvent::MouseButtonPress, position, position, global,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, position, position, global,
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &release);
  Settle();
  return true;
}

bool Type(QQuickWindow* window, QQuickItem* item, const QString& text,
          const QString& scroll_name) {
  if (!Reveal(window, item, scroll_name)) return false;
  item->setProperty("text", QString());
  item->forceActiveFocus(Qt::OtherFocusReason);
  for (QChar character : text) {
    QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier,
                    QString(character));
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_unknown, Qt::NoModifier,
                      QString(character));
    QCoreApplication::sendEvent(window, &release);
  }
  Settle();
  return item->property("text").toString() == text;
}

QByteArray Read(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray Digest(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return QByteArray();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&file) ? hash.result() : QByteArray();
}

bool WaitUntil(const std::function<bool()>& predicate, int timeout_ms) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeout_ms) Settle();
  return predicate();
}

bool WritePrivate(const QString& path, const QByteArray& content) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const bool written = file.write(content) == content.size();
  file.close();
  return file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner) &&
         written;
}

struct ServerGuard {
  QProcess process;
  ~ServerGuard() {
    if (process.state() != QProcess::NotRunning) {
      process.terminate();
      if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(3000);
      }
    }
  }
};

int RunCertificateRestartStage(QQuickWindow* window,
                               backup_modern::RemoteController* remote,
                               const QString& output) {
  int passed = 0;
  int failed = 0;
  const auto check = [&](bool ok, const char* label) {
    std::printf("CERTIFICATE_RESTART %s=%s\n", label, ok ? "PASS" : "FAIL");
    if (!ok)
      std::fprintf(stderr, "CERTIFICATE_RESTART kind=%s\n",
                   qPrintable(remote->lastErrorKindForTest()));
    std::fflush(stdout);
    ok ? ++passed : ++failed;
    return ok;
  };
  const QDir fixture(
      qEnvironmentVariable("BACKUP_CERTIFICATE_RESTART_FIXTURE"));
  const QString stage =
      qEnvironmentVariable("BACKUP_CERTIFICATE_RESTART_STAGE");
  const QString profile =
      qEnvironmentVariable("BACKUP_CERTIFICATE_RESTART_PROFILE");
  const QString secret =
      QString::fromUtf8(Read(fixture.filePath("restart.password"))).trimmed();
  const QString roots = fixture.filePath("root.key.pub");
  const QString server_id = QStringLiteral("acceptance-loopback-server");
  const QString port =
      QString::fromUtf8(Read(fixture.filePath("backup.port"))).trimmed();
  const QString bp1_port =
      QString::fromUtf8(Read(fixture.filePath("bpsec1.port"))).trimmed();
  const QString pin =
      QString::fromUtf8(Read(fixture.filePath("bpsec1.server-key"))).trimmed();
  const QString host = QStringLiteral("127.0.0.1");
  const QString user = QStringLiteral("certificate-restart");
  window->resize(1280, 980);
  window->requestActivate();
  window->setProperty("currentPageKey", QStringLiteral("management"));
  QObject* management =
      window->findChild<QObject*>(QStringLiteral("managementPage"));
  if (management)
    management->setProperty("currentTab", QStringLiteral("cloud"));
  Settle();
  QObject* page = window->findChild<QObject*>(QStringLiteral("remotePage"));
  QObject* roots_dialog =
      window->findChild<QObject*>(QStringLiteral("remoteTrustedRootsDialog"));
  QObject* identity_dialog =
      window->findChild<QObject*>(QStringLiteral("remoteServerIdentityDialog"));
  auto* identity = qobject_cast<backup_modern::ServerIdentityImport*>(
      qmlContext(window)
          ->contextProperty(QStringLiteral("serverIdentity"))
          .value<QObject*>());
  if (!check(page && roots_dialog && identity_dialog && identity &&
                 secret.size() >= 16 && !profile.isEmpty() &&
                 QDir().mkpath(output),
             "REAL_GUI_AND_PRIVATE_FIXTURE"))
    return 1;
  check(!remote->authenticated() && !remote->sharedSessionReady() &&
            remote->username().isEmpty() && remote->tunnelPidForTest() == 0 &&
            page->property("draftPassword").toString().isEmpty(),
        "FRESH_GUI_PROCESS_HAS_NO_CREDENTIALS_OR_SESSION");
  const auto select = [&](QObject* dialog, const QString& path) {
    return dialog->setProperty("selectedFile", QUrl::fromLocalFile(path)) &&
           QMetaObject::invokeMethod(dialog, "accepted", Qt::DirectConnection);
  };
  const auto tabs = [&](const char* name, const QString& key) {
    QObject* control = window->findChild<QObject*>(QString::fromLatin1(name));
    const bool ok = control && QMetaObject::invokeMethod(control, "activated",
                                                         Qt::DirectConnection,
                                                         Q_ARG(QString, key));
    Settle();
    return ok;
  };
  const auto completed = [&](bool accepted) {
    return accepted && remote->waitForIdle(45000) &&
           remote->lastErrorKindForTest() == QStringLiteral("none");
  };
  const auto login = [&]() {
    return completed(remote->login(host, port, user, secret));
  };
  const auto capture = [&](const QString& name) {
    Reveal(window,
           FindItem(window->contentItem(),
                    QStringLiteral("remoteExpectedServerIdField")),
           QStringLiteral("remotePageScroll"));
    Settle();
    const QImage image = window->grabWindow();
    return !image.isNull() && image.save(output + QLatin1Char('/') + name);
  };
  const auto identity_matches = [&]() {
    return remote->authMode() == QStringLiteral("certificate") &&
           remote->trustedRootsFile() == roots &&
           remote->expectedServerId() == server_id &&
           page->property("draftTrustedRoots").toString() == roots &&
           page->property("draftExpectedServerId").toString() == server_id;
  };
  if (stage == QStringLiteral("invalid-profile")) {
    const QFileInfo before_info(profile);
    const bool regular = before_info.isFile() && !before_info.isSymLink();
    const QByteArray before = regular ? Read(profile) : QByteArray();
    check(!remote->connectionProfileError().isEmpty() &&
              remote->trustedRootsFile().isEmpty() &&
              remote->expectedServerId().isEmpty() &&
              remote->serverKeyPin().isEmpty() &&
              (regular ? Read(profile) == before : QFileInfo(profile).exists()),
          "MALFORMED_DISK_PROFILE_REJECTED_WITHOUT_OVERWRITE");
  } else if (stage == QStringLiteral("prepare")) {
    if (!check(remote->connectionProfileError().isEmpty() &&
                   tabs("remoteConnectionModeTabs", QStringLiteral("direct")) &&
                   remote->setServerKeyPin(pin) &&
                   completed(remote->registerAccount(host, bp1_port, user,
                                                     secret, secret)) &&
                   completed(remote->login(host, bp1_port, user, secret)),
               "REAL_BPSEC1_SESSION_BEFORE_SWITCH"))
      return 1;
    check(tabs("remoteAuthModeTabs", QStringLiteral("certificate")) &&
              !remote->authenticated() && !remote->sharedSessionReady() &&
              remote->trustedRootsFile().isEmpty() &&
              remote->expectedServerId().isEmpty(),
          "BPSEC1_TO_BPSEC2_DROPS_OLD_SESSION_AND_REQUIRES_IDENTITY");
    const int dispatched = remote->dispatchedRequestCountForTest();
    check(!remote->login(host, port, user, secret) &&
              remote->lastErrorKindForTest() == QStringLiteral("validation") &&
              remote->dispatchedRequestCountForTest() == dispatched,
          "MISSING_CERTIFICATE_IDENTITY_NEVER_DISPATCHES");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &escape);
    Settle();
    if (!check(
            select(roots_dialog, roots) &&
                select(identity_dialog,
                       fixture.filePath("renamed-not-the-server-id.bpcert")) &&
                identity->serverId() == server_id && identity_matches(),
            "REAL_ROOT_AND_CERTIFICATE_DIALOG_READ_SIGNED_SERVER_ID"))
      return 1;
    const QByteArray trusted_profile = Read(profile);
    for (const QString& rejected :
         {QStringLiteral("tampered.bpcert"), QStringLiteral("expired.bpcert"),
          QStringLiteral("wrong-root")}) {
      if (rejected == QStringLiteral("wrong-root"))
        page->setProperty("draftTrustedRoots",
                          fixture.filePath("wrong-root.key.pub"));
      const QString candidate = rejected == QStringLiteral("wrong-root")
                                    ? fixture.filePath("server.bpcert")
                                    : fixture.filePath(rejected);
      check(select(identity_dialog, candidate) &&
                identity->serverId().isEmpty() &&
                !identity->message().isEmpty() &&
                remote->trustedRootsFile() == roots &&
                remote->expectedServerId() == server_id &&
                Read(profile) == trusted_profile,
            qPrintable(QStringLiteral("REJECTED_IMPORT_PRESERVES_TRUST_") +
                       rejected));
      page->setProperty("draftTrustedRoots", roots);
    }
    if (!check(completed(
                   remote->registerAccount(host, port, user, secret, secret)) &&
                   login() && identity_matches() && !remote->tunnelReady(),
               "IMPORTED_BPSEC2_IDENTITY_REACHES_REAL_DIRECT_SERVER"))
      return 1;
    check(tabs("remoteConnectionModeTabs", QStringLiteral("ssh")) &&
              !remote->authenticated() && !remote->sharedSessionReady() &&
              identity_matches(),
          "TCP_TO_SSH_INVALIDATES_SESSION_AND_PRESERVES_CERTIFICATE_IDENTITY");
    remote->setSshProgram(fixture.filePath("fixture-ssh"));
    remote->setSshHost(
        QString::fromUtf8(Read(fixture.filePath("ssh.target"))).trimmed());
    if (!check(login() && remote->tunnelReady() &&
                   remote->tunnelOwnedForTest() && identity_matches(),
               "IMPORTED_IDENTITY_REACHES_REAL_SSH_SERVER"))
      return 1;
    check(tabs("remoteConnectionModeTabs", QStringLiteral("direct")) &&
              !remote->authenticated() && !remote->tunnelReady() &&
              identity_matches() && login(),
          "SSH_TO_TCP_RECONNECTS_WITH_CURRENT_CERTIFICATE_IDENTITY");
    remote->logoutLocal();
    check(capture(QStringLiteral("01-imported-direct-profile.png")),
          "IMPORTED_PROFILE_SCREENSHOT");
  } else if (stage == QStringLiteral("reopen-direct") ||
             stage == QStringLiteral("reopen-ssh")) {
    const QString expected_mode = stage == QStringLiteral("reopen-direct")
                                      ? QStringLiteral("direct")
                                      : QStringLiteral("ssh");
    if (!check(identity_matches() &&
                   remote->connectionMode() == expected_mode &&
                   page->property("draftConnectionMode").toString() ==
                       expected_mode &&
                   remote->host() == host && remote->portText() == port &&
                   remote->connectionProfileError().isEmpty(),
               "NEW_GUI_LOADS_DISK_PROFILE_AND_QML_FIELDS_CONSISTENTLY"))
      return 1;
    // Only the fixture executable is injected; it is intentionally never loaded
    // from the persisted product profile. OpenSSH owns authentication secrets.
    remote->setSshProgram(fixture.filePath("fixture-ssh"));
    if (!check(login() && identity_matches() &&
                   remote->tunnelReady() ==
                       (expected_mode == QStringLiteral("ssh")),
               "NEW_GUI_REAUTHENTICATES_WITH_IMPORTED_IDENTITY"))
      return 1;
    remote->logoutLocal();
    remote->stopTunnel();
    if (expected_mode == QStringLiteral("direct")) {
      for (bool wrong_root : {false, true}) {
        remote->setCertificateSettings(
            wrong_root ? fixture.filePath("wrong-root.key.pub") : roots,
            wrong_root ? server_id : QStringLiteral("wrong-server-id"));
        const bool accepted = remote->login(host, port, user, secret);
        check(
            accepted && remote->waitForIdle(45000) &&
                !remote->authenticated() && !remote->sharedSessionReady() &&
                remote->lastErrorKindForTest() ==
                    (wrong_root ? QStringLiteral("server-certificate-untrusted")
                                : QStringLiteral(
                                      "server-certificate-wrong-server-id")),
            wrong_root
                ? "REOPENED_GUI_REJECTS_WRONG_ROOT_WITH_EXACT_CLASS"
                : "REOPENED_GUI_REJECTS_WRONG_SERVER_ID_WITH_EXACT_CLASS");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &escape);
        Settle();
      }
      remote->setCertificateSettings(roots, server_id);
      const bool switched =
          tabs("remoteAuthModeTabs", QStringLiteral("pin")) &&
          tabs("remoteAuthModeTabs", QStringLiteral("certificate"));
      check(switched && remote->trustedRootsFile().isEmpty() &&
                remote->expectedServerId().isEmpty() &&
                page->property("draftTrustedRoots").toString().isEmpty() &&
                page->property("draftExpectedServerId").toString().isEmpty() &&
                QJsonDocument::fromJson(Read(profile))
                    .object()
                    .value("expected_server_id")
                    .toString()
                    .isEmpty(),
            "REOPENED_GUI_BPSEC2_BPSEC1_BPSEC2_CLEARS_STALE_IDENTITY_ON_DISK_"
            "AND_UI");
      check(
          select(roots_dialog, roots) &&
              select(identity_dialog, fixture.filePath("server.bpcert")) &&
              identity_matches() && login(),
          "REOPENED_GUI_REIMPORTS_AND_REAUTHENTICATES_AFTER_AUTH_MODE_SWITCH");
      remote->logoutLocal();
      check(tabs("remoteConnectionModeTabs", QStringLiteral("ssh")) &&
                identity_matches(),
            "SAVE_SSH_PROFILE_FOR_THIRD_GUI_PROCESS");
    }
    check(capture(stage + QStringLiteral(".png")), "REOPEN_PROFILE_SCREENSHOT");
  } else {
    check(false, "KNOWN_RESTART_STAGE");
  }
  if (stage != QStringLiteral("invalid-profile")) {
    const QByteArray disk = Read(profile);
    const QJsonObject object = QJsonDocument::fromJson(disk).object();
    check(!disk.isEmpty() && !disk.contains(secret.toUtf8()) &&
              !object.contains("username") && !object.contains("password") &&
              !object.contains("token") && !object.contains("ssh_program") &&
              object.value("expected_server_id").toString() == server_id &&
              object.value("trusted_roots_file").toString() == roots,
          "DISK_PROFILE_HAS_PUBLIC_SETTINGS_WITHOUT_CREDENTIALS");
    check(QFileInfo(profile).permissions() ==
              (QFileDevice::ReadOwner | QFileDevice::WriteOwner |
               QFileDevice::ReadUser | QFileDevice::WriteUser),
          "DISK_PROFILE_PRIVATE_PERMISSIONS");
  }
  remote->logoutLocal();
  remote->stopTunnel();
  std::printf("CERTIFICATE_RESTART stage=%s pid=%lld passed=%d failed=%d\n",
              qPrintable(stage),
              static_cast<long long>(QCoreApplication::applicationPid()),
              passed, failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace

int RunCredentialUiTest(QQuickWindow* window,
                        backup_modern::ScheduleController* schedule,
                        backup_modern::RealtimeController* realtime,
                        backup_modern::RemoteController* remote,
                        backup_modern::BackupController* controller,
                        const QString& output) {
  if (!qEnvironmentVariable("BACKUP_CERTIFICATE_RESTART_STAGE").isEmpty())
    return RunCertificateRestartStage(window, remote, output);
  int passed = 0;
  int failed = 0;
  const auto check = [&](bool ok, const char* label) {
    std::printf("CREDENTIAL_UI %s=%s\n", label, ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    ok ? ++passed : ++failed;
    return ok;
  };
  QTemporaryDir work;
  if (!check(work.isValid() && QDir().mkpath(output), "ISOLATED_WORKSPACE"))
    return 1;
  const QString source = work.filePath(QStringLiteral("source"));
  const QString repository = work.filePath(QStringLiteral("repository"));
  if (!check(QDir().mkpath(source) &&
                 WritePrivate(source + QStringLiteral("/fixture.txt"),
                              "synthetic credential GUI fixture\n") &&
                 controller->saveRepositoryPath(repository),
             "SYNTHETIC_SOURCE_AND_REPOSITORY"))
    return 1;
  schedule->start();
  realtime->start();
  window->resize(1280, 980);
  window->requestActivate();
  Settle();
  check(!schedule->sessionPasswordUnlocked() &&
            !realtime->sessionPasswordUnlocked() &&
            !remote->sharedSessionReady(),
        "NEW_PROCESS_STARTS_LOCKED_AND_LOGGED_OUT");
  const QString password = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString wrong_password =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto capture = [&](const QString& filename) {
    Settle();
    const QImage image = window->grabWindow();
    check(!image.isNull() && image.save(output + QLatin1Char('/') + filename),
          "PASSWORD_FREE_SCREENSHOT");
  };
  const auto item = [&](const QString& name) {
    return FindItem(window->contentItem(), name);
  };

  for (const QString& prefix :
       {QStringLiteral("schedule"), QStringLiteral("realtime")}) {
    const bool scheduled = prefix == QStringLiteral("schedule");
    const QString task_repository =
        work.filePath(prefix + QStringLiteral("-repository"));
    if (!check(controller->saveRepositoryPath(task_repository) &&
                   WritePrivate(source + QStringLiteral("/fixture.txt"),
                                prefix.toUtf8() + " baseline\n"),
               scheduled ? "SCHEDULE_ISOLATED_REPOSITORY"
                         : "REALTIME_ISOLATED_REPOSITORY"))
      return 1;
    window->setProperty("currentPageKey", scheduled
                                              ? QStringLiteral("scheduled")
                                              : QStringLiteral("realtime"));
    Settle();
    const QString scroll_name = prefix + QStringLiteral("PageScroll");
    QQuickItem* scroll = item(scroll_name);
    QQuickItem* page = scroll == nullptr ? nullptr : scroll->parentItem();
    QQuickItem* first = item(prefix + QStringLiteral("ArchivePasswordField"));
    QQuickItem* confirmation =
        item(prefix + QStringLiteral("ArchivePasswordConfirmationField"));
    QQuickItem* save = item(scheduled ? QStringLiteral("saveScheduleButton")
                                      : QStringLiteral("saveRealtimeButton"));
    if (!check(page != nullptr && first != nullptr && confirmation != nullptr &&
                   save != nullptr,
               scheduled ? "SCHEDULE_QML_CONTROLS" : "REALTIME_QML_CONTROLS"))
      return 1;
    page->setProperty("advancedExpanded", true);
    page->setProperty("draftEnabled", false);
    page->setProperty("draftSource", source);
    page->setProperty("draftRetain", QStringLiteral("5"));
    page->setProperty("draftPackIndex", 0);
    page->setProperty("draftCompressionIndex", 0);
    page->setProperty("draftStrategyIndex", 1);
    page->setProperty("draftEncryptionIndex", 1);
    if (scheduled) {
      page->setProperty("draftFrequencyValue", QStringLiteral("2"));
      page->setProperty("draftFrequencyUnit", QStringLiteral("hours"));
    }
    Settle();
    check(first->property("echoMode").toInt() == 2 &&
              confirmation->property("echoMode").toInt() == 2,
          scheduled ? "SCHEDULE_BOTH_PASSWORD_FIELDS_MASKED"
                    : "REALTIME_BOTH_PASSWORD_FIELDS_MASKED");
    check(
        Type(window, first, password, scroll_name) &&
            Type(window, confirmation, wrong_password, scroll_name) &&
            Click(window, save, scroll_name) &&
            !(scheduled ? schedule->sessionPasswordUnlocked()
                        : realtime->sessionPasswordUnlocked()),
        scheduled ? "SCHEDULE_MISMATCH_REFUSED" : "REALTIME_MISMATCH_REFUSED");
    check(Type(window, confirmation, password, scroll_name) &&
              Click(window, save, scroll_name) &&
              (scheduled ? schedule->sessionPasswordUnlocked()
                         : realtime->sessionPasswordUnlocked()) &&
              first->property("text").toString().isEmpty() &&
              confirmation->property("text").toString().isEmpty(),
          scheduled ? "SCHEDULE_SAVE_UNLOCKS_AND_CLEARS"
                    : "REALTIME_SAVE_UNLOCKS_AND_CLEARS");
    const QByteArray stored =
        Read(scheduled ? schedule->storePath() : realtime->storePath());
    check(!stored.isEmpty() && !stored.contains(password.toUtf8()) &&
              stored.contains("/@backup-project-session/"),
          scheduled ? "SCHEDULE_CONFIG_HAS_REFERENCE_ONLY"
                    : "REALTIME_CONFIG_HAS_REFERENCE_ONLY");
    check((scheduled ? schedule->sourcePath() : realtime->sourcePath()) ==
                  source &&
              (scheduled ? schedule->retainCount() : realtime->retainCount()) ==
                  5 &&
              (scheduled ? schedule->strategyKey() : realtime->strategyKey()) ==
                  QStringLiteral("incremental") &&
              (!scheduled || schedule->intervalMinutes() == 120),
          scheduled ? "SCHEDULE_UNLOCK_PRESERVES_OTHER_EDITS"
                    : "REALTIME_UNLOCK_PRESERVES_OTHER_EDITS");
    const auto save_scheduled_enabled = [&](bool enabled) {
      return schedule->setEnabled(enabled) &&
             schedule->strategyKey() == QStringLiteral("incremental");
    };
    const bool baseline_started =
        scheduled
            ? save_scheduled_enabled(true) && schedule->runNow() &&
                  schedule->waitForIdle(90000) && schedule->lastSucceeded()
            : realtime->setEnabled(true) &&
                  WaitUntil([&]() { return realtime->snapshotCount() >= 1; },
                            90000) &&
                  realtime->waitForIdle(90000) && realtime->lastSucceeded();
    backupproject::BackupCatalog catalog;
    std::vector<backupproject::BackupRecord> records;
    std::string task_error;
    const bool full_created =
        baseline_started &&
        catalog.List(task_repository.toStdString(), &records, &task_error) &&
        records.size() == 1 && !records.front().incremental_delta &&
        records.front().password_required &&
        records.front().encryption_method ==
            backupproject::EncryptionMethod::kAes256CtrHmacSha256;
    check(full_created, scheduled ? "SCHEDULE_SESSION_AES_FULL_CREATED"
                                  : "REALTIME_SESSION_AES_FULL_CREATED");
    const bool modified =
        WritePrivate(source + QStringLiteral("/fixture.txt"),
                     prefix.toUtf8() + " changed small file\n");
    const bool delta_finished =
        modified &&
        (scheduled
             ? schedule->runNow() && schedule->waitForIdle(90000) &&
                   schedule->lastSucceeded()
             : WaitUntil([&]() { return realtime->snapshotCount() >= 2; },
                         90000) &&
                   realtime->waitForIdle(90000) && realtime->lastSucceeded());
    records.clear();
    std::string delta_name;
    if (delta_finished &&
        catalog.List(task_repository.toStdString(), &records, &task_error)) {
      for (const auto& record : records) {
        if (record.incremental_delta && record.password_required &&
            record.encryption_method ==
                backupproject::EncryptionMethod::kAes256CtrHmacSha256)
          delta_name = record.file_name;
      }
    }
    check(full_created && !delta_name.empty(),
          scheduled ? "SCHEDULE_SESSION_AES_DELTA_CREATED"
                    : "REALTIME_SESSION_AES_DELTA_CREATED");
    backupproject::RestoreOptions restore_options;
    restore_options.password = password.toStdString();
    backupproject::RestoreReport report;
    const QString restored =
        work.filePath(prefix + QStringLiteral("-restored"));
    const bool restored_ok =
        !delta_name.empty() &&
        backupproject::RestoreSnapshotChain(
            task_repository.toStdString(), delta_name, restored.toStdString(),
            restore_options, &report, &task_error);
    const QByteArray expected_digest =
        Digest(source + QStringLiteral("/fixture.txt"));
    check(restored_ok && !expected_digest.isEmpty() &&
              Digest(restored + QStringLiteral("/fixture.txt")) ==
                  expected_digest,
          scheduled ? "SCHEDULE_SESSION_AES_CHAIN_RESTORE_SHA256"
                    : "REALTIME_SESSION_AES_CHAIN_RESTORE_SHA256");
    if (scheduled) {
      Settle();
      const QVariantList history = schedule->history();
      const QString diagnostic = history.value(0)
                                     .toMap()
                                     .value(QStringLiteral("diagnostic"))
                                     .toString();
      check(!diagnostic.isEmpty() &&
                schedule->lastResultText().contains(
                    QStringLiteral("备份已完成")) &&
                !schedule->lastResultText().contains(diagnostic),
            "SCHEDULE_RESULT_KEEPS_TECHNICAL_DIAGNOSTIC_IN_DETAILS");
      QQuickItem* details =
          item(QStringLiteral("scheduleHistoryDetailsToggle0"));
      QQuickItem* raw = item(QStringLiteral("scheduleHistoryDiagnostic0"));
      QQuickItem* summary = item(QStringLiteral("scheduleHistorySummary0"));
      if (!check(details != nullptr && raw != nullptr && summary != nullptr &&
                     !raw->isVisible() &&
                     !summary->property("text").toString().contains(diagnostic),
                 "SCHEDULE_HISTORY_DIAGNOSTIC_COLLAPSED_BY_DEFAULT"))
        return 1;
      check(Click(window, details, scroll_name) &&
                Reveal(window, raw, scroll_name) &&
                raw->property("text").toString() == diagnostic,
            "SCHEDULE_HISTORY_DETAILS_SHOW_ORIGINAL_DIAGNOSTIC");
      check(Click(window, details, scroll_name) && !raw->isVisible() &&
                schedule->history()
                        .value(0)
                        .toMap()
                        .value(QStringLiteral("diagnostic"))
                        .toString() == diagnostic,
            "SCHEDULE_HISTORY_DETAILS_COLLAPSE_PRESERVES_DIAGNOSTIC");
    }
    check(
        scheduled ? save_scheduled_enabled(false) : realtime->setEnabled(false),
        scheduled ? "SCHEDULE_TASK_DISABLED_AFTER_EXECUTION"
                  : "REALTIME_TASK_DISABLED_AFTER_EXECUTION");
    check(Type(window, first, password, scroll_name),
          scheduled ? "SCHEDULE_PASSWORD_REENTERED"
                    : "REALTIME_PASSWORD_REENTERED");
    window->setProperty("currentPageKey", QStringLiteral("home"));
    Settle();
    check(first->property("text").toString().isEmpty() &&
              confirmation->property("text").toString().isEmpty(),
          scheduled ? "SCHEDULE_LEAVING_PAGE_CLEARS_INPUT"
                    : "REALTIME_LEAVING_PAGE_CLEARS_INPUT");
    if (scheduled) {
      schedule->lockArchivePassword();
      schedule->reload();
    } else {
      realtime->lockArchivePassword();
      realtime->reload();
    }
    window->setProperty("currentPageKey", scheduled
                                              ? QStringLiteral("scheduled")
                                              : QStringLiteral("realtime"));
    Settle();
    check(!(scheduled ? schedule->sessionPasswordUnlocked()
                      : realtime->sessionPasswordUnlocked()) &&
              (scheduled ? schedule->encryptionNote()
                         : realtime->encryptionNote())
                  .contains(QStringLiteral("输入")),
          scheduled ? "SCHEDULE_RELOAD_REQUIRES_UNLOCK"
                    : "REALTIME_RELOAD_REQUIRES_UNLOCK");
    page->setProperty("advancedExpanded", true);
    Settle();
    QQuickItem* note = item(prefix + QStringLiteral("EncryptionNote"));
    QQuickItem* viewport = scroll->property("contentItem").value<QQuickItem*>();
    const auto in_view = [&](QQuickItem* control) {
      if (control == nullptr || !control->isVisible() || viewport == nullptr)
        return false;
      const QPointF position = control->mapToItem(viewport, QPointF());
      return position.x() >= 0 && position.y() >= 0 &&
             position.x() + control->width() <= viewport->width() &&
             position.y() + control->height() <= viewport->height();
    };
    if (!check(Reveal(window, first, scroll_name) && in_view(first) &&
                   in_view(confirmation) && in_view(note) &&
                   first->property("text").toString().isEmpty() &&
                   confirmation->property("text").toString().isEmpty() &&
                   note->property("text").toString().contains(
                       QStringLiteral("输入")),
               scheduled ? "SCHEDULE_PASSWORD_CAPTURE_FIELDS_AND_UNLOCK_NOTE"
                         : "REALTIME_PASSWORD_CAPTURE_FIELDS_AND_UNLOCK_NOTE"))
      return 1;
    capture(scheduled ? QStringLiteral("17-schedule-session-password.png")
                      : QStringLiteral("18-realtime-session-password.png"));
  }

  // 只使用本轮创建的环回服务器、随机身份和内存中的合成账号密码。
  QDir().mkpath(work.filePath(QStringLiteral("server-state")));
  const QString key_file = work.filePath(QStringLiteral("server-state/key"));
  const QString secret_file =
      work.filePath(QStringLiteral("server-state/secret"));
  const QString log_file =
      work.filePath(QStringLiteral("server-state/server.log"));
  backupproject::net::TransportIdentity identity;
  std::string error;
  QByteArray random_secret(32, '\0');
  for (char& byte : random_secret)
    byte = static_cast<char>(QRandomGenerator::global()->bounded(256));
  if (!check(backupproject::net::GenerateTransportIdentity(&identity, &error) &&
                 backupproject::net::SaveTransportIdentity(
                     key_file.toStdString(), identity, false, &error) &&
                 WritePrivate(secret_file, "BACKUP_TOKEN_SECRET=" +
                                               random_secret.toHex() + "\n"),
             "SYNTHETIC_SERVER_IDENTITY"))
    return 1;
  const QString pin =
      QStringLiteral("sha256:") +
      QString::fromStdString(
          backupproject::crypto::X25519Fingerprint(identity.public_key));
  ServerGuard server;
  server.process.setProgram(QCoreApplication::applicationDirPath() +
                            QStringLiteral("/backup-server"));
  server.process.setArguments(
      {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"),
       QStringLiteral("--port"), QStringLiteral("0"), QStringLiteral("--root"),
       work.filePath(QStringLiteral("server-data")), QStringLiteral("--db"),
       work.filePath(QStringLiteral("server-state/db.sqlite3")),
       QStringLiteral("--secret-file"), secret_file,
       QStringLiteral("--transport-key-file"), key_file,
       QStringLiteral("--log-file"), log_file, QStringLiteral("--pid-file"),
       work.filePath(QStringLiteral("server-state/pid")),
       QStringLiteral("--quiet")});
  server.process.start();
  int port = 0;
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 15000 && port == 0) {
    Settle();
    const QString log = QString::fromUtf8(Read(log_file));
    const QString marker = QStringLiteral("listening on 127.0.0.1:");
    const int at = log.indexOf(marker);
    if (at >= 0) {
      int end = at + marker.size();
      while (end < log.size() && log.at(end).isDigit()) ++end;
      port = log.mid(at + marker.size(), end - at - marker.size()).toInt();
    }
    if (server.process.state() == QProcess::NotRunning) break;
  }
  if (!check(port > 0 && server.process.state() == QProcess::Running,
             "OWNED_LOOPBACK_SERVER_READY"))
    return 1;
  const QString host = QStringLiteral("127.0.0.1");
  const QString port_text = QString::number(port);
  const QString username = QStringLiteral("session-gui-user");
  const QString login_password =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  remote->setConnectionMode(QStringLiteral("direct"));
  remote->setAuthMode(QStringLiteral("pin"));
  if (!check(
          remote->registerAccountWithPin(host, port_text, username,
                                         login_password, login_password, pin) &&
              remote->waitForIdle(30000) &&
              remote->lastErrorKindForTest() == QStringLiteral("none"),
          "REAL_SYNTHETIC_ACCOUNT_REGISTERED"))
    return 1;
  const auto login = [&]() {
    return remote->loginWithPin(host, port_text, username, login_password,
                                pin) &&
           remote->waitForIdle(30000) && remote->sharedSessionReady();
  };
  if (!check(login(), "REAL_LOGIN_PUBLISHES_SESSION")) return 1;
  for (const QString& prefix :
       {QStringLiteral("schedule"), QStringLiteral("realtime")}) {
    const bool scheduled = prefix == QStringLiteral("schedule");
    window->setProperty("currentPageKey", scheduled
                                              ? QStringLiteral("scheduled")
                                              : QStringLiteral("realtime"));
    Settle();
    QQuickItem* scroll = item(prefix + QStringLiteral("PageScroll"));
    QQuickItem* page = scroll == nullptr ? nullptr : scroll->parentItem();
    if (!check(page != nullptr, "CLOUD_TARGET_PAGE")) return 1;
    page->setProperty("draftRepositoryType", QStringLiteral("cloud"));
    Settle();
    check(Click(window, item(prefix + QStringLiteral("UseCloudSessionButton")),
                prefix + QStringLiteral("PageScroll")) &&
              page->property("draftCloudCredentialsFile").toString() ==
                  remote->sharedSessionReference() &&
              page->property("draftCloudRepositoryId").toString() ==
                  remote->sharedSessionRepositoryId(),
          scheduled ? "SCHEDULE_REUSES_LOGGED_IN_CONNECTION"
                    : "REALTIME_REUSES_LOGGED_IN_CONNECTION");
    if (scheduled) {
      page->setProperty("draftEncryptionIndex", 0);
      page->setProperty("draftStrategyIndex", 0);
      const bool saved =
          Click(window, item(QStringLiteral("saveScheduleButton")),
                QStringLiteral("schedulePageScroll")) &&
          schedule->repositoryType() == QStringLiteral("cloud") &&
          schedule->cloudCredentialsFile() == remote->sharedSessionReference();
      const bool ran = saved && schedule->setEnabled(true) &&
                       schedule->runNow() && schedule->waitForIdle(90000) &&
                       schedule->lastSucceeded();
      check(ran && remote->refreshList() && remote->waitForIdle(30000) &&
                remote->snapshotCountForTest() == 1,
            "SHARED_LOGIN_REACHES_REAL_CLOUD_AUTOMATIC_TASK");
      check(schedule->setEnabled(false), "CLOUD_AUTOMATIC_TASK_DISABLED");
    }
  }
  remote->logoutLocal();
  Settle();
  check(!remote->sharedSessionReady() &&
            !backupproject::SessionCloudCredentialsAvailable() &&
            !item(QStringLiteral("realtimeUseCloudSessionButton"))->isEnabled(),
        "LOGOUT_CLEARS_SHARED_CONNECTION_AND_BUTTON");
  if (!check(login(), "LOGIN_CAN_UNLOCK_CLOUD_AGAIN")) return 1;
  remote->setCertificateSettings(QString(), QStringLiteral("different-server"));
  check(!remote->sharedSessionReady(),
        "IDENTITY_SETTINGS_INVALIDATE_SHARED_LOGIN");
  if (!check(login(), "LOGIN_AFTER_IDENTITY_CHANGE")) return 1;
  remote->logoutLocal();
  const QString active_pin = remote->serverKeyPin();
  const bool pending_login =
      remote->loginWithPin(host, port_text, username, login_password, pin);
  const QString attempted_pin =
      QStringLiteral("sha256:") + QString(64, QLatin1Char('0'));
  const QString busy_pin_result = remote->applyServerKeyPin(attempted_pin);
  const bool mutation_rejected =
      busy_pin_result == QStringLiteral("invalid") &&
      remote->serverKeyPin() == active_pin &&
      remote->serverKeyPinError().contains(QStringLiteral("正在处理"));
  const bool login_completed =
      pending_login && remote->busy() && mutation_rejected &&
      remote->waitForIdle(30000) &&
      remote->lastErrorKindForTest() == QStringLiteral("none") &&
      remote->authenticated() && remote->sharedSessionReady();
  check(login_completed, "BUSY_LOGIN_REJECTS_PIN_CHANGE");
  const QString applied_pin = remote->applyServerKeyPin(attempted_pin);
  const bool changed_pin_invalidates_session =
      applied_pin == QStringLiteral("applied") && !remote->authenticated() &&
      !remote->sharedSessionReady();
  check(changed_pin_invalidates_session,
        "APPLIED_PIN_CHANGE_INVALIDATES_OLD_SESSION");
  remote->applyServerKeyPin(active_pin);
  remote->logoutLocal();
  window->setProperty("currentPageKey", QStringLiteral("management"));
  QObject* management =
      window->findChild<QObject*>(QStringLiteral("managementPage"));
  if (management != nullptr)
    management->setProperty("currentTab", QStringLiteral("cloud"));
  Settle();
  QObject* remote_page =
      window->findChild<QObject*>(QStringLiteral("remotePage"));
  QObject* restore_dialog =
      window->findChild<QObject*>(QStringLiteral("remoteChainRestoreDialog"));
  if (check(remote_page != nullptr && restore_dialog != nullptr,
            "CHAIN_RESTORE_PASSWORD_DIALOG")) {
    for (bool escape : {false, true}) {
      remote_page->setProperty("pendingRestoreId",
                               QStringLiteral("synthetic-chain"));
      remote_page->setProperty("pendingRestoreName",
                               QStringLiteral("合成备份"));
      remote_page->setProperty("draftRestorePath",
                               work.filePath(QStringLiteral("restore")));
      QMetaObject::invokeMethod(restore_dialog, "open");
      Settle();
      // Popup 打开后才把内容接入窗口的视觉树；关闭时不能从 childItems 找它。
      QQuickItem* restore_password =
          item(QStringLiteral("remoteChainRestorePasswordField"));
      const bool typed = Type(window, restore_password, password, QString());
      if (escape) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &release);
      } else {
        Click(window, item(QStringLiteral("remoteChainRestoreCancelButton")),
              QString());
      }
      Settle();
      check(typed && restore_password != nullptr &&
                !restore_dialog->property("visible").toBool() &&
                restore_password->property("text").toString().isEmpty() &&
                remote_page->property("pendingRestoreId").toString().isEmpty(),
            escape ? "CHAIN_RESTORE_ESCAPE_CLEARS_PASSWORD"
                   : "CHAIN_RESTORE_CANCEL_CLEARS_PASSWORD");
    }
  }
  realtime->stop();
  schedule->stop();
  std::printf("CREDENTIAL_UI passed=%d failed=%d\n", passed, failed);
  return failed == 0 ? 0 : 1;
}

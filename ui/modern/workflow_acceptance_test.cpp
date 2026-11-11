// workflow_acceptance_test.cpp
//
// 使用真实控制器、合成大文件与签名测试证书验证人工验收中的边界。

#include "workflow_acceptance_test.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <cstdio>

#include "backup_catalog.h"
#include "backup_controller.h"
#include "bpcert.h"
#include "ed25519.h"
#include "server_identity_import.h"

namespace {

bool Write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray Digest(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return QByteArray();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return QByteArray();
  return hash.result();
}

void Pump(int milliseconds) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < milliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
}

QQuickItem* FindItem(QQuickItem* parent, const char* name) {
  if (parent->objectName() == QLatin1String(name)) return parent;
  for (QQuickItem* child : parent->childItems()) {
    QQuickItem* found = FindItem(child, name);
    if (found != nullptr) return found;
  }
  return nullptr;
}

bool RevealAndClick(QQuickWindow* window, QQuickItem* item) {
  QQuickItem* scroll = FindItem(window->contentItem(), "backupPageScroll");
  if (item == nullptr || scroll == nullptr) return false;
  QQuickItem* flick = scroll->property("contentItem").value<QQuickItem*>();
  if (flick == nullptr) return false;
  const qreal top = item->mapToItem(flick, QPointF()).y();
  const qreal current = flick->property("contentY").toReal();
  const qreal maximum = qMax(
      qreal(0), flick->property("contentHeight").toReal() - flick->height());
  flick->setProperty("contentY",
                     qBound(qreal(0), current + top - 160, maximum));
  Pump(100);
  if (!item->isVisible() || !item->isEnabled()) return false;
  const QPointF position =
      item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
  const QPointF global = window->mapToGlobal(position.toPoint());
  QMouseEvent press(QEvent::MouseButtonPress, position, position, global,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, position, position, global,
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &release);
  return true;
}

void DismissError(QQuickWindow* window) {
  QObject* dialog =
      window->findChild<QObject*>(QStringLiteral("userErrorDialog"));
  if (dialog == nullptr || !dialog->property("visible").toBool()) return;
  QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &press);
  QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &release);
  Pump(100);
}

}  // namespace

int RunWorkflowAcceptanceTest(QQuickWindow* window,
                              backup_modern::BackupController* controller,
                              backup_modern::ServerIdentityImport* identity,
                              const QString& output) {
  int passed = 0;
  int failed = 0;
  const auto check = [&](bool ok, const char* name) {
    std::printf("WORKFLOW %s=%s\n", name, ok ? "PASS" : "FAIL");
    ok ? ++passed : ++failed;
    return ok;
  };
  if (!check(QDir().mkpath(output), "OUTPUT_DIRECTORY")) return 1;
  QTemporaryDir work;
  if (!check(work.isValid(), "SYNTHETIC_WORKSPACE")) return 1;
  const auto error_connection = QObject::connect(
      controller, &backup_modern::BackupController::userError, window,
      [](const QString&, const QString&, const QString&,
         const QString& technical) {
        std::fprintf(stderr, "WORKFLOW operation diagnostic: %s\n",
                     qPrintable(technical));
      });
  Q_UNUSED(error_connection);
  std::printf("WORKFLOW platform=%s\n",
              qPrintable(QGuiApplication::platformName()));
  const auto capture = [&](const QString& name) {
    Pump(150);
    const QImage image = window->grabWindow();
    return !image.isNull() && image.save(output + QLatin1Char('/') + name);
  };

  // 测试私钥仅存在于本进程；输出文件只有公钥和公开证书。
  const std::string seed(32, '\x25');
  std::string public_key;
  std::string error;
  if (!check(backupproject::crypto::Ed25519PublicKeyFromSeed(seed, &public_key,
                                                             &error),
             "TEST_ISSUER"))
    return 1;
  const QString roots = work.filePath(QStringLiteral("trusted-roots.pub"));
  const QString cert_path = work.filePath(QStringLiteral("server.bpcert"));
  check(Write(roots,
              QByteArray::fromStdString(
                  "acceptance-root " +
                  backupproject::crypto::Ed25519FormatPublicKeyHex(public_key) +
                  " unlimited\n")),
        "TEST_PUBLIC_ROOT");
  backupproject::crypto::Bpcert1 certificate;
  certificate.server_id = "acceptance-server";
  certificate.server_public_key.assign(32, '\x45');
  certificate.issuer_id = "acceptance-root";
  certificate.serial_number = 1;
  certificate.not_before = backupproject::crypto::Bpcert1NowUnixSeconds() - 60;
  certificate.not_after = certificate.not_before + 3600;
  std::string raw;
  if (!check(
          backupproject::crypto::Bpcert1Issue(certificate, seed, &raw, &error),
          "SIGNED_TEST_CERTIFICATE"))
    return 1;
  Write(cert_path, QByteArray::fromStdString(raw));
  check(identity->importCertificate(roots, cert_path) &&
            identity->serverId() == QStringLiteral("acceptance-server"),
        "SIGNED_IDENTITY_IMPORT");
  std::printf("WORKFLOW identity diagnostic: %s\n",
              qPrintable(identity->message()));
  std::string tampered = raw;
  tampered.back() ^= 1;
  Write(cert_path, QByteArray::fromStdString(tampered));
  check(!identity->importCertificate(roots, cert_path) &&
            identity->serverId().isEmpty(),
        "TAMPERED_SIGNATURE_REJECTED");
  Write(roots,
        "untrusted " +
            QByteArray::fromStdString(
                backupproject::crypto::Ed25519FormatPublicKeyHex(public_key)) +
            " unlimited\n");
  Write(cert_path, QByteArray::fromStdString(raw));
  check(!identity->importCertificate(roots, cert_path),
        "UNKNOWN_ISSUER_REJECTED");
  Write(roots,
        "acceptance-root " +
            QByteArray::fromStdString(
                backupproject::crypto::Ed25519FormatPublicKeyHex(public_key)) +
            " unlimited\n");
  certificate.not_after = certificate.not_before - 1;
  certificate.not_before -= 7200;
  certificate.not_after = certificate.not_before + 3600;
  backupproject::crypto::Bpcert1Issue(certificate, seed, &tampered, &error);
  Write(cert_path, QByteArray::fromStdString(tampered));
  check(!identity->importCertificate(roots, cert_path),
        "EXPIRED_IDENTITY_REJECTED");
  Write(cert_path, QByteArray::fromStdString(raw));
  check(identity->importCertificate(roots, cert_path),
        "VALID_IDENTITY_REIMPORT");
  window->setProperty("currentPageKey", QStringLiteral("management"));
  QObject* management =
      window->findChild<QObject*>(QStringLiteral("managementPage"));
  if (management != nullptr)
    management->setProperty("currentTab", QStringLiteral("cloud"));
  QObject* page = window->findChild<QObject*>(QStringLiteral("remotePage"));
  if (page != nullptr) {
    page->setProperty("draftServerKind", QStringLiteral("custom"));
    page->setProperty("draftAuthMode", QStringLiteral("certificate"));
    page->setProperty("draftConnectionMode", QStringLiteral("direct"));
    page->setProperty("draftTrustedRoots", roots);
    page->setProperty("draftExpectedServerId", identity->serverId());
    check(capture(QStringLiteral("16-cloud-certificate-import.png")),
          "IDENTITY_SCREENSHOT");
  }

  const QString source = work.filePath(QStringLiteral("source"));
  const QString repository = work.filePath(QStringLiteral("repository"));
  QDir().mkpath(source);
  const QString large = source + QStringLiteral("/large.bin");
  QFile file(large);
  if (!check(file.open(QIODevice::WriteOnly), "LARGE_FILE_OPEN")) return 1;
  const QByteArray chunk(1 << 20, 'q');
  bool written = true;
  for (int i = 0; i < 64; ++i)
    written = file.write(chunk) == chunk.size() && written;
  file.close();
  check(written, "SYNTHETIC_64M_FILE");
  check(Write(source + QStringLiteral("/说明.txt"), "acceptance\n") &&
            Write(source + QStringLiteral("/empty"), QByteArray()),
        "CHINESE_AND_EMPTY");
  if (!check(controller->saveRepositoryPath(repository),
             "CONFIGURE_REPOSITORY"))
    return 1;
  controller->setSourcePath(source);
  window->setProperty("currentPageKey", QStringLiteral("quick"));
  Pump(150);
  QObject* scroll =
      window->findChild<QObject*>(QStringLiteral("backupPageScroll"));
  if (scroll != nullptr) {
    QObject* content = qvariant_cast<QObject*>(scroll->property("contentItem"));
    if (content != nullptr) content->setProperty("contentY", 430);
  }
  int intermediate = 0;
  bool progress_shot = false;
  bool phase_indeterminate = true;
  int unknown_ui_updates = 0;
  int measured_ui_updates = 0;
  bool phase_ui_consistent = true;
  bool unknown_progress_shot = false;
  bool capturing_unknown_progress = false;
  double last_writing_ratio = -1.0;
  bool cumulative_writing = true;
  const auto connection = QObject::connect(
      controller, &backup_modern::BackupController::progressChanged, window,
      [&]() {
        if (controller->busy() && !controller->progressPhase().isEmpty()) {
          QQuickItem* bar =
              FindItem(window->contentItem(), "backupProgressBar");
          QQuickItem* busy =
              FindItem(window->contentItem(), "backupBusyIndicator");
          QQuickItem* text =
              FindItem(window->contentItem(), "backupProgressText");
          const double ratio = controller->progressRatio();
          if (bar == nullptr || busy == nullptr || text == nullptr) {
            phase_ui_consistent = false;
          } else if (ratio < 0) {
            ++unknown_ui_updates;
            phase_ui_consistent =
                phase_ui_consistent && !bar->isVisible() && busy->isVisible() &&
                busy->property("running").toBool() &&
                text->property("text").toString() == controller->progressText();
            if (!unknown_progress_shot && !capturing_unknown_progress) {
              capturing_unknown_progress = true;
              const QImage image = window->grabWindow();
              unknown_progress_shot =
                  !image.isNull() &&
                  image.save(output +
                             QStringLiteral("/08a-local-backup-busy.png"));
              capturing_unknown_progress = false;
            }
          } else {
            ++measured_ui_updates;
            phase_ui_consistent =
                phase_ui_consistent && bar->isVisible() && !busy->isVisible() &&
                !busy->property("running").toBool() &&
                std::abs(bar->property("value").toDouble() - ratio) < 0.000001;
            cumulative_writing =
                cumulative_writing && ratio >= last_writing_ratio;
            last_writing_ratio = ratio;
          }
        }
        if (controller->progressPhase() != QStringLiteral("writing")) {
          phase_indeterminate =
              phase_indeterminate && controller->progressRatio() < 0;
        } else if (controller->progressBytesDone() > 0 &&
                   controller->progressBytesDone() <
                       controller->progressBytesTotal()) {
          ++intermediate;
          if (!progress_shot) {
            const QImage image = window->grabWindow();
            progress_shot =
                !image.isNull() &&
                image.save(output +
                           QStringLiteral("/08-local-backup-progress.png"));
          }
        }
      });
  const bool started = controller->startBackup();
  const bool done = started && controller->waitForIdle(180000);
  QObject::disconnect(connection);
  check(done && controller->lastSucceeded(), "LOCAL_FULL_BACKUP");
  check(intermediate >= 2, "REAL_INTERMEDIATE_BYTE_PROGRESS");
  check(phase_indeterminate, "UNKNOWN_PHASE_ACTIVITY");
  check(progress_shot, "PROGRESS_SCREENSHOT");
  check(
      unknown_ui_updates > 0 && measured_ui_updates > 0 && phase_ui_consistent,
      "REAL_PHASE_PROGRESS_UI");
  check(cumulative_writing, "MEASURED_PROGRESS_NEVER_DECREASES");
  check(unknown_progress_shot, "UNKNOWN_PHASE_BUSY_SCREENSHOT");
  const QStringList archives =
      QDir(repository).entryList({QStringLiteral("*.bak")}, QDir::Files);
  if (!check(archives.size() == 1, "ONE_FULL_SNAPSHOT")) return 1;
  check(QFile::exists(repository + QLatin1Char('/') + archives.first() +
                      QStringLiteral(".manifest")) &&
            QFile::exists(repository + QLatin1Char('/') + archives.first() +
                          QStringLiteral(".identity")),
        "FULL_BASELINE_SIDECARS");
  const QString restore = work.filePath(QStringLiteral("restore"));
  window->setProperty("currentPageKey", QStringLiteral("management"));
  if (management != nullptr)
    management->setProperty("currentTab", QStringLiteral("local"));
  Pump(100);
  const bool restore_started =
      controller->startManagedRestore(archives.first(), restore);
  QQuickItem* restore_progress =
      FindItem(window->contentItem(), "managementProgressRow");
  QQuickItem* restore_busy =
      FindItem(window->contentItem(), "managementBusyIndicator");
  QQuickItem* restore_bar =
      FindItem(window->contentItem(), "managementProgressBar");
  QQuickItem* restore_text =
      FindItem(window->contentItem(), "managementProgressText");
  check(restore_started && restore_progress != nullptr &&
            restore_progress->isVisible() && restore_busy != nullptr &&
            restore_busy->isVisible() &&
            restore_busy->property("running").toBool() &&
            restore_bar != nullptr && !restore_bar->isVisible() &&
            restore_text != nullptr &&
            restore_text->property("text").toString().contains(
                QStringLiteral("正在恢复")),
        "RESTORE_BUSY_WITHOUT_PERCENTAGE");
  check(restore_started && controller->waitForIdle(180000) &&
            controller->lastSucceeded() &&
            Digest(large) == Digest(restore + QStringLiteral("/large.bin")),
        "FULL_RESTORE_SHA256");
  window->setProperty("currentPageKey", QStringLiteral("management"));
  if (management != nullptr)
    management->setProperty("currentTab", QStringLiteral("local"));
  check(capture(QStringLiteral("24-local-restore-complete.png")),
        "RESTORE_SCREENSHOT");
  const bool occupied_started =
      controller->startManagedRestore(archives.first(), restore);
  if (occupied_started) controller->waitForIdle(180000);
  check(!controller->lastSucceeded() &&
            Digest(large) == Digest(restore + QStringLiteral("/large.bin")),
        "NONEMPTY_TARGET_PRESERVED");
  check(capture(QStringLiteral("23-chinese-error-dialog.png")),
        "NONEMPTY_TARGET_ERROR_SCREENSHOT");
  DismissError(window);

  // 小型合成源走真正的QML开始按钮，验证加密选项没有在切换增量时被清掉。
  // 口令仅在本次进程内使用，日志与截图只记录清空/遮罩结论。
  const QString encrypted_source =
      work.filePath(QStringLiteral("encrypted-source"));
  const QString encrypted_repository =
      work.filePath(QStringLiteral("encrypted-repository"));
  QDir().mkpath(encrypted_source);
  QByteArray stable_data(256 * 1024, '\0');
  for (int index = 0; index < stable_data.size(); ++index) {
    stable_data[index] = static_cast<char>((index * 31 + index / 257) & 255);
  }
  check(Write(encrypted_source + QStringLiteral("/stable.bin"), stable_data) &&
            Write(encrypted_source + QStringLiteral("/说明.txt"), "before\n") &&
            Write(encrypted_source + QStringLiteral("/empty"), QByteArray()),
        "ENCRYPTED_GUI_SYNTHETIC_SOURCE");
  if (!check(controller->saveRepositoryPath(encrypted_repository) &&
                 controller->waitForCatalogIdle(30000),
             "ENCRYPTED_GUI_NEW_REPOSITORY"))
    return 1;
  controller->setSourcePath(encrypted_source);
  window->setProperty("currentPageKey", QStringLiteral("quick"));
  window->requestActivate();
  Pump(100);
  QQuickItem* panel = FindItem(window->contentItem(), "backupOptionsPanel");
  QQuickItem* start_button =
      FindItem(window->contentItem(), "startBackupButton");
  QQuickItem* password_field = FindItem(window->contentItem(), "passwordField");
  QQuickItem* confirmation_field =
      FindItem(window->contentItem(), "confirmPasswordField");
  if (!check(panel != nullptr && start_button != nullptr &&
                 password_field != nullptr && confirmation_field != nullptr,
             "ENCRYPTED_GUI_CONTROLS"))
    return 1;
  check(RevealAndClick(window,
                       FindItem(window->contentItem(), "backupOptionsToggle")),
        "ENCRYPTED_GUI_EXPAND_OPTIONS_CLICK");
  Pump(100);
  panel->setProperty("strategyKey", QStringLiteral("full"));
  panel->setProperty("packKey", QStringLiteral("mypack"));
  panel->setProperty("compressionKey", QStringLiteral("none"));
  panel->setProperty("encryptionKey",
                     QStringLiteral("aes-256-ctr-hmac-sha256"));
  const QString archive_secret =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  password_field->setProperty("text", archive_secret);
  confirmation_field->setProperty("text", archive_secret);
  window->setProperty("currentPageKey", QStringLiteral("home"));
  Pump(100);
  check(password_field->property("text").toString().isEmpty() &&
            confirmation_field->property("text").toString().isEmpty(),
        "ENCRYPTED_GUI_LEAVING_PAGE_CLEARS_PASSWORDS");
  window->setProperty("currentPageKey", QStringLiteral("quick"));
  Pump(100);
  password_field->setProperty("text", archive_secret);
  confirmation_field->setProperty("text", archive_secret);
  check(password_field->property("echoMode").toInt() != 0 &&
            confirmation_field->property("echoMode").toInt() != 0 &&
            !password_field->property("displayText")
                 .toString()
                 .contains(archive_secret),
        "ENCRYPTED_GUI_PASSWORD_MASKED");
  RevealAndClick(window, password_field);
  check(capture(QStringLiteral("10-encrypted-backup-password.png")),
        "ENCRYPTED_GUI_PASSWORD_SCREENSHOT");
  const bool encrypted_full_click = RevealAndClick(window, start_button);
  check(encrypted_full_click &&
            password_field->property("text").toString().isEmpty() &&
            confirmation_field->property("text").toString().isEmpty(),
        "ENCRYPTED_GUI_FULL_PASSWORDS_CLEARED_ON_SUBMIT");
  if (!check(encrypted_full_click && controller->waitForIdle(180000) &&
                 controller->lastSucceeded(),
             "ENCRYPTED_GUI_FULL_BACKUP"))
    return 1;
  controller->waitForCatalogIdle(30000);
  const QStringList encrypted_full_names =
      QDir(encrypted_repository)
          .entryList({QStringLiteral("*.bak")}, QDir::Files);
  if (!check(encrypted_full_names.size() == 1, "ENCRYPTED_GUI_ONE_BASELINE"))
    return 1;
  const auto panel_diagnostic = [&](const char* phase) {
    QQuickItem* selector =
        FindItem(window->contentItem(), "encryptionSelector");
    std::printf(
        "WORKFLOW encrypted_panel phase=%s strategy=%s encryption=%s "
        "combination_allowed=%d combination_error=%s selector_enabled=%d "
        "selector_index=%d busy=%d status_title=%s status_message=%s\n",
        phase, qPrintable(panel->property("strategyKey").toString()),
        qPrintable(panel->property("encryptionKey").toString()),
        panel->property("strategyCombinationAllowed").toBool(),
        qPrintable(panel->property("combinationError").toString()),
        selector != nullptr && selector->isEnabled(),
        selector == nullptr ? -1 : selector->property("currentIndex").toInt(),
        controller->busy(), qPrintable(controller->statusTitle()),
        qPrintable(controller->statusMessage()));
    std::fflush(stdout);
  };
  panel_diagnostic("full_complete");

  check(Write(encrypted_source + QStringLiteral("/说明.txt"), "after\n") &&
            Write(encrypted_source + QStringLiteral("/added.txt"), "new\n"),
        "ENCRYPTED_GUI_SMALL_CHANGE");
  panel->setProperty("strategyKey", QStringLiteral("incremental"));
  Pump(50);
  panel_diagnostic("incremental_selected");
  QQuickItem* encryption_selector =
      FindItem(window->contentItem(), "encryptionSelector");
  check(panel->property("encryptionKey").toString() ==
                QStringLiteral("aes-256-ctr-hmac-sha256") &&
            panel->property("strategyCombinationAllowed").toBool() &&
            encryption_selector != nullptr && encryption_selector->isEnabled(),
        "ENCRYPTED_GUI_INCREMENTAL_PRESERVES_ENCRYPTION");
  password_field->setProperty("text", archive_secret);
  confirmation_field->setProperty("text", archive_secret);
  const bool encrypted_delta_click = RevealAndClick(window, start_button);
  check(encrypted_delta_click &&
            password_field->property("text").toString().isEmpty() &&
            confirmation_field->property("text").toString().isEmpty(),
        "ENCRYPTED_GUI_DELTA_PASSWORDS_CLEARED_ON_SUBMIT");
  if (!check(encrypted_delta_click && controller->waitForIdle(180000) &&
                 controller->lastSucceeded(),
             "ENCRYPTED_GUI_INCREMENTAL_BACKUP"))
    return 1;
  panel_diagnostic("incremental_complete");
  controller->waitForCatalogIdle(30000);
  backupproject::BackupCatalog encrypted_catalog;
  std::vector<backupproject::BackupRecord> encrypted_records;
  check(encrypted_catalog.List(encrypted_repository.toStdString(),
                               &encrypted_records, &error),
        "ENCRYPTED_GUI_CATALOG_LIST");
  QString delta_name;
  std::uint64_t full_size = 0;
  std::uint64_t delta_size = 0;
  bool encrypted_catalog_correct = encrypted_records.size() == 2;
  for (const auto& record : encrypted_records) {
    QFile archive(QString::fromStdString(record.archive_path));
    const QByteArray magic = archive.open(QIODevice::ReadOnly)
                                 ? archive.read(8).toHex()
                                 : QByteArray("unreadable");
    std::printf(
        "WORKFLOW encrypted_record name=%s recognized=%d delta=%d "
        "password_required=%d bytes=%llu encryption=%d magic_hex=%s "
        "diagnostic=%s\n",
        record.file_name.c_str(), record.recognized_archive,
        record.incremental_delta, record.password_required,
        static_cast<unsigned long long>(record.archive_size),
        static_cast<int>(record.encryption_method), magic.constData(),
        record.diagnostic.c_str());
    encrypted_catalog_correct = encrypted_catalog_correct &&
                                record.recognized_archive &&
                                record.password_required;
    if (record.incremental_delta) {
      delta_name = QString::fromStdString(record.file_name);
      delta_size = record.archive_size;
    } else {
      full_size = record.archive_size;
    }
  }
  check(encrypted_catalog_correct && !delta_name.isEmpty(),
        "ENCRYPTED_GUI_REAL_DELTA_PASSWORD_REQUIRED");
  check(delta_size > 0 && full_size > 0 && delta_size < full_size / 2,
        "ENCRYPTED_GUI_PHYSICAL_DELTA_SMALLER");
  const QString encrypted_restore =
      work.filePath(QStringLiteral("encrypted-restore"));
  const bool encrypted_restore_started =
      controller->startManagedRestoreWithPassword(delta_name, encrypted_restore,
                                                  archive_secret);
  check(encrypted_restore_started && controller->waitForIdle(180000) &&
            controller->lastSucceeded() &&
            Digest(encrypted_source + QStringLiteral("/stable.bin")) ==
                Digest(encrypted_restore + QStringLiteral("/stable.bin")) &&
            Digest(encrypted_source + QStringLiteral("/说明.txt")) ==
                Digest(encrypted_restore + QStringLiteral("/说明.txt")) &&
            Digest(encrypted_source + QStringLiteral("/added.txt")) ==
                Digest(encrypted_restore + QStringLiteral("/added.txt")),
        "ENCRYPTED_GUI_INCREMENTAL_RESTORE_SHA256");
  const QString wrong_destination =
      work.filePath(QStringLiteral("wrong-encrypted-restore"));
  const bool wrong_started = controller->startManagedRestoreWithPassword(
      delta_name, wrong_destination,
      QStringLiteral("incorrect archive phrase"));
  if (wrong_started) controller->waitForIdle(180000);
  check(!controller->lastSucceeded() && !QFile::exists(wrong_destination),
        "ENCRYPTED_GUI_WRONG_PASSWORD_NO_PUBLISH");
  check(capture(QStringLiteral("23-encrypted-password-error.png")),
        "ENCRYPTED_GUI_WRONG_PASSWORD_SCREENSHOT");
  DismissError(window);
  window->setProperty("currentPageKey", QStringLiteral("management"));
  if (management != nullptr)
    management->setProperty("currentTab", QStringLiteral("local"));
  check(capture(QStringLiteral("09-encrypted-incremental-record.png")),
        "ENCRYPTED_GUI_DELTA_SCREENSHOT");
  controller->clearStatus();
  std::printf("WORKFLOW passed=%d failed=%d intermediate_updates=%d\n", passed,
              failed, intermediate);
  return failed == 0 ? 0 : 1;
}

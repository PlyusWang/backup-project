// Tests the production controller without acquiring the GUI application lock.
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QVariantMap>
#include <string>
#include <vector>

#include "backup_catalog.h"
#include "backup_controller.h"
#include "incremental_backup.h"
#include "test_support.h"

namespace bp = backupproject;
namespace ts = test_support;
namespace modern = backup_modern;

namespace {
bool WaitForScan(modern::BackupController* controller) {
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(controller,
                   &modern::BackupController::baselineScanStateChanged, &loop,
                   [&]() {
                     if (!controller->baselineScanBusy()) loop.quit();
                   });
  if (!controller->baselineScanBusy()) return true;
  timeout.start(120000);
  loop.exec();
  return !controller->baselineScanBusy();
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const std::string work = ts::FreshDir("baseline-controller");
  const std::string source = work + "/source";
  const std::string repository = work + "/repository";
  const std::string other_repo = work + "/other-repository";
  const std::string foreign = work + "/foreign";
  ts::Check(ts::Mkdir(source, 0755) && ts::Mkdir(repository, 0755) &&
                ts::Mkdir(other_repo, 0755) && ts::Mkdir(foreign, 0755) &&
                ts::WriteFile(source + "/data.txt", "old\n", 0644),
            "controller fixture directories and data");
  bp::Filter filter;
  bp::BackupOptions options;
  bp::IncrementalOutcome outcome;
  std::string error;
  const auto full = [&](const std::string& name,
                        const bp::BackupOptions& selected) {
    return bp::RunManagedFullBackup(source, repository, name,
                                    bp::RepositoryIdentity(repository), filter,
                                    selected, {}, {}, &outcome, &error);
  };
  ts::Check(full("old.bak", options), "old full fixture", error);
  ts::Check(ts::WriteFile(source + "/data.txt", "current\n", 0644) &&
                full("current.bak", options),
            "newer full fixture", error);
  bp::BackupOptions encrypted = options;
  encrypted.encryption_method = bp::EncryptionMethod::kAes256CtrHmacSha256;
  encrypted.password = "synthetic-controller-session-only";
  ts::Check(full("encrypted.bak", encrypted), "AES scan fixture", error);

  modern::OperationGate gate;
  modern::BackupController controller(
      QString::fromStdString(work + "/config.json"), &gate);
  controller.setSourcePath(QString::fromStdString(source));
  ts::Check(controller.saveRepositoryPath(QString::fromStdString(repository)),
            "configure actual controller repository");
  ts::Check(controller.waitForCatalogIdle(10000), "initial catalog settled");
  const auto scan = [&]() {
    return controller.refreshBaselineCandidates("mypack", "none", "none", "");
  };

  ts::Section("asynchronous scan and stale input rejection");
  ts::Check(scan() && controller.baselineScanBusy(),
            "scan returns immediately with a busy property");
  ts::Check(!scan(), "second scan is refused while one is active");
  ts::Check(
      WaitForScan(&controller) && controller.baselineCandidates().size() == 3,
      "asynchronous scan returns all candidates");
  bool saw_usable = false;
  for (const auto& row : controller.baselineCandidates()) {
    const QVariantMap candidate = row.toMap();
    saw_usable |= candidate.value("fileName") == QStringLiteral("old.bak") &&
                  candidate.value("usable").toBool();
    ts::Check(
        !candidate.contains("password") && candidate.contains("snapshotId") &&
            candidate.contains("lineageId") && candidate.contains("reason"),
        "public row contains identities and reasons, no password");
  }
  ts::Check(saw_usable, "controller exposes usable same-source baseline");

  ts::Check(scan(), "start source-staleness scan");
  controller.setSourcePath(QString::fromStdString(foreign));
  ts::Check(controller.baselineCandidates().isEmpty() &&
                WaitForScan(&controller) &&
                controller.baselineCandidates().isEmpty(),
            "source change discards an in-flight result");
  controller.setSourcePath(QString::fromStdString(source));
  ts::Check(scan(), "start rules-staleness scan");
  ts::Check(controller.addFilterRule("include", "ext:txt"),
            "edit filters during scan");
  ts::Check(
      WaitForScan(&controller) && controller.baselineCandidates().isEmpty(),
      "filter edit discards an in-flight result");
  controller.clearFilterRules();
  ts::Check(scan(), "start repository-staleness scan");
  ts::Check(controller.saveRepositoryPath(QString::fromStdString(other_repo)) &&
                WaitForScan(&controller) &&
                controller.baselineCandidates().isEmpty(),
            "repository change discards an in-flight result");
  ts::Check(controller.saveRepositoryPath(QString::fromStdString(repository)) &&
                controller.waitForCatalogIdle(10000),
            "restore repository");
  ts::Check(scan(), "start option-staleness scan");
  controller.invalidateBaselineCandidates();
  ts::Check(
      WaitForScan(&controller) && controller.baselineCandidates().isEmpty(),
      "explicit option invalidation discards an in-flight result");
  ts::Check(
      !controller.refreshBaselineCandidates("unknown", "none", "none", "") &&
          !controller.baselineScanBusy() &&
          !controller.baselineScanError().isEmpty(),
      "invalid options refuse before launching a scan");
  ts::Check(!controller.refreshBaselineCandidates(
                "mypack", "none", "aes-256-ctr-hmac-sha256", "") &&
                !controller.baselineScanError().isEmpty(),
            "encrypted scan requires a session password");

  bool event_processed_while_busy = false;
  ts::Check(controller.refreshBaselineCandidates(
                "mypack", "none", "aes-256-ctr-hmac-sha256",
                QString::fromStdString(encrypted.password)),
            "start genuine encrypted verification");
  QTimer::singleShot(0, &controller, [&]() {
    event_processed_while_busy = controller.baselineScanBusy();
  });
  ts::Check(WaitForScan(&controller) && event_processed_while_busy,
            "main event loop remains responsive during authentication");
  std::string config;
  ts::Check(ts::ReadFile(work + "/config.json", &config) &&
                config.find(encrypted.password) == std::string::npos,
            "scan password is absent from persisted configuration");

  ts::Section("selected parent reaches production backup worker");
  ts::Check(controller.startBackupWithSelectedBaseline(
                "incremental", "mypack", "none", "none", "", "", "old.bak") &&
                controller.waitForIdle(120000) && controller.lastSucceeded(),
            "controller executes explicitly selected older baseline");
  std::vector<bp::BackupRecord> records;
  bp::BackupCatalog catalog;
  ts::Check(catalog.List(repository, &records, &error),
            "read actual generated archive");
  bool explicit_parent = false;
  for (const auto& record : records) {
    if (record.incremental_delta && record.parent_file_name == "old.bak")
      explicit_parent = true;
  }
  ts::Check(
      explicit_parent,
      "real delta uses selected parent rather than automatic newest full");
  const auto count_before = records.size();
  QString failure_title;
  QString failure_summary;
  QObject::connect(&controller, &modern::BackupController::userError,
                   &controller,
                   [&](const QString& title, const QString& summary,
                       const QString&, const QString&) {
                     failure_title = title;
                     failure_summary = summary;
                   });
  ts::Check(
      controller.startBackupWithSelectedBaseline(
          "incremental", "mypack", "none", "none", "", "", "missing.bak") &&
          controller.waitForIdle(120000) && !controller.lastSucceeded(),
      "invalid explicit parent fails in actual backup worker");
  ts::Check(catalog.List(repository, &records, &error) &&
                records.size() == count_before,
            "failed explicit selection publishes no archive");
  ts::Check(failure_title == QStringLiteral("增量备份未能继续") &&
                failure_summary.contains(QStringLiteral("基线")),
            "explicit selection error uses the shared Chinese error signal");
  ts::Check(controller.waitForCatalogIdle(10000), "final catalog settled");
  const int result = ts::Finish("baseline_controller_test");
  if (result != 0) ts::Note("preserved failure fixture: " + work);
  // Controller futures finish before the fixture is removed.
  if (result == 0) ts::RemoveTree(work);
  return result;
}

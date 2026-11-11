// automation_ui_test.cpp
//
// 真正操作定时/实时页面，并从落盘配置、快照恢复和文件摘要核对执行事实。
#include "automation_ui_test.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMap>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <functional>

#include "backup_controller.h"
#include "backup_credentials.h"
#include "incremental_delta.h"
#include "incremental_restore.h"
#include "realtime_controller.h"
#include "realtime_store.h"
#include "schedule_controller.h"
#include "schedule_store.h"

namespace {

void Settle(int milliseconds = 60) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

bool Wait(const std::function<bool()>& predicate, int timeout = 15000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeout) Settle();
  return predicate();
}

QQuickItem* Find(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name) return parent;
  for (QQuickItem* child : parent->childItems()) {
    if (QQuickItem* found = Find(child, name)) return found;
  }
  return nullptr;
}

void Key(QQuickWindow* window, int key,
         Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QKeyEvent press(QEvent::KeyPress, key, modifiers);
  QCoreApplication::sendEvent(window, &press);
  QKeyEvent release(QEvent::KeyRelease, key, modifiers);
  QCoreApplication::sendEvent(window, &release);
  Settle();
}

bool Reveal(QQuickWindow* window, QQuickItem* item, const QString& scroll) {
  if (item == nullptr || !item->isVisible() || !item->isEnabled()) return false;
  if (!scroll.isEmpty()) {
    QQuickItem* view = Find(window->contentItem(), scroll);
    QQuickItem* flick =
        view ? view->property("contentItem").value<QQuickItem*>() : nullptr;
    if (flick == nullptr) return false;
    // 等布局完成后，按 originY 的实际范围夹边，不能把短页卷到空白区。
    Settle(120);
    const qreal origin = flick->property("originY").toReal();
    const qreal current = flick->property("contentY").toReal();
    const qreal top = item->mapToItem(flick, QPointF()).y();
    const qreal maximum =
        origin + qMax(qreal(0), flick->property("contentHeight").toReal() -
                                    flick->height());
    flick->setProperty("contentY",
                       qBound(origin, current + top - 150, maximum));
    Settle(100);
    const qreal actual = flick->property("contentY").toReal();
    if (actual < origin - 1 || actual > maximum + 1) return false;
  }
  const QPointF center =
      item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
  return center.x() >= 0 && center.y() >= 0 && center.x() < window->width() &&
         center.y() < window->height();
}

bool Click(QQuickWindow* window, QQuickItem* item,
           const QString& scroll = QString()) {
  if (!Reveal(window, item, scroll)) return false;
  const QPointF position =
      item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
  const QPointF global = window->mapToGlobal(position.toPoint());
  QMouseEvent move(QEvent::MouseMove, position, position, global, Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &move);
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
          const QString& scroll) {
  if (!Click(window, item, scroll)) return false;
  Key(window, Qt::Key_A, Qt::ControlModifier);
  QInputMethodEvent input;
  input.setCommitString(text);
  QCoreApplication::sendEvent(window, &input);
  Settle();
  return item->property("text").toString() == text;
}

bool Select(QQuickWindow* window, QQuickItem* item, int index,
            const QString& scroll) {
  if (!Click(window, item, scroll)) return false;
  Key(window, Qt::Key_Home);
  for (int i = 0; i < index; ++i) Key(window, Qt::Key_Down);
  Key(window, Qt::Key_Return);
  return item->property("currentIndex").toInt() == index;
}

bool Write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray Read(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QMap<QString, QByteArray> Manifest(const QString& path) {
  QMap<QString, QByteArray> result;
  QDirIterator iterator(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const QString file_path = iterator.next();
    const QString relative = QDir(path).relativeFilePath(file_path);
    if (QFileInfo(file_path).isDir()) {
      result.insert(relative + QLatin1Char('/'), "directory");
      continue;
    }
    QFile file(file_path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
      return QMap<QString, QByteArray>();
    result.insert(relative, hash.result().toHex());
  }
  return result;
}

bool Restored(const QString& repository, const QString& name,
              const QString& destination, const QString& source,
              const QString& evidence, const QString& password = QString()) {
  std::string error;
  backupproject::RestoreOptions options;
  options.password = password.toStdString();
  if (name.isEmpty() ||
      !backupproject::RestoreSnapshotChain(
          repository.toStdString(), name.toStdString(),
          destination.toStdString(), options, nullptr, &error)) {
    std::fprintf(stderr, "AUTOMATION_UI restore diagnostic=%s\n",
                 error.c_str());
    return false;
  }
  const auto expected = Manifest(source);
  const auto actual = Manifest(destination);
  QByteArray rows("path\tSHA256_or_directory\n");
  for (auto iterator = actual.cbegin(); iterator != actual.cend(); ++iterator)
    rows += iterator.key().toUtf8() + '\t' + iterator.value() + '\n';
  return !expected.isEmpty() && expected == actual && Write(evidence, rows);
}

// Read the real container selected by the automatic service. The subsequent
// chain restore verifies its authenticated bytes and hashes the restored tree.
bool SnapshotMethods(const QString& repository, const QString& name,
                     backupproject::PackMethod pack,
                     backupproject::EncryptionMethod encryption, bool delta,
                     const QString& evidence,
                     backupproject::CompressionMethod compression =
                         backupproject::CompressionMethod::kZstd) {
  if (name.isEmpty()) return false;
  backupproject::ContainerHeader header;
  std::string error;
  const std::string path = (repository + "/" + name).toStdString();
  const bool inspected =
      delta ? backupproject::InspectDeltaPayloadHeader(path, &header, &error)
            : backupproject::InspectContainerFile(path, &header, &error);
  if (!inspected) {
    std::fprintf(stderr, "AUTOMATION_UI container diagnostic=%s\n",
                 error.c_str());
    return false;
  }
  const QByteArray rows = "snapshot\tpack_id\tcompression_id\tencryption_id\n" +
                          name.toUtf8() + '\t' +
                          QByteArray::number(header.pack_method) + '\t' +
                          QByteArray::number(header.compression_method) + '\t' +
                          QByteArray::number(header.encryption_method) + '\n';
  return header.pack_method == static_cast<std::uint8_t>(pack) &&
         header.compression_method == static_cast<std::uint8_t>(compression) &&
         header.encryption_method == static_cast<std::uint8_t>(encryption) &&
         Write(evidence, rows);
}

}  // namespace

int RunAutomationUiTest(QQuickWindow* window,
                        backup_modern::BackupController* controller,
                        backup_modern::ScheduleController* schedule,
                        backup_modern::RealtimeController* realtime,
                        const QString& output) {
  int passed = 0;
  int failed = 0;
  const auto check = [&](bool ok, const char* label) {
    std::printf("AUTOMATION_UI %s=%s\n", label, ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    ok ? ++passed : ++failed;
    return ok;
  };
  QTemporaryDir work;
  if (!check(work.isValid() && QDir().mkpath(output), "ISOLATED_WORKSPACE"))
    return 1;
  std::printf("AUTOMATION_UI platform=%s\n",
              qPrintable(QGuiApplication::platformName()));
  const QString source_a = work.filePath("source-a");
  const QString source_b = work.filePath("source-b");
  const QString repository = work.filePath("repository");
  if (!check(QDir().mkpath(source_a + "/子目录/空目录") &&
                 QDir().mkpath(source_b + "/子目录") &&
                 Write(source_a + "/a.txt", "source A\n") &&
                 Write(source_a + "/子目录/中文.txt", "nested A\n") &&
                 Write(source_a + "/empty.txt", "") &&
                 Write(source_b + "/b.txt", "source B\n") &&
                 Write(source_b + "/子目录/中文.txt", "nested B\n") &&
                 controller->saveRepositoryPath(repository),
             "SYNTHETIC_FIXTURES"))
    return 1;
  schedule->start();
  realtime->start();
  window->resize(1280, 980);
  window->requestActivate();
  Settle(200);
  int schedule_finished = 0;
  int realtime_finished = 0;
  const auto schedule_connection = QObject::connect(
      schedule, &backup_modern::ScheduleController::operationFinished, window,
      [&](bool ok) {
        ++schedule_finished;
        std::printf("AUTOMATION_UI schedule_event=%d success=%d\n",
                    schedule_finished, ok);
      });
  const auto realtime_connection = QObject::connect(
      realtime, &backup_modern::RealtimeController::operationFinished, window,
      [&](bool ok) {
        ++realtime_finished;
        std::printf("AUTOMATION_UI realtime_event=%d success=%d\n",
                    realtime_finished, ok);
      });
  const auto item = [&](const QString& name) {
    return Find(window->contentItem(), name);
  };
  const auto navigate = [&](const QString& key) {
    return Click(window, item("nav_" + key)) &&
           window->property("currentPageKey").toString() == key;
  };
  const auto dismiss = [&]() {
    QObject* dialog =
        window->findChild<QObject*>(QStringLiteral("userErrorDialog"));
    if (dialog && dialog->property("visible").toBool())
      Key(window, Qt::Key_Escape);
  };
  const auto capture = [&](const char* name) {
    Settle(150);
    const QImage frame = window->grabWindow();
    return !frame.isNull() &&
           frame.save(output + QLatin1Char('/') + QString::fromLatin1(name));
  };
  const QString scheduled_scroll("schedulePageScroll");
  const QString realtime_scroll("realtimePageScroll");
  const auto schedule_click = [&](const char* name) {
    return Click(window, item(QString::fromLatin1(name)), scheduled_scroll);
  };
  const auto realtime_click = [&](const char* name) {
    return Click(window, item(QString::fromLatin1(name)), realtime_scroll);
  };
  const auto schedule_type = [&](const char* name, const QString& text) {
    return Type(window, item(QString::fromLatin1(name)), text,
                scheduled_scroll);
  };
  const auto realtime_type = [&](const char* name, const QString& text) {
    return Type(window, item(QString::fromLatin1(name)), text, realtime_scroll);
  };

  if (!check(navigate("scheduled") &&
                 schedule_type("scheduleSourceField", source_a) &&
                 schedule_type("scheduleFrequencyValueField", "24") &&
                 Select(window, item("scheduleFrequencyUnitCombo"), 1,
                        scheduled_scroll) &&
                 schedule_type("scheduleRetainField", "5") &&
                 Select(window, item("scheduleStrategyCombo"), 1,
                        scheduled_scroll),
             "SCHEDULE_REAL_INPUT"))
    return 1;
  check(schedule_click("saveScheduleButton") && !schedule->enabled() &&
            schedule->sourcePath() == source_a &&
            schedule->intervalMinutes() == 1440 &&
            schedule->strategyKey() == "incremental",
        "SCHEDULE_SAVE_PRESERVES_DISABLED");
  check(schedule_type("scheduleSourceField", source_b) &&
            schedule_type("scheduleFrequencyValueField", "2") &&
            Select(window, item("scheduleFrequencyUnitCombo"), 1,
                   scheduled_scroll) &&
            schedule_click("scheduleEnabledToggle") && schedule->enabled() &&
            schedule->sourcePath() == source_b &&
            schedule->intervalMinutes() == 120 && schedule->holdsRunnerLock() &&
            schedule->tickActiveForTest() &&
            item("runScheduleNowButton")->isEnabled(),
        "SCHEDULE_ENABLE_SAVES_DRAFT_AND_RUN_AVAILABLE");
  std::printf(
      "AUTOMATION_UI schedule enabled=%d source=%s interval=%d "
      "lock=%d tick=%d run_available=%d\n",
      schedule->enabled(), qPrintable(schedule->sourcePath()),
      schedule->intervalMinutes(), schedule->holdsRunnerLock(),
      schedule->tickActiveForTest(), item("runScheduleNowButton")->isEnabled());
  check(capture("01-schedule-enabled.png"), "SCHEDULE_ENABLED_SCREENSHOT");
  int before = schedule_finished;
  check(schedule_click("runScheduleNowButton") && Wait([&]() {
          return schedule_finished > before && !schedule->libraryBusy();
        }) &&
            schedule->lastSucceeded(),
        "SCHEDULE_IMMEDIATE_EXECUTION");
  auto snapshots = schedule->managedSnapshots();
  check(!snapshots.isEmpty() &&
            Restored(repository,
                     snapshots.last().toMap().value("fileName").toString(),
                     work.filePath("restored-b"), source_b,
                     output + "/schedule-b-sha256.tsv"),
        "SCHEDULE_ACTUAL_SOURCE_RESTORE_SHA256");

  before = schedule_finished;
  check(
      schedule_type("scheduleSourceField", source_a) &&
          Select(window, item("scheduleStrategyCombo"), 0, scheduled_scroll) &&
          schedule_click("runScheduleNowButton") && Wait([&]() {
            return schedule_finished > before && !schedule->libraryBusy();
          }) &&
          schedule->sourcePath() == source_a &&
          schedule->strategyKey() == "full",
      "SCHEDULE_RUN_COMMITS_VISIBLE_DRAFT");
  snapshots = schedule->managedSnapshots();
  check(!snapshots.isEmpty() &&
            Restored(repository,
                     snapshots.last().toMap().value("fileName").toString(),
                     work.filePath("restored-a"), source_a,
                     output + "/schedule-a-sha256.tsv"),
        "SCHEDULE_VISIBLE_SOURCE_RESTORE_SHA256");
  check(capture("02-schedule-run-draft.png"), "SCHEDULE_RUN_SCREENSHOT");
  // 无论旧版是否执行了错误配置，继续从真实保存按钮收敛到有效状态。
  check(schedule_type("scheduleSourceField", source_a) &&
            schedule_type("scheduleRetainField", "6") &&
            schedule_click("saveScheduleButton") && schedule->enabled() &&
            schedule->retainCount() == 6,
        "SCHEDULE_SAVE_PRESERVES_ENABLED");
  const int schedule_events_before_disable = schedule_finished;
  check(schedule_type("scheduleRetainField", "12abc") &&
            schedule_click("scheduleEnabledToggle") && !schedule->enabled() &&
            !schedule->tickActiveForTest() && !schedule->holdsRunnerLock() &&
            schedule->retainCount() == 6,
        "SCHEDULE_DISABLE_IGNORES_INVALID_DRAFT");
  check(capture("03-schedule-disable-invalid-draft.png"),
        "SCHEDULE_DISABLE_SCREENSHOT");
  dismiss();
  if (schedule->enabled()) {
    schedule_type("scheduleRetainField", "6");
    schedule_click("scheduleEnabledToggle");
  }
  check(!schedule->enabled() &&
            schedule_finished == schedule_events_before_disable,
        "SCHEDULE_DISABLE_NO_NEW_EXECUTION");

  check(schedule_type("scheduleRetainField", "6") &&
            schedule_type("scheduleSourceField",
                          work.filePath("missing-source")) &&
            schedule_click("scheduleEnabledToggle") && !schedule->enabled(),
        "SCHEDULE_INVALID_SOURCE_ENABLE_REJECTED");
  dismiss();
  check(schedule_type("scheduleSourceField", source_a),
        "SCHEDULE_RESTORE_DRAFT");
  const QString schedule_saved = schedule->storePath() + ".ui-test-saved";
  const QByteArray saved_schedule_bytes = Read(schedule->storePath());
  const bool blocked_schedule =
      QFile::rename(schedule->storePath(), schedule_saved) &&
      QDir().mkdir(schedule->storePath());
  check(blocked_schedule && schedule_click("scheduleEnabledToggle") &&
            !schedule->enabled() && !schedule->tickActiveForTest(),
        "SCHEDULE_WRITE_FAILURE_NOT_ENABLED");
  dismiss();
  if (blocked_schedule) {
    QDir().rmdir(schedule->storePath());
    check(QFile::rename(schedule_saved, schedule->storePath()) &&
              Read(schedule->storePath()) == saved_schedule_bytes,
          "SCHEDULE_WRITE_FAILURE_ORIGINAL_PRESERVED");
  }
  schedule->reload();
  check(!schedule->enabled() && schedule->sourcePath() == source_a &&
            schedule->retainCount() == 6,
        "SCHEDULE_RELOAD_PERSISTED_CONFIGURATION");

  const QString realtime_repository = work.filePath("realtime-repository");
  check(controller->saveRepositoryPath(realtime_repository),
        "REALTIME_ISOLATED_REPOSITORY");
  if (!check(navigate("realtime") &&
                 realtime_type("realtimeSourceField", source_a) &&
                 realtime_type("realtimeRetainField", "6") &&
                 Select(window, item("realtimeStrategyCombo"), 1,
                        realtime_scroll) &&
                 realtime_click("realtimeAdvancedToggle") &&
                 realtime_type("realtimeDebounceField", "200") &&
                 realtime_type("realtimeMaxWaitField", "500"),
             "REALTIME_REAL_INPUT"))
    return 1;
  check(realtime_click("saveRealtimeButton") && !realtime->enabled() &&
            !realtime->watching() && realtime->sourcePath() == source_a &&
            realtime->debounceMs() == 200 && realtime->maxWaitMs() == 500 &&
            realtime->strategyKey() == "incremental",
        "REALTIME_SAVE_PRESERVES_DISABLED");
  before = realtime_finished;
  check(realtime_type("realtimeSourceField", source_b) &&
            realtime_click("realtimeEnabledToggle") && realtime->enabled() &&
            realtime->sourcePath() == source_b && realtime->watching() &&
            Wait([&]() {
              return realtime_finished > before && !realtime->libraryBusy() &&
                     !realtime->pending();
            }),
        "REALTIME_ENABLE_SAVES_DRAFT_AND_WATCHES");
  check(realtime->lastSucceeded() && realtime->snapshotCount() == 1 &&
            Restored(realtime_repository, realtime->lastSnapshotName(),
                     work.filePath("realtime-baseline"), source_b,
                     output + "/realtime-baseline-sha256.tsv"),
        "REALTIME_BASELINE_RESTORE_SHA256");
  std::printf("AUTOMATION_UI realtime baseline snapshots=%d outcome=%s\n",
              realtime->snapshotCount(),
              qPrintable(realtime->lastOutcomeKind()));
  before = realtime_finished;
  check(Write(source_b + "/b.txt", "source B real file event\n") && Wait([&]() {
          return realtime_finished > before && !realtime->libraryBusy() &&
                 !realtime->pending();
        }) &&
            realtime->lastSucceeded() &&
            realtime->lastOutcomeKind() == "delta" &&
            realtime->snapshotCount() == 2 &&
            Restored(realtime_repository, realtime->lastSnapshotName(),
                     work.filePath("realtime-delta"), source_b,
                     output + "/realtime-delta-sha256.tsv"),
        "REALTIME_FILE_EVENT_REAL_DELTA_RESTORE_SHA256");
  check(capture("04-realtime-delta.png"), "REALTIME_DELTA_SCREENSHOT");
  const int realtime_snapshots = realtime->snapshotCount();
  before = realtime_finished;
  check(realtime_type("realtimeRetainField", "7") &&
            realtime_click("saveRealtimeButton") && realtime->enabled() &&
            Wait([&]() {
              return realtime_finished > before && !realtime->libraryBusy() &&
                     !realtime->pending();
            }) &&
            realtime->lastOutcomeKind() == "no-changes" &&
            realtime->snapshotCount() == realtime_snapshots &&
            realtime->retainCount() == 7,
        "REALTIME_SAVE_ENABLED_NO_PSEUDO_SNAPSHOT");
  const int watchers = realtime->watchCount();
  for (int pass = 0; pass < 2; ++pass) {
    before = realtime_finished;
    realtime->reload();
    check(Wait([&]() {
            return realtime_finished > before && !realtime->libraryBusy() &&
                   !realtime->pending();
          }) &&
              realtime->watching() && realtime->watchCount() == watchers &&
              realtime->snapshotCount() == realtime_snapshots &&
              realtime->lastOutcomeKind() == "no-changes" &&
              navigate("scheduled") && navigate("realtime"),
          "ENABLED_RELOAD_SWITCH_NO_DUPLICATE_WATCHERS_OR_SNAPSHOTS");
  }
  const int events_before_disable = realtime_finished;
  check(realtime_type("realtimeRetainField", "12abc") &&
            realtime_click("realtimeEnabledToggle") && !realtime->enabled() &&
            !realtime->watching() && realtime->watchCount() == 0 &&
            !realtime->pending() && realtime->retainCount() == 7,
        "REALTIME_DISABLE_IGNORES_INVALID_DRAFT");
  check(capture("05-realtime-disable-invalid-draft.png"),
        "REALTIME_DISABLE_SCREENSHOT");
  dismiss();
  // 旧版失败时先保留实际触发证据，再通过有效表单正常停用。
  check(Write(source_b + "/b.txt", "changed after disable click\n"),
        "REALTIME_POST_DISABLE_FILE_CHANGE");
  Settle(1800);
  check(!realtime->enabled() && !realtime->watching() &&
            realtime_finished == events_before_disable &&
            realtime->snapshotCount() == realtime_snapshots,
        "REALTIME_POST_DISABLE_NO_EVENT_OR_SNAPSHOT");
  if (realtime->enabled()) {
    realtime_type("realtimeRetainField", "7");
    realtime_click("realtimeEnabledToggle");
    Wait([&]() { return !realtime->libraryBusy(); });
  }
  check(realtime_type("realtimeRetainField", "7") &&
            realtime_type("realtimeSourceField",
                          work.filePath("missing-source")) &&
            realtime_click("realtimeEnabledToggle") && !realtime->enabled() &&
            !realtime->watching(),
        "REALTIME_INVALID_SOURCE_ENABLE_REJECTED");
  dismiss();
  check(realtime_type("realtimeSourceField", source_b),
        "REALTIME_RESTORE_DRAFT");
  const QString realtime_saved = realtime->storePath() + ".ui-test-saved";
  const QByteArray saved_realtime_bytes = Read(realtime->storePath());
  const bool blocked_realtime =
      QFile::rename(realtime->storePath(), realtime_saved) &&
      QDir().mkdir(realtime->storePath());
  check(blocked_realtime && realtime_click("realtimeEnabledToggle") &&
            !realtime->enabled() && !realtime->watching(),
        "REALTIME_WRITE_FAILURE_NOT_ENABLED");
  dismiss();
  if (blocked_realtime) {
    QDir().rmdir(realtime->storePath());
    check(QFile::rename(realtime_saved, realtime->storePath()) &&
              Read(realtime->storePath()) == saved_realtime_bytes,
          "REALTIME_WRITE_FAILURE_ORIGINAL_PRESERVED");
  }

  for (int pass = 0; pass < 3; ++pass) {
    check(navigate("scheduled") &&
              item("scheduleSourceField")->property("text").toString() ==
                  schedule->sourcePath() &&
              navigate("realtime") &&
              item("realtimeSourceField")->property("text").toString() ==
                  realtime->sourcePath(),
          "REPEATED_PAGE_SWITCH_CONFIGURATION");
    schedule->reload();
    realtime->reload();
    Settle();
  }
  backupproject::ScheduleDocument disk_schedule;
  backupproject::RealtimeConfig disk_realtime;
  std::string error;
  check(backupproject::ScheduleStore(schedule->storePath().toStdString())
                    .Load(&disk_schedule, &error) ==
                backupproject::ScheduleLoadStatus::kLoaded &&
            backupproject::RealtimeStore(realtime->storePath().toStdString())
                    .Load(&disk_realtime, &error) ==
                backupproject::RealtimeLoadStatus::kLoaded &&
            !disk_schedule.config.enabled && !disk_realtime.enabled &&
            disk_schedule.config.source_path == source_a.toStdString() &&
            disk_realtime.source_path == source_b.toStdString() &&
            disk_schedule.config.retain_count == 6 &&
            disk_realtime.retain_count == 7 && !schedule->enabled() &&
            !realtime->enabled() && !realtime->watching() &&
            !schedule->tickActiveForTest(),
        "DISK_RELOAD_AND_UI_RUNNING_STATE_CONSISTENT");
  // 页面以外也验证共享控制器事务：目标草稿不能提前改实际运行配置。
  const QByteArray schedule_before_target = Read(schedule->storePath());
  check(schedule->setRepositoryTarget("cloud",
                                      work.filePath("unused-credentials"),
                                      "cloud://synthetic-target") &&
            !schedule->saveConfigFromFrequencyText(false, source_a, "12abc",
                                                   "hours", "6", "mypack",
                                                   "none", {}, {}, "full") &&
            schedule->repositoryType() == "local" &&
            schedule->configForTest().repository_type == "local" &&
            Read(schedule->storePath()) == schedule_before_target,
        "SCHEDULE_FAILED_SAVE_TARGET_IS_ATOMIC");
  dismiss();
  schedule->reload();
  const QByteArray realtime_before_target = Read(realtime->storePath());
  check(realtime->setRepositoryTarget("cloud",
                                      work.filePath("unused-credentials"),
                                      "cloud://synthetic-target") &&
            !realtime->saveConfigFromText(false, source_b, "12abc", "500", "7",
                                          "mypack", "none", {}, {},
                                          "incremental") &&
            realtime->repositoryType() == "local" &&
            realtime->configForTest().repository_type == "local" &&
            Read(realtime->storePath()) == realtime_before_target,
        "REALTIME_FAILED_SAVE_TARGET_IS_ATOMIC");
  dismiss();
  realtime->reload();
  check(Write(output + "/schedule-final.json", Read(schedule->storePath())) &&
            Write(output + "/realtime-final.json", Read(realtime->storePath())),
        "PERSISTED_CONFIGURATION_EVIDENCE");
  std::printf(
      "AUTOMATION_UI initial_watchers=%d final_watchers=%d "
      "schedule_events=%d realtime_events=%d\n",
      watchers, realtime->watchCount(), schedule_finished, realtime_finished);
  check(capture("06-realtime-persisted-disabled.png"),
        "PERSISTED_DISABLED_SCREENSHOT");
  // 用真实密码输入复现“失败的密码草稿覆盖正在运行的旧凭据”。
  for (const QString& prefix :
       {QStringLiteral("schedule"), QStringLiteral("realtime")}) {
    const bool scheduled = prefix == "schedule";
    const QString scroll_name = prefix + "PageScroll";
    const QString save_name =
        scheduled ? "saveScheduleButton" : "saveRealtimeButton";
    const QString fixture = work.filePath(prefix + "-aes-source");
    const QString task_repository = work.filePath(prefix + "-aes-repository");
    const QString password_p =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString password_q =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    check(QDir().mkpath(fixture + "/子目录") &&
              Write(fixture + "/secret.txt", "AES baseline content\n") &&
              Write(fixture + "/子目录/空文件.txt", "") &&
              controller->saveRepositoryPath(task_repository) &&
              navigate(scheduled ? "scheduled" : "realtime"),
          scheduled ? "SCHEDULE_AES_ISOLATED_FIXTURE"
                    : "REALTIME_AES_ISOLATED_FIXTURE");
    const auto type = [&](const QString& suffix, const QString& value) {
      return Type(window, item(prefix + suffix), value, scroll_name);
    };
    const auto click = [&](const QString& name) {
      return Click(window, item(name), scroll_name);
    };
    QQuickItem* scroll = item(scroll_name);
    QQuickItem* page = scroll ? scroll->parentItem() : nullptr;
    if (!check(page != nullptr, "AES_PAGE_CONTROLS")) return 1;
    if (!page->property("advancedExpanded").toBool())
      check(click(prefix + "AdvancedToggle"), "AES_OPEN_ADVANCED_BY_MOUSE");
    const int initial_events =
        scheduled ? schedule_finished : realtime_finished;
    check(
        type("SourceField", fixture) && type("RetainField", "6") &&
            Select(window, item(prefix + "StrategyCombo"), 1, scroll_name) &&
            Select(window, item(prefix + "EncryptionCombo"), 1, scroll_name) &&
            type("ArchivePasswordField", password_p) &&
            type("ArchivePasswordConfirmationField", password_p) &&
            click(save_name) &&
            (scheduled ? !schedule->enabled() : !realtime->enabled()) &&
            click(prefix + "EnabledToggle"),
        scheduled ? "SCHEDULE_AES_P_SAVED_AND_ENABLED"
                  : "REALTIME_AES_P_SAVED_AND_ENABLED");
    QElapsedTimer baseline_elapsed;
    baseline_elapsed.start();
    if (scheduled) click("runScheduleNowButton");
    const bool baseline_completed = Wait(
        [&]() {
          return scheduled
                     ? schedule_finished > initial_events &&
                           !schedule->libraryBusy()
                     : realtime_finished > initial_events &&
                           !realtime->libraryBusy() && !realtime->pending();
        },
        180000);
    check(baseline_completed, scheduled ? "SCHEDULE_AES_P_BASELINE_CREATED"
                                        : "REALTIME_AES_P_BASELINE_CREATED");
    std::printf(
        "AUTOMATION_UI aes_phase=baseline service=%s elapsed_ms=%lld "
        "completed=%d\n",
        qPrintable(prefix), static_cast<long long>(baseline_elapsed.elapsed()),
        baseline_completed);
    std::fflush(stdout);
    const auto snapshot_name = [&]() {
      if (!scheduled) return realtime->lastSnapshotName();
      const auto list = schedule->managedSnapshots();
      return list.isEmpty() ? QString()
                            : list.last().toMap().value("fileName").toString();
    };
    check(Restored(task_repository, snapshot_name(),
                   work.filePath(prefix + "-aes-baseline-restored"), fixture,
                   output + "/" + prefix + "-aes-baseline-sha256.tsv",
                   password_p),
          scheduled ? "SCHEDULE_AES_P_BASELINE_RESTORE_SHA256"
                    : "REALTIME_AES_P_BASELINE_RESTORE_SHA256");
    const auto live_reference = [&]() {
      return scheduled ? schedule->configForTest().encryption_password_file
                       : realtime->configForTest().encryption_password_file;
    };
    const std::string original_reference = live_reference();
    const QByteArray original_store =
        Read(scheduled ? schedule->storePath() : realtime->storePath());
    check(type("RetainField", "12abc") &&
              type("ArchivePasswordField", password_q) &&
              type("ArchivePasswordConfirmationField", password_q) &&
              click(save_name),
          scheduled ? "SCHEDULE_AES_Q_INVALID_DRAFT_SUBMITTED"
                    : "REALTIME_AES_Q_INVALID_DRAFT_SUBMITTED");
    std::string current_secret;
    std::string credential_error;
    check(live_reference() == original_reference &&
              backupproject::ReadCredentialFile(
                  original_reference, &current_secret, &credential_error) &&
              current_secret == password_p.toStdString() &&
              Read(scheduled ? schedule->storePath() : realtime->storePath()) ==
                  original_store &&
              page->property("draftDirty").toBool(),
          scheduled ? "SCHEDULE_REJECTED_Q_PRESERVES_ACTIVE_P_AND_DIRTY_DRAFT"
                    : "REALTIME_REJECTED_Q_PRESERVES_ACTIVE_P_AND_DIRTY_DRAFT");
    dismiss();
    check(type("RetainField", "6") && page->property("draftDirty").toBool(),
          scheduled ? "SCHEDULE_Q_PASSWORD_ALONE_REMAINS_UNSAVED"
                    : "REALTIME_Q_PASSWORD_ALONE_REMAINS_UNSAVED");
    const int events_before_change =
        scheduled ? schedule_finished : realtime_finished;
    QElapsedTimer active_trigger_elapsed;
    active_trigger_elapsed.start();
    const bool active_trigger_completed =
        Write(fixture + "/secret.txt", "AES changed after rejected draft\n") &&
        (!scheduled || schedule->runNow()) &&
        Wait(
            [&]() {
              return scheduled
                         ? schedule_finished > events_before_change &&
                               !schedule->libraryBusy()
                         : realtime_finished > events_before_change &&
                               !realtime->libraryBusy() && !realtime->pending();
            },
            180000);
    check(active_trigger_completed,
          scheduled ? "SCHEDULE_ACTIVE_TRIGGER_AFTER_REJECTED_Q"
                    : "REALTIME_FILE_EVENT_AFTER_REJECTED_Q");
    std::printf(
        "AUTOMATION_UI aes_phase=active-trigger service=%s elapsed_ms=%lld "
        "completed=%d\n",
        qPrintable(prefix),
        static_cast<long long>(active_trigger_elapsed.elapsed()),
        active_trigger_completed);
    std::fflush(stdout);
    backupproject::SnapshotChain actual_chain;
    std::string chain_error;
    check(backupproject::ResolveSnapshotChain(task_repository.toStdString(),
                                              snapshot_name().toStdString(),
                                              &actual_chain, &chain_error) &&
              actual_chain.delta_count == 1 &&
              Restored(task_repository, snapshot_name(),
                       work.filePath(prefix + "-aes-delta-restored"), fixture,
                       output + "/" + prefix + "-aes-delta-sha256.tsv",
                       password_p),
          scheduled ? "SCHEDULE_FAILED_Q_REAL_DELTA_STILL_RESTORES_WITH_P"
                    : "REALTIME_FAILED_Q_REAL_DELTA_STILL_RESTORES_WITH_P");
    backupproject::RestoreOptions wrong_options;
    wrong_options.password = password_q.toStdString();
    const QString rejected_target = work.filePath(prefix + "-wrong-q");
    check(!backupproject::RestoreSnapshotChain(
              task_repository.toStdString(), snapshot_name().toStdString(),
              rejected_target.toStdString(), wrong_options, nullptr,
              &chain_error) &&
              !QFileInfo::exists(rejected_target),
          scheduled ? "SCHEDULE_REJECTED_Q_CANNOT_RESTORE_OR_PUBLISH"
                    : "REALTIME_REJECTED_Q_CANNOT_RESTORE_OR_PUBLISH");
    check(capture(qPrintable(prefix + "-aes-failed-save.png")),
          "AES_FAILURE_PASSWORD_FREE_SCREENSHOT");
    dismiss();
    check(type("RetainField", "12abc") && click(prefix + "EnabledToggle") &&
              (scheduled ? !schedule->enabled() : !realtime->enabled()),
          scheduled ? "SCHEDULE_AES_DISABLED_WITH_INVALID_DRAFT"
                    : "REALTIME_AES_DISABLED_WITH_INVALID_DRAFT");
  }
  // Load an actual legacy configuration, then edit through the same controls
  // as a user. Keeping the repository/source unchanged isolates the compression
  // strategy change: Zstd must establish its own full baseline in that
  // repository.
  for (const QString& prefix :
       {QStringLiteral("schedule"), QStringLiteral("realtime")}) {
    const bool scheduled = prefix == "schedule";
    const QString label = prefix + "-legacy-migration";
    const QString scroll_name = prefix + "PageScroll";
    const QString save_name =
        scheduled ? "saveScheduleButton" : "saveRealtimeButton";
    const QString fixture = work.filePath(label + "-source");
    const QString task_repository = work.filePath(label + "-repository");
    const auto verify = [&](bool ok, const QString& suffix) {
      return check(ok, qPrintable(label + "_" + suffix));
    };
    const auto click = [&](const QString& name) {
      return Click(window, item(name), scroll_name);
    };
    const auto type = [&](const QString& suffix, const QString& value) {
      return Type(window, item(prefix + suffix), value, scroll_name);
    };
    bool prepared =
        !schedule->enabled() && !realtime->enabled() &&
        QDir().mkpath(fixture + "/子目录/空目录") &&
        Write(fixture + "/data.txt", "legacy config preserves its codec\n") &&
        Write(fixture + "/子目录/中文.txt", "legacy migration 中文\n") &&
        Write(fixture + "/empty.txt", "") &&
        controller->saveRepositoryPath(task_repository) &&
        navigate(scheduled ? "realtime" : "scheduled");
    if (prepared && scheduled) {
      prepared = schedule->setEncryptionChoice("none", "") &&
                 schedule->setRepositoryTarget("local", "", "") &&
                 schedule->saveConfigFromFrequencyText(
                     false, fixture, "24", "hours", "6", "mypack",
                     "lzss-huffman", {}, {}, "incremental");
      schedule->reload();
    } else if (prepared) {
      prepared = realtime->setEncryptionChoice("none", "") &&
                 realtime->setRepositoryTarget("local", "", "") &&
                 realtime->saveConfigFromText(false, fixture, "200", "500", "6",
                                              "mypack", "lzss-huffman", {}, {},
                                              "incremental");
      realtime->reload();
    }
    if (!verify(prepared && navigate(scheduled ? "scheduled" : "realtime"),
                "PREEXISTING_CONFIG_LOADED")) {
      schedule->stop();
      realtime->stop();
      return 1;
    }
    QQuickItem* scroll = item(scroll_name);
    QQuickItem* page = scroll ? scroll->parentItem() : nullptr;
    if (!verify(page != nullptr, "PAGE_CONTROLS")) return 1;
    if (!page->property("advancedExpanded").toBool())
      verify(click(prefix + "AdvancedToggle"), "OPEN_ADVANCED_BY_MOUSE");
    const auto shows_legacy = [&]() {
      QQuickItem* combo = item(prefix + "CompressionCombo");
      return combo && combo->property("count").toInt() == 4 &&
             combo->property("currentIndex").toInt() == 3 &&
             combo->property("currentText").toString() ==
                 "LZSS + Huffman（旧格式）" &&
             (scheduled ? schedule->compressionKey()
                        : realtime->compressionKey()) == "lzss-huffman";
    };
    const auto saved_method = [&](backupproject::CompressionMethod compression,
                                  std::uint32_t retain,
                                  const QString& evidence) {
      std::string error;
      backupproject::ScheduleDocument disk_schedule_legacy;
      backupproject::RealtimeConfig disk_realtime_legacy;
      const auto matches = [&](const auto& config) {
        return !config.enabled && config.source_path == fixture.toStdString() &&
               config.compression_method == compression &&
               config.retain_count == retain &&
               config.pack_method == backupproject::PackMethod::kMyPack &&
               config.encryption_method ==
                   backupproject::EncryptionMethod::kNone;
      };
      const bool valid =
          scheduled
              ? backupproject::ScheduleStore(
                    schedule->storePath().toStdString())
                            .Load(&disk_schedule_legacy, &error) ==
                        backupproject::ScheduleLoadStatus::kLoaded &&
                    matches(disk_schedule_legacy.config) &&
                    schedule->configForTest().compression_method == compression
              : backupproject::RealtimeStore(
                    realtime->storePath().toStdString())
                            .Load(&disk_realtime_legacy, &error) ==
                        backupproject::RealtimeLoadStatus::kLoaded &&
                    matches(disk_realtime_legacy) &&
                    realtime->configForTest().compression_method == compression;
      return valid && Write(evidence, Read(scheduled ? schedule->storePath()
                                                     : realtime->storePath()));
    };
    verify(shows_legacy() &&
               saved_method(backupproject::CompressionMethod::kLzssHuffman, 6,
                            output + "/" + label + "-loaded-config.json"),
           "EXACT_LEGACY_LABEL_KEY_AND_ENUM");
    verify(capture(qPrintable(label + "-loaded.png")),
           "ACTUAL_LEGACY_SCREENSHOT");
    verify(
        type("RetainField", "7") && click(save_name) && shows_legacy() &&
            saved_method(backupproject::CompressionMethod::kLzssHuffman, 7,
                         output + "/" + label + "-unrelated-save-config.json"),
        "UNRELATED_EDIT_PRESERVES_LEGACY_CODEC");
    const auto names = [&]() {
      return QDir(task_repository).entryList({"*.bak"}, QDir::Files);
    };
    const auto created_since = [&](const QStringList& before_names) {
      QStringList created;
      for (const QString& name : names())
        if (!before_names.contains(name)) created.push_back(name);
      return created.size() == 1 ? created.front() : QString();
    };
    const auto events = [&]() {
      return scheduled ? schedule_finished : realtime_finished;
    };
    const auto enabled_run = [&]() {
      const int before_events = events();
      return click(prefix + "EnabledToggle") &&
             (scheduled ? schedule->enabled() && schedule->holdsRunnerLock()
                        : realtime->enabled() && realtime->watching()) &&
             (!scheduled || click("runScheduleNowButton")) &&
             Wait(
                 [&]() {
                   return events() > before_events &&
                          (scheduled ? !schedule->libraryBusy()
                                     : !realtime->libraryBusy() &&
                                           !realtime->pending());
                 },
                 180000) &&
             (scheduled ? schedule->lastSucceeded()
                        : realtime->lastSucceeded());
    };
    const QStringList before_legacy = names();
    verify(before_legacy.isEmpty() && enabled_run(),
           "LEGACY_SERVICE_F0_CREATED");
    const QString legacy_name = created_since(before_legacy);
    verify(SnapshotMethods(task_repository, legacy_name,
                           backupproject::PackMethod::kMyPack,
                           backupproject::EncryptionMethod::kNone, false,
                           output + "/" + label + "-legacy-methods.tsv",
                           backupproject::CompressionMethod::kLzssHuffman) &&
               Restored(task_repository, legacy_name,
                        work.filePath(label + "-legacy-restored"), fixture,
                        output + "/" + label + "-legacy-sha256.tsv"),
           "ACTUAL_ID2_BACKUP_RESTORES_SHA256");
    verify(click(prefix + "EnabledToggle") &&
               (scheduled ? !schedule->enabled()
                          : !realtime->enabled() && !realtime->watching()),
           "LEGACY_DISABLED_BEFORE_MIGRATION");
    const bool selected_zstd =
        Select(window, item(prefix + "CompressionCombo"), 2, scroll_name);
    const bool saved_zstd = selected_zstd && click(save_name);
    const bool disk_zstd =
        saved_method(backupproject::CompressionMethod::kZstd, 7,
                     output + "/" + label + "-explicit-zstd-config.json");
    QQuickItem* compression_combo = item(prefix + "CompressionCombo");
    std::printf(
        "AUTOMATION_UI legacy_migration service=%s selected=%d saved=%d "
        "key=%s index=%d count=%d text=%s draft=%d disk=%d\n",
        qPrintable(prefix), selected_zstd, saved_zstd,
        qPrintable(scheduled ? schedule->compressionKey()
                             : realtime->compressionKey()),
        compression_combo->property("currentIndex").toInt(),
        compression_combo->property("count").toInt(),
        qPrintable(compression_combo->property("currentText").toString()),
        page->property("draftCompressionIndex").toInt(), disk_zstd);
    std::fflush(stdout);
    verify(
        selected_zstd && saved_zstd &&
            (scheduled ? schedule->compressionKey()
                       : realtime->compressionKey()) == "zstd" &&
            compression_combo->property("currentText").toString() == "Zstd" &&
            compression_combo->property("count").toInt() == 3 && disk_zstd,
        "EXPLICIT_GUI_CHOICE_MIGRATES_TO_ZSTD");
    Reveal(window, compression_combo, scroll_name);
    verify(capture(qPrintable(label + "-zstd-saved.png")),
           "ACTUAL_ZSTD_SCREENSHOT");
    const QStringList before_zstd = names();
    verify(before_zstd.size() == 1 && enabled_run(),
           "ZSTD_SERVICE_RUN_AFTER_MIGRATION");
    const QString zstd_name = created_since(before_zstd);
    backupproject::SnapshotChain migrated_chain;
    std::string chain_error;
    verify(!zstd_name.isEmpty() && zstd_name != legacy_name &&
               names().size() == 2 &&
               SnapshotMethods(task_repository, zstd_name,
                               backupproject::PackMethod::kMyPack,
                               backupproject::EncryptionMethod::kNone, false,
                               output + "/" + label + "-zstd-methods.tsv") &&
               backupproject::ResolveSnapshotChain(
                   task_repository.toStdString(), zstd_name.toStdString(),
                   &migrated_chain, &chain_error) &&
               migrated_chain.delta_count == 0 &&
               migrated_chain.file_names.size() == 1 &&
               migrated_chain.file_names.front() == zstd_name.toStdString() &&
               (scheduled || realtime->lastOutcomeKind() == "full-baseline") &&
               Restored(task_repository, zstd_name,
                        work.filePath(label + "-zstd-restored"), fixture,
                        output + "/" + label + "-zstd-sha256.tsv"),
           "ACTUAL_ID3_NEW_FULL_BASELINE_AND_RESTORE_SHA256");
    verify(Restored(task_repository, legacy_name,
                    work.filePath(label + "-legacy-after-migration"), fixture,
                    output + "/" + label + "-legacy-still-readable-sha256.tsv"),
           "LEGACY_ARCHIVE_STILL_RESTORES_AFTER_MIGRATION");
    verify(
        click(prefix + "EnabledToggle") &&
            (scheduled ? !schedule->enabled() && !schedule->tickActiveForTest()
                       : !realtime->enabled() && !realtime->watching()),
        "MIGRATED_SERVICE_DISABLED");
  }

  check(navigate("quick"), "MANUAL_COMPRESSION_MIGRATION_PAGE");
  QQuickItem* options_panel = item("backupOptionsPanel");
  QQuickItem* manual_compression = item("compressionSelector");
  if (!check(options_panel && manual_compression,
             "MANUAL_COMPRESSION_MIGRATION_CONTROLS"))
    return 1;
  options_panel->setProperty("expanded", true);
  options_panel->setProperty("compressionKey", "lzss-huffman");
  Settle();
  check(manual_compression->property("count").toInt() == 4 &&
            manual_compression->property("currentIndex").toInt() == 3 &&
            manual_compression->property("currentText")
                .toString()
                .contains(QStringLiteral("旧格式")),
        "MANUAL_LEGACY_MODEL_RETAINS_SELECTED_KEY");
  check(Select(window, manual_compression, 2, "backupPageScroll") &&
            options_panel->property("compressionKey").toString() == "zstd" &&
            manual_compression->property("count").toInt() == 3 &&
            manual_compression->property("currentIndex").toInt() == 2 &&
            manual_compression->property("currentText").toString() == "Zstd",
        "MANUAL_MODEL_REPLACEMENT_PRESERVES_ZSTD_SELECTION");
  check(capture("manual-legacy-migration-zstd-selected.png"),
        "MANUAL_MIGRATED_COMPRESSION_SCREENSHOT");

  // Exercise the same controls and live services with the new shared codec.
  // Every case owns a source/repository so another task's baseline cannot make
  // an incorrect format, stale source, or false incremental decision pass.
  const QStringList zstd_pack_keys = {"mypack", "ustar", "fast-ustar"};
  const backupproject::PackMethod zstd_packs[] = {
      backupproject::PackMethod::kMyPack, backupproject::PackMethod::kUstar,
      backupproject::PackMethod::kFastUstar};
  for (const QString& prefix :
       {QStringLiteral("schedule"), QStringLiteral("realtime")}) {
    const bool scheduled = prefix == "schedule";
    const QString scroll_name = prefix + "PageScroll";
    const QString save_name =
        scheduled ? "saveScheduleButton" : "saveRealtimeButton";
    for (int pack_index = 0; pack_index < zstd_pack_keys.size(); ++pack_index) {
      for (int encryption_index = 0; encryption_index < 2; ++encryption_index) {
        const bool encrypted = encryption_index == 1;
        const auto encryption =
            encrypted ? backupproject::EncryptionMethod::kAes256CtrHmacSha256
                      : backupproject::EncryptionMethod::kNone;
        const QString case_name = prefix + "-zstd-" +
                                  zstd_pack_keys[pack_index] +
                                  (encrypted ? "-aes" : "-none");
        const auto verify = [&](bool ok, const QString& suffix) {
          return check(ok, qPrintable(case_name + "_" + suffix));
        };
        const QString fixture = work.filePath(case_name + "-source");
        const QString task_repository =
            work.filePath(case_name + "-repository");
        const QString password =
            encrypted ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                      : QString();
        const auto click = [&](const QString& name) {
          return Click(window, item(name), scroll_name);
        };
        const auto type = [&](const QString& suffix, const QString& value) {
          return Type(window, item(prefix + suffix), value, scroll_name);
        };
        if (!verify(!schedule->enabled() && !realtime->enabled() &&
                        QDir().mkpath(fixture + "/子目录/空目录") &&
                        Write(fixture + "/small.txt", "Zstd F0 original\n") &&
                        Write(fixture + "/removed.txt", "remove in D1\n") &&
                        Write(fixture + "/子目录/中文.txt", "中文路径内容\n") &&
                        Write(fixture + "/子目录/empty.txt", "") &&
                        controller->saveRepositoryPath(task_repository) &&
                        navigate(scheduled ? "scheduled" : "realtime"),
                    "ISOLATED_FIXTURE")) {
          schedule->stop();
          realtime->stop();
          return 1;
        }
        QQuickItem* scroll = item(scroll_name);
        QQuickItem* page = scroll ? scroll->parentItem() : nullptr;
        if (!verify(page != nullptr, "PAGE_CONTROLS")) return 1;
        if (!page->property("advancedExpanded").toBool())
          verify(click(prefix + "AdvancedToggle"), "OPEN_ADVANCED_BY_MOUSE");
        bool configured =
            type("SourceField", fixture) && type("RetainField", "6") &&
            Select(window, item(prefix + "StrategyCombo"), 1, scroll_name) &&
            Select(window, item(prefix + "PackCombo"), pack_index,
                   scroll_name) &&
            Select(window, item(prefix + "CompressionCombo"), 2, scroll_name) &&
            Select(window, item(prefix + "EncryptionCombo"), encryption_index,
                   scroll_name);
        if (configured && encrypted)
          configured = type("ArchivePasswordField", password) &&
                       type("ArchivePasswordConfirmationField", password);
        if (configured && scheduled)
          configured = type("FrequencyValueField", "24") &&
                       Select(window, item("scheduleFrequencyUnitCombo"), 1,
                              scroll_name);
        if (configured && !scheduled)
          configured =
              type("DebounceField", "200") && type("MaxWaitField", "500");
        verify(configured && click(save_name) &&
                   (scheduled ? !schedule->enabled() : !realtime->enabled()) &&
                   (scheduled ? schedule->compressionKey()
                              : realtime->compressionKey()) == "zstd" &&
                   (scheduled ? schedule->packKey() : realtime->packKey()) ==
                       zstd_pack_keys[pack_index],
               "REAL_FORM_SAVED_WITH_EXACT_CODEC");
        backupproject::ScheduleDocument disk_schedule_zstd;
        backupproject::RealtimeConfig disk_realtime_zstd;
        std::string config_error;
        const bool disk_loaded =
            scheduled ? backupproject::ScheduleStore(
                            schedule->storePath().toStdString())
                                .Load(&disk_schedule_zstd, &config_error) ==
                            backupproject::ScheduleLoadStatus::kLoaded
                      : backupproject::RealtimeStore(
                            realtime->storePath().toStdString())
                                .Load(&disk_realtime_zstd, &config_error) ==
                            backupproject::RealtimeLoadStatus::kLoaded;
        const auto matches_saved = [&](const auto& saved) {
          return !saved.enabled && saved.source_path == fixture.toStdString() &&
                 saved.compression_method ==
                     backupproject::CompressionMethod::kZstd &&
                 saved.pack_method == zstd_packs[pack_index] &&
                 saved.encryption_method == encryption;
        };
        verify(disk_loaded &&
                   (scheduled ? matches_saved(disk_schedule_zstd.config)
                              : matches_saved(disk_realtime_zstd)) &&
                   Write(output + "/" + case_name + "-config.json",
                         Read(scheduled ? schedule->storePath()
                                        : realtime->storePath())),
               "DISK_CONFIG_RETAINS_ZSTD_PACK_AND_ENCRYPTION");
        const auto snapshot_names = [&]() {
          return QDir(task_repository).entryList({"*.bak"}, QDir::Files);
        };
        const auto created_since = [&](const QStringList& before_names) {
          QStringList created;
          for (const QString& name : snapshot_names())
            if (!before_names.contains(name)) created.push_back(name);
          return created.size() == 1 ? created.front() : QString();
        };
        const auto events = [&]() {
          return scheduled ? schedule_finished : realtime_finished;
        };
        const auto completed = [&](int prior_events) {
          return Wait(
                     [&]() {
                       return events() > prior_events &&
                              (scheduled ? !schedule->libraryBusy()
                                         : !realtime->libraryBusy() &&
                                               !realtime->pending());
                     },
                     180000) &&
                 (scheduled ? schedule->lastSucceeded()
                            : realtime->lastSucceeded());
        };
        const int before_baseline = events();
        const QStringList empty_repository = snapshot_names();
        verify(
            empty_repository.isEmpty() && click(prefix + "EnabledToggle") &&
                (scheduled ? schedule->enabled() && schedule->holdsRunnerLock()
                           : realtime->enabled() && realtime->watching()) &&
                (!scheduled || click("runScheduleNowButton")) &&
                completed(before_baseline),
            "ENABLED_SERVICE_CREATED_F0");
        const QString baseline = created_since(empty_repository);
        backupproject::SnapshotChain baseline_chain;
        std::string chain_error;
        verify(
            SnapshotMethods(task_repository, baseline, zstd_packs[pack_index],
                            encryption, false,
                            output + "/" + case_name + "-f0-methods.tsv") &&
                backupproject::ResolveSnapshotChain(
                    task_repository.toStdString(), baseline.toStdString(),
                    &baseline_chain, &chain_error) &&
                baseline_chain.delta_count == 0 &&
                Restored(task_repository, baseline,
                         work.filePath(case_name + "-f0-restored"), fixture,
                         output + "/" + case_name + "-f0-sha256.tsv", password),
            "REAL_F0_HEADER_AND_RESTORE_SHA256");
        const QStringList before_delta_names = snapshot_names();
        const int before_delta = events();
        verify(Write(fixture + "/small.txt", "Zstd D1 modified\n") &&
                   Write(fixture + "/子目录/新增.txt", "Zstd D1 added\n") &&
                   QFile::remove(fixture + "/removed.txt") &&
                   (!scheduled || click("runScheduleNowButton")) &&
                   completed(before_delta),
               "REAL_TRIGGER_AFTER_ADD_MODIFY_DELETE");
        const QString delta = created_since(before_delta_names);
        backupproject::SnapshotChain delta_chain;
        backupproject::DeltaEnvelope envelope;
        verify(
            backupproject::ResolveSnapshotChain(task_repository.toStdString(),
                                                delta.toStdString(),
                                                &delta_chain, &chain_error) &&
                delta_chain.delta_count == 1 &&
                delta_chain.file_names.front() == baseline.toStdString() &&
                backupproject::ReadDeltaEnvelope(
                    (task_repository + "/" + delta).toStdString(), &envelope,
                    &chain_error) &&
                envelope.parent_file_name == baseline.toStdString() &&
                envelope.added == 1 && envelope.modified == 1 &&
                envelope.removed == 1 &&
                SnapshotMethods(task_repository, delta, zstd_packs[pack_index],
                                encryption, true,
                                output + "/" + case_name + "-d1-methods.tsv") &&
                Restored(task_repository, delta,
                         work.filePath(case_name + "-d1-restored"), fixture,
                         output + "/" + case_name + "-d1-sha256.tsv", password),
            "REAL_D1_PARENT_CODEC_AND_RESTORE_SHA256");
        const QStringList before_unchanged_names = snapshot_names();
        const int before_unchanged = events();
        if (!scheduled) realtime->reload();
        verify((!scheduled || click("runScheduleNowButton")) &&
                   completed(before_unchanged) &&
                   snapshot_names() == before_unchanged_names &&
                   (scheduled || realtime->lastOutcomeKind() == "no-changes"),
               "UNCHANGED_SOURCE_CREATES_NO_SNAPSHOT");
        if (encrypted) {
          backupproject::RestoreOptions wrong;
          wrong.password =
              QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
          const QString rejected = work.filePath(case_name + "-wrong-password");
          verify(!backupproject::RestoreSnapshotChain(
                     task_repository.toStdString(), delta.toStdString(),
                     rejected.toStdString(), wrong, nullptr, &chain_error) &&
                     !QFileInfo::exists(rejected),
                 "WRONG_AES_PASSWORD_PUBLISHES_NO_TARGET");
        }
        // Captures use the real xcb window and never include a plaintext
        // secret.
        verify(capture(qPrintable(case_name + "-enabled.png")),
               "ACTUAL_WINDOW_SCREENSHOT");
        const int before_disable = events();
        const QStringList before_disabled_names = snapshot_names();
        verify(
            click(prefix + "EnabledToggle") &&
                (scheduled
                     ? !schedule->enabled() && !schedule->tickActiveForTest()
                     : !realtime->enabled() && !realtime->watching() &&
                           realtime->watchCount() == 0) &&
                Write(fixture + "/small.txt", "changed after Zstd disable\n"),
            "DISABLED_SERVICE_BEFORE_NEW_FILE_CHANGE");
        Settle(1800);
        verify(events() == before_disable &&
                   snapshot_names() == before_disabled_names,
               "POST_DISABLE_NO_EVENT_OR_SNAPSHOT");
      }
    }
  }

  schedule->stop();
  realtime->stop();
  QObject::disconnect(schedule_connection);
  QObject::disconnect(realtime_connection);
  std::printf("AUTOMATION_UI SUMMARY passed=%d failed=%d\n", passed, failed);
  return failed == 0 ? 0 : 1;
}

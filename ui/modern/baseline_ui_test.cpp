// Explicit test entry: production BackupPage, real Qt input and actual
// archives.
#include "baseline_ui_test.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <vector>

#include "app_theme.h"
#include "backup_controller.h"
#include "filter_rule_model.h"
#include "incremental_backup.h"
#include "incremental_restore.h"

namespace {
void Settle() {
  QEventLoop loop;
  QTimer::singleShot(80, &loop, &QEventLoop::quit);
  loop.exec();
}
QQuickItem* Find(QQuickItem* parent, const QString& name,
                 const QString& candidate = QString()) {
  if (parent->objectName() == name &&
      (candidate.isEmpty() ||
       parent->property("candidateFileName").toString() == candidate))
    return parent;
  for (QQuickItem* child : parent->childItems()) {
    if (auto* found = Find(child, name, candidate)) return found;
  }
  return nullptr;
}
bool RevealInViewport(QQuickWindow* window, QQuickItem* item) {
  if (item == nullptr || !item->isVisible()) return false;
  // Both the outer ScrollView and the candidate ListView may clip this item.
  // Scroll each real Flickable, then verify the final input point is visible.
  for (QQuickItem* parent = item->parentItem(); parent != nullptr;
       parent = parent->parentItem()) {
    if (!parent->property("contentY").isValid()) continue;
    const QRectF bounds = item->mapRectToItem(parent, item->boundingRect());
    double content_y = parent->property("contentY").toDouble();
    constexpr double margin = 12;
    if (bounds.top() < margin)
      content_y += bounds.top() - margin;
    else if (bounds.bottom() > parent->height() - margin)
      content_y += bounds.bottom() - parent->height() + margin;
    const double maximum = qMax(
        0.0, parent->property("contentHeight").toDouble() - parent->height());
    parent->setProperty("contentY", qBound(0.0, content_y, maximum));
    Settle();
  }
  const QPointF scene = item->mapToScene(item->boundingRect().center());
  if (!QRectF(0, 0, window->width(), window->height()).contains(scene))
    return false;
  for (QQuickItem* parent = item->parentItem(); parent != nullptr;
       parent = parent->parentItem()) {
    if (parent->clip() &&
        !parent->boundingRect().contains(parent->mapFromScene(scene)))
      return false;
  }
  return true;
}
void Click(QQuickWindow* window, QQuickItem* item) {
  if (!RevealInViewport(window, item)) {
    std::fprintf(stderr,
                 "[baseline-ui] mouse target is outside the real viewport\n");
    return;
  }
  const QPointF pos = item->mapToScene({item->width() / 2, item->height() / 2});
  const QPointF global = window->mapToGlobal(pos.toPoint());
  QMouseEvent move(QEvent::MouseMove, pos, pos, global, Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &move);
  QMouseEvent press(QEvent::MouseButtonPress, pos, pos, global, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &press);
  QObject* grabber = press.exclusiveGrabber(press.point(0));
  std::fprintf(
      stderr,
      "[baseline-ui] mouse press %s accepted=%d pressed=%d hover=%d "
      "grabber=%s\n",
      qPrintable(item->objectName()), press.isAccepted(),
      item->property("pressed").toBool(), item->property("hovered").toBool(),
      grabber == nullptr ? "none" : grabber->metaObject()->className());
  Settle();
  // Construct the release after dispatching the press: Qt 6's pointing-device
  // event state and grabbers must observe the actual input sequence.
  QMouseEvent release(QEvent::MouseButtonRelease, pos, pos, global,
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &release);
  std::fprintf(stderr,
               "[baseline-ui] mouse release %s accepted=%d pressed=%d\n",
               qPrintable(item->objectName()), release.isAccepted(),
               item->property("pressed").toBool());
  Settle();
}
void Space(QQuickWindow* window, QQuickItem* item) {
  if (!RevealInViewport(window, item)) {
    std::fprintf(
        stderr, "[baseline-ui] keyboard target is outside the real viewport\n");
    return;
  }
  item->forceActiveFocus();
  QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
  QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &press);
  QCoreApplication::sendEvent(window, &release);
  Settle();
}
bool WaitScan(backup_modern::BackupController* controller) {
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(controller,
                   &backup_modern::BackupController::baselineScanStateChanged,
                   &loop, [&]() {
                     if (!controller->baselineScanBusy()) loop.quit();
                   });
  if (controller->baselineScanBusy()) {
    timer.start(120000);
    loop.exec();
  }
  Settle();
  return !controller->baselineScanBusy();
}
bool Write(const QString& path, const QByteArray& data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray Digest(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return {};
  return hash.result();
}
void ScrollBottom(QQuickItem* page) {
  auto* scroll = Find(page, QStringLiteral("backupPageScroll"));
  QObject* content = scroll == nullptr
                         ? nullptr
                         : scroll->property("contentItem").value<QObject*>();
  if (content != nullptr)
    content->setProperty(
        "contentY", qMax(0.0, content->property("contentHeight").toDouble() -
                                  content->property("height").toDouble()));
  Settle();
}
}  // namespace

int RunBaselinePickerTest(QQuickWindow* main_window,
                          backup_modern::AppTheme* theme) {
  int passed = 0;
  int failed = 0;
  const auto check = [&](bool condition, const char* label) {
    std::printf("[baseline-ui] %s %s\n", condition ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    condition ? ++passed : ++failed;
  };
  auto* engine = qmlEngine(main_window);
  check(engine != nullptr, "real QML engine available");
  if (engine == nullptr) return 1;
  QTemporaryDir work;
  check(work.isValid(), "isolated synthetic fixture");
  if (!work.isValid()) return 1;
  const QString source = work.filePath(QStringLiteral("source"));
  const QString foreign = work.filePath(QStringLiteral("foreign"));
  const QString repository = work.filePath(QStringLiteral("repository"));
  check(QDir().mkpath(source) && QDir().mkpath(foreign) &&
            QDir().mkpath(repository) &&
            Write(source + QStringLiteral("/中文.txt"), "before\n") &&
            Write(source + QStringLiteral("/empty"), "") &&
            Write(foreign + QStringLiteral("/foreign"), "foreign\n"),
        "actual Chinese and empty file fixture");
  backupproject::Filter filter;
  backupproject::BackupOptions options;
  backupproject::IncrementalOutcome outcome;
  std::string error;
  const auto full = [&](const QString& directory, const char* name) {
    return backupproject::RunManagedFullBackup(
        directory.toStdString(), repository.toStdString(), name,
        backupproject::RepositoryIdentity(repository.toStdString()), filter,
        options, {}, {}, &outcome, &error);
  };
  check(full(foreign, "foreign.bak") && full(source, "selected-full.bak") &&
            Write(source + QStringLiteral("/中文.txt"), "after\n") &&
            Write(source + QStringLiteral("/new"), "new\n"),
        "real managed baselines and changed source");
  backup_modern::OperationGate gate;
  backup_modern::BackupController controller(
      work.filePath(QStringLiteral("config.json")), &gate);
  backup_modern::FilterRuleModel rules(&controller);
  controller.setSourcePath(source);
  check(controller.saveRepositoryPath(repository) &&
            controller.waitForCatalogIdle(10000),
        "isolated production controller");
  QQmlContext context(engine->rootContext());
  context.setContextProperty(QStringLiteral("controller"), &controller);
  context.setContextProperty(QStringLiteral("filterRuleModel"), &rules);
  QQuickWindow input_window;
  input_window.setTitle(QStringLiteral("Backup Project · 增量基线"));
  input_window.resize(1280, 1000);
  const QString screenshot_directory =
      qEnvironmentVariable("BACKUP_BASELINE_SCREENSHOTS");
  const auto capture = [&](const char* name) {
    if (screenshot_directory.isEmpty()) return;
    check(QDir().mkpath(screenshot_directory), "screenshot directory");
    input_window.requestUpdate();
    Settle();
    const QImage frame = input_window.grabWindow();
    check(
        !frame.isNull() &&
            frame.save(
                QDir(screenshot_directory).filePath(QString::fromLatin1(name))),
        name);
  };
  const bool original_dark = theme->dark();
  input_window.setColor(theme->background());
  QQmlComponent component(
      engine, QUrl(QStringLiteral("qrc:/qml/pages/BackupPage.qml")));
  std::unique_ptr<QObject> object(component.create(&context));
  auto* page = qobject_cast<QQuickItem*>(object.get());
  check(page != nullptr, "actual BackupPage created");
  if (page == nullptr) {
    std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
    return 1;
  }
  page->setParentItem(input_window.contentItem());
  const auto resize_page = [&]() {
    page->setWidth(input_window.width());
    page->setHeight(input_window.height());
  };
  resize_page();
  // The window manager may clamp the requested size to the actual desktop.
  // Keep the page and ScrollView viewport bound to that accepted window size.
  QObject::connect(&input_window, &QQuickWindow::widthChanged, page,
                   resize_page);
  QObject::connect(&input_window, &QQuickWindow::heightChanged, page,
                   resize_page);
  if (!screenshot_directory.isEmpty()) {
    work.setAutoRemove(false);
    std::fprintf(stderr, "[baseline-ui] fixture=%s\n", qPrintable(work.path()));
  }
  QObject::connect(
      &controller, &backup_modern::BackupController::baselineScanStateChanged,
      &input_window, [&]() {
        std::fprintf(
            stderr, "[baseline-ui] scan busy=%d candidates=%lld error=%s\n",
            controller.baselineScanBusy(),
            static_cast<long long>(controller.baselineCandidates().size()),
            qPrintable(controller.baselineScanError()));
      });
  input_window.show();
  input_window.requestActivate();
  Settle();
  auto* panel = Find(page, QStringLiteral("backupOptionsPanel"));
  check(panel != nullptr, "actual shared options panel");
  if (panel == nullptr) return 1;
  panel->setProperty("strategyKey", QStringLiteral("incremental"));
  Settle();
  ScrollBottom(page);
  auto* picker = Find(page, QStringLiteral("incrementalBaselinePanel"));
  auto* refresh = Find(page, QStringLiteral("checkIncrementalBaselinesButton"));
  check(picker != nullptr && refresh != nullptr && refresh->isVisible(),
        "baseline panel visible in actual page");
  if (picker == nullptr || refresh == nullptr) return 1;
  const auto diagnose = [&](const char* stage, QQuickItem* item) {
    std::fprintf(
        stderr,
        "[baseline-ui] %s window=%dx%d active=%d source=%s repository=%s\n",
        stage, input_window.width(), input_window.height(),
        input_window.isActive(), qPrintable(controller.sourcePath()),
        qPrintable(controller.repositoryPath()));
    auto* item_context = qmlContext(item);
    QObject* effective_controller =
        item_context == nullptr
            ? nullptr
            : item_context->contextProperty("controller").value<QObject*>();
    std::fprintf(
        stderr, "[baseline-ui] effective controller=%p fixture controller=%p\n",
        static_cast<void*>(effective_controller),
        static_cast<void*>(&controller));
    for (QQuickItem* current = item; current != nullptr;
         current = current->parentItem()) {
      const QPointF scene = current->mapToScene(QPointF(0, 0));
      std::fprintf(
          stderr,
          "[baseline-ui] item=%s class=%s scene=(%.1f,%.1f) "
          "size=(%.1f,%.1f) visible=%d enabled=%d clip=%d contentY=%.1f\n",
          qPrintable(current->objectName()), current->metaObject()->className(),
          scene.x(), scene.y(), current->width(), current->height(),
          current->isVisible(), current->isEnabled(), current->clip(),
          current->property("contentY").toDouble());
    }
    std::fflush(stderr);
  };
  diagnose("before refresh click", refresh);
  capture("00-before-refresh.png");
  Click(&input_window, refresh);
  const bool scan_finished = WaitScan(&controller);
  check(scan_finished && controller.baselineCandidates().size() == 2,
        "real mouse refresh validates repository candidates");
  if (!scan_finished || controller.baselineCandidates().size() != 2) {
    diagnose("refresh failed", refresh);
    std::fprintf(stderr,
                 "[baseline-ui] scan result count=%lld busy=%d error=%s\n",
                 static_cast<long long>(controller.baselineCandidates().size()),
                 controller.baselineScanBusy(),
                 qPrintable(controller.baselineScanError()));
    capture("00-failed-refresh.png");
  }
  ScrollBottom(page);
  auto* selectable =
      Find(page, QStringLiteral("selectIncrementalBaselineButton"),
           QStringLiteral("selected-full.bak"));
  auto* unavailable =
      Find(page, QStringLiteral("selectIncrementalBaselineButton"),
           QStringLiteral("foreign.bak"));
  check(selectable != nullptr && selectable->isEnabled() &&
            unavailable != nullptr && !unavailable->isEnabled(),
        "compatible baseline enabled and foreign source disabled");
  if (selectable == nullptr || unavailable == nullptr) {
    capture("00-failed-candidates.png");
    std::printf("[baseline-ui] passed=%d failed=%d\n", passed, failed);
    return 1;
  }
  theme->setDark(false);
  input_window.setColor(theme->background());
  Settle();
  capture("01-baseline-light.png");
  theme->setDark(true);
  input_window.setColor(theme->background());
  Settle();
  capture("02-baseline-dark.png");
  Click(&input_window, selectable);
  check(picker->property("selectedFileName").toString() ==
            QStringLiteral("selected-full.bak"),
        "mouse selects a usable real baseline");
  Click(&input_window, unavailable);
  check(picker->property("selectedFileName").toString() ==
            QStringLiteral("selected-full.bak"),
        "disabled foreign candidate cannot replace selection");
  auto* automatic = Find(page, QStringLiteral("useAutomaticBaselineButton"));
  Space(&input_window, automatic);
  check(automatic != nullptr &&
            picker->property("selectedFileName").toString().isEmpty(),
        "keyboard restores automatic selection");
  Space(&input_window, selectable);
  check(picker->property("selectedFileName").toString() ==
            QStringLiteral("selected-full.bak"),
        "keyboard selects the same usable baseline");
  auto* password = Find(panel, QStringLiteral("passwordField"));
  check(password != nullptr, "real password field exists");
  if (password != nullptr)
    password->setProperty("text", "temporary-session-edit");
  Settle();
  check(
      controller.baselineCandidates().isEmpty() &&
          picker->property("selectedFileName").toString().isEmpty(),
      "real password field edit invalidates verified candidates and selection");
  if (password != nullptr) password->setProperty("text", "");
  ScrollBottom(page);
  Click(&input_window, refresh);
  check(WaitScan(&controller), "recheck after password invalidation");
  ScrollBottom(page);
  selectable = Find(page, QStringLiteral("selectIncrementalBaselineButton"),
                    QStringLiteral("selected-full.bak"));
  Click(&input_window, selectable);
  auto* start = Find(page, QStringLiteral("startBackupButton"));
  Click(&input_window, start);
  check(start != nullptr && controller.waitForIdle(120000) &&
            controller.lastSucceeded(),
        "actual BackupPage start button succeeds");
  std::vector<backupproject::BackupRecord> records;
  backupproject::BackupCatalog catalog;
  check(catalog.List(repository.toStdString(), &records, &error),
        "read actual output catalog");
  std::string delta;
  for (const auto& record : records) {
    if (record.incremental_delta &&
        record.parent_file_name == "selected-full.bak")
      delta = record.file_name;
  }
  check(!delta.empty(), "actual page passes its selected parent to core");
  const QString restored = work.filePath(QStringLiteral("restored"));
  backupproject::RestoreOptions restore_options;
  check(!delta.empty() &&
            backupproject::RestoreSnapshotChain(
                repository.toStdString(), delta, restored.toStdString(),
                restore_options, nullptr, &error),
        "restore actual UI-created delta");
  bool equal = true;
  for (const QString& name : {QStringLiteral("中文.txt"),
                              QStringLiteral("empty"), QStringLiteral("new")}) {
    const QByteArray source_digest = Digest(source + "/" + name);
    equal &= !source_digest.isEmpty() &&
             source_digest == Digest(restored + "/" + name);
  }
  check(equal, "all restored files match independent Qt SHA-256");
  check(QDir(restored).entryList(QDir::Files).size() == 3,
        "restore publishes exactly the expected files");
  capture("03-baseline-backup-complete.png");
  theme->setDark(original_dark);
  check(controller.waitForCatalogIdle(10000), "catalog settled before cleanup");
  std::printf("[baseline-ui] passed=%d failed=%d\n", passed, failed);
  return failed == 0 ? 0 : 1;
}

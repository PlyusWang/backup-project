// 真实 Qt 输入事件验证下拉交互、卡片详情、历史显示及响应式对齐。
#include "ux_acceptance_test.h"

#include <qqml.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

#include "app_theme.h"
#include "config_manager.h"
#include "realtime_backup_service.h"
#include "realtime_controller.h"
#include "schedule_controller.h"
#include "schedule_store.h"
#include "user_error.h"

namespace {

void Settle() {
  QEventLoop loop;
  QTimer::singleShot(50, &loop, &QEventLoop::quit);
  loop.exec();
}

QQuickItem* FindItem(QQuickItem* parent, const char* name) {
  if (parent->objectName() == QLatin1String(name)) return parent;
  for (QQuickItem* child : parent->childItems()) {
    QQuickItem* found = FindItem(child, name);
    if (found != nullptr) return found;
  }
  return nullptr;
}

void Key(QQuickWindow* window, int key) {
  QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &press);
  QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &release);
  Settle();
}

void Pointer(QQuickWindow* window, QQuickItem* item, bool click) {
  const QPointF position =
      item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
  const QPointF global = window->mapToGlobal(position.toPoint());
  QMouseEvent move(QEvent::MouseMove, position, position, global, Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(window, &move);
  if (click) {
    QMouseEvent press(QEvent::MouseButtonPress, position, position, global,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, position, position, global,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &release);
  }
  Settle();
}

void Wheel(QQuickWindow* window, QQuickItem* item, int delta,
           bool pixel = false) {
  const QPointF position =
      item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
  QWheelEvent event(position, window->mapToGlobal(position.toPoint()),
                    pixel ? QPoint(0, delta) : QPoint(),
                    pixel ? QPoint() : QPoint(0, delta), Qt::NoButton,
                    Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(window, &event);
  Settle();
}

}  // namespace

int RunUxAcceptanceTest(QQuickWindow* window, backup_modern::AppTheme* theme,
                        backup_modern::ScheduleController* schedule,
                        backup_modern::RealtimeController* realtime) {
  Q_UNUSED(schedule);
  Q_UNUSED(realtime);
  int passed = 0;
  int failed = 0;
  const auto check = [&](bool condition, const char* label) {
    std::printf("[ux-acceptance] %s %s\n", condition ? "PASS" : "FAIL", label);
    condition ? ++passed : ++failed;
  };
  for (const char* kind : {"ssh-auth", "ssh-hostkey", "ssh-timeout",
                           "ssh-host-unreachable", "ssh-forward", "ssh-exit",
                           "raw-password", "raw-corrupt", "integrity"}) {
    const auto error = backup_modern::ExplainOperationError(
        QString::fromLatin1(kind), QStringLiteral("连接"));
    check(!error.title.isEmpty() && !error.summary.isEmpty() &&
              !error.suggestion.isEmpty() &&
              !error.suggestion.contains(
                  QStringLiteral("检查路径、权限与可用空间")),
          kind);
  }
  check(backup_modern::ExplainOperationError(QStringLiteral("ssh-auth"), {})
                    .title == QStringLiteral("SSH 认证失败") &&
            backup_modern::ExplainOperationError(QStringLiteral("ssh-hostkey"),
                                                 {})
                    .title == QStringLiteral("SSH 服务器身份校验失败") &&
            backup_modern::ExplainOperationError(QStringLiteral("ssh-timeout"),
                                                 {})
                    .title == QStringLiteral("SSH 连接超时"),
        "SSH authentication, host identity and timeout have distinct "
        "explanations");
  const QString screenshot_directory =
      qEnvironmentVariable("BACKUP_UX_SCREENSHOTS");
  if (!screenshot_directory.isEmpty()) {
    check(QDir().mkpath(screenshot_directory), "screenshot directory ready");
  }
  const auto capture = [&](QQuickWindow* target, const char* file_name) {
    if (screenshot_directory.isEmpty()) return;
    target->requestUpdate();
    Settle();
    const QImage frame = target->grabWindow();
    const bool saved =
        !frame.isNull() &&
        frame.save(QDir(screenshot_directory)
                       .filePath(QString::fromLatin1(file_name)));
    check(saved, file_name);
  };
  QQmlEngine* engine = qmlEngine(window);
  check(engine != nullptr, "QML engine available");
  if (engine == nullptr) return 1;

  window->setProperty("currentPageKey", QStringLiteral("remote"));
  Settle();
  QQuickItem* technical_card =
      FindItem(window->contentItem(), "remoteTechnicalCard");
  check(technical_card != nullptr && !technical_card->isVisible(),
        "remote page hides raw technical errors");
  const bool original_dark = theme->dark();
  window->setProperty("currentPageKey", QStringLiteral("home"));
  window->setWidth(1440);
  window->setHeight(1000);
  theme->setDark(false);
  Settle();
  capture(window, "01-home-light.png");
  theme->setDark(true);
  Settle();
  capture(window, "02-home-dark.png");

  QQuickWindow input_window;
  input_window.resize(920, 740);
  input_window.show();
  input_window.requestActivate();
  QQmlComponent combo_component(
      engine, QUrl(QStringLiteral("qrc:/qml/components/AppComboBox.qml")));
  std::unique_ptr<QObject> combo_object(combo_component.create());
  QQuickItem* combo = qobject_cast<QQuickItem*>(combo_object.get());
  check(combo != nullptr, "shared ComboBox created");
  if (combo == nullptr) {
    std::fprintf(stderr, "%s\n", qPrintable(combo_component.errorString()));
    return 1;
  }
  combo->setParentItem(input_window.contentItem());
  combo->setX(30);
  combo->setY(30);
  combo->setWidth(380);
  QStringList labels;
  for (int index = 0; index < 24; ++index) {
    labels.append(QStringLiteral("Option %1").arg(index));
  }
  combo->setProperty("model", labels);
  combo->setProperty("currentIndex", 8);
  QObject* popup = combo->property("popup").value<QObject*>();
  QQuickItem* list = popup == nullptr
                         ? nullptr
                         : popup->property("contentItem").value<QQuickItem*>();
  check(popup != nullptr && list != nullptr, "popup content available");
  if (popup == nullptr || list == nullptr) return 1;

  QQmlComponent bound_component(engine);
  bound_component.setData(
      R"(
import QtQuick
import "qrc:/qml/components"
Item {
    property int committedIndex: 0
    AppComboBox {
        objectName: "boundCombo"
        width: 360
        model: ["first", "second", "third"]
        currentIndex: parent.committedIndex
        onActivated: function(index) { parent.committedIndex = index }
    }
})",
      QUrl(QStringLiteral("qrc:/qml/combo-binding-test.qml")));
  std::unique_ptr<QObject> bound_object(bound_component.create());
  QQuickItem* bound_root = qobject_cast<QQuickItem*>(bound_object.get());
  QQuickItem* bound_combo =
      bound_root ? FindItem(bound_root, "boundCombo") : nullptr;
  check(bound_combo != nullptr, "bound ComboBox fixture created");
  if (bound_root != nullptr && bound_combo != nullptr) {
    bound_root->setParentItem(input_window.contentItem());
    bound_root->setY(170);
    bound_root->setSize(QSizeF(380, 80));
    input_window.requestActivate();
    Settle();
    Pointer(&input_window, bound_combo, true);
    Key(&input_window, Qt::Key_Home);
    Key(&input_window, Qt::Key_Down);
    Key(&input_window, Qt::Key_Return);
    check(bound_root->property("committedIndex").toInt() == 1 &&
              bound_combo->property("currentIndex").toInt() == 1,
          "user selection commits through bound model");
    bound_root->setProperty("committedIndex", 2);
    Settle();
    check(bound_combo->property("currentIndex").toInt() == 2,
          "selection preserves binding after external normalization");
    Key(&input_window, Qt::Key_Home);
    check(bound_root->property("committedIndex").toInt() == 0,
          "selection after external update uses synchronized value");
    bound_root->setVisible(false);
  }

  for (bool dark : {false, true}) {
    theme->setDark(dark);
    input_window.setColor(theme->background());
    combo->setProperty("currentIndex", 8);
    combo->forceActiveFocus();
    Settle();
    Key(&input_window, Qt::Key_Up);
    check(combo->property("currentIndex").toInt() == 7,
          "closed Up selects previous");
    Key(&input_window, Qt::Key_Down);
    check(combo->property("currentIndex").toInt() == 8,
          "closed Down selects next");
    Key(&input_window, Qt::Key_Home);
    check(combo->property("currentIndex").toInt() == 0,
          "closed Home selects first");
    Key(&input_window, Qt::Key_End);
    check(combo->property("currentIndex").toInt() == 23,
          "closed End selects last");
    Key(&input_window, Qt::Key_PageUp);
    check(combo->property("currentIndex").toInt() < 23,
          "closed PageUp selects an earlier item");
    Key(&input_window, Qt::Key_PageDown);
    check(combo->property("currentIndex").toInt() == 23,
          "closed PageDown reaches last item");
    Pointer(&input_window, combo, false);
    Wheel(&input_window, combo, 120);
    check(combo->property("currentIndex").toInt() == 22,
          "closed wheel selects once");
    Wheel(&input_window, combo, -120);
    check(combo->property("currentIndex").toInt() == 23,
          "closed reverse wheel selects once");
    Wheel(&input_window, combo, 40, true);
    check(combo->property("currentIndex").toInt() == 22,
          "closed touchpad pixel scroll selects once");
    capture(&input_window, dark ? "04-combobox-closed-dark.png"
                                : "04-combobox-closed-light.png");
    combo->setProperty("enabled", false);
    Wheel(&input_window, combo, 120);
    check(combo->property("currentIndex").toInt() == 22,
          "disabled ComboBox ignores wheel");
    combo->setProperty("enabled", true);
    input_window.contentItem()->forceActiveFocus();
    Pointer(&input_window, input_window.contentItem(), false);
    Wheel(&input_window, input_window.contentItem(), 120);
    check(combo->property("currentIndex").toInt() == 22,
          "wheel outside unfocused ComboBox does not change selection");
    combo->forceActiveFocus();
    Key(&input_window, Qt::Key_Return);
    check(popup->property("visible").toBool(), "Enter opens popup");
    const auto rows = [list]() {
      QList<QQuickItem*> result;
      std::function<void(QQuickItem*)> collect = [&](QQuickItem* parent) {
        if (parent->objectName() == QLatin1String("comboItemRow"))
          result.append(parent);
        for (QQuickItem* child : parent->childItems()) collect(child);
      };
      collect(list);
      return result;
    };
    const auto activeRows = [&rows]() {
      QList<int> result;
      for (QQuickItem* row : rows()) {
        if (row->property("active").toBool())
          result.append(row->property("index").toInt());
      }
      std::sort(result.begin(), result.end());
      return result;
    };
    const auto checkedRows = [&rows]() {
      QList<int> result;
      for (QQuickItem* row : rows()) {
        QQuickItem* check = FindItem(row, "comboItemCheck");
        if (check != nullptr && check->isVisible())
          result.append(row->property("index").toInt());
      }
      std::sort(result.begin(), result.end());
      return result;
    };
    const auto activeThemeLayerMatches = [&rows, theme](int index) {
      for (QQuickItem* row : rows()) {
        if (row->property("index").toInt() != index ||
            !row->property("active").toBool())
          continue;
        QQuickItem* layer = FindItem(row, "comboItemActiveLayer");
        return layer != nullptr &&
               layer->property("color").value<QColor>() == theme->accentSoft();
      }
      return false;
    };
    check(combo->property("activeIndex").toInt() == 22 &&
              activeRows() == QList<int>{22} &&
              checkedRows() == QList<int>{22} && activeThemeLayerMatches(22),
          "popup opens with one themed active candidate and an independent "
          "selected check");
    capture(&input_window, dark ? "05-combobox-selected-dark.png"
                                : "05-combobox-selected-light.png");
    Key(&input_window, Qt::Key_Home);
    check(combo->property("activeIndex").toInt() == 0 &&
              activeRows() == QList<int>{0} &&
              combo->property("currentIndex").toInt() == 22,
          "popup Home moves the sole active highlight without changing "
          "selection");
    Key(&input_window, Qt::Key_PageDown);
    check(combo->property("activeIndex").toInt() > 0 &&
              activeRows().size() == 1 &&
              combo->property("currentIndex").toInt() == 22,
          "popup PageDown navigates without committing");
    check(activeRows().size() == 1 &&
              activeRows().first() == combo->property("activeIndex").toInt(),
          "popup keyboard focus uses the same single active highlight");
    capture(&input_window, dark ? "06-combobox-keyboard-dark.png"
                                : "06-combobox-keyboard-light.png");
    Key(&input_window, Qt::Key_End);
    check(combo->property("activeIndex").toInt() == 23 &&
              activeRows() == QList<int>{23} &&
              combo->property("currentIndex").toInt() == 22,
          "popup End moves active highlight to last item only");
    Key(&input_window, Qt::Key_PageUp);
    check(combo->property("activeIndex").toInt() < 23 &&
              activeRows().size() == 1 &&
              combo->property("currentIndex").toInt() == 22,
          "popup PageUp navigates without committing");
    Key(&input_window, Qt::Key_End);
    Key(&input_window, Qt::Key_Up);
    check(combo->property("activeIndex").toInt() == 22 &&
              activeRows() == QList<int>{22},
          "popup Up moves the unified active highlight one item");
    Key(&input_window, Qt::Key_Down);
    Key(&input_window, Qt::Key_Return);
    check(!popup->property("visible").toBool() &&
              combo->property("currentIndex").toInt() == 23,
          "popup Enter commits keyboard choice");
    Key(&input_window, Qt::Key_Return);
    Key(&input_window, Qt::Key_Home);
    Key(&input_window, Qt::Key_Escape);
    check(!popup->property("visible").toBool() &&
              combo->property("currentIndex").toInt() == 23,
          "Escape dismisses without changing selection");
    Pointer(&input_window, combo, true);
    check(popup->property("visible").toBool(), "mouse opens popup");
    Wheel(&input_window, list, 1200);
    check(list->property("contentY").toReal() < 500,
          "wheel scrolls long popup");
    QQuickItem* row = FindItem(list, "comboItemRow");
    check(row != nullptr, "popup has selectable delegates");
    if (row != nullptr) {
      // 虚拟化列表只点视口中的行，不能把未显示的缓存行当作可点击项。
      std::function<QQuickItem*(QQuickItem*)> visible_row =
          [&](QQuickItem* parent) -> QQuickItem* {
        if (parent->objectName() == QLatin1String("comboItemRow")) {
          const QPointF point = parent->mapToItem(list, QPointF(5, 22));
          if (point.y() >= 0 && point.y() < list->height()) return parent;
        }
        for (QQuickItem* child : parent->childItems()) {
          QQuickItem* candidate = visible_row(child);
          if (candidate != nullptr) return candidate;
        }
        return nullptr;
      };
      row = visible_row(list);
      if (row != nullptr) {
        const int expected = row->property("index").toInt();
        Pointer(&input_window, row, false);
        check(combo->property("activeIndex").toInt() == expected &&
                  activeRows() == QList<int>{expected} &&
                  combo->property("currentIndex").toInt() == 23,
              "mouse hover moves the sole active highlight, preserving "
              "committed check");
        Pointer(&input_window, row, true);
        check(!popup->property("visible").toBool() &&
                  combo->property("currentIndex").toInt() == expected,
              "mouse click commits the indicated item");
      } else {
        check(false, "visible popup row found");
      }
    }
    QMetaObject::invokeMethod(popup, "close");
  }
  theme->setDark(false);
  input_window.setColor(theme->background());
  combo->setVisible(false);

  const QVariantMap local_properties = {
      {QStringLiteral("fileNameText"), QStringLiteral("backup-20261029.bak")},
      {QStringLiteral("sizeText"), QStringLiteral("12 KiB")},
      {QStringLiteral("modifiedText"), QStringLiteral("2026-10-29 12:00")},
      {QStringLiteral("recognized"), true},
      {QStringLiteral("formatVersion"), 2},
      {QStringLiteral("entryCountText"), QStringLiteral("3")},
      {QStringLiteral("diagnosticText"), QString()},
      {QStringLiteral("hasPipelineMethods"), true},
      {QStringLiteral("packMethodText"), QStringLiteral("MyPack")},
      {QStringLiteral("compressionMethodText"), QStringLiteral("Huffman")},
      {QStringLiteral("encryptionMethodText"), QStringLiteral("AES")},
      {QStringLiteral("passwordRequired"), true},
      {QStringLiteral("recordKind"), QStringLiteral("incremental")},
      {QStringLiteral("isDelta"), true},
      {QStringLiteral("parentFileName"), QStringLiteral("parent.bak")},
      {QStringLiteral("chainRestorable"), true},
      {QStringLiteral("chainDiagnostic"), QString()},
      {QStringLiteral("originText"), QStringLiteral("Manual")},
      {QStringLiteral("scheduledChangesText"), QString()}};
  QQmlComponent local_component(
      engine, QUrl(QStringLiteral("qrc:/qml/components/BackupRecordCard.qml")));
  std::unique_ptr<QObject> local_object(
      local_component.createWithInitialProperties(local_properties));
  QQuickItem* local_card = qobject_cast<QQuickItem*>(local_object.get());
  check(local_card != nullptr, "local record card created");
  if (local_card != nullptr) {
    local_card->setParentItem(input_window.contentItem());
    local_card->setPosition(QPointF(20, 20));
    local_card->setWidth(880);
    Settle();
    local_card->setHeight(local_card->implicitHeight());
    QQuickItem* local_restore = FindItem(local_card, "backupRecordRestore");
    QQuickItem* local_delete = FindItem(local_card, "backupRecordDelete");
    QQuickItem* local_copy = FindItem(local_card, "backupRecordCopyToCloud");
    check(local_restore != nullptr && local_delete != nullptr &&
              local_copy != nullptr && local_restore->height() > 0 &&
              local_restore->height() == local_delete->height() &&
              local_restore->implicitHeight() == local_copy->implicitHeight(),
          "local record action buttons share one height");
    const qreal collapsed_height = local_card->implicitHeight();
    QQuickItem* details = FindItem(local_card, "backupRecordDetailsButton");
    QQuickItem* pipeline = FindItem(local_card, "backupRecordPipeline");
    check(details != nullptr && pipeline != nullptr && !pipeline->isVisible(),
          "local record starts compact");
    capture(&input_window, "07-local-record-compact.png");
    if (details != nullptr) {
      Pointer(&input_window, details, true);
      check(local_card->property("detailsExpanded").toBool() &&
                pipeline != nullptr && pipeline->isVisible() &&
                local_card->implicitHeight() > collapsed_height,
            "local details expand with mouse");
      local_card->setHeight(local_card->implicitHeight());
      capture(&input_window, "08-local-record-expanded.png");
      details->forceActiveFocus();
      Key(&input_window, Qt::Key_Space);
      check(!local_card->property("detailsExpanded").toBool() &&
                local_card->property("fileNameText").toString() ==
                    QStringLiteral("backup-20261029.bak"),
            "keyboard collapses local details without changing record");
    }
    local_card->setProperty("chainRestorable", false);
    local_card->setProperty("chainDiagnostic",
                            QStringLiteral("Missing parent"));
    Settle();
    QQuickItem* warning = FindItem(local_card, "backupRecordChainBroken");
    QQuickItem* restore = FindItem(local_card, "backupRecordRestore");
    check(warning != nullptr && warning->isVisible() && restore != nullptr &&
              !restore->isEnabled(),
          "compact record retains broken-chain warning and restore guard");
    local_card->setHeight(local_card->implicitHeight());
    capture(&input_window, "09-local-chain-warning.png");
    local_card->setVisible(false);
  } else {
    std::fprintf(stderr, "%s\n", qPrintable(local_component.errorString()));
  }

  QQmlComponent remote_component(
      engine,
      QUrl(QStringLiteral("qrc:/qml/components/RemoteSnapshotCard.qml")));
  std::unique_ptr<QObject> remote_object(remote_component.create());
  QQuickItem* remote_card = qobject_cast<QQuickItem*>(remote_object.get());
  check(remote_card != nullptr, "cloud record card created");
  if (remote_card != nullptr) {
    remote_card->setParentItem(input_window.contentItem());
    remote_card->setPosition(QPointF(20, 20));
    remote_card->setWidth(880);
    remote_card->setProperty("nameText", QStringLiteral("cloud-20261029.bak"));
    remote_card->setProperty("sizeText", QStringLiteral("16 KiB"));
    remote_card->setProperty("kindText", QStringLiteral("完整备份"));
    remote_card->setProperty("kindKey", QStringLiteral("full"));
    remote_card->setProperty("snapshotId", QStringLiteral("test-snapshot"));
    remote_card->setProperty("createdText", QStringLiteral("2026-10-29"));
    Settle();
    remote_card->setHeight(remote_card->implicitHeight());
    QQuickItem* remote_restore =
        FindItem(remote_card, "remoteSnapshotRestoreButton");
    QQuickItem* remote_download =
        FindItem(remote_card, "remoteSnapshotDownloadButton");
    QQuickItem* remote_delete =
        FindItem(remote_card, "remoteSnapshotDeleteButton");
    check(remote_restore != nullptr && remote_download != nullptr &&
              remote_delete != nullptr && remote_restore->height() > 0 &&
              remote_restore->height() == remote_download->height() &&
              remote_restore->height() == remote_delete->height(),
          "remote record action buttons share one height");
    QQuickItem* details = FindItem(remote_card, "remoteSnapshotDetailsButton");
    QQuickItem* time = FindItem(remote_card, "remoteSnapshotTime");
    check(details != nullptr && time != nullptr && !time->isVisible(),
          "cloud record starts compact without duplicate timestamp");
    capture(&input_window, "10-cloud-record-compact.png");
    if (details != nullptr) {
      details->forceActiveFocus();
      Key(&input_window, Qt::Key_Space);
      check(remote_card->property("detailsExpanded").toBool() &&
                time != nullptr && time->isVisible(),
            "cloud details expand with keyboard");
      remote_card->setHeight(remote_card->implicitHeight());
      capture(&input_window, "11-cloud-record-expanded.png");
      Pointer(&input_window, details, true);
      check(!remote_card->property("detailsExpanded").toBool() &&
                remote_card->property("snapshotId").toString() ==
                    QStringLiteral("test-snapshot"),
            "cloud details collapse without changing snapshot");
    }
    remote_card->setVisible(false);
  }
  input_window.hide();

  // 列表顺序属于展示，读取展示后再读磁盘，确认历史没有被写回反转。
  QTemporaryDir fixture;
  const QString schedule_path =
      fixture.path() + QStringLiteral("/schedule.json");
  backupproject::ScheduleStore store(schedule_path.toStdString());
  backupproject::ScheduleDocument document;
  for (int index = 0; index < 6; ++index) {
    backupproject::ScheduleHistoryEntry entry;
    entry.started_at_sec = 100 + index;
    entry.finished_at_sec = 100 + index;
    entry.diagnostic = std::to_string(index);
    document.state.history.push_back(entry);
  }
  std::string error;
  check(store.Save(document, &error), "history fixture saved");
  backup_modern::ScheduleController history_controller(
      schedule_path, fixture.path() + QStringLiteral("/config.json"), nullptr,
      nullptr);
  history_controller.reload();
  const QVariantList history = history_controller.history();
  check(history.size() == 6 &&
            history.front()
                    .toMap()
                    .value(QStringLiteral("diagnostic"))
                    .toString() == QStringLiteral("5") &&
            history.back()
                    .toMap()
                    .value(QStringLiteral("diagnostic"))
                    .toString() == QStringLiteral("0"),
        "history presentation is newest first");
  check(
      !history.isEmpty() && history.front()
                                    .toMap()
                                    .value(QStringLiteral("resultText"))
                                    .toString() == QStringLiteral("备份未完成"),
      "history uses Chinese result text");
  backupproject::ScheduleDocument unchanged;
  check(store.Load(&unchanged, &error) ==
                backupproject::ScheduleLoadStatus::kLoaded &&
            unchanged.state.history.front().diagnostic == "0" &&
            unchanged.state.history.back().diagnostic == "5",
        "history presentation preserves persisted order");

  // 在独立上下文中载入产品页面，使用合成历史，点击实际折叠按钮。
  // 不向当前应用的计划配置或用户历史写入测试数据。
  QQmlContext history_context(engine->rootContext());
  history_context.setContextProperty(QStringLiteral("schedule"),
                                     &history_controller);
  QQmlComponent history_component(
      engine, QUrl(QStringLiteral("qrc:/qml/pages/SchedulePage.qml")));
  std::unique_ptr<QObject> history_object(
      history_component.create(&history_context));
  QQuickItem* history_page = qobject_cast<QQuickItem*>(history_object.get());
  check(history_page != nullptr, "schedule page with isolated history created");
  if (history_page != nullptr) {
    history_page->setParentItem(input_window.contentItem());
    history_page->setWidth(920);
    history_page->setHeight(740);
    input_window.show();
    input_window.requestActivate();
    Settle();
    QObject* repeater = history_page->findChild<QObject*>(
        QStringLiteral("scheduleHistoryList"));
    QQuickItem* toggle = FindItem(history_page, "scheduleHistoryToggle");
    QQuickItem* scroll = FindItem(history_page, "schedulePageScroll");
    QQuickItem* flick =
        scroll == nullptr
            ? nullptr
            : scroll->property("contentItem").value<QQuickItem*>();
    check(repeater != nullptr && repeater->property("count").toInt() == 3,
          "schedule history initially shows latest three");
    const auto reveal_toggle = [&]() {
      if (toggle == nullptr || flick == nullptr) return;
      const qreal delta = toggle->mapToItem(flick, QPointF()).y() - 100;
      flick->setProperty("contentY",
                         flick->property("contentY").toReal() + delta);
      Settle();
    };
    reveal_toggle();
    capture(&input_window, "12-schedule-history-collapsed.png");
    if (toggle != nullptr) {
      Pointer(&input_window, toggle, true);
      check(history_page->property("historyExpanded").toBool() &&
                repeater != nullptr && repeater->property("count").toInt() == 6,
            "mouse expands full available schedule history");
      reveal_toggle();
      capture(&input_window, "13-schedule-history-expanded.png");
      toggle->forceActiveFocus();
      Key(&input_window, Qt::Key_Space);
      check(!history_page->property("historyExpanded").toBool() &&
                repeater != nullptr && repeater->property("count").toInt() == 3,
            "keyboard collapses schedule history");
    } else {
      check(false, "schedule history toggle available");
    }
    history_page->setVisible(false);
    input_window.hide();
  } else {
    std::fprintf(stderr, "%s\n", qPrintable(history_component.errorString()));
  }

  const QString realtime_source = fixture.path() + QStringLiteral("/source");
  const QString realtime_repository =
      fixture.path() + QStringLiteral("/repository");
  QDir().mkpath(realtime_source);
  QDir().mkpath(realtime_repository);
  backupproject::RealtimeConfig realtime_config;
  realtime_config.enabled = true;
  realtime_config.source_path = realtime_source.toStdString();
  realtime_config.retain_count = 10;
  const std::string repository_identity =
      backupproject::RepositoryIdentity(realtime_repository.toStdString());
  bool realtime_ready = true;
  for (int index = 0; index < 5; ++index) {
    QFile file(realtime_source + QStringLiteral("/sample.txt"));
    realtime_ready = file.open(QIODevice::WriteOnly) && realtime_ready;
    file.write(QByteArray::number(index));
    file.close();
    backupproject::RealtimeEventSummary events;
    events.generation = index + 1;
    events.event_count = 1;
    backupproject::RealtimeOutcome outcome;
    realtime_ready = backupproject::RunRealtimeBackupOnce(
                         realtime_config, realtime_repository.toStdString(),
                         repository_identity, events, 1793260800 + index,
                         &outcome, &error) &&
                     realtime_ready;
  }
  check(realtime_ready, "five real isolated realtime snapshots created");
  const QString realtime_app_config =
      fixture.path() + QStringLiteral("/realtime-config.json");
  backupproject::ConfigManager config_store(realtime_app_config.toStdString());
  backupproject::AppConfig app_config;
  app_config.backup_repository_path = realtime_repository.toStdString();
  check(config_store.Save(app_config, &error),
        "isolated realtime repository configuration saved");
  backup_modern::RealtimeController realtime_history(
      fixture.path() + QStringLiteral("/realtime.json"), realtime_app_config,
      nullptr, nullptr);
  realtime_history.reload();
  check(realtime_history.waitForIdle(10000), "realtime list refresh completed");
  const QVariantList snapshots = realtime_history.snapshots();
  check(snapshots.size() == 5 &&
            snapshots.front()
                    .toMap()
                    .value(QStringLiteral("createdText"))
                    .toString() > snapshots.back()
                                      .toMap()
                                      .value(QStringLiteral("createdText"))
                                      .toString(),
        "realtime snapshots display newest first");
  QQmlContext realtime_context(engine->rootContext());
  realtime_context.setContextProperty(QStringLiteral("realtime"),
                                      &realtime_history);
  QQmlComponent realtime_component(
      engine, QUrl(QStringLiteral("qrc:/qml/pages/RealtimePage.qml")));
  std::unique_ptr<QObject> realtime_object(
      realtime_component.create(&realtime_context));
  QQuickItem* realtime_page = qobject_cast<QQuickItem*>(realtime_object.get());
  check(realtime_page != nullptr, "isolated realtime history page created");
  if (realtime_page != nullptr) {
    realtime_page->setParentItem(input_window.contentItem());
    realtime_page->setWidth(920);
    realtime_page->setHeight(740);
    input_window.show();
    input_window.requestActivate();
    Settle();
    QObject* repeater = realtime_page->findChild<QObject*>(
        QStringLiteral("realtimeSnapshotList"));
    QQuickItem* toggle = FindItem(realtime_page, "realtimeHistoryToggle");
    QQuickItem* scroll = FindItem(realtime_page, "realtimePageScroll");
    QQuickItem* flick =
        scroll == nullptr
            ? nullptr
            : scroll->property("contentItem").value<QQuickItem*>();
    check(repeater != nullptr && repeater->property("count").toInt() == 3,
          "realtime history initially shows latest three");
    if (toggle != nullptr && flick != nullptr) {
      flick->setProperty("contentY",
                         toggle->mapToItem(flick, QPointF()).y() - 100);
      Settle();
      capture(&input_window, "14-realtime-history-collapsed.png");
      Pointer(&input_window, toggle, true);
      check(realtime_page->property("historyExpanded").toBool() &&
                repeater != nullptr && repeater->property("count").toInt() == 5,
            "mouse expands realtime history");
      capture(&input_window, "15-realtime-history-expanded.png");
      toggle->forceActiveFocus();
      Key(&input_window, Qt::Key_Space);
      check(!realtime_page->property("historyExpanded").toBool() &&
                repeater != nullptr && repeater->property("count").toInt() == 3,
            "keyboard collapses realtime history");
    } else {
      check(false, "realtime history toggle available");
    }
    realtime_page->setVisible(false);
    input_window.hide();
  } else {
    std::fprintf(stderr, "%s\n", qPrintable(realtime_component.errorString()));
  }

  window->setProperty("currentPageKey", QStringLiteral("management"));
  window->requestActivate();
  QQuickItem* management =
      window->findChild<QQuickItem*>(QStringLiteral("managementPage"));
  if (management != nullptr) management->setProperty("currentTab", "local");
  for (int width : {1100, 1920}) {
    window->setWidth(width);
    Settle();
    QQuickItem* title = FindItem(window->contentItem(), "managementTitle");
    QQuickItem* column =
        FindItem(window->contentItem(), "managementLocalColumn");
    check(title != nullptr && column != nullptr &&
              std::abs(title->mapToScene(QPointF()).x() -
                       column->mapToScene(QPointF()).x()) < 1.0,
          "management title aligns with local content at window width");
    capture(window, width == 1100 ? "16-navigation-local-narrow.png"
                                  : "17-navigation-local-wide.png");
    if (management != nullptr) management->setProperty("currentTab", "cloud");
    Settle();
    QQuickItem* remote_column =
        FindItem(window->contentItem(), "remoteContentColumn");
    check(title != nullptr && remote_column != nullptr &&
              std::abs(title->mapToScene(QPointF()).x() -
                       remote_column->mapToScene(QPointF()).x()) < 1.0,
          "management title aligns with cloud content at window width");
    capture(window, width == 1100 ? "18-navigation-cloud-narrow.png"
                                  : "19-navigation-cloud-wide.png");
    if (management != nullptr) management->setProperty("currentTab", "local");
  }
  theme->setDark(original_dark);
  std::printf("[ux-acceptance] passed=%d failed=%d\n", passed, failed);
  return failed == 0 ? 0 : 1;
}

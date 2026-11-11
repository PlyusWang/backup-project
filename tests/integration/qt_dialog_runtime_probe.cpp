// 使用包内 Qt 验证对话框，不链接产品代码。
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>

namespace {
QStringList warnings;
QJsonArray steps;
void messageHandler(QtMsgType type, const QMessageLogContext &,
                    const QString &text) {
  if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
    warnings.append(text);
  std::fprintf(stderr, "[qt-dialog-probe] %s\n", qPrintable(text));
  if (type == QtFatalMsg) std::abort();
}
void require(bool condition, const QString &message) {
  if (!condition) throw std::runtime_error(message.toStdString());
}
void waitFor(const std::function<bool()> &predicate,
             const QString &description) {
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(warnings.isEmpty(), description + ": Qt emitted a warning");
    if (predicate()) return;
    QThread::msleep(10);
  } while (timer.elapsed() < 5000);
  require(false, "Timed out: " + description);
}
QQuickItem *findVisual(QQuickItem *parent,
                       const std::function<bool(QQuickItem *)> &predicate) {
  if (!parent) return nullptr;
  if (parent->isVisible() && predicate(parent)) return parent;
  for (QQuickItem *child : parent->childItems())
    if (QQuickItem *found = findVisual(child, predicate)) return found;
  return nullptr;
}
QQuickItem *findVisual(const std::function<bool(QQuickItem *)> &predicate) {
  for (QWindow *window : QGuiApplication::allWindows())
    if (auto *quickWindow = qobject_cast<QQuickWindow *>(window))
      if (QQuickItem *found = findVisual(quickWindow->contentItem(), predicate))
        return found;
  return nullptr;
}
QQuickItem *namedItem(const QString &name) {
  return findVisual(
      [&](QQuickItem *item) { return item->objectName() == name; });
}
void pointerEvent(QQuickItem *item, QEvent::Type type,
                  Qt::MouseButtons buttons) {
  require(item && item->window() && item->isVisible() && item->isEnabled() &&
              item->width() > 0 && item->height() > 0,
          "Click target is not visible and enabled");
  QQuickWindow *window = item->window();
  const QPointF scene =
      item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
  const QPointF global = window->mapToGlobal(scene.toPoint());
  QMouseEvent event(type, scene, scene, global, Qt::LeftButton, buttons,
                    Qt::NoModifier);
  static ulong timestamp = 1000;
  event.setTimestamp(timestamp += 20);
  QCoreApplication::sendEvent(window, &event);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}
void click(QQuickItem *item) {
  pointerEvent(item, QEvent::MouseButtonPress, Qt::LeftButton);
  pointerEvent(item, QEvent::MouseButtonRelease, Qt::NoButton);
}
void clickButton(const QString &label) {
  QQuickItem *button = nullptr;
  waitFor(
      [&] {
        button = findVisual([&](QQuickItem *item) {
          return item->property("text").toString() == label &&
                 item->isEnabled() &&
                 item->metaObject()->indexOfSignal("clicked()") >= 0;
        });
        return button;
      },
      "button " + label);
  click(button);
}
QJsonArray loadedQtObjects(const QString &appDir) {
  QFile maps("/proc/self/maps");
  require(maps.open(QIODevice::ReadOnly), "Cannot read process maps");
  QSet<QString> objects;
  for (const QByteArray &line : maps.readAll().split('\n')) {
    const int offset = line.indexOf('/');
    if (offset < 0) continue;
    const QString path = QString::fromLocal8Bit(line.mid(offset)).trimmed();
    if (!path.contains(".so") ||
        !(path.contains("/libQt6") || path.contains("/qt6/") ||
          path.contains("/qml/")))
      continue;
    const QString canonical = QFileInfo(path).canonicalFilePath();
    require(!canonical.isEmpty() && canonical.startsWith(appDir + '/'),
            "Qt runtime escaped AppDir: " + path);
    objects.insert(canonical);
  }
  QStringList ordered(objects.begin(), objects.end());
  ordered.sort();
  require(!ordered.isEmpty(), "No loaded Qt runtime objects recorded");
  return QJsonArray::fromStringList(ordered);
}
void snapshot(QQuickWindow *window, const QString &directory,
              const QString &name) {
  if (directory.isEmpty()) return;
  const QImage image = window->grabWindow();
  require(!image.isNull(), "Rendered window capture is empty: " + name);
  require(image.save(directory + '/' + name + ".png"),
          "Cannot save snapshot: " + name);
}
}  // namespace

int main(int argc, char **argv) {
  QString appDir, qmlRoot, evidenceDir;
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) return 2;
    const QString option = QString::fromLocal8Bit(argv[index]);
    const QString value = QString::fromLocal8Bit(argv[index + 1]);
    if (option == "--appdir")
      appDir = QFileInfo(value).canonicalFilePath();
    else if (option == "--qml-root")
      qmlRoot = QFileInfo(value).canonicalFilePath();
    else if (option == "--evidence-dir")
      evidenceDir = value;
    else
      return 2;
  }
  if (appDir.isEmpty()) return 2;
  if (qmlRoot.isEmpty()) qmlRoot = appDir + "/usr/qml";
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QCoreApplication::setLibraryPaths({appDir + "/usr/plugins"});
  qputenv("QT_PLUGIN_PATH", (appDir + "/usr/plugins").toLocal8Bit());
  qputenv("QT_QPA_PLATFORM_PLUGIN_PATH",
          (appDir + "/usr/plugins/platforms").toLocal8Bit());
  qputenv("QT_QPA_PLATFORMTHEME", "");
  qInstallMessageHandler(messageHandler);
  QGuiApplication app(argc, argv);
  QCoreApplication::setLibraryPaths({appDir + "/usr/plugins"});
  QQuickStyle::setStyle("Basic");
  QJsonObject report{{"appdir", appDir},
                     {"qml_root", qmlRoot},
                     {"platform", QGuiApplication::platformName()},
                     {"pid", QCoreApplication::applicationPid()},
                     {"native_dialogs_disabled", true},
                     {"product_code_linked", false}};
  try {
    QTemporaryDir fixture;
    require(fixture.isValid(), "Cannot create dialog fixture");
    const QString selectedFile = fixture.path() + "/selected-file.probe";
    const QString selectedFolder = fixture.path() + "/selected-folder";
    require(QDir().mkpath(selectedFolder + "/inside"),
            "Cannot create folder fixture");
    QFile file(selectedFile);
    require(file.open(QIODevice::WriteOnly), "Cannot create file fixture");
    require(file.write("dialog runtime fixture\n") > 0,
            "Cannot write file fixture");
    file.close();
    if (!evidenceDir.isEmpty())
      require(QDir().mkpath(evidenceDir), "Cannot create evidence directory");
    QQmlApplicationEngine engine;
    // 排除宿主机 QML 路径；qrc 提供包内对话框实现。
    engine.setImportPathList(
        {qmlRoot, "qrc:/qt/qml", "qrc:/qt-project.org/imports"});
    engine.rootContext()->setContextProperty(
        "fixtureUrl", QUrl::fromLocalFile(fixture.path()));
    engine.loadData(R"QML(
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
ApplicationWindow {
    id: probe
    objectName: "dialogProbeWindow"
    width: 900
    height: 700
    visible: true
    title: "Packaged Qt dialog runtime probe"
    color: "#efefef"
    property int fileAccepted: 0
    property int fileRejected: 0
    property int folderAccepted: 0
    property int folderRejected: 0
    FileDialog {
        objectName: "probeFileDialog"
        title: "Probe file selection"
        fileMode: FileDialog.OpenFile
        options: FileDialog.DontUseNativeDialog
        currentFolder: fixtureUrl
        nameFilters: ["Probe files (*.probe)"]
        acceptLabel: "Accept probe file"
        rejectLabel: "Cancel probe file"
        onAccepted: probe.fileAccepted += 1
        onRejected: probe.fileRejected += 1
    }
    FolderDialog {
        objectName: "probeFolderDialog"
        title: "Probe folder selection"
        options: FolderDialog.DontUseNativeDialog
        currentFolder: fixtureUrl
        acceptLabel: "Accept probe folder"
        rejectLabel: "Cancel probe folder"
        onAccepted: probe.folderAccepted += 1
        onRejected: probe.folderRejected += 1
    }
}
)QML",
                    QUrl("qrc:/qt-dialog-runtime-probe.qml"));
    require(!engine.rootObjects().isEmpty(), "Probe QML failed to load");
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    require(window, "Probe root is not a QQuickWindow");
    waitFor([&] { return window->isVisible() && window->isExposed(); },
            "probe window exposed");
    QObject *fileDialog = window->findChild<QObject *>("probeFileDialog");
    QObject *folderDialog = window->findChild<QObject *>("probeFolderDialog");
    require(fileDialog && folderDialog, "Dialog objects not found");
    const auto open = [&](QObject *dialog, const QString &kind) {
      require(QMetaObject::invokeMethod(dialog, "open", Qt::DirectConnection),
              "Cannot open " + kind);
      waitFor(
          [&] {
            QQuickItem *list = namedItem(kind + "DialogListView");
            return dialog->property("visible").toBool() && list &&
                   list->property("count").toInt() > 0 && list->width() > 0;
          },
          kind + " non-native list populated");
      snapshot(window, evidenceDir, kind + "-open");
      steps.append(kind + ".opened_populated");
    };
    open(fileDialog, "file");
    clickButton("Cancel probe file");
    waitFor(
        [&] {
          return !fileDialog->property("visible").toBool() &&
                 window->property("fileRejected").toInt() == 1;
        },
        "file cancellation");
    steps.append("file.cancelled");
    open(fileDialog, "file");
    QQuickItem *fileDelegate = nullptr;
    waitFor(
        [&] {
          fileDelegate = findVisual([&](QQuickItem *item) {
            return item->objectName().startsWith("fileDialogDelegate") &&
                   item->property("file").toUrl().toLocalFile() == selectedFile;
          });
          return fileDelegate;
        },
        "file delegate");
    click(fileDelegate);
    waitFor(
        [&] {
          return fileDialog->property("selectedFile").toUrl().toLocalFile() ==
                     selectedFile ||
                 fileDialog->property("currentFile").toUrl().toLocalFile() ==
                     selectedFile;
        },
        "file selected by pointer");
    snapshot(window, evidenceDir, "file-selected");
    clickButton("Accept probe file");
    waitFor(
        [&] {
          return !fileDialog->property("visible").toBool() &&
                 window->property("fileAccepted").toInt() == 1;
        },
        "file acceptance");
    require(fileDialog->property("selectedFile").toUrl().toLocalFile() ==
                selectedFile,
            "Accepted file differs from pointer selection");
    steps.append("file.selected_and_accepted");
    open(folderDialog, "folder");
    clickButton("Cancel probe folder");
    waitFor(
        [&] {
          return !folderDialog->property("visible").toBool() &&
                 window->property("folderRejected").toInt() == 1;
        },
        "folder cancellation");
    steps.append("folder.cancelled");
    open(folderDialog, "folder");
    QQuickItem *folderDelegate = nullptr;
    waitFor(
        [&] {
          folderDelegate = findVisual([&](QQuickItem *item) {
            return item->objectName().startsWith("folderDialogDelegate") &&
                   item->property("file").toUrl().toLocalFile() ==
                       selectedFolder;
          });
          return folderDelegate;
        },
        "folder delegate");
    click(folderDelegate);
    waitFor(
        [&] {
          return folderDialog->property("selectedFolder")
                     .toUrl()
                     .toLocalFile() == selectedFolder;
        },
        "folder selected by pointer");
    snapshot(window, evidenceDir, "folder-selected");
    clickButton("Accept probe folder");
    waitFor(
        [&] {
          return !folderDialog->property("visible").toBool() &&
                 window->property("folderAccepted").toInt() == 1;
        },
        "folder acceptance");
    require(folderDialog->property("selectedFolder").toUrl().toLocalFile() ==
                selectedFolder,
            "Accepted folder differs from pointer selection");
    steps.append("folder.selected_and_accepted");
    require(window->isVisible(), "Probe window closed during dialog lifecycle");
    require(warnings.isEmpty(), "Qt warnings occurred");
    report.insert("loaded_qt_objects", loadedQtObjects(appDir));
    report.insert("import_paths",
                  QJsonArray::fromStringList(engine.importPathList()));
    report.insert("file_accepted", window->property("fileAccepted").toInt());
    report.insert("file_rejected", window->property("fileRejected").toInt());
    report.insert("folder_accepted",
                  window->property("folderAccepted").toInt());
    report.insert("folder_rejected",
                  window->property("folderRejected").toInt());
    report.insert("passed", true);
  } catch (const std::exception &error) {
    report.insert("passed", false);
    report.insert("error", QString::fromLocal8Bit(error.what()));
  }
  report.insert("steps", steps);
  report.insert("qt_warnings", QJsonArray::fromStringList(warnings));
  const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
  std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
  return report.value("passed").toBool() ? 0 : 1;
}

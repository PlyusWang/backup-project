#!/usr/bin/env bash
# 非原生文件、文件夹对话框共用 FolderListModel。
# QML_MODULES_PATHS 可指定用户目录中提取的同版本 Qt QML 包。
qt_dialog_runtime_preflight() {
  local qt_qml roots root
  qt_qml="$(qtpaths6 --query QT_INSTALL_QML 2>/dev/null ||
    qmake6 -query QT_INSTALL_QML 2>/dev/null || true)"
  roots="${QML_MODULES_PATHS:-}${QML_MODULES_PATHS:+:}$qt_qml"
  local -a import_roots=()
  IFS=':' read -r -a import_roots <<< "$roots"
  for root in "${import_roots[@]}"; do
    [ -n "$root" ] || continue
    root="$root/Qt/labs/folderlistmodel"
    if [ -s "$root/qmldir" ] &&
       [ -s "$root/libqmlfolderlistmodelplugin.so" ]; then
      return 0
    fi
  done
  printf '%s\n' \
    '[release] ERROR: 缺少 Qt.labs.folderlistmodel。' \
    '请安装与构建 Qt 版本匹配的 qml6-module-qt-labs-folderlistmodel，' \
    '或通过 QML_MODULES_PATHS 指定已提取的模块目录。' >&2
  return 1

}

qt_dialog_runtime_verify() {
  local runtime="$1" rel
  for rel in \
    qml/Qt/labs/folderlistmodel/qmldir \
    qml/Qt/labs/folderlistmodel/libqmlfolderlistmodelplugin.so \
    lib/libQt6LabsFolderListModel.so.6 \
    qml/QtQuick/Dialogs/qmldir \
    qml/QtQuick/Dialogs/quickimpl/qmldir \
    qml/QtQuick/Controls/qmldir \
    qml/QtQuick/Controls/impl/qmldir \
    qml/QtQuick/Controls/Basic/qmldir \
    qml/QtQuick/Controls/Basic/impl/qmldir \
    qml/QtQuick/Layouts/qmldir \
    qml/QtQuick/Templates/qmldir; do
    [ -s "$runtime/$rel" ] || {
      printf '[release] ERROR: Qt 对话框运行文件缺失：%s\n' \
        "$runtime/$rel" >&2
      return 1
    }
  done
}

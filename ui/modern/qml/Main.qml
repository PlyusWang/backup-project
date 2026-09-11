// Main.qml
//
// 窗口骨架：自绘标题栏 + 左侧导航 + 右侧页面区。
// 页面切换只做 150ms 淡入，不做滑动/弹簧——现代感来自一致与克制，不是动画数量。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window

import "components"
import "pages"

ApplicationWindow {
    id: root

    width: 1180
    height: 760
    minimumWidth: 960
    minimumHeight: 620
    visible: true
    title: "备份工具"
    color: theme.background

    // --native-frame 时退回 GNOME 原生标题栏：Wayland 下自绘标题栏万一表现不稳，
    // 一条命令就能切回系统行为，不需要改 QML，也不需要复杂的 hack。
    flags: useNativeFrame ? Qt.Window : (Qt.Window | Qt.FramelessWindowHint)
    readonly property bool maximized: root.visibility === Window.Maximized

    // 当前页面索引；--screenshot 模式会直接从 C++ 改这个属性逐页抓图。
    property int currentPage: 0

    // 侧栏底部主题按钮要显示“当前是哪套主题”，所以直接读 theme.dark。
    function toggleTheme() { theme.toggle() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------- 自绘标题栏 ----------
        Rectangle {
            id: titleBar
            visible: !useNativeFrame
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: theme.sidebar

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: theme.border
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 6
                spacing: 8

                AppIcon { name: "app"; size: 18; color: theme.accent }
                Text {
                    text: "备份工具"
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: theme.textPrimary
                }
                Item { Layout.fillWidth: true }

                AppButton {
                    iconName: "minimize"; variant: "flat"; implicitWidth: 40; implicitHeight: 30
                    onClicked: root.showMinimized()
                }
                AppButton {
                    iconName: root.maximized ? "restore-window" : "maximize"
                    variant: "flat"; implicitWidth: 40; implicitHeight: 30
                    onClicked: root.maximized ? root.showNormal() : root.showMaximized()
                }
                AppButton {
                    iconName: "close"; variant: "flat"; implicitWidth: 40; implicitHeight: 30
                    onClicked: root.close()
                }
            }

            // 拖动窗口交给合成器（xdg_toplevel.move），不自己算鼠标位移：
            // 那种旧写法在 Wayland 上经常和合成器打架。
            DragHandler {
                target: null
                onActiveChanged: if (active) root.startSystemMove()
            }
            TapHandler {
                onDoubleTapped: root.maximized ? root.showNormal() : root.showMaximized()
            }
        }

        // ---------- 主体 ----------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 侧栏
            Rectangle {
                Layout.preferredWidth: 228
                Layout.fillHeight: true
                color: theme.sidebar

                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: theme.border
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4

                    Text {
                        text: "备份工具"
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        color: theme.textPrimary
                        Layout.leftMargin: 8
                        Layout.topMargin: 6
                        Layout.bottomMargin: 10
                    }

                    NavItem {
                        Layout.fillWidth: true
                        text: "首页"
                        iconName: "home"
                        checked: root.currentPage === 0
                        onClicked: root.currentPage = 0
                    }
                    NavItem {
                        Layout.fillWidth: true
                        text: "备份"
                        iconName: "backup"
                        checked: root.currentPage === 1
                        onClicked: root.currentPage = 1
                    }
                    NavItem {
                        Layout.fillWidth: true
                        text: "恢复"
                        iconName: "restore"
                        checked: root.currentPage === 2
                        onClicked: root.currentPage = 2
                    }

                    Item { Layout.fillHeight: true }

                    AppButton {
                        Layout.fillWidth: true
                        variant: "flat"
                        iconName: theme.dark ? "moon" : "sun"
                        text: theme.dark ? "深色" : "浅色"
                        onClicked: root.toggleTheme()
                    }
                }
            }

            // 页面区：三页叠在同一位置，切换时当前页淡入。
            StackLayout {
                id: pageStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.currentPage

                HomePage {
                    opacity: root.currentPage === 0 ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    onNavigateTo: function (pageIndex) { root.currentPage = pageIndex }
                }

                OperationPage {
                    mode: "backup"
                    opacity: root.currentPage === 1 ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                }

                OperationPage {
                    mode: "restore"
                    opacity: root.currentPage === 2 ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                }
            }
        }
    }

    // ---------- 无边框窗口的缩放热区 ----------
    // 同样交给 startSystemResize，由合成器处理，避免在 Wayland 上自己算尺寸。
    // 只做四条边不做四角：角落会被相邻边的热区覆盖，足够用且代码更短。
    Item {
        anchors.fill: parent
        visible: !useNativeFrame
        z: 100

        MouseArea {
            width: 6; height: parent.height; anchors.left: parent.left
            cursorShape: Qt.SizeHorCursor
            onPressed: root.startSystemResize(Qt.LeftEdge)
        }
        MouseArea {
            width: 6; height: parent.height; anchors.right: parent.right
            cursorShape: Qt.SizeHorCursor
            onPressed: root.startSystemResize(Qt.RightEdge)
        }
        MouseArea {
            width: parent.width; height: 6; anchors.top: parent.top
            cursorShape: Qt.SizeVerCursor
            onPressed: root.startSystemResize(Qt.TopEdge)
        }
        MouseArea {
            width: parent.width; height: 6; anchors.bottom: parent.bottom
            cursorShape: Qt.SizeVerCursor
            onPressed: root.startSystemResize(Qt.BottomEdge)
        }
    }
}

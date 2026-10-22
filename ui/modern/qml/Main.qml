// Main.qml
//
// 窗口骨架：自绘标题栏 + 左侧导航 + 右侧页面区。
// 页面切换只做 150ms 淡入，不做滑动/弹簧——现代感来自一致与克制，不是动画数量。

// 导航使用**语义 key**，不再使用会随插入顺序变化而失效的数字下标。
// pageKeys 的顺序只决定 StackLayout 的子项顺序；任何跳转都按 key 走，
// 页面插入 / 删除不会让某个按钮静默跳到别的页面。
// C++ 自检通过 currentPageKey 切页，因此同样不依赖下标。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window

import "components"
import "pages"

ApplicationWindow {
    id: root

    readonly property var pageKeys: ["home", "quick", "scheduled",
                                    "realtime", "management"]
    readonly property var pageTitles: ({
        "home": "首页",
        "quick": "快速备份",
        "scheduled": "定时备份",
        "realtime": "实时备份",
        "management": "备份管理"
    })
    readonly property var pageIcons: ({
        "home": "home",
        "quick": "backup",
        "scheduled": "clock",
        "realtime": "refresh",
        "management": "folder"
    })

    // 当前页的稳定标识。空串永远不会出现：goTo() 只接受 pageKeys 里的值。
    property string currentPageKey: "home"
    // 派生下标只给 StackLayout 用；它不是导航接口。
    readonly property int currentPage:
        Math.max(0, pageKeys.indexOf(currentPageKey))

    width: 1180
    height: 760
    minimumWidth: 960
    minimumHeight: 620
    visible: true
    title: "备份工具"
    color: theme.background

    // 切换页面只改 key：非法 key 被忽略，不会把窗口切成空白。
    function goTo(key) {
        if (pageKeys.indexOf(key) >= 0)
            root.currentPageKey = key
    }

    // --native-frame 时退回 GNOME 原生标题栏。
    flags: useNativeFrame ? Qt.Window : (Qt.Window | Qt.FramelessWindowHint)
    readonly property bool maximized: root.visibility === Window.Maximized

    // 核心没有取消能力，所以任务进行中一律不允许关窗。
    // 三个 writer 缺一不可：controller.busy（手动备份 / 恢复）、
    // schedule.libraryBusy、realtime.libraryBusy，以及远程传输。
    onClosing: function (close) {
        if (controller.busy || schedule.libraryBusy || realtime.libraryBusy
                || remote.busy) {
            close.accepted = false
            busyCloseDialog.open()
        }
    }

    // 页面产生的临时提示只属于该页面：离开即消费，回来不自动复现。
    // 正在跑的操作用 busy 挡住，它属于全局运行状态。
    function dismissTransientMessage(key) {
        if (key === "quick")
            controller.dismissPageStatus("backup")
        else if (key === "management") {
            controller.dismissPageStatus("management")
            controller.dismissPageStatus("settings")
            remote.clearStatus()
        } else if (key === "scheduled") {
            schedule.clearStatus()
        } else if (key === "realtime") {
            realtime.clearStatus()
        }
    }

    property string previousPageKey: "home"
    onCurrentPageKeyChanged: {
        dismissTransientMessage(previousPageKey)
        previousPageKey = currentPageKey
    }

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
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    color: theme.textPrimary
                }
                Item { Layout.fillWidth: true }

                AppButton {
                    iconName: "minimize"; variant: "flat";
                    implicitWidth: 40; implicitHeight: 30
                    onClicked: root.showMinimized()
                }
                AppButton {
                    iconName: root.maximized ? "restore-window" : "maximize"
                    variant: "flat"; implicitWidth: 40; implicitHeight: 30
                    onClicked: root.maximized
                        ? root.showNormal()
                        : root.showMaximized()
                }
                AppButton {
                    iconName: "close"; variant: "flat";
                    implicitWidth: 40; implicitHeight: 30
                    onClicked: root.close()
                }
            }

            DragHandler {
                target: null
                onActiveChanged: if (active) root.startSystemMove()
            }
            TapHandler {
                onDoubleTapped: root.maximized
                    ? root.showNormal()
                    : root.showMaximized()
            }
        }

        // ---------- 主体 ----------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

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
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                        color: theme.textPrimary
                        Layout.leftMargin: 8
                        Layout.topMargin: 6
                        Layout.bottomMargin: 10
                    }

                    Repeater {
                        model: root.pageKeys

                        delegate: NavItem {
                            required property string modelData
                            objectName: "nav_" + modelData
                            Layout.fillWidth: true
                            text: root.pageTitles[modelData]
                            iconName: root.pageIcons[modelData]
                            checked: root.currentPageKey === modelData
                            onClicked: root.goTo(modelData)
                        }
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

            // 五页同时存在、只切 visible：页面不重建，各页的临时状态
            // （比如正在输入的口令）会跨页保留；离开时清掉的是提示，
            // 不是用户输入。子项顺序 = pageKeys 顺序。
            StackLayout {
                id: pageStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.currentPage

                HomePage {
                    onNavigateTo: function (key) { root.goTo(key) }
                }

                BackupPage {
                    onOpenSettings: {
                        root.goTo("management")
                        managementPage.showTab("local", "settings")
                    }
                }

                SchedulePage {
                }

                RealtimePage {
                }

                BackupManagementPage {
                    id: managementPage
                    onOpenSettings: managementPage.showTab("local", "settings")
                }
            }
        }
    }

    // ---------- 无边框窗口的缩放热区 ----------
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

    // 任务进行中尝试关窗时的提示。只给“知道了”：核心没有安全取消。
    Dialog {
        id: busyCloseDialog
        objectName: "busyCloseDialog"
        anchors.centerIn: parent
        modal: true
        padding: 18

        background: Rectangle {
            color: theme.surfaceElevated
            border.width: 1
            border.color: theme.border
            radius: 10
        }

        contentItem: ColumnLayout {
            spacing: 12

            Text {
                text: "操作正在进行"
                color: theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Text {
                Layout.preferredWidth: 300
                text: "备份、恢复或传输尚未完成。为避免留下不完整结果，"
                    + "请等待当前操作结束后再退出。"
                color: theme.textSecondary
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            AppButton {
                Layout.alignment: Qt.AlignRight
                text: "知道了"
                onClicked: busyCloseDialog.close()
            }
        }
    }

    // 统一错误对话框：用户主动操作失败时才弹，一个错误只有这一条主要反馈。
    // 页面内的状态条保持简短中文；完整技术细节折叠在这里。
    UserErrorDialog {
        id: userErrorDialog
        objectName: "userErrorDialog"
    }

    Connections {
        target: controller
        function onUserError(title, summary, suggestion, technical) {
            userErrorDialog.showError(title, summary, suggestion, technical)
        }
    }
    Connections {
        target: schedule
        function onUserError(title, summary, suggestion, technical) {
            userErrorDialog.showError(title, summary, suggestion, technical)
        }
    }
    Connections {
        target: realtime
        function onUserError(title, summary, suggestion, technical) {
            userErrorDialog.showError(title, summary, suggestion, technical)
        }
    }
    Connections {
        target: remote
        function onUserError(title, summary, suggestion, technical) {
            userErrorDialog.showError(title, summary, suggestion, technical)
        }
        // 仓库复制成功后，两边列表都要刷新：复制把数据从一个仓库搬到了另一个，
        // 只刷新当前可见的那一边会让另一边显示过期内容。
        function onOperationFinished(kind, succeeded) {
            if (!succeeded)
                return
            if (kind === "copy-to-cloud" || kind === "copy-to-local") {
                controller.refreshBackups()
                remote.refreshList()
            }
        }
    }
}

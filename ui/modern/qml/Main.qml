// Main.qml
//
// 窗口骨架：自绘标题栏 + 左侧导航 + 右侧页面区。
// 页面切换只做 150ms 淡入，不做滑动/弹簧——现代感来自一致与克制，不是动画数量。

// 这一层只做窗口外壳：页面数据、忙闲标志与错误文案全部来自 C++ 注册的
// 上下文对象（controller / schedule / realtime / remote / theme），QML 只
// 持有"当前在哪一页"以及由它派生的可见性。
//
// useNativeFrame 也是 C++ 传进来的（--native-frame），它同时决定自绘标题栏
// 与四边缩放热区的可见性；几处绑定必须一致，否则会出现"有热区没标题栏"
// 这种自相矛盾的窗口。
//
// 忙碌判定横跨四个对象：手动备份与恢复在 controller，计划与实时各自有
// libraryBusy，远程在 remote；onClosing 是它们唯一的汇总点。
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

    // --native-frame 时退回 GNOME 原生标题栏：
    // Wayland 下自绘标题栏万一表现不稳，一条命令就能切回系统行为，
    // 不需要改 QML，也不需要复杂的 hack。
    flags: useNativeFrame ? Qt.Window : (Qt.Window | Qt.FramelessWindowHint)
    // 最大化状态从 visibility 反推，而不是自己维护一个 bool：窗口管理器
    // 也能最大化（双击标题栏、快捷键），自维护的标志会和现实脱节。
    // 无边框时自绘标题栏接管最小化 / 最大化 / 关闭，拖动与双击最大化交给
    // 合成器；切换原生标题栏只是这一个窗口标志的差别，不需要平台代码。
    readonly property bool maximized: root.visibility === Window.Maximized

    // 核心没有取消能力，所以任务进行中一律不允许关窗，免得让用户以为
    // “关掉窗口 = 安全取消”。自绘的 ×、Alt+F4、系统菜单、窗口管理器走的
    // 都是这一个信号，所以守卫放在这里，而不是只放在那个按钮里。
    //
    // 三个 writer 缺一不可：手动备份 / 恢复落在 controller.busy，而计划评估与
    // 实时触发都跑在 QtConcurrent 上，真正落盘的那一位是各自的 libraryBusy ——
    // 只看 controller.busy 会漏掉"实时备份正在写归档时 Alt+F4 能关掉窗口"。
    // 这是同一条全局不变式（持久状态操作进行中不得关窗），不是三种特例。
    onClosing: function (close) {
        if (controller.busy || schedule.libraryBusy || realtime.libraryBusy
                || remote.busy) {
            close.accepted = false
            busyCloseDialog.open()
        }
    }

    // 页面编号（跨语言契约：C++ 的 --screenshot 与自动化测试按号切页）：
    //   0 首页 / 1 备份 / 2 自动备份 / 3 备份管理
    //   4 设置 / 5 实时备份 / 6 远程备份
    // 下面 StackLayout 的子项顺序必须与这套编号一致；dismissTransientMessage
    // 里的分支也按同一套编号分发状态清理。
    // 当前页面索引；--screenshot 模式会直接从 C++ 改这个属性逐页抓图。
    property int currentPage: 0
    // 上一次停留的页面。用属性记住"离开的是哪一页"，而不是把清理逻辑塞进每个
    // 导航入口：NavItem、首页卡片、页面内的跳转、--screenshot 从 C++ 改属性，
    // 走的是同一条路。
    property int previousPage: 0

    // 页面产生的临时提示只属于该页面：离开即消费，回来不自动复现。
    // 正在跑的操作用 busy 挡住（控制器里也挡一次），它属于全局运行状态。
    function dismissTransientMessage(pageIndex) {
        if (pageIndex === 1)
            controller.dismissPageStatus("backup")
        else if (pageIndex === 3)
            controller.dismissPageStatus("management")
        else if (pageIndex === 4)
            controller.dismissPageStatus("settings")
        else if (pageIndex === 2)
            schedule.clearStatus()
        else if (pageIndex === 5)
            realtime.clearStatus()
        else if (pageIndex === 6)
            remote.clearStatus()
    }

    onCurrentPageChanged: {
        dismissTransientMessage(previousPage)
        previousPage = currentPage
    }

    // 侧栏底部主题按钮要显示“当前是哪套主题”，所以直接读 theme.dark。
    function toggleTheme() { theme.toggle() }

    // 布局：标题栏 + 主体（侧栏 + 页面区）两行一列。侧栏宽度写死 228 是
    // 刻意的：它不随窗口宽度伸缩，窗口变宽只影响页面区，视觉上更稳定。
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
                    // 和系统关闭走同一条路径：任务进行中会被 onClosing 拦下。
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
                        font.pixelSize: 18
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
                    // 定时备份是一个独立的业务页面，不是备份页上的一个开关：
                    // 它有自己的源目录、周期、保留策略与运行历史。
                    NavItem {
                        Layout.fillWidth: true
                        text: "自动备份"
                        iconName: "clock"
                        checked: root.currentPage === 2
                        onClicked: root.currentPage = 2
                    }
                    // 实时备份同样是独立页面：它有自己的源目录、合并窗口与
                    // 实时快照列表，不是"备份"页上的一个开关。
                    NavItem {
                        Layout.fillWidth: true
                        text: "实时备份"
                        iconName: "refresh"
                        checked: root.currentPage === 5
                        onClicked: root.currentPage = 5
                    }
                    // 恢复不再是独立页面：它是"备份管理"里的一个动作，
                    // 与课程设计里的"备份 / 管理备份数据 / 备份设置"结构一致。
                    NavItem {
                        Layout.fillWidth: true
                        text: "备份管理"
                        iconName: "folder"
                        checked: root.currentPage === 3
                        onClicked: root.currentPage = 3
                    }
                    // 远程备份同样是一个独立业务页面：它有自己的服务器连接、
                    // 云端备份列表与上传 / 下载 / 删除动作，不是"备份管理"里的
                    // 一个选项。
                    NavItem {
                        objectName: "remoteNavItem"
                        Layout.fillWidth: true
                        text: "远程备份"
                        iconName: "cloud"
                        checked: root.currentPage === 6
                        onClicked: root.currentPage = 6
                    }
                    NavItem {
                        Layout.fillWidth: true
                        text: "设置"
                        iconName: "settings"
                        checked: root.currentPage === 4
                        onClicked: root.currentPage = 4
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

            // 七页同时存在、只切 currentIndex：页面不重建，各页的临时状态
            // （比如正在输入的口令）会跨页保留；离开时清掉的是提示，
            // 不是用户输入。
            // 子项顺序 = currentPage 编号，新增页面只能追加到末尾。
            // 页面区：七页叠在同一位置，切换时当前页淡入。
            StackLayout {
                id: pageStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.currentPage

                HomePage {
                    opacity: root.currentPage === 0 ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 150; easing.type: Easing.OutCubic
                        }
                    }
                    onNavigateTo: function (pageIndex) {
                        root.currentPage = pageIndex }
                }

                BackupPage {
                    opacity: root.currentPage === 1 ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 150; easing.type: Easing.OutCubic
                        }
                    }
                    // 页面自己不改 root.currentPage，只发意图，跳转由窗口决定。
                    onOpenSettings: root.currentPage = 4
                }

                SchedulePage {
                    opacity: root.currentPage === 2 ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 150; easing.type: Easing.OutCubic
                        }
                    }
                }

                BackupManagementPage {
                    opacity: root.currentPage === 3 ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 150; easing.type: Easing.OutCubic
                        }
                    }
                    onOpenSettings: root.currentPage = 4
                }

                SettingsPage {
                    opacity: root.currentPage === 4 ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 150; easing.type: Easing.OutCubic
                        }
                    }
                }

                RealtimePage {
                    opacity: root.currentPage === 5 ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 150; easing.type: Easing.OutCubic
                        }
                    }
                }

                RemotePage {
                    opacity: root.currentPage === 6 ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 150; easing.type: Easing.OutCubic
                        }
                    }
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
    // 覆盖整窗的缩放热区层，z=100 保证它在页面之上。Item 自身没有事件处理，
    // 只有四条 6px 的 MouseArea 会吃按下事件，中间区域照常透给页面；所以
    // 这四条边不能再加宽，否则会抢掉靠边控件的点击。
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

    // 任务进行中尝试关窗时的提示。只给“知道了”：核心没有安全取消，
    // 所以这里不提供“强制退出 / 取消任务”这种做不到的按钮。
    Dialog {
        id: busyCloseDialog
        objectName: "busyCloseDialog"
        anchors.centerIn: parent
        modal: true
        // 同 restorePasswordDialog / deleteDialog：一套自定义 Dialog 样式，
        // 不应该只有其中一个有内容边距。
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
                text: "备份、恢复或实时/定时备份尚未完成。"
                    + "为避免留下不完整结果，请等待当前操作结束后再退出。"
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
}

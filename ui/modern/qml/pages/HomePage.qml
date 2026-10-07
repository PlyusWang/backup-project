// HomePage.qml
//
// 首页只展示真实存在的东西：入口按钮 + 当前操作状态。
// 不加“备份次数 / 节省空间 / 安全评分”这类当前根本没有的数据。

// 职责边界：本页只负责“把入口摆出来 + 显示全局状态”。它不读配置、不碰备份引擎、
// 不发起任何操作：按钮只发出导航意图（navigateTo），切页由 Main.qml 决定；
// 状态是 controller 的只读投影（statusScope / statusKind / 消息文本等）。
import QtQuick
import QtQuick.Layouts

// 目录导入：AppCard / AppButton / AppIcon / StatusBanner 这些类型名由该目录下的
// 文件导出，路径相对本文件。新增组件不必在这里登记，但目录名不可改。
import "../components"

Item {
    id: page

    // 首页不直接操作窗口，交给 Main.qml 处理跳转，页面之间保持解耦。
    // 页面编号 = Main.qml 里 StackLayout 的子项顺序，是一份跨文件协议：
    //   1 = BackupPage（手动备份）   2 = SchedulePage（自动备份）
    //   3 = BackupManagementPage（备份管理）
    // 新增页面只能追加到 StackLayout 末尾：插在中间会让本页按钮静默跳错页，
    // 而且没有编译期检查。本页不持有页面状态，只发导航意图。
    signal navigateTo(int pageIndex)

    // 整页只有一列，锚在顶部居中而非垂直居中：窗口变高时内容留在上方，
    // 底部留白交给末尾的弹性 Item；变窄时先由 width 绑定减掉左右边距。
    ColumnLayout {
        id: column
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 32
        width: Math.min(parent.width - 64, 900)
        spacing: 18

        Text {
            text: "备份工具"
            font.pixelSize: 30
            font.weight: Font.DemiBold
            color: theme.textPrimary
        }

        Text {
            text: "本地数据备份与恢复"
            font.pixelSize: 17
            color: theme.textSecondary
            Layout.topMargin: -10
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 16

            // 卡片高度由内容决定（AppCard.implicitHeight），三张卡片再用
            // fillHeight 拉平成同一个高度：文字在窄窗口下多折一行时，卡片跟着
            // 变高，而不是把按钮挤出下边框。
            AppCard {
                // objectName 是 GUI 自检（main.cpp --gui-test）的定位锚点：
                // homeCard0..2 与 homeAction0..2 会被逐个抓出来做点击回放和
                // 几何断言（按钮必须落在卡片内，且三个按钮纵坐标一致）。
                // 改名等于删掉这部分覆盖。
                objectName: "homeCard0"
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    // 图标名来自 AppIcon 的 switch 表：拼错只会留一个空位，
                    // 不会有任何报错。
                    AppIcon { name: "backup"; size: 22; color: theme.accent }
                    Text {
                        text: "备份"
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                        color: theme.textPrimary
                    }
                    Text {
                        text: "选择一个目录，自动命名并保存到备份仓库"
                        font.pixelSize: 16
                        color: theme.textSecondary
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                    // 弹性占位把按钮压到卡片底部：三张卡片被 Layout.fillHeight
                    // 拉成等高，说明文字多折一行的那张仍然让按钮落在同一条
                    // 水平线上（自检会验证这个不变量）。
                    Item { Layout.fillHeight: true }
                    AppButton {
                        objectName: "homeAction0"
                        text: "开始备份"
                        // 三张卡片一律 primary、不分主次：它们是并列入口，
                        // 把某一项画成次级等于在暗示它不重要。secondary /
                        // flat 留给页面内部的次级操作。
                        variant: "primary"
                        onClicked: page.navigateTo(1)
                    }
                }
            }

            AppCard {
                objectName: "homeCard1"
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    AppIcon { name: "folder"; size: 22; color: theme.accent }
                    Text {
                        text: "备份管理"
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                        color: theme.textPrimary
                    }
                    Text {
                        text: "查看仓库中的备份，从中恢复或删除"
                        font.pixelSize: 16
                        color: theme.textSecondary
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.fillHeight: true }
                    AppButton {
                        objectName: "homeAction1"
                        text: "打开备份管理"
                        variant: "primary"
                        onClicked: page.navigateTo(3)
                    }
                }
            }

            AppCard {
                objectName: "homeCard2"
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    AppIcon { name: "clock"; size: 22; color: theme.accent }
                    Text {
                        text: "自动备份"
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                        color: theme.textPrimary
                    }
                    Text {
                        text: "定时触发 + 完整快照：" +
                              "只有真的发生变化才建立新副本"
                        font.pixelSize: 16
                        color: theme.textSecondary
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.fillHeight: true }
                    AppButton {
                        objectName: "homeAction2"
                        text: "打开自动备份"
                        variant: "primary"
                        onClicked: page.navigateTo(2)
                    }
                }
            }
        }

        Text {
            text: "当前状态"
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: theme.textSecondary
            Layout.topMargin: 4
        }

        // "当前状态"只显示**全局**状态（scope 为空）：空闲基线，以及后台任务
        // 正在跑时那条"正在备份/恢复"。各业务页的结果提示都带自己的 scope，
        // 属于哪一页就只出现在哪一页，不会漏到这里。
        StatusBanner {
            objectName: "homeStatusBanner"
            Layout.fillWidth: true
            // 首页只显示全局消息（controller.statusScope 为空的那一类）：
            // 首页自己不发起操作，不会有 scope 为 home 的消息，controller
            // 侧的 scope 常量只有 backup / management / settings 三种。
            pageScope: "home"
            scope: controller.statusScope
            kind: controller.statusKind
            title: controller.statusTitle
            message: controller.statusMessage
        }

        // 末尾弹性占位：column 没有 anchors.bottom，高度 = 内容隐式高度，
        // 所以这一行当前不产生视觉差异；将来若给 column 受限高度才会生效。
        Item { Layout.fillHeight: true }
    }
}

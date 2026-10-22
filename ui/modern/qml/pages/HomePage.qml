// HomePage.qml
//
// 首页只展示真实存在的东西：四个入口 + 当前操作状态。
// 四张卡片在宽屏下同行等宽，窄屏自动退化成 2×2 或单列；
// 按钮落在卡片底部，同一行的按钮因此保持同一条基线。

// 职责边界：本页只负责“把入口摆出来 + 显示全局状态”。它不读配置、不碰备份
// 引擎、不发起任何操作：按钮只发出语义导航 key，切页由 Main.qml 决定；
// 状态是 controller 的只读投影。
import QtQuick
import QtQuick.Layouts

import "../components"

Item {
    id: page

    // 语义导航：home / quick / scheduled / realtime / management。
    signal navigateTo(string key)

    // 宽屏四列、中等宽度两列、窄屏单列。阈值按页面可用宽度算，
    // 不按窗口宽度：侧栏固定 228，窗口宽度不能直接代表卡片可用空间。
    readonly property int columns: page.width >= 900 ? 4
                                 : (page.width >= 560 ? 2 : 1)

    ColumnLayout {
        id: column
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 32
        width: Math.min(parent.width - 64, 1080)
        spacing: 18

        Text {
            text: "备份工具"
            font.pixelSize: 30
            font.weight: Font.DemiBold
            color: theme.textPrimary
        }

        Text {
            text: "本地与云端数据备份、恢复和管理"
            font.pixelSize: 17
            color: theme.textSecondary
            Layout.topMargin: -10
        }

        GridLayout {
            id: entries
            Layout.fillWidth: true
            Layout.topMargin: 6
            columns: page.columns
            columnSpacing: 16
            rowSpacing: 16

            Repeater {
                model: [
                    {
                        "key": "quick",
                        "title": "快速备份",
                        "detail": "选择一个文件夹，立即创建备份",
                        "action": "开始备份",
                        "icon": "backup"
                    },
                    {
                        "key": "scheduled",
                        "title": "定时备份",
                        "detail": "按周期自动检查，有变化才备份",
                        "action": "设置定时备份",
                        "icon": "clock"
                    },
                    {
                        "key": "realtime",
                        "title": "实时备份",
                        "detail": "监视文件夹，变化后自动备份",
                        "action": "设置实时备份",
                        "icon": "refresh"
                    },
                    {
                        "key": "management",
                        "title": "备份管理",
                        "detail": "查看本地与云端备份，恢复或删除",
                        "action": "打开备份管理",
                        "icon": "folder"
                    }
                ]

                delegate: AppCard {
                    id: card
                    required property var modelData
                    required property int index

                    // objectName 是 GUI 自检的定位锚点：
                    // homeCard0..3 / homeAction0..3 会被逐个抓出来做点击回放
                    // 和几何断言（按钮必须落在卡片内，同一行按钮纵坐标一致）。
                    objectName: "homeCard" + index
                    Layout.fillWidth: true
                    Layout.preferredHeight: 216
                    Layout.alignment: Qt.AlignTop

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 10

                        AppIcon {
                            name: card.modelData["icon"]
                            size: 22
                            color: theme.accent
                        }
                        Text {
                            text: card.modelData["title"]
                            font.pixelSize: 20
                            font.weight: Font.DemiBold
                            color: theme.textPrimary
                        }
                        Text {
                            Layout.fillWidth: true
                            // 固定两行高度：四张卡在同一行时的按钮基线
                            // 因此与说明文字实际折几行无关。
                            Layout.preferredHeight: 46
                            text: card.modelData["detail"]
                            font.pixelSize: 15
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        Item { Layout.fillHeight: true }
                        AppButton {
                            objectName: "homeAction" + card.index
                            text: card.modelData["action"]
                            // 四张卡片一律 primary、不分主次：它们是并列入口。
                            variant: "primary"
                            onClicked: page.navigateTo(card.modelData["key"])
                        }
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

        // "当前状态"只显示全局状态（scope 为空）：空闲基线，以及后台任务
        // 正在跑时那条"正在备份/恢复"。各业务页的结果提示带自己的 scope。
        StatusBanner {
            objectName: "homeStatusBanner"
            Layout.fillWidth: true
            pageScope: "home"
            scope: controller.statusScope
            kind: controller.statusKind
            title: controller.statusTitle
            message: controller.statusMessage
        }

        Item { Layout.fillHeight: true }
    }
}

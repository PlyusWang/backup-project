// HomePage.qml
//
// 首页只展示真实存在的东西：入口按钮 + 当前操作状态。
// 不加“备份次数 / 节省空间 / 安全评分”这类当前根本没有的数据。

import QtQuick
import QtQuick.Layouts

import "../components"

Item {
    id: page

    // 首页不直接操作窗口，交给 Main.qml 处理跳转，页面之间保持解耦。
    signal navigateTo(int pageIndex)

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
                objectName: "homeCard0"
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

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
                    Item { Layout.fillHeight: true }
                    AppButton {
                        objectName: "homeAction0"
                        text: "开始备份"
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
                        text: "定时触发 + 完整快照：只有真的发生变化才建立新副本"
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
            pageScope: "home"
            scope: controller.statusScope
            kind: controller.statusKind
            title: controller.statusTitle
            message: controller.statusMessage
        }

        Item { Layout.fillHeight: true }
    }
}

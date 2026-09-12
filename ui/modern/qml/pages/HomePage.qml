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
            font.pixelSize: 26
            font.weight: Font.DemiBold
            color: theme.textPrimary
        }

        Text {
            text: "本地数据备份与恢复"
            font.pixelSize: 13
            color: theme.textSecondary
            Layout.topMargin: -10
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 16

            AppCard {
                Layout.fillWidth: true
                Layout.preferredHeight: 196

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    AppIcon { name: "backup"; size: 22; color: theme.accent }
                    Text {
                        text: "备份"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textPrimary
                    }
                    Text {
                        text: "选择一个目录并保存到备份仓库"
                        font.pixelSize: 12
                        color: theme.textSecondary
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.fillHeight: true }
                    AppButton {
                        text: "开始备份"
                        variant: "primary"
                        onClicked: page.navigateTo(1)
                    }
                }
            }

            AppCard {
                Layout.fillWidth: true
                Layout.preferredHeight: 196

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    AppIcon { name: "restore"; size: 22; color: theme.accent }
                    Text {
                        text: "恢复"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textPrimary
                    }
                    Text {
                        text: "从已有备份仓库恢复目录"
                        font.pixelSize: 12
                        color: theme.textSecondary
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.fillHeight: true }
                    AppButton {
                        text: "开始恢复"
                        variant: "primary"
                        onClicked: page.navigateTo(2)
                    }
                }
            }
        }

        Text {
            text: "当前状态"
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: theme.textSecondary
            Layout.topMargin: 4
        }

        StatusBanner {
            Layout.fillWidth: true
            kind: controller.statusKind
            title: controller.statusTitle
            message: controller.statusMessage
        }

        Item { Layout.fillHeight: true }
    }
}

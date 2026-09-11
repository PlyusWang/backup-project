// OperationPage.qml
//
// 备份页与恢复页共用的实现：结构与交互完全一样，只有文案和调用的控制器方法不同，
// 所以用一个 mode 参数区分，避免维护两份几乎逐行重复的 QML。

import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page

    property string mode: "backup"
    readonly property bool isBackup: mode === "backup"

    // 两个路径框共用一个目录选择器；targetField 记录这次是给哪个框选的。
    property int targetField: 0

    ColumnLayout {
        id: column
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 30
        width: Math.min(parent.width - 64, 900)
        spacing: 16

        Text {
            text: page.isBackup ? "备份" : "恢复"
            font.pixelSize: 24
            font.weight: Font.DemiBold
            color: theme.textPrimary
        }

        Text {
            text: page.isBackup ? "把一个目录完整备份到指定仓库。" : "从备份仓库恢复目录树。"
            font.pixelSize: 13
            color: theme.textSecondary
            Layout.topMargin: -8
        }

        AppCard {
            Layout.fillWidth: true
            Layout.topMargin: 4

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

                Text {
                    text: page.isBackup ? "源目录" : "备份仓库"
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: theme.textSecondary
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppTextField {
                        Layout.fillWidth: true
                        enabled: !controller.busy
                        placeholderText: "可直接输入路径，也可以点击“浏览”"
                        text: page.isBackup ? controller.sourcePath : controller.repositoryPath
                        onTextEdited: {
                            if (page.isBackup)
                                controller.sourcePath = text
                            else
                                controller.repositoryPath = text
                        }
                    }

                    AppButton {
                        text: "浏览"
                        enabled: !controller.busy
                        onClicked: {
                            page.targetField = 0
                            folderDialog.currentFolder = page.isBackup ? controller.sourcePath : controller.repositoryPath
                            folderDialog.open()
                        }
                    }
                }

                Text {
                    text: page.isBackup ? "备份仓库" : "恢复目录"
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: theme.textSecondary
                    Layout.topMargin: 8
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppTextField {
                        Layout.fillWidth: true
                        enabled: !controller.busy
                        placeholderText: "可直接输入路径，也可以点击“浏览”"
                        text: page.isBackup ? controller.repositoryPath : controller.restorePath
                        onTextEdited: {
                            if (page.isBackup)
                                controller.repositoryPath = text
                            else
                                controller.restorePath = text
                        }
                    }

                    AppButton {
                        text: "浏览"
                        enabled: !controller.busy
                        onClicked: {
                            page.targetField = 1
                            folderDialog.currentFolder = page.isBackup ? controller.repositoryPath : controller.restorePath
                            folderDialog.open()
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 14
                    spacing: 12

                    AppButton {
                        text: page.isBackup ? "开始备份" : "开始恢复"
                        variant: "primary"
                        // busy 时禁用：整个程序只有一个控制器，天然保证同一时刻只有一个操作。
                        enabled: !controller.busy
                        onClicked: page.isBackup ? controller.startBackup() : controller.startRestore()
                    }

                    // 不确定进度条：核心没有百分比回调，这里只表达“在跑”。
                    Rectangle {
                        id: progressTrack
                        Layout.fillWidth: true
                        Layout.preferredHeight: 3
                        radius: 1.5
                        color: theme.hover
                        visible: controller.busy
                        clip: true

                        Rectangle {
                            id: progressChunk
                            width: progressTrack.width * 0.3
                            height: parent.height
                            radius: 1.5
                            color: theme.accent

                            NumberAnimation on x {
                                running: controller.busy
                                loops: Animation.Infinite
                                from: -progressChunk.width
                                to: progressTrack.width
                                duration: 1150
                                easing.type: Easing.InOutQuad
                            }
                        }
                    }
                }
            }
        }

        StatusBanner {
            Layout.fillWidth: true
            kind: controller.statusKind
            title: controller.statusTitle
            message: controller.statusMessage
        }

        Item { Layout.fillHeight: true }
    }

    // 目录选择器只负责“帮忙填”：选完之后输入框仍然可以手改，
    // 因为备份仓库允许是一个还不存在的路径，不能被选择器限制住。
    FolderDialog {
        id: folderDialog
        title: "选择目录"
        onAccepted: {
            var path = folderDialog.selectedFolder.toString()
            path = path.replace(/^file:\/\//, "")
            if (page.targetField === 0) {
                if (page.isBackup)
                    controller.sourcePath = path
                else
                    controller.repositoryPath = path
            } else {
                if (page.isBackup)
                    controller.repositoryPath = path
                else
                    controller.restorePath = path
            }
            controller.clearStatus()
        }
    }
}

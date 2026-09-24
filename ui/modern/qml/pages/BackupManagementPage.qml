// BackupManagementPage.qml
//
// 备份管理页：列出当前仓库里的备份，并从列表发起恢复与删除。
// 恢复已经不是独立页面，而是这一页里的一个动作。
//
// 列表数据只来自 controller.backupRecords。每一项都不带 archive 完整路径：
// 恢复与删除都只传 file name，由 BackupCatalog 自己解析并校验。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import "../components"

Item {
    id: page

    signal openSettings()

    ScrollView {
        id: pageScroll
        objectName: "managementPageScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        Component.onCompleted: {
            if (pageScroll.contentItem)
                pageScroll.contentItem.boundsBehavior = Flickable.StopAtBounds
        }
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: column
            x: Math.max(32, (pageScroll.availableWidth - width) / 2)
            y: 20
            width: Math.min(pageScroll.availableWidth - 64, 1400)
            spacing: 16

            Text {
                text: "备份管理"
                font.pixelSize: 30
                font.weight: Font.DemiBold
                color: theme.textPrimary
            }

            Text {
                text: "管理备份仓库中的备份文件：恢复或删除。"
                font.pixelSize: 17
                color: theme.textSecondary
                Layout.topMargin: -8
            }

            AppCard {
                Layout.fillWidth: true
                Layout.topMargin: 4

                RowLayout {
                    anchors.fill: parent
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Text {
                            text: controller.repositoryConfigured ? "当前备份仓库" : "尚未配置备份仓库"
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            color: theme.textSecondary
                        }

                        Text {
                            objectName: "managementRepositoryPath"
                            Layout.fillWidth: true
                            visible: controller.repositoryConfigured
                            text: controller.repositoryPath
                            font.pixelSize: 16
                            color: theme.textPrimary
                            wrapMode: Text.WrapAnywhere
                        }
                    }

                    AppButton {
                        objectName: "refreshBackupsButton"
                        visible: controller.repositoryConfigured
                        text: "刷新"
                        iconName: "refresh"
                        enabled: !controller.catalogBusy && !controller.busy
                        onClicked: controller.refreshBackups()
                    }

                    AppButton {
                        visible: !controller.repositoryConfigured
                        text: "前往设置"
                        iconName: "settings"
                        onClicked: page.openSettings()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: controller.catalogBusy
                spacing: 8

                AppIcon { name: "refresh"; size: 16; color: theme.accent }
                Text {
                    objectName: "catalogBusyText"
                    text: "正在刷新备份列表……"
                    font.pixelSize: 15
                    color: theme.textSecondary
                }
                Item { Layout.fillWidth: true }
            }

            // 刷新失败是这一页自己的错误，用 catalogError 展示，
            // 不去覆盖一次刚成功的备份 / 恢复提示。
            AppCard {
                Layout.fillWidth: true
                visible: controller.catalogError.length > 0

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "无法读取备份仓库"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.error
                    }

                    TextEdit {
                        objectName: "catalogErrorText"
                        Layout.fillWidth: true
                        text: controller.catalogError
                        readOnly: true
                        selectByMouse: true
                        cursorVisible: false
                        textFormat: TextEdit.PlainText
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: TextEdit.WrapAnywhere
                    }
                }
            }

            Text {
                objectName: "catalogEmptyText"
                Layout.fillWidth: true
                Layout.topMargin: 8
                visible: controller.repositoryConfigured
                         && !controller.catalogBusy
                         && controller.catalogError.length === 0
                         && controller.backupRecords.length === 0
                text: "暂无备份"
                font.pixelSize: 16
                color: theme.textSecondary
            }

            Repeater {
                model: controller.backupRecords

                delegate: BackupRecordCard {
                    required property var modelData

                    fileNameText: String(modelData["fileName"] || "")
                    sizeText: String(modelData["sizeText"] || "")
                    modifiedText: String(modelData["modifiedTimeText"] || "")
                    recognized: Boolean(modelData["recognizedArchive"])
                    formatVersion: Number(modelData["formatVersion"] || 0)
                    entryCountText: String(modelData["entryCount"] || 0)
                    diagnosticText: String(modelData["diagnostic"] || "")
                    busy: controller.busy || controller.catalogBusy
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
    }
}

// BackupManagementPage.qml
//
// 统一的“备份管理”：本地仓库与云端仓库不再是两个产品入口，而是同一个
// 仓库模型下的两个存储位置。
//
// 结构：
//   [本地仓库] [云端仓库]
//     本地：当前仓库 / 仓库设置（可折叠）/ 备份列表（恢复、复制到云端、删除）
//     云端：连接与认证 / 云端备份列表（恢复、下载到本地仓库、删除）
//
// 页面不持有备份数据：本地列表来自 controller.backupRecords，云端列表来自
// remote.snapshots；两边都只把状态画出来、把点击转成控制器调用。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page

    // 只在“本地仓库”标签页内跳转设置区时使用；不再有独立的设置页面。
    signal openSettings()

    objectName: "managementPage"

    // local / cloud
    property string currentTab: "local"
    // 本地标签页内的分区：backups / settings
    property string localSection: "backups"
    property bool settingsExpanded: false
    property string draftPath: ""

    function showTab(tab, section) {
        if (tab === "local" || tab === "cloud")
            page.currentTab = tab
        if (section === "settings") {
            page.settingsExpanded = true
            page.localSection = "settings"
        }
    }

    // 草稿只在首帧与保存值变化时同步：用户输入只写 draftPath，
    // 保存成功之后由 savedPath 的变化回填。
    readonly property string savedPath: controller.repositoryPath
    Component.onCompleted: draftPath = controller.repositoryPath
    onSavedPathChanged: draftPath = controller.repositoryPath

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------- 页头 ----------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: page.currentTab === "cloud"
                ? cloudPage.contentInset : localColumn.x
            Layout.rightMargin: Layout.leftMargin
            Layout.topMargin: 24
            spacing: 10

            Text {
                objectName: "managementTitle"
                text: "备份管理"
                font.pixelSize: 30
                font.weight: Font.DemiBold
                color: theme.textPrimary
            }

            Text {
                text: "查看、恢复和管理您的备份。"
                font.pixelSize: 17
                color: theme.textSecondary
            }

            SegmentedTabs {
                objectName: "managementTabs"
                Layout.preferredWidth: 360
                Layout.topMargin: 4
                model: [
                    { "key": "local", "text": "本地仓库" },
                    { "key": "cloud", "text": "云端仓库" }
                ]
                currentKey: page.currentTab
                onActivated: function (key) { page.currentTab = key }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: page.currentTab === "cloud" ? 1 : 0

            // ---------- 本地仓库 ----------
            ScrollView {
                id: localScroll
                objectName: "managementPageScroll"
                clip: true
                contentWidth: availableWidth
                Component.onCompleted: {
                    if (localScroll.contentItem)
                        localScroll.contentItem.boundsBehavior =
                            Flickable.StopAtBounds
                }
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                ColumnLayout {
                    id: localColumn
                    objectName: "managementLocalColumn"
                    x: Math.max(32, (localScroll.availableWidth - width) / 2)
                    y: 16
                    width: Math.min(localScroll.availableWidth - 64, 1400)
                    spacing: 16

                    AppCard {
                        Layout.fillWidth: true

                        RowLayout {
                            anchors.fill: parent
                            spacing: 12

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Text {
                                    text: controller.repositoryConfigured
                                           ? "当前本地仓库"
                                          : "尚未配置本地仓库"
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

                                Text {
                                    Layout.fillWidth: true
                                    visible: !controller.repositoryConfigured
                                    text: "先选择一个目录作为本地备份仓库。"
                                    font.pixelSize: 15
                                    color: theme.textSecondary
                                    wrapMode: Text.WordWrap
                                }
                            }

                            AppButton {
                                objectName: "changeRepositoryButton"
                                text: "仓库设置"
                                iconName: "settings"
                                enabled: !controller.busy
                                onClicked: {
                                    page.settingsExpanded =
                                        !page.settingsExpanded
                                    page.localSection = page.settingsExpanded
                                        ? "settings" : "backups"
                                }
                            }

                            AppButton {
                                objectName: "refreshBackupsButton"
                                visible: controller.repositoryConfigured
                                text: "刷新"
                                iconName: "refresh"
                                enabled: !controller.catalogBusy
                                         && !controller.busy
                                onClicked: controller.refreshBackups()
                            }
                        }
                    }

                    // ---------- 仓库设置（折叠） ----------
                    AppCard {
                        objectName: "localRepositorySettings"
                        Layout.fillWidth: true
                        visible: page.settingsExpanded

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 8

                            Text {
                                text: "本地仓库设置"
                                font.pixelSize: 18
                                font.weight: Font.DemiBold
                                color: theme.textSecondary
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                AppTextField {
                                    id: repositoryField
                                    objectName: "repositoryPathField"
                                    Layout.fillWidth: true
                                    enabled: !controller.busy
                                    placeholderText:
                                        "输入目录路径，或点击“浏览目录”选择"
                                    text: page.draftPath
                                    onTextEdited: page.draftPath = text
                                }

                                AppButton {
                                    objectName: "browseRepositoryButton"
                                    text: "浏览目录"
                                    iconName: "folder"
                                    enabled: !controller.busy
                                    onClicked: {
                                        repositoryDialog.currentFolder =
                                            controller.directoryDialogStartUrl(
                                                page.draftPath)
                                        repositoryDialog.open()
                                    }
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "目录不存在时会自动创建；手写路径不会被"
                                    + "选择器限制。"
                                font.pixelSize: 15
                                color: theme.textSecondary
                            }

                            Text {
                                objectName: "savedRepositoryText"
                                Layout.fillWidth: true
                                text: controller.repositoryConfigured
                                      ? "当前保存的备份仓库："
                                        + controller.repositoryPath
                                      : "尚未保存备份仓库。"
                                font.pixelSize: 15
                                color: theme.textSecondary
                                wrapMode: Text.WrapAnywhere
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                AppButton {
                                    objectName: "saveRepositoryButton"
                                    text: "保存设置"
                                    variant: "primary"
                                    enabled: !controller.busy
                                    onClicked:
                                        controller.saveRepositoryPath(
                                            page.draftPath)
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: controller.catalogBusy
                        spacing: 8

                        AppIcon {
                            name: "refresh"; size: 16; color: theme.accent
                        }
                        Text {
                            objectName: "catalogBusyText"
                            text: "正在读取本地备份列表……"
                            font.pixelSize: 15
                            color: theme.textSecondary
                        }
                        Item { Layout.fillWidth: true }
                    }

                    AppCard {
                        Layout.fillWidth: true
                        visible: controller.catalogError.length > 0

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 6

                            Text {
                                text: "无法读取本地仓库"
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
                        text: "本地仓库中还没有备份。"
                        font.pixelSize: 16
                        color: theme.textSecondary
                    }

                    // 动态卡片由普通 Column 定位，外层布局只持有稳定容器。
                    Column {
                        id: localRecordColumn
                        Layout.fillWidth: true
                        visible: controller.backupRecords.length > 0
                        spacing: 16

                        Repeater {
                            model: controller.backupRecords

                            delegate: BackupRecordCard {
                                required property var modelData
                                width: localRecordColumn.width
                                height: implicitHeight

                                fileNameText: String(modelData["fileName"] || "")
                                sizeText: String(modelData["sizeText"] || "")
                                modifiedText: String(
                                    modelData["modifiedTimeText"] || "")
                                recognized: Boolean(
                                    modelData["recognizedArchive"])
                                formatVersion: Number(
                                    modelData["formatVersion"] || 0)
                                entryCountText: String(
                                    modelData["entryCount"] || 0)
                                diagnosticText: String(
                                    modelData["diagnostic"] || "")
                                hasPipelineMethods: Boolean(
                                    modelData["hasPipelineMethods"])
                                packMethodText: String(
                                    modelData["packMethodText"] || "")
                                compressionMethodText: String(
                                    modelData["compressionMethodText"] || "")
                                encryptionMethodText: String(
                                    modelData["encryptionMethodText"] || "")
                                passwordRequired: Boolean(
                                    modelData["passwordRequired"])
                                recordKind: String(
                                    modelData["recordKind"] || "full")
                                isDelta: Boolean(modelData["isDelta"])
                                parentFileName: String(
                                    modelData["parentFileName"] || "")
                                chainRestorable:
                                modelData["chainRestorable"] === undefined
                                    ? true
                                    : Boolean(modelData["chainRestorable"])
                                chainDiagnostic: String(
                                    modelData["chainDiagnostic"] || "")
                                originText:
                                schedule.originForFile(
                                    String(modelData["fileName"] || ""))
                                scheduledChangesText:
                                schedule.changesForFile(
                                    String(modelData["fileName"] || ""))
                                // 复制到云端是产品动作，依赖云端会话；未登录时
                                // 按钮仍然可见但禁用，用户能看到“为什么现在不能点”。
                                copyToCloudVisible: true
                                copyToCloudEnabled:
                                    remote.authenticated && !controller.busy
                                busy: controller.busy || controller.catalogBusy
                                onCopyToCloudRequested: {
                                    remote.copyLocalArchiveToCloud(
                                        String(modelData["fileName"] || ""))
                                }
                            }
                        }
                    }

                    // 本地页只显示属于本页的状态；备份/恢复的全局状态在
                    // 首页与发起它的页面各自呈现。
                    StatusBanner {
                        objectName: "managementStatusBanner"
                        Layout.fillWidth: true
                        pageScope: "management"
                        scope: controller.statusScope
                        kind: controller.statusKind
                        title: controller.statusTitle
                        message: controller.statusMessage
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---------- 云端仓库 ----------
            // 云端不是另一套备份方式：它复用 RemotePage 的连接、认证、
            // 列表、恢复、下载与删除，只把这部分嵌进统一的仓库入口。
            RemotePage {
                id: cloudPage
                embedded: true
            }
        }
    }

    FolderDialog {
        id: repositoryDialog
        objectName: "repositoryFolderDialog"
        title: "选择本地备份仓库目录"
        onAccepted: {
            const chosen =
                controller.localPathFromUrl(repositoryDialog.selectedFolder)
            if (chosen !== "")
                page.draftPath = chosen
        }
    }
}

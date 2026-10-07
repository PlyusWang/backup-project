// BackupManagementPage.qml
//
// 备份管理页：列出当前仓库里的备份，并从列表发起恢复与删除。
// 恢复已经不是独立页面，而是这一页里的一个动作。
//
// 列表数据只来自 controller.backupRecords。每一项都不带 archive 完整路径：
// 恢复与删除都只传 file name，由 BackupCatalog 自己解析并校验。

// 页面不持有数据：controller.backupRecords 是唯一来源，刷新 / 恢复 / 删除
// 都由控制器发起，这里只把状态画出来、把点击转成调用。所以页面重建不会
// 丢状态 —— 状态本来就不在这一层。
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
        // StopAtBounds 只能设在 ScrollView 内部的 contentItem 上，Qt 没有把
        // 这个属性透出来，所以在 Component.onCompleted 里取一次。目的是让
        // 内容不足一屏时不产生橡皮筋回弹。
        Component.onCompleted: {
            if (pageScroll.contentItem)
                pageScroll.contentItem.boundsBehavior = Flickable.StopAtBounds
        }
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: column
            // 居中但保留最小边距：宽窗口下按剩余空间平分，窄窗口下退化成
            // 左右各 32px（与上面 width 的 availableWidth - 64 配对）。
            // 宽度上限 1400 是为了超宽屏上不把一行文字拉得太长。
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
                            text: controller.repositoryConfigured
                                   ? "当前备份仓库"
                                  : "尚未配置备份仓库"
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

                    // 三个按钮按状态互斥：配好仓库显示"更改仓库 + 刷新"，
                    // 否则只显示"前往设置"。刷新还要受 busy 约束：
                    // 目录扫描与写归档不能并行。
                    // 配好仓库不等于不能再改：配置还在、目录被删掉，
                    // 或者单纯想换个位置，都会走到这里。
                    // 换仓库统一在设置页完成，这一页只发跳转意图，
                    // 所以"无法读取备份仓库"时用户也能直接过去处理。
                    AppButton {
                        objectName: "changeRepositoryButton"
                        visible: controller.repositoryConfigured
                        text: "更改仓库"
                        iconName: "settings"
                        enabled: !controller.busy
                        onClicked: page.openSettings()
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
                        objectName: "goToSettingsButton"
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
                    // catalogBusy 与 backup 的 busy 是控制器里两个独立标志：
                    // 刷新列表不会被误当成"备份正在进行"。
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
                // 列表读取失败走自己的错误通道 catalogError：它不会被"备份
                // 成功"之类的全局提示覆盖，两者可以同时可见。
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
                // "暂无备份"的四个条件缺一不可：已配置仓库、不在刷新中、没有
                // 错误、列表为空。少了 catalogBusy 会在每次刷新时闪一下空表，
                // 少了 catalogError 会把"读不出来"说成"没有"。
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

                // modelData 是 C++ 传来的 QVariantMap：字段可能整体缺失（例如
                // v1 归档没有 pipeline 元数据），所以每一项都做一次带默认值的
                // 强制转换，缺键时退化成空串 / 0 / false，而不是把 undefined
                // 绑进界面。
                delegate: BackupRecordCard {
                    required property var modelData

                    fileNameText: String(modelData["fileName"] || "")
                    sizeText: String(modelData["sizeText"] || "")
                    modifiedText: String(modelData["modifiedTimeText"] || "")
                    recognized: Boolean(modelData["recognizedArchive"])
                    formatVersion: Number(modelData["formatVersion"] || 0)
                    entryCountText: String(modelData["entryCount"] || 0)
                    diagnosticText: String(modelData["diagnostic"] || "")
                    // v2 才有的 pipeline 元数据：缺字段一律按 legacy 处理，
                    // 卡片因此不会显示任何算法名（也不会显示 v2 的三个文案）。
                    hasPipelineMethods: Boolean(modelData["hasPipelineMethods"])
                    packMethodText: String(modelData["packMethodText"] || "")
                    compressionMethodText: String(
                        modelData["compressionMethodText"] || "")
                    encryptionMethodText: String(
                        modelData["encryptionMethodText"] || "")
                    passwordRequired: Boolean(modelData["passwordRequired"])
                    // 快照种类与依赖链状态。缺键时按"完整、可恢复"退化：
                    // 没有这几个字段的老目录不该被误判成不可恢复。退化只
                    // 影响提示，恢复能不能成仍然由核心裁决。
                    recordKind: String(modelData["recordKind"] || "full")
                    isDelta: Boolean(modelData["isDelta"])
                    parentFileName: String(
                        modelData["parentFileName"] || "")
                    chainRestorable:
                    modelData["chainRestorable"] === undefined
                        ? true
                        : Boolean(modelData["chainRestorable"])
                    chainDiagnostic: String(
                        modelData["chainDiagnostic"] || "")
                    // 来源与计划变化摘要来自 ScheduleStore（不是文件名解析）：
                    // 手动备份不会被自动淘汰，"这条是谁建的"必须一眼看得出来。
                    // 这两个调用连同实参必须保持在同一行：静态契约按整串匹配，
                    // 拆行还会让 qmllint 对 schedule 的放行规则一起失效。
                    originText:
                    schedule.originForFile(String(modelData["fileName"] || ""))
                    scheduledChangesText:
                    schedule.changesForFile(String(modelData["fileName"] || ""))
                    busy: controller.busy || controller.catalogBusy
                }
            }

            // pageScope 必须与 Main.qml 的 dismissTransientMessage 分支字符串
            // 一致（"management"），否则离开这一页时这条提示不会被消费，回到
            // 页面还会再看到一次。
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
}

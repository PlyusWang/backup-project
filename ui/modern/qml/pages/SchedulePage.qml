// SchedulePage.qml
//
// 自动备份页：定时触发 + 完整快照。
//
// 这一页只做四件事：展示、编辑配置、点启停 / 立即运行、展示 history。
// 它不算 next run、不扫描文件树、不做 retention、不删归档、不对比 manifest、
// 也不自己维护计划快照列表 —— 全部来自 ScheduleController，而 ScheduleController
// 背后是与 backupctl 共用的同一份核心。
//
// 页面上每一个能点的东西都是真的：没有增量策略的假按钮，也没有实时触发的假按钮，
// 只有一行架构说明写清楚当前支持什么、后续会扩展什么。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page

    // draft 语义与设置页一致：输入框里的是草稿，点"保存计划"才写进控制器。
    property bool draftEnabled: schedule.enabled
    property string draftSource: schedule.sourcePath
    property string draftInterval: String(schedule.intervalMinutes)
    property string draftRetain: String(schedule.retainCount)
    property int draftPackIndex: 0
    property int draftCompressionIndex: 0
    property var draftInclude: []
    property var draftExclude: []
    property string draftIncludeInput: ""
    property string draftExcludeInput: ""

    readonly property var packKeys: ["mypack", "ustar", "fast-ustar"]
    readonly property var packLabels: ["MyPack", "USTAR", "Fast USTAR"]
    readonly property var compressionKeys: ["none", "huffman", "lzss-huffman"]
    readonly property var compressionLabels: ["不压缩", "Huffman", "LZSS + Huffman"]

    function syncFromController() {
        page.draftEnabled = schedule.enabled
        page.draftSource = schedule.sourcePath
        page.draftInterval = String(schedule.intervalMinutes)
        page.draftRetain = String(schedule.retainCount)
        page.draftInclude = schedule.includeRules
        page.draftExclude = schedule.excludeRules
        page.draftPackIndex = Math.max(0, page.packKeys.indexOf(schedule.packKey))
        page.draftCompressionIndex = Math.max(0, page.compressionKeys.indexOf(schedule.compressionKey))
    }

    // 已保存配置的"指纹"：它一变就说明控制器那边的配置换了（保存成功，
    // 或者有人用 backupctl 改了同一份 store），草稿跟着重置。
    //
    // 用属性变化处理函数而不是 Connections：Connections 在 Qt 6 里由 QtQml 提供，
    // 静态检查会因为没 import 那个模块而报"未找到类型"；一条普通绑定既够用，
    // 也不需要为了一个信号多引一个模块。
    readonly property string savedSignature: [
        schedule.enabled, schedule.sourcePath, schedule.intervalMinutes,
        schedule.retainCount, schedule.packKey, schedule.compressionKey,
        schedule.includeRules.join(","), schedule.excludeRules.join(",")
    ].join("|")
    onSavedSignatureChanged: syncFromController()

    Component.onCompleted: syncFromController()

    ScrollView {
        id: pageScroll
        objectName: "schedulePageScroll"
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
                text: "自动备份"
                font.pixelSize: 30
                font.weight: Font.DemiBold
                color: theme.textPrimary
            }

            Text {
                objectName: "scheduleSupportedModeText"
                Layout.fillWidth: true
                text: schedule.supportedModeText
                font.pixelSize: 17
                color: theme.textSecondary
                wrapMode: Text.WordWrap
                Layout.topMargin: -8
            }

            Text {
                objectName: "scheduleRunScopeText"
                Layout.fillWidth: true
                text: "定时任务在本程序或 backupctl schedule watch 运行期间执行。"
                font.pixelSize: 15
                color: theme.textSecondary
                wrapMode: Text.WordWrap
            }

            // ---------- 计划配置 ----------
            AppCard {
                Layout.fillWidth: true
                Layout.topMargin: 4

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Text {
                            text: "启用定时备份"
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            color: theme.textSecondary
                        }

                        AppButton {
                            objectName: "scheduleEnabledToggle"
                            text: page.draftEnabled ? "已启用" : "已停用"
                            variant: page.draftEnabled ? "primary" : "secondary"
                            enabled: !schedule.libraryBusy
                            onClicked: page.draftEnabled = !page.draftEnabled
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        text: "源目录"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.topMargin: 4
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        AppTextField {
                            id: sourceField
                            objectName: "scheduleSourceField"
                            Layout.fillWidth: true
                            enabled: !schedule.libraryBusy
                            placeholderText: "输入目录路径，或点击“浏览目录”选择"
                            text: page.draftSource
                            onTextEdited: page.draftSource = text
                        }

                        AppButton {
                            objectName: "browseScheduleSourceButton"
                            text: "浏览目录"
                            iconName: "folder"
                            enabled: !schedule.libraryBusy
                            onClicked: {
                                sourceDialog.currentFolder = schedule.directoryDialogStartUrl(page.draftSource)
                                sourceDialog.open()
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "计划有它自己的源目录，不会跟随备份页上临时输入的路径。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        spacing: 10

                        Text {
                            text: "周期"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                        AppTextField {
                            id: intervalField
                            objectName: "scheduleIntervalField"
                            implicitWidth: 110
                            enabled: !schedule.libraryBusy
                            text: page.draftInterval
                            onTextEdited: page.draftInterval = text
                        }
                        Text {
                            text: "分钟"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            text: "保留版本"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                        AppTextField {
                            id: retainField
                            objectName: "scheduleRetainField"
                            implicitWidth: 110
                            enabled: !schedule.libraryBusy
                            text: page.draftRetain
                            onTextEdited: page.draftRetain = text
                        }
                        Text {
                            text: "个计划快照"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        spacing: 10

                        Text {
                            text: "打包"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                        AppComboBox {
                            id: packBox
                            objectName: "schedulePackCombo"
                            implicitWidth: 190
                            enabled: !schedule.libraryBusy
                            model: page.packLabels
                            currentIndex: page.draftPackIndex
                            onActivated: page.draftPackIndex = currentIndex
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            text: "压缩"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                        AppComboBox {
                            id: compressionBox
                            objectName: "scheduleCompressionCombo"
                            implicitWidth: 210
                            enabled: !schedule.libraryBusy
                            model: page.compressionLabels
                            currentIndex: page.draftCompressionIndex
                            onActivated: page.draftCompressionIndex = currentIndex
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        spacing: 10

                        Text {
                            text: "加密"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                        Text {
                            objectName: "scheduleEncryptionText"
                            text: "不加密"
                            font.pixelSize: 16
                            color: theme.textPrimary
                        }
                    }

                    Text {
                        objectName: "scheduleEncryptionNote"
                        Layout.fillWidth: true
                        text: "无人值守的定时任务没有安全的持久密钥来源，因此本版本固定不加密，也不会保存任何明文密码。需要加密时请用手动备份（密码只在交互终端里输入）。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    // ---------- Filter ----------
                    Text {
                        text: "筛选规则（include / exclude）"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.topMargin: 10
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "语法与备份页完全一致，规则由核心的 Filter 解析并校验；这里不做第二套解析。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        model: page.draftInclude.length + page.draftExclude.length
                        delegate: RowLayout {
                            required property int index
                            Layout.fillWidth: true
                            spacing: 8
                            readonly property bool isInclude: index < page.draftInclude.length
                            readonly property string ruleText: isInclude
                                ? page.draftInclude[index]
                                : page.draftExclude[index - page.draftInclude.length]
                            Text {
                                Layout.fillWidth: true
                                text: (parent.isInclude ? "include  " : "exclude  ") + parent.ruleText
                                font.pixelSize: 15
                                color: theme.textPrimary
                                elide: Text.ElideMiddle
                            }
                            AppButton {
                                text: "移除"
                                variant: "flat"
                                implicitWidth: 72
                                enabled: !schedule.libraryBusy
                                onClicked: {
                                    if (parent.isInclude) {
                                        const next = page.draftInclude.slice()
                                        next.splice(index, 1)
                                        page.draftInclude = next
                                    } else {
                                        const next = page.draftExclude.slice()
                                        next.splice(index - page.draftInclude.length, 1)
                                        page.draftExclude = next
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        AppTextField {
                            id: includeInput
                            objectName: "scheduleIncludeField"
                            Layout.fillWidth: true
                            enabled: !schedule.libraryBusy
                            placeholderText: "例如 ext:cpp;h"
                            text: page.draftIncludeInput
                            onTextEdited: page.draftIncludeInput = text
                        }
                        AppButton {
                            objectName: "addScheduleIncludeButton"
                            text: "添加 include"
                            enabled: !schedule.libraryBusy && page.draftIncludeInput !== ""
                            onClicked: {
                                const error = schedule.validateRule("include", page.draftIncludeInput)
                                if (error !== "") {
                                    page.draftIncludeInput = page.draftIncludeInput
                                    invalidRuleText.text = error
                                    return
                                }
                                invalidRuleText.text = ""
                                const next = page.draftInclude.slice()
                                next.push(page.draftIncludeInput)
                                page.draftInclude = next
                                page.draftIncludeInput = ""
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        AppTextField {
                            id: excludeInput
                            objectName: "scheduleExcludeField"
                            Layout.fillWidth: true
                            enabled: !schedule.libraryBusy
                            placeholderText: "例如 path:**/build/**"
                            text: page.draftExcludeInput
                            onTextEdited: page.draftExcludeInput = text
                        }
                        AppButton {
                            objectName: "addScheduleExcludeButton"
                            text: "添加 exclude"
                            enabled: !schedule.libraryBusy && page.draftExcludeInput !== ""
                            onClicked: {
                                const error = schedule.validateRule("exclude", page.draftExcludeInput)
                                if (error !== "") {
                                    invalidRuleText.text = error
                                    return
                                }
                                invalidRuleText.text = ""
                                const next = page.draftExclude.slice()
                                next.push(page.draftExcludeInput)
                                page.draftExclude = next
                                page.draftExcludeInput = ""
                            }
                        }
                    }

                    Text {
                        id: invalidRuleText
                        objectName: "scheduleInvalidRuleText"
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: ""
                        font.pixelSize: 15
                        color: theme.error
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        spacing: 12

                        AppButton {
                            objectName: "saveScheduleButton"
                            text: "保存计划"
                            variant: "primary"
                            enabled: !schedule.libraryBusy
                            // 周期与保留数量按**文本**交给 C++：QML 的 parseInt
                            // 会把 "12abc" 悄悄变成 12，而 backupctl 会明确拒绝它。
                            // 解析规则只有一份，在共享核心里。
                            onClicked: schedule.saveConfigFromText(
                                page.draftEnabled,
                                page.draftSource,
                                page.draftInterval,
                                page.draftRetain,
                                page.packKeys[page.draftPackIndex],
                                page.compressionKeys[page.draftCompressionIndex],
                                page.draftInclude,
                                page.draftExclude)
                        }

                        AppButton {
                            objectName: "runScheduleNowButton"
                            text: "立即检查并运行"
                            iconName: "backup"
                            enabled: !schedule.libraryBusy && schedule.enabled
                            onClicked: schedule.runNow()
                        }

                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // ---------- 运行状态 ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "运行状态"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    Text {
                        objectName: "scheduleLastRunText"
                        Layout.fillWidth: true
                        text: "上次运行：" + schedule.lastRunText
                        font.pixelSize: 16
                        color: theme.textPrimary
                    }

                    Text {
                        objectName: "scheduleNextRunText"
                        Layout.fillWidth: true
                        text: "下次运行：" + schedule.nextRunText
                        font.pixelSize: 16
                        color: theme.textPrimary
                    }

                    Text {
                        objectName: "scheduleLastResultText"
                        Layout.fillWidth: true
                        text: "最近结果：" + schedule.lastResultText
                        font.pixelSize: 16
                        color: theme.textPrimary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "scheduleRunnerText"
                        Layout.fillWidth: true
                        text: "运行者：" + schedule.runnerMessage
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "schedulePendingText"
                        Layout.fillWidth: true
                        visible: schedule.pending
                        text: "已到期但当前正忙，将在当前操作结束后补跑一次。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "scheduleLoadErrorText"
                        Layout.fillWidth: true
                        visible: schedule.loadError !== ""
                        text: schedule.loadError
                        font.pixelSize: 15
                        color: theme.error
                        wrapMode: Text.WordWrap
                    }

                    // 挂起是**持续状态**，与状态栏里那一句瞬时提示不同：
                    // 只要悬挂原因还在（落盘配置不合法），这行就一直亮着。
                    Text {
                        objectName: "scheduleSuspendedText"
                        Layout.fillWidth: true
                        visible: schedule.suspended
                        text: "定时备份已挂起：落盘的计划配置不合法。程序不会自动修改它，"
                              + "也不会自动重试；请修正后重新保存计划。"
                        font.pixelSize: 15
                        color: theme.error
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ---------- history ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "运行历史"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    Text {
                        objectName: "scheduleHistoryEmptyText"
                        visible: schedule.history.length === 0
                        text: "还没有运行记录。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                    }

                    Repeater {
                        objectName: "scheduleHistoryList"
                        model: schedule.history
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: modelData["timeText"] + "  ·  " + modelData["resultKey"]
                                font.pixelSize: 15
                                color: theme.textPrimary
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData["changesText"]
                                      + (modelData["archiveName"] !== "" ? "  ·  " + modelData["archiveName"] : "")
                                      + (modelData["diagnostic"] !== "" ? "  ·  " + modelData["diagnostic"] : "")
                                font.pixelSize: 14
                                color: theme.textSecondary
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
            }

            // ---------- 计划快照 ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "计划快照（只列 scheduler 自己管理的那些）"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    Text {
                        objectName: "scheduleManagedEmptyText"
                        visible: schedule.managedSnapshots.length === 0
                        text: "还没有由计划任务创建的快照。手动备份不会出现在这里，也不会被自动淘汰。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        objectName: "scheduleManagedList"
                        model: schedule.managedSnapshots
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: modelData["fileName"] + "  ·  " + modelData["createdText"]
                                      + "  ·  " + modelData["sizeText"]
                                      + "  ·  " + modelData["packText"] + " / " + modelData["compressionText"]
                                font.pixelSize: 15
                                color: theme.textPrimary
                                elide: Text.ElideMiddle
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData["changesText"]
                                font.pixelSize: 14
                                color: theme.textSecondary
                            }
                        }
                    }
                }
            }

            StatusBanner {
                Layout.fillWidth: true
                kind: schedule.statusKind
                title: schedule.statusTitle
                message: schedule.statusMessage
            }

            Item { Layout.fillHeight: true }
        }
    }

    FolderDialog {
        id: sourceDialog
        objectName: "scheduleSourceFolderDialog"
        title: "选择定时备份的源目录"
        onAccepted: {
            const chosen = schedule.localPathFromUrl(sourceDialog.selectedFolder)
            if (chosen !== "")
                page.draftSource = chosen
        }
    }
}

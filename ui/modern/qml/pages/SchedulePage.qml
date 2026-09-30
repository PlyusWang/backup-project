// SchedulePage.qml
//
// 自动备份页：定时触发 + 完整快照 / 增量策略。
//
// 信息分层与实时备份页保持同一套产品语言（人工验收："两个自动化页面应该形成
// 同一套产品语言"）：
//   1. 常用设置 —— 定时备份状态、备份目录、备份频率、备份方式、保留版本、保存
//   2. 高级设置 —— 打包格式、压缩方式、筛选规则、加密说明（默认折叠）
//   3. 运行状态 —— 上次 / 下次 / 最近结果 / 运行者
//   4. 运行历史 —— 每次评估的结果
//   5. 计划快照 —— 由 scheduler 自己管理、会被自动淘汰的那些
//
// 这一页只做四件事：展示、编辑配置、点启停 / 立即执行一次、展示 history。
// 它不算 next run、不扫描文件树、不做 retention、不删归档、不对比 manifest、
// 也不自己维护计划快照列表 —— 全部来自 ScheduleController，而 ScheduleController
// 背后是与 backupctl 共用的同一份核心。
//
// 两个"人话化"的地方值得单独说明：
//   * 备份频率是"每 [值] [单位]"而不是"周期 [60] 分钟"。换算（值 × 单位 ->
//     interval_minutes）在 C++ 的 schedule_frequency.cpp 里做，存储 schema
//     一个字节都没变：核心、CLI、ScheduleStore 继续只看 interval_minutes。
//   * 筛选规则用 FilterRuleEditor —— 与备份页、实时页**同一个组件**，用户选
//     条件、填取值，DSL 由共享 builder 生成。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page

    // draft 语义与设置页一致：输入框里的是草稿，点"保存设置"才写进控制器。
    property bool draftEnabled: schedule.enabled
    property string draftSource: schedule.sourcePath
    property string draftFrequencyValue: schedule.frequencyValueText
    property string draftFrequencyUnit: schedule.frequencyUnitKey
    property string draftRetain: String(schedule.retainCount)
    property int draftStrategyIndex: 0
    property int draftPackIndex: 0
    property int draftCompressionIndex: 0

    // 高级设置默认收起：一打开页面先看到日常要用的那几项。
    property bool advancedExpanded: false

    readonly property var strategyKeys: ["full", "incremental"]
    // 与手动 / 实时页用同一组词（人工验收：不要一处 Full、一处完整快照）。
    readonly property var strategyLabels: ["完整备份", "增量备份"]
    readonly property var packKeys: ["mypack", "ustar", "fast-ustar"]
    readonly property var packLabels: ["MyPack（推荐）", "USTAR（兼容格式）", "Fast USTAR（兼容格式）"]
    readonly property var compressionKeys: ["none", "huffman", "lzss-huffman"]
    readonly property var compressionLabels: ["不压缩", "Huffman", "LZSS + Huffman"]

    // 频率单位来自控制器（背后是共享核心的分钟边界），界面不自己定义单位表。
    readonly property var frequencyUnits: schedule.frequencyUnits()
    readonly property var frequencyUnitKeys: frequencyUnits.map(function (unit) { return unit.key })
    readonly property var frequencyUnitLabels: frequencyUnits.map(function (unit) { return unit.label })

    // 备份方式的短解释：只解释当前选中的那一种，措辞与实时页逐字一致。
    readonly property string strategyHelper: page.draftStrategyIndex === 1
        ? "增量备份：首次建立完整基线，之后只保存变化，更节省空间。"
        : "完整备份：每次生成一份可以独立恢复的完整备份。"

    function syncFromController() {
        page.draftEnabled = schedule.enabled
        page.draftSource = schedule.sourcePath
        page.draftFrequencyValue = schedule.frequencyValueText
        page.draftFrequencyUnit = schedule.frequencyUnitKey
        page.draftRetain = String(schedule.retainCount)
        page.draftStrategyIndex = Math.max(0, page.strategyKeys.indexOf(schedule.strategyKey))
        page.draftPackIndex = Math.max(0, page.packKeys.indexOf(schedule.packKey))
        page.draftCompressionIndex = Math.max(0, page.compressionKeys.indexOf(schedule.compressionKey))
        // 落盘配置里的规则读进共享编辑器（校验仍然走共享 builder）。读不懂时
        // 编辑器会显示共享核心给出的原因，这里不吞掉它。
        ruleEditor.loadRules(schedule.includeRules, schedule.excludeRules)
    }

    // 已保存配置的"指纹"：它一变就说明控制器那边的配置换了（保存成功，
    // 或者有人用 backupctl 改了同一份 store），草稿跟着重置。
    //
    // 用属性变化处理函数而不是 Connections：Connections 在 Qt 6 里由 QtQml 提供，
    // 静态检查会因为没 import 那个模块而报"未找到类型"；一条普通绑定既够用，
    // 也不需要为了一个信号多引一个模块。
    readonly property string savedSignature: [
        schedule.enabled, schedule.sourcePath, schedule.intervalMinutes,
        schedule.retainCount, schedule.strategyKey,
        schedule.packKey, schedule.compressionKey,
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

            // 副标题面向普通用户：只讲"它能帮我做什么"。
            Text {
                objectName: "scheduleSupportedModeText"
                Layout.fillWidth: true
                text: schedule.supportedModeText
                font.pixelSize: 17
                color: theme.textSecondary
                wrapMode: Text.WordWrap
                Layout.topMargin: -8
            }

            // 面向用户的运行范围说明。检查进程、备份仓库这些细节留给下面的
            // 「高级设置」——主流程里不出现命令行工具名。
            Text {
                objectName: "scheduleRunScopeText"
                Layout.fillWidth: true
                text: "定时任务只在本程序运行期间执行：关掉程序就不会再触发，重新打开后会按计划继续。"
                font.pixelSize: 15
                color: theme.textSecondary
                wrapMode: Text.WordWrap
            }

            // ---------- 1. 常用设置 ----------
            AppCard {
                Layout.fillWidth: true
                Layout.topMargin: 4

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    Text {
                        text: "常用设置"
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    // 启用状态：左边是"现在是什么状态"，右边是"能做的动作"。
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Text {
                            text: "●"
                            font.pixelSize: 18
                            color: page.draftEnabled ? theme.success : theme.textDisabled
                        }

                        Text {
                            text: page.draftEnabled ? "定时备份已启用" : "定时备份已停用"
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            color: theme.textPrimary
                        }

                        Item { Layout.fillWidth: true }

                        AppButton {
                            objectName: "scheduleEnabledToggle"
                            text: page.draftEnabled ? "停用" : "启用"
                            variant: page.draftEnabled ? "secondary" : "primary"
                            enabled: !schedule.libraryBusy
                            onClicked: page.draftEnabled = !page.draftEnabled
                        }
                    }

                    Text {
                        text: "备份目录"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.topMargin: 2
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        AppTextField {
                            id: sourceField
                            objectName: "scheduleSourceField"
                            Layout.fillWidth: true
                            enabled: !schedule.libraryBusy
                            placeholderText: "输入目录路径，或点击“选择目录”选择"
                            text: page.draftSource
                            onTextEdited: page.draftSource = text
                        }

                        AppButton {
                            objectName: "browseScheduleSourceButton"
                            text: "选择目录"
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
                        text: "计划有它自己的备份目录，不会跟随备份页上临时输入的路径。"
                        font.pixelSize: 14
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    // ---- 备份频率：值 + 单位，不是"周期 60 分钟" ----
                    Text {
                        text: "备份频率"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.topMargin: 2
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "每"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }

                        AppTextField {
                            id: frequencyField
                            objectName: "scheduleFrequencyValueField"
                            implicitWidth: 110
                            enabled: !schedule.libraryBusy
                            text: page.draftFrequencyValue
                            onTextEdited: page.draftFrequencyValue = text
                        }

                        AppComboBox {
                            id: frequencyUnitBox
                            objectName: "scheduleFrequencyUnitCombo"
                            implicitWidth: 130
                            enabled: !schedule.libraryBusy
                            model: page.frequencyUnitLabels
                            currentIndex: Math.max(0, page.frequencyUnitKeys.indexOf(page.draftFrequencyUnit))
                            onActivated: page.draftFrequencyUnit = page.frequencyUnitKeys[currentIndex]
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        objectName: "scheduleFrequencyHintText"
                        Layout.fillWidth: true
                        text: "例如“每 1 小时”“每 2 天”。程序负责换算成分钟，命令行里读到的也是同一个值。"
                        font.pixelSize: 14
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    // ---- 备份方式 ----
                    Text {
                        text: "备份方式"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.topMargin: 2
                    }

                    AppComboBox {
                        id: strategyBox
                        objectName: "scheduleStrategyCombo"
                        implicitWidth: 200
                        enabled: !schedule.libraryBusy
                        model: page.strategyLabels
                        currentIndex: page.draftStrategyIndex
                        onActivated: page.draftStrategyIndex = currentIndex
                    }

                    Text {
                        objectName: "scheduleStrategyHelperText"
                        Layout.fillWidth: true
                        text: page.strategyHelper
                        font.pixelSize: 14
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    // ---- 保留版本 ----
                    Text {
                        text: "保留版本"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.topMargin: 2
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "保留最近"
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
                            text: "个版本"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }

                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        spacing: 12

                        AppButton {
                            objectName: "saveScheduleButton"
                            text: "保存设置"
                            variant: "primary"
                            enabled: !schedule.libraryBusy
                            // 频率（值 + 单位）与保留数量按**文本**交给 C++：
                            // QML 的 parseInt 会把 "12abc" 悄悄变成 12，而 backupctl
                            // 会明确拒绝它。解析规则只有一份，在共享核心里。
                            onClicked: schedule.saveConfigFromFrequencyText(
                                page.draftEnabled,
                                page.draftSource,
                                page.draftFrequencyValue,
                                page.draftFrequencyUnit,
                                page.draftRetain,
                                page.packKeys[page.draftPackIndex],
                                page.compressionKeys[page.draftCompressionIndex],
                                ruleEditor.includeRuleTexts,
                                ruleEditor.excludeRuleTexts,
                                page.strategyKeys[page.draftStrategyIndex])
                        }

                        AppButton {
                            objectName: "runScheduleNowButton"
                            text: "立即执行一次"
                            iconName: "backup"
                            enabled: !schedule.libraryBusy && schedule.enabled
                            onClicked: schedule.runNow()
                        }

                        Item { Layout.fillWidth: true }
                    }

                    // 按钮文案不许承诺"一定产生新备份"：EvaluateNow 在没有变化时
                    // 不会写出新的快照。
                    Text {
                        objectName: "runScheduleNowHintText"
                        Layout.fillWidth: true
                        text: "立即检查当前状态，并在需要时创建备份。"
                        font.pixelSize: 14
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ---------- 2. 高级设置（默认折叠） ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Text {
                            text: "高级设置"
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            color: theme.textSecondary
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "打包、压缩与筛选规则，一般保持默认即可。"
                            font.pixelSize: 15
                            color: theme.textSecondary
                            elide: Text.ElideRight
                        }

                        AppButton {
                            objectName: "scheduleAdvancedToggle"
                            text: page.advancedExpanded ? "收起 ▾" : "展开 ▸"
                            variant: "flat"
                            onClicked: page.advancedExpanded = !page.advancedExpanded
                        }
                    }

                    // 折叠区：容器与区内的每个具名控件都显式跟随折叠状态，
                    // 收起时不只是"父级看不见"。折叠状态不持久化。
                    ColumnLayout {
                        id: advancedSection
                        objectName: "scheduleAdvancedSection"
                        Layout.fillWidth: true
                        spacing: 14
                        visible: page.advancedExpanded

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 24

                            ColumnLayout {
                                spacing: 4

                                Text {
                                    text: "打包格式"
                                    font.pixelSize: 16
                                    color: theme.textSecondary
                                }

                                AppComboBox {
                                    id: packBox
                                    objectName: "schedulePackCombo"
                                    visible: page.advancedExpanded
                                    implicitWidth: 210
                                    enabled: !schedule.libraryBusy
                                    model: page.packLabels
                                    currentIndex: page.draftPackIndex
                                    onActivated: page.draftPackIndex = currentIndex
                                }
                            }

                            ColumnLayout {
                                spacing: 4

                                Text {
                                    text: "压缩方式"
                                    font.pixelSize: 16
                                    color: theme.textSecondary
                                }

                                AppComboBox {
                                    id: compressionBox
                                    objectName: "scheduleCompressionCombo"
                                    visible: page.advancedExpanded
                                    implicitWidth: 210
                                    enabled: !schedule.libraryBusy
                                    model: page.compressionLabels
                                    currentIndex: page.draftCompressionIndex
                                    onActivated: page.draftCompressionIndex = currentIndex
                                }
                            }

                            Item { Layout.fillWidth: true }
                        }

                        // 筛选规则：与备份页 / 实时页共用同一个可视化编辑器。
                        FilterRuleEditor {
                            id: ruleEditor
                            ruleModel: scheduleFilterRuleModel
                            objectPrefix: "schedule"
                            busy: schedule.libraryBusy
                            heading: "筛选规则"
                            intro: "只有符合条件的文件会参与备份。如果同时命中包含和排除规则，以排除规则为准。"
                            Layout.fillWidth: true
                        }

                        // 加密：一条弱提示，不再摆一个永远点不动的下拉框。
                        // 说明来自控制器（控制器读的是核心里那句唯一来源）。
                        Text {
                            objectName: "scheduleEncryptionNote"
                            visible: page.advancedExpanded
                            Layout.fillWidth: true
                            text: schedule.encryptionNote
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "scheduleEncryptionText"
                            visible: page.advancedExpanded
                            text: "加密：暂不支持"
                            font.pixelSize: 14
                            color: theme.textDisabled
                        }

                    }
                }
            }

            // ---------- 3. 运行状态 ----------
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
                              + "也不会自动重试；请修正后重新保存设置。"
                        font.pixelSize: 15
                        color: theme.error
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ---------- 4. 运行历史 ----------
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

            // ---------- 5. 计划快照 ----------
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

            // 计划页的状态来自 ScheduleController，只有这一页绑定它；pageScope
            // 仍然写全，让"临时提示只属于产生它的页面"这条契约在四处保持一致。
            StatusBanner {
                objectName: "scheduleStatusBanner"
                Layout.fillWidth: true
                pageScope: "schedule"
                scope: "schedule"
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
        title: "选择定时备份的备份目录"
        onAccepted: {
            const chosen = schedule.localPathFromUrl(sourceDialog.selectedFolder)
            if (chosen !== "")
                page.draftSource = chosen
        }
    }
}

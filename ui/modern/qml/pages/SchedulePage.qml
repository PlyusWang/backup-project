// SchedulePage.qml
//
// 自动备份页：定时触发 + 完整快照 / 增量策略。
//
// 信息分层与实时备份页保持同一套产品语言（"两个自动化页面应该形成
// 同一套产品语言"）：
//   1. 常用设置 —— 定时备份状态、备份目录、备份频率、备份方式、保留版本、保存
//   2. 高级设置 —— 打包格式、压缩方式、筛选规则、加密说明（默认折叠）
//   3. 运行状态 —— 上次 / 下次 / 最近结果 / 运行者
//   4. 运行历史 —— 每次评估的结果
//   5. 计划快照 —— 由 scheduler 自己管理、会被自动淘汰的那些
//
// 这一页只做四件事：展示、编辑配置、点启停 / 立即执行一次、展示 history。
// 它不算 next run、不扫描文件树、不做 retention、不删归档、不对比 manifest、
// 也不自己维护计划快照列表 —— 全部来自 ScheduleController，
// 而 ScheduleController 背后是与 backupctl 共用的同一份核心。
//
// 两个"人话化"的地方值得单独说明：
//   * 备份频率是"每 [值] [单位]"而不是"周期 [60] 分钟"。换算（值 × 单位 ->
//     interval_minutes）在 C++ 的 schedule_frequency.cpp 里做，存储 schema
//     一个字节都没变：核心、CLI、ScheduleStore 继续只看 interval_minutes。
//   * 筛选规则用 FilterRuleEditor —— 与备份页、实时页**同一个组件**，用户选
//     条件、填取值，DSL 由共享 builder 生成。
//
// ---- 数据流（单向）----
//
//   ScheduleController（C++，背后是与 CLI 共用的核心）
//        |  属性绑定：enabled / sourcePath / frequencyValueText / ...
//        v
//   本页的 draft* 属性（草稿；只有用户点击才改）
//        |  "保存设置" 一次性把全部字段交给 saveConfigFromFrequencyText
//        v
//   控制器校验 -> 写 store -> 属性变化 -> savedSignature 变 -> 草稿重置
//
// 因此页面不持有配置状态、也不做校验：解析、合法性判断、单位换算都在共享
// 核心里，GUI 与 backupctl 读同一份 store 时不会出现两套解释。
//
// ---- objectName 契约 ----
// 具名控件（scheduleSourceField、saveScheduleButton、scheduleHistoryList
// 等）是 UI 自动化测试的抓手，改名等同于改测试接口。

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
    // 草稿里频率值与保留数量刻意保存为**字符串**：QML 的 parseInt 会把
    // "12abc" 悄悄截成 12，而共享核心会明确拒绝它。原样转交给 C++ 才能保证
    // GUI 与 backupctl 用的是同一套解析规则。下拉框则用下标，因为选项表由
    // 控制器提供，页面只认下标对应的 key。
    property int draftCompressionIndex: 0
    property int draftEncryptionIndex: 0
    // 配置草稿只保存会话引用或高级口令文件路径。
    property string draftCredentialFile: ""
    property string draftRepositoryType: schedule.repositoryType
    property string draftCloudCredentialsFile: schedule.cloudCredentialsFile
    property string draftCloudRepositoryId: schedule.cloudRepositoryId

    property bool savingConfig: false
    property bool passwordFileExpanded: false
    property bool cloudFileExpanded: false

    function clearPasswordInputs() {
        if (archivePasswordField)
            archivePasswordField.text = ""
        if (archivePasswordConfirmationField)
            archivePasswordConfirmationField.text = ""
    }

    function useLoggedInConnection() {
        if (!remote.sharedSessionReady)
            return
        page.draftCloudCredentialsFile = remote.sharedSessionReference
        page.draftCloudRepositoryId = remote.sharedSessionRepositoryId
    }

    function prepareCredentials() {
        if (page.draftRepositoryType === "cloud"
                && page.draftCloudCredentialsFile.length === 0)
            page.useLoggedInConnection()
        if (page.encryptionKeyValue === "none") {
            page.clearPasswordInputs()
            page.draftCredentialFile = ""
        } else if (archivePasswordField.text.length > 0
                   || archivePasswordConfirmationField.text.length > 0) {
            if (!schedule.unlockArchivePassword(
                    archivePasswordField.text,
                    archivePasswordConfirmationField.text))
                return false
            page.draftCredentialFile = schedule.credentialFile
            page.clearPasswordInputs()
        }
        return true
    }

    onVisibleChanged: {
        if (!visible)
            page.clearPasswordInputs()
    }
    Component.onDestruction: page.clearPasswordInputs()

    // 高级设置默认收起：一打开页面先看到日常要用的那几项。
    property bool advancedExpanded: false
    property bool historyExpanded: false
    readonly property var visibleHistory: page.historyExpanded
        ? schedule.history : schedule.history.slice(0, 3)

    // key 是页面与核心之间的协议，label 由控制器给出（见下面几行）：这样
    // 界面上不会一处写 Full、一处写完整快照。下标落空时统一退到 0，用户看到
    // 的是默认项而不是空白下拉框。
    readonly property var strategyKeys: ["full", "incremental"]
    // 与手动 / 实时页用同一组词（不要一处 Full、一处完整快照）。
    readonly property var strategyLabels: controller.optionStrategyLabels
    readonly property var packKeys: ["mypack", "ustar", "fast-ustar"]
    readonly property var packLabels: controller.optionPackLabels
    readonly property var compressionKeys: ["none", "huffman", "lzss-huffman"]
    readonly property var compressionLabels: controller.optionCompressionLabels
    // 三种备份方式共用同一组加密 key / 显示名（backupOptions 是唯一来源）。
    readonly property var encryptionKeys: ["none",
                                           "aes-256-ctr-hmac-sha256",
                                           "des-cbc-hmac-sha256"]
    readonly property var encryptionLabels: backupOptions.encryptionLabels
    readonly property string encryptionKeyValue:
        page.encryptionKeys[page.draftEncryptionIndex]
    // 组合校验问共享真值表；文案由 backupOptions 翻译成中文。
    readonly property string optionError: backupOptions.validate(
        "scheduled",
        page.strategyKeys[page.draftStrategyIndex],
        page.packKeys[page.draftPackIndex],
        page.compressionKeys[page.draftCompressionIndex],
        page.encryptionKeyValue,
        page.draftCredentialFile.length > 0
            || archivePasswordField.text.length > 0)

    // 频率单位来自控制器（背后是共享核心的分钟边界），界面不自己定义单位表。
    // 单位表来自控制器，背后是共享核心的分钟边界（分钟 / 小时 / 天）。页面
    // 不自己写 60 或 1440，否则核心改了换算规则就会出现两个版本的"一天"。
    readonly property var frequencyUnits: schedule.frequencyUnits()
    readonly property var frequencyUnitKeys: frequencyUnits.map(
        function (unit) { return unit.key })
    readonly property var frequencyUnitLabels: frequencyUnits.map(
        function (unit) { return unit.label })

    // 备份方式的短解释：只解释当前选中的那一种，措辞与实时页逐字一致。
    readonly property string strategyHelper: page.draftStrategyIndex === 1
        ? "增量备份：首次建立完整基线，之后只保存变化，更节省空间。"
        : "完整备份：每次生成一份可以独立恢复的完整备份。"

    readonly property bool draftDirty: page.draftSource !== schedule.sourcePath
        || page.draftFrequencyValue !== schedule.frequencyValueText
        || page.draftFrequencyUnit !== schedule.frequencyUnitKey
        || page.draftRetain !== String(schedule.retainCount)
        || page.strategyKeys[page.draftStrategyIndex] !== schedule.strategyKey
        || page.packKeys[page.draftPackIndex] !== schedule.packKey
        || page.compressionKeys[page.draftCompressionIndex]
           !== schedule.compressionKey
        || page.encryptionKeys[page.draftEncryptionIndex]
           !== schedule.encryptionKey
        || page.draftCredentialFile !== schedule.credentialFile
        || page.draftRepositoryType !== schedule.repositoryType
        || page.draftCloudCredentialsFile !== schedule.cloudCredentialsFile
        || page.draftCloudRepositoryId !== schedule.cloudRepositoryId
        || ruleEditor.rulesSignature !== page.savedRulesSignature

    // 启停操作与保存设置共用同一条提交路径。传入的 enabledValue 是本次
    // 要写入的实际状态；只有核心保存成功后，控制器状态和界面才会改变。
    function saveDraft(enabledValue) {
        page.savingConfig = true
        try {
            if (!page.prepareCredentials())
                return false
            let targetId = page.draftCloudRepositoryId
            if (page.draftRepositoryType === "cloud"
                    && targetId.length === 0
                    && page.draftCloudCredentialsFile.length > 0) {
                targetId = schedule.cloudRepositoryIdForFile(
                    page.draftCloudCredentialsFile)
                page.draftCloudRepositoryId = targetId
            }
            if (!schedule.setRepositoryTarget(
                        page.draftRepositoryType,
                        page.draftCloudCredentialsFile,
                        targetId))
                return false
            if (!schedule.setEncryptionChoice(
                        page.encryptionKeyValue,
                        page.draftCredentialFile))
                return false
            const saved = schedule.saveConfigFromFrequencyText(
                enabledValue,
                page.draftSource,
                page.draftFrequencyValue,
                page.draftFrequencyUnit,
                page.draftRetain,
                page.packKeys[page.draftPackIndex],
                page.compressionKeys[page.draftCompressionIndex],
                ruleEditor.includeRuleTexts,
                ruleEditor.excludeRuleTexts,
                page.strategyKeys[page.draftStrategyIndex])
            if (saved) {
                if (page.encryptionKeyValue === "none"
                        || page.draftCredentialFile.indexOf(
                            "/@backup-project-session/") !== 0)
                    schedule.lockArchivePassword()
                page.syncFromController()
            }
            return saved
        } finally {
            page.savingConfig = false
        }
    }

    // 控制器 -> 草稿的单向同步。只在页面完成加载和已保存配置的指纹变化时
    // 调用：用户在输入框里打字期间不会被后台的属性刷新覆盖掉草稿。
    function syncFromController() {
        page.draftEnabled = schedule.enabled
        page.draftSource = schedule.sourcePath
        page.draftFrequencyValue = schedule.frequencyValueText
        page.draftFrequencyUnit = schedule.frequencyUnitKey
        page.draftRetain = String(schedule.retainCount)
        page.draftStrategyIndex =
            Math.max(0, page.strategyKeys.indexOf(schedule.strategyKey))
        page.draftPackIndex =
            Math.max(0, page.packKeys.indexOf(schedule.packKey))
        page.draftCompressionIndex =
            Math.max(0, page.compressionKeys.indexOf(schedule.compressionKey))
        page.draftEncryptionIndex =
            Math.max(0, page.encryptionKeys.indexOf(schedule.encryptionKey))
        page.draftCredentialFile = schedule.credentialFile
        page.draftRepositoryType = schedule.repositoryType
        page.draftCloudCredentialsFile = schedule.cloudCredentialsFile
        page.draftCloudRepositoryId = schedule.cloudRepositoryId
        page.passwordFileExpanded = page.draftCredentialFile.length > 0
            && page.draftCredentialFile.indexOf("/@backup-project-session/") !== 0
        page.cloudFileExpanded = page.draftCloudCredentialsFile.length > 0
            && page.draftCloudCredentialsFile !== remote.sharedSessionReference
        // 落盘配置里的规则读进共享编辑器（校验仍然走共享 builder）。读不懂时
        // 编辑器会显示共享核心给出的原因，这里不吞掉它。
        ruleEditor.loadRules(schedule.includeRules, schedule.excludeRules)
    }

    // 已保存配置的"指纹"：它一变就说明控制器那边的配置换了（保存成功，
    // 或者有人用 backupctl 改了同一份 store），草稿跟着重置。
    //
    // 用属性变化处理函数而不是 Connections：Connections 在 Qt 6 里由
    // QtQml 提供，静态检查会因为没 import 那个模块而报"未找到类型"；
    // 一条普通绑定既够用，也不需要为了一个信号多引一个模块。
    readonly property string savedSignature: [
        schedule.enabled, schedule.sourcePath, schedule.intervalMinutes,
        schedule.retainCount, schedule.strategyKey,
        schedule.packKey, schedule.compressionKey,
        schedule.encryptionKey, schedule.credentialFile,
        schedule.repositoryType, schedule.cloudCredentialsFile,
        schedule.cloudRepositoryId,
        schedule.includeRules.join(","), schedule.excludeRules.join(",")
    ].join("|")
    readonly property string savedRulesSignature:
        schedule.includeRules.join("\n")
        + "\u0000" + schedule.excludeRules.join("\n")
    onSavedSignatureChanged: {
        if (!page.savingConfig)
            syncFromController()
    }

    Component.onCompleted: syncFromController()

    // 滚动容器：contentWidth 绑定 availableWidth，让内部 ColumnLayout 能按
    // 可用宽度自己居中；StopAtBounds 关掉越界回弹，桌面端更像普通设置页。
    // 横向滚动条永远关闭——布局在最小宽度以上是自适应的，出现横向滚动反而
    // 说明窗口被拖得比设计下限更窄。
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
            // 居中且限宽：左右至少留 32 像素，内容最宽 1400 像素。宽屏上
            // 不让表单拉成一条横线，窄屏上也不会贴边。
            x: Math.max(32, (pageScroll.availableWidth - width) / 2)
            y: 20
            width: Math.min(pageScroll.availableWidth - 64, 1400)
            spacing: 16

            Text {
                text: "定时备份"
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
                // 运行范围声明：定时只在程序运行期间生效。这是产品承诺，
                // 放在主流程里；实现细节（进程检查、仓库）留给高级设置。
                objectName: "scheduleRunScopeText"
                Layout.fillWidth: true
                text: "定时任务只在本程序运行期间执行：" +
                      "关掉程序就不会再触发，重新打开后会按计划继续。"
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
                            color: schedule.enabled
                                    ? theme.success
                                   : theme.textDisabled
                        }

                        Text {
                            text: schedule.enabled
                                   ? "定时备份已启用"
                                  : "定时备份已停用"
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            color: theme.textPrimary
                        }

                        Item { Layout.fillWidth: true }

                        AppButton {
                            objectName: "scheduleEnabledToggle"
                            text: schedule.enabled ? "停用" : "启用"
                            variant: schedule.enabled ? "secondary" : "primary"
                            enabled: !schedule.libraryBusy
                            onClicked: page.saveDraft(!schedule.enabled)
                        }
                    }

                    Text {
                        visible: page.draftDirty
                        text: "改动尚未保存"
                        font.pixelSize: 14
                        color: theme.warning
                    }

                    Text {
                        text: "目标仓库"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.topMargin: 2
                    }

                    SegmentedTabs {
                        objectName: "scheduleTargetTabs"
                        Layout.preferredWidth: 360
                        enabled: !schedule.libraryBusy
                        currentKey: page.draftRepositoryType
                        model: [
                            { "key": "local", "text": "本地仓库" },
                            { "key": "cloud", "text": "云端仓库" }
                        ]
                        onActivated: function (key) {
                            page.draftRepositoryType = key
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.draftRepositoryType === "cloud"
                        spacing: 6

                        Text {
                            objectName: "scheduleCloudSessionStatus"
                            Layout.fillWidth: true
                            text: page.draftCloudCredentialsFile.length > 0
                                  && page.draftCloudCredentialsFile !== remote.sharedSessionReference
                                  ? "已选择高级凭据文件，将在执行时验证连接。"
                                  : !remote.sharedSessionReady
                                  ? "尚未登录云端，请先在备份管理中登录。重启后需要重新登录。"
                                  : page.draftCloudRepositoryId.length > 0
                                    && page.draftCloudCredentialsFile === remote.sharedSessionReference
                                    && page.draftCloudRepositoryId !== remote.sharedSessionRepositoryId
                                    ? "当前登录连接已更换，请确认后重新选择。"
                                    : "已登录云端，本次会话可直接使用。"
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            AppButton {
                                objectName: "scheduleUseCloudSessionButton"
                                text: "使用已登录连接"
                                enabled: remote.sharedSessionReady && !schedule.libraryBusy
                                onClicked: page.useLoggedInConnection()
                            }
                            AppButton {
                                objectName: "scheduleCloudFileToggle"
                                text: page.cloudFileExpanded ? "收起高级设置" : "高级：凭据文件"
                                variant: "flat"
                                onClicked: page.cloudFileExpanded = !page.cloudFileExpanded
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: page.cloudFileExpanded
                            spacing: 6
                            Text {
                                text: "云端凭据文件（仅本人可读）"
                                color: theme.textSecondary
                                font.pixelSize: 14
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                AppTextField {
                                    objectName: "scheduleCloudCredentialsField"
                                    Layout.fillWidth: true
                                    text: page.draftCloudCredentialsFile === remote.sharedSessionReference
                                          ? "" : page.draftCloudCredentialsFile
                                    onTextEdited: {
                                        page.draftCloudCredentialsFile = text
                                        page.draftCloudRepositoryId = ""
                                    }
                                    placeholderText: "选择云端凭据文件的绝对路径"
                                }
                                AppButton {
                                    text: "验证连接设置"
                                    enabled: page.draftCloudCredentialsFile.length > 0
                                    onClicked: {
                                        const id = schedule.cloudRepositoryIdForFile(
                                            page.draftCloudCredentialsFile)
                                        if (id.length > 0)
                                            page.draftCloudRepositoryId = id
                                    }
                                }
                            }
                            Text {
                                objectName: "scheduleCloudRepositoryId"
                                Layout.fillWidth: true
                                text: page.draftCloudRepositoryId.length > 0
                                      ? "仓库标识：" + page.draftCloudRepositoryId
                                      : "尚未验证连接设置"
                                font.pixelSize: 13
                                color: theme.textSecondary
                                wrapMode: Text.WrapAnywhere
                            }
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
                            placeholderText: "输入目录路径，" +
                                             "或点击“选择目录”选择"
                            text: page.draftSource
                            onTextEdited: page.draftSource = text
                        }

                        AppButton {
                            // 起始目录从控制器要，URL 与本地路径的换算也在
                            // 控制器里：跨平台细节不进 QML。
                            objectName: "browseScheduleSourceButton"
                            text: "选择目录"
                            iconName: "folder"
                            enabled: !schedule.libraryBusy
                            onClicked: {
                                sourceDialog.currentFolder =
                                    schedule.directoryDialogStartUrl(
                                        page.draftSource)
                                sourceDialog.open()
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        // 计划目录会持久化到 store，和备份页上临时输入的路径
                        // 是两回事；这句话就是用来消除这个常见误解的。
                        text: "计划有它自己的备份目录，" +
                              "不会跟随备份页上临时输入的路径。"
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
                            // 显示 label、存 key。换算成 interval_minutes 是
                            // 核心的职责，存储 schema 不因此变化。
                            objectName: "scheduleFrequencyUnitCombo"
                            implicitWidth: 130
                            enabled: !schedule.libraryBusy
                            model: page.frequencyUnitLabels
                            currentIndex: Math.max(
                                0, page.frequencyUnitKeys.indexOf(
                                    page.draftFrequencyUnit))
                            onActivated: page.draftFrequencyUnit =
                                page.frequencyUnitKeys[currentIndex]
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        objectName: "scheduleFrequencyHintText"
                        Layout.fillWidth: true
                        text: "例如“每 1 小时”“每 2 天”。" +
                              "程序负责换算成分钟，" +
                              "命令行里读到的也是同一个值。"
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
                            // 同频率：保留数量也按文本交给 C++ 解析。这是
                            // 计划自己的保留策略，与手动删除归档是两回事。
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
                            // 一次调用提交全部字段：控制器要么整份接受、要么
                            // 整份拒绝并给出原因，不会出现"保存了一半"的配置。
                            objectName: "saveScheduleButton"
                            text: "保存设置"
                            variant: "primary"
                            enabled: !schedule.libraryBusy
                                     && page.optionError.length === 0
                            // 频率（值 + 单位）与保留数量按**文本**交给 C++：
                            // QML 的 parseInt 会把 "12abc" 悄悄变成 12，
                            // 而 backupctl 会明确拒绝它。解析规则只有一份，
                            // 在共享核心里。
                            // 先把加密选择交给控制器草稿（只改内存），
                            // 再由唯一的保存入口整份提交。任何一步拒绝都不写盘。
                            onClicked: page.saveDraft(schedule.enabled)
                        }

                        AppButton {
                            // 手动触发走的是与定时器**完全相同**的评估路径；
                            // 没启用计划时禁用，避免用户以为手动跑能长期生效。
                            objectName: "runScheduleNowButton"
                            text: "立即执行一次"
                            iconName: "backup"
                            enabled: !schedule.libraryBusy && schedule.enabled
                            onClicked: schedule.runNow()
                        }

                        Item { Layout.fillWidth: true }
                    }

                    // 按钮文案不许承诺"一定产生新备份"：
                    // EvaluateNow 在没有变化时不会写出新的快照。
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
                            // 这里的默认值与核心默认值一致，所以"什么都不改"的
                            // 计划，行为与命令行建出来的计划完全相同。
                            text: "打包、压缩与筛选规则，一般保持默认即可。"
                            font.pixelSize: 15
                            color: theme.textSecondary
                            elide: Text.ElideRight
                        }

                        AppButton {
                            objectName: "scheduleAdvancedToggle"
                            text: page.advancedExpanded ? "收起 ▾" : "展开 ▸"
                            variant: "flat"
                            onClicked: page.advancedExpanded =
                                !page.advancedExpanded
                        }
                    }

                    // 折叠区：容器与区内的每个具名控件都显式跟随折叠状态，
                    // 收起时不只是"父级看不见"。折叠状态不持久化。
                    ColumnLayout {
                        // 折叠区：容器本身也显式跟随 advancedExpanded，收起时
                        // 不只是"父级看不见"——具名控件在测试里始终可寻址。
                        // 折叠状态不持久化，每次打开页面都从收起开始。
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
                                    onActivated: page.draftPackIndex =
                                        currentIndex
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
                                    onActivated: page.draftCompressionIndex =
                                        currentIndex
                                }
                            }

                            Item { Layout.fillWidth: true }
                        }

                        // 筛选规则：与备份页 / 实时页共用同一个可视化编辑器。
                        FilterRuleEditor {
                            // 与备份页、实时页共用同一个可视化规则编辑器；
                            // 规则的 DSL 文本由共享 builder 生成与校验，页面
                            // 只搬运 includeRuleTexts / excludeRuleTexts。
                            id: ruleEditor
                            ruleModel: scheduleFilterRuleModel
                            objectPrefix: "schedule"
                            busy: schedule.libraryBusy
                            heading: "筛选规则"
                            intro: "只有符合条件的文件会参与备份。" +
                                   "如果同时命中包含和排除规则，" +
                                   "以排除规则为准。"
                            Layout.fillWidth: true
                        }

                        // 加密：与快速备份、实时备份同一组 key 与显示名。
                        // 无人值守任务没有交互输入，口令只来自一个 0600 文件。
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 24
                            visible: page.advancedExpanded

                            ColumnLayout {
                                spacing: 4

                                Text {
                                    text: "加密方式"
                                    font.pixelSize: 16
                                    color: theme.textSecondary
                                }

                                AppComboBox {
                                    objectName: "scheduleEncryptionCombo"
                                    implicitWidth: 260
                                    enabled: !schedule.libraryBusy
                                    model: page.encryptionLabels
                                    currentIndex: page.draftEncryptionIndex
                                    onActivated: page.draftEncryptionIndex =
                                        currentIndex
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                visible: page.encryptionKeyValue !== "none"

                                AppTextField {
                                    id: archivePasswordField
                                    objectName: "scheduleArchivePasswordField"
                                    Layout.fillWidth: true
                                    enabled: !schedule.libraryBusy
                                    echoMode: TextInput.Password
                                    placeholderText: schedule.sessionPasswordUnlocked
                                                     ? "输入新归档密码（留空沿用已解锁密码）"
                                                     : "归档密码"
                                }

                                AppTextField {
                                    id: archivePasswordConfirmationField
                                    objectName: "scheduleArchivePasswordConfirmationField"
                                    Layout.fillWidth: true
                                    enabled: !schedule.libraryBusy
                                    echoMode: TextInput.Password
                                    placeholderText: "再次输入归档密码"
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: "保存后在本次会话内解锁；重启程序后需要重新输入密码。"
                                    color: theme.textSecondary
                                    font.pixelSize: 14
                                    wrapMode: Text.WordWrap
                                }

                                AppButton {
                                    objectName: "schedulePasswordFileToggle"
                                    text: page.passwordFileExpanded ? "收起高级设置" : "高级：口令文件"
                                    variant: "flat"
                                    onClicked: page.passwordFileExpanded = !page.passwordFileExpanded
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    visible: page.passwordFileExpanded

                                    AppTextField {
                                        objectName: "scheduleCredentialField"
                                        Layout.fillWidth: true
                                        enabled: !schedule.libraryBusy
                                        placeholderText: "选择仅本人可读的口令文件"
                                        text: page.draftCredentialFile.indexOf(
                                                  "/@backup-project-session/") === 0
                                              ? "" : page.draftCredentialFile
                                        onTextEdited: page.draftCredentialFile = text
                                    }

                                    AppButton {
                                        objectName: "scheduleCredentialBrowse"
                                        text: "选择文件"
                                        iconName: "folder"
                                        enabled: !schedule.libraryBusy
                                        onClicked: {
                                            credentialDialog.currentFolder =
                                                controller.directoryDialogStartUrl(
                                                    page.draftCredentialFile)
                                            credentialDialog.open()
                                        }
                                    }
                                }
                            }
                        }

                        Text {
                            objectName: "scheduleEncryptionNote"
                            visible: page.advancedExpanded
                            Layout.fillWidth: true
                            text: schedule.encryptionNote
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        // 组合不成立时在保存按钮上方直接说明，而不是等核心拒绝。
                        Text {
                            objectName: "scheduleOptionError"
                            visible: page.advancedExpanded
                                     && page.optionError.length > 0
                            Layout.fillWidth: true
                            text: page.optionError
                            font.pixelSize: 14
                            color: theme.error
                            wrapMode: Text.WordWrap
                        }

                    }
                }
            }

            // 纯展示区，刻意不放任何按钮：启停与立即执行都在"常用设置"里，
            // 同一动作只有一个人口，避免两处状态不一致。
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
                        // 所有时间与结果文本都由控制器预先格式化好：时间格式、
                        // 本地化、"从未运行"这类措辞只有一份实现。
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
                        // pending 是**瞬时**状态（已到期但正忙，之后会补跑），
                        // 与下面持续亮着的 suspended 是两件事，所以分两行显示。
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
                        // 读盘失败时控制器把原始原因放在 loadError，页面原样
                        // 显示：路径、权限这类信息只有用户能处理，替换成
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
                        text: "定时备份已挂起：落盘的计划配置不合法。" +
                              "程序不会自动修改它，" +
                              "也不会自动重试；请修正后重新保存设置。"
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

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "运行历史"
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            color: theme.textSecondary
                        }
                        Item { Layout.fillWidth: true }
                        AppButton {
                            objectName: "scheduleHistoryToggle"
                            visible: schedule.history.length > 3
                            text: page.historyExpanded ? "收起"
                                : "查看全部（" + schedule.history.length + "）"
                            variant: "flat"
                            onClicked: page.historyExpanded =
                                !page.historyExpanded
                        }
                    }

                    Text {
                        objectName: "scheduleHistoryEmptyText"
                        visible: schedule.history.length === 0
                        text: "还没有运行记录。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                    }

                    Column {
                        id: historyColumn
                        Layout.fillWidth: true
                        spacing: 6

                        Repeater {
                            objectName: "scheduleHistoryList"
                            // 展示顺序与数量由页面提供；外层布局只管理稳定容器。
                            model: page.visibleHistory
                            delegate: Column {
                                id: historyEntry
                                required property var modelData
                                required property int index
                                property bool detailsExpanded: false
                                width: historyColumn.width
                                height: implicitHeight
                                spacing: 2

                                Text {
                                    width: parent.width
                                    text: modelData["timeText"]
                                          + "  ·  " + modelData["resultText"]
                                    font.pixelSize: 15
                                    color: theme.textPrimary
                                    wrapMode: Text.WordWrap
                                }
                                Text {
                                    objectName: "scheduleHistorySummary" + index
                                    width: parent.width
                                    text: modelData["changesText"]
                                          + (modelData["archiveName"] !== ""
                                             ? "  ·  " + modelData["archiveName"]
                                             : "")
                                    font.pixelSize: 14
                                    color: theme.textSecondary
                                    wrapMode: Text.WordWrap
                                }
                                AppButton {
                                    objectName: "scheduleHistoryDetailsToggle" + index
                                    visible: modelData["diagnostic"] !== ""
                                    text: historyEntry.detailsExpanded
                                          ? "收起详情" : "详情"
                                    variant: "flat"
                                    onClicked: historyEntry.detailsExpanded =
                                        !historyEntry.detailsExpanded
                                }
                                Text {
                                    objectName: "scheduleHistoryDiagnostic" + index
                                    width: parent.width
                                    visible: historyEntry.detailsExpanded
                                             && modelData["diagnostic"] !== ""
                                    text: modelData["diagnostic"]
                                    font.pixelSize: 14
                                    color: theme.textSecondary
                                    wrapMode: Text.WordWrap
                                }
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
                        text: "还没有由计划任务创建的快照。" +
                              "手动备份不会出现在这里，也不会被自动淘汰。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        objectName: "scheduleManagedList"
                        // 只列 scheduler 自己管理的快照：手动备份不在其中，
                        // 也不会被保留策略淘汰。
                        model: schedule.managedSnapshots
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                // 计划快照行由控制器格式化好的字段拼成；文件名
                                // 过长时中间省略，尾部的大小与方式仍然可读。
                                text: modelData["fileName"]
                                      + "  ·  " + modelData["createdText"]
                                      + "  ·  " + modelData["sizeText"]
                                      + "  ·  " + modelData["packText"]
                                      + " / " + modelData["compressionText"]
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
                // pageScope 与 scope 都写 schedule：临时提示只属于产生它的
                // 页面，别的页面产生的横幅不会漂到这里来。
                pageScope: "schedule"
                scope: "schedule"
                kind: schedule.statusKind
                title: schedule.statusTitle
                message: schedule.statusMessage
            }

            // 末尾的弹性空白把卡片顶到上方；没有它，内容不足一屏时整列会被
            // 垂直居中，标题的位置随窗口高度上下跳。
            Item { Layout.fillHeight: true }
        }
    }

    // 目录选择对话框放在页面根节点下（而不是某个布局里）：弹窗在 Qt 里是
    // 顶层窗口，塞进布局会参与尺寸计算。选中后的 URL 转本地路径仍然交给
    // 控制器，空串表示用户选的路径无法转换，此时保持原草稿不动。
    FolderDialog {
        id: sourceDialog
        objectName: "scheduleSourceFolderDialog"
        title: "选择定时备份的备份目录"
        onAccepted: {
            const chosen =
                schedule.localPathFromUrl(sourceDialog.selectedFolder)
            if (chosen !== "")
                page.draftSource = chosen
        }
    }

    // 口令文件只选路径；内容由核心在每次运行时读取，界面从不显示它。
    FileDialog {
        id: credentialDialog
        objectName: "scheduleCredentialDialog"
        title: "选择加密口令文件"
        fileMode: FileDialog.OpenFile
        onAccepted: {
            const chosen =
                controller.localPathFromUrl(credentialDialog.selectedFile)
            if (chosen !== "")
                page.draftCredentialFile = chosen
        }
    }
}

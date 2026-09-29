// RealtimePage.qml
//
// 实时备份页：文件事件触发 + 完整快照 / 增量策略。
//
// 页面按"用户先要看什么"分五层，而不是把控制器状态一次铺开：
//   1. 常用设置 —— 启用状态、备份目录、备份方式、保留版本、保存设置
//   2. 高级设置 —— 响应延迟、最长等待、打包、压缩、筛选规则、加密说明（默认折叠）
//   3. 运行状态 —— 先给一句结论，再给必要的错误说明
//   4. 技术详情 —— 控制器给出的原始状态，排查问题时才展开（默认折叠）
//   5. 最近备份 —— 实时触发创建出来的备份版本
//
// 这一页只做四件事：展示、编辑配置、点启停、展示最近备份。
// 它不直接监听文件系统事件、不拼 repository 路径、不自己解析 Filter、不判断
// option support、也不自己执行 retention —— 全部来自 RealtimeController，
// 而 RealtimeController 背后是与 backupctl realtime 共用的同一份核心。
//
// 业务值一律来自 realtime（RealtimeController）与 theme：数字按**文本**交给
// 共享核心解析，运行状态只做显示层翻译；"该不该监听 / 该不该备份 / 某个选项
// 支不支持"都不在这一层判断。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page

    // draft 语义与自动备份页一致：输入框里的是草稿，点"保存设置"才写进控制器。
    property bool draftEnabled: realtime.enabled
    property string draftSource: realtime.sourcePath
    property string draftDebounce: String(realtime.debounceMs)
    property string draftMaxWait: String(realtime.maxWaitMs)
    property string draftRetain: String(realtime.retainCount)
    property int draftStrategyIndex: 0
    property int draftPackIndex: 0
    property int draftCompressionIndex: 0

    // 两个折叠区都默认收起，而且**不持久化**：折叠是这一屏的临时视图状态，
    // 不是配置；重新进入页面时回到"常用设置 + 运行状态优先"的默认样子。
    property bool advancedExpanded: false
    property bool technicalExpanded: false

    readonly property var strategyKeys: ["full", "incremental"]
    readonly property var strategyLabels: ["完整备份", "增量备份"]
    readonly property var packKeys: ["mypack", "ustar", "fast-ustar"]
    readonly property var packLabels: ["MyPack（推荐）", "USTAR（兼容格式）", "Fast USTAR（兼容格式）"]
    readonly property var compressionKeys: ["none", "huffman", "lzss-huffman"]
    readonly property var compressionLabels: ["不压缩", "Huffman", "LZSS + Huffman"]

    // 备份方式的短解释：只解释当前选中的那一种，避免两个术语同时出现。
    // 措辞与自动备份页逐字一致（人工验收：三个页面统一产品语言）。
    readonly property string strategyHelper: page.draftStrategyIndex === 1
        ? "增量备份：首次建立完整基线，之后只保存变化，更节省空间。"
        : "完整备份：每次生成一份可以独立恢复的完整备份。"

    // 草稿与已保存配置是否一致。只用来提示"这次改动还没生效"，
    // 不参与任何"能不能保存"的判断 —— 那个判断在共享核心里。
    readonly property bool draftDirty: page.draftEnabled !== realtime.enabled
        || page.draftSource !== realtime.sourcePath
        || page.draftDebounce !== String(realtime.debounceMs)
        || page.draftMaxWait !== String(realtime.maxWaitMs)
        || page.draftRetain !== String(realtime.retainCount)
        || page.strategyKeys[page.draftStrategyIndex] !== realtime.strategyKey
        || page.packKeys[page.draftPackIndex] !== realtime.packKey
        || page.compressionKeys[page.draftCompressionIndex] !== realtime.compressionKey
        || ruleEditor.rulesSignature !== page.savedRulesSignature

    function syncFromController() {
        page.draftEnabled = realtime.enabled
        page.draftSource = realtime.sourcePath
        page.draftDebounce = String(realtime.debounceMs)
        page.draftMaxWait = String(realtime.maxWaitMs)
        page.draftRetain = String(realtime.retainCount)
        page.draftStrategyIndex = Math.max(0, page.strategyKeys.indexOf(realtime.strategyKey))
        page.draftPackIndex = Math.max(0, page.packKeys.indexOf(realtime.packKey))
        page.draftCompressionIndex = Math.max(0, page.compressionKeys.indexOf(realtime.compressionKey))
        // 落盘配置里的规则读进共享编辑器（校验仍然走共享 builder）。读不懂时
        // 编辑器会显示共享核心给出的原因，这里不吞掉它。
        ruleEditor.loadRules(realtime.includeRules, realtime.excludeRules)
    }

    // 已保存配置里的规则文本，用作"改动尚未保存"的比较基准。分隔符与编辑器
    // 的 rulesSignature 一致，两边是同一套拼接方式。
    readonly property string savedRulesSignature: realtime.includeRules.join("\n")
        + "\u0000" + realtime.excludeRules.join("\n")

    // 已保存配置的"指纹"：它一变就说明控制器那边的配置换了（保存成功，或者
    // 有人用 backupctl realtime set 改了同一份 store），草稿跟着重置。
    readonly property string savedSignature: [
        realtime.enabled, realtime.sourcePath, realtime.debounceMs,
        realtime.maxWaitMs, realtime.retainCount, realtime.strategyKey,
        realtime.packKey, realtime.compressionKey,
        realtime.includeRules.join(","), realtime.excludeRules.join(",")
    ].join("|")
    onSavedSignatureChanged: syncFromController()

    Component.onCompleted: syncFromController()

    // ---------- 运行状态：显示层翻译 ----------
    //
    // phaseKey 的取值由 RealtimeController 定义（disabled / watching /
    // debouncing / resync / snapshot_created / no_changes / retention_warning /
    // watch_degraded / watch_recovered / config_error / failed）。这里只把它
    // 翻译成一句人话，不新增判断条件：什么时候算"在监听"、什么时候算"不可用"，
    // 事实全部来自控制器；watchStateText 里那句人类可读的原因也是控制器给的。
    function runStateFor(phaseKey, watchDegraded, watchStateText) {
        if (phaseKey === "config_error")
            return { glyph: "⚠", tone: "error", title: "实时备份已暂停", detail: "" }
        if (phaseKey === "failed")
            return { glyph: "⚠", tone: "error", title: "上一次实时备份没有成功",
                     detail: "失败原因见页面下方的提示；处理之后重新保存设置即可继续。" }
        if (watchDegraded || phaseKey === "watch_degraded")
            return { glyph: "⚠", tone: "warning", title: "监听暂时不可用",
                     detail: watchStateText }
        if (phaseKey === "debouncing")
            return { glyph: "●", tone: "busy", title: "正在等待文件稳定",
                     detail: "已检测到变化，稍后开始备份。" }
        if (phaseKey === "resync")
            return { glyph: "●", tone: "busy", title: "正在备份",
                     detail: "正在创建新的实时备份。" }
        if (phaseKey === "snapshot_created")
            return { glyph: "●", tone: "ok", title: "刚刚完成一次备份",
                     detail: "新的实时备份已经写入备份仓库。" }
        if (phaseKey === "retention_warning")
            return { glyph: "⚠", tone: "warning", title: "备份已完成，旧版本没有清理完",
                     detail: "新的备份已经写入；保留策略这一次没有全部执行成功。" }
        if (phaseKey === "no_changes")
            return { glyph: "●", tone: "ok", title: "正在监听",
                     detail: "刚才的变化已经检查过，没有需要新备份的内容。" }
        if (phaseKey === "watch_recovered")
            return { glyph: "●", tone: "ok", title: "正在监听",
                     detail: "监听已经恢复，文件变化后会自动备份。" }
        if (phaseKey === "watching")
            return { glyph: "●", tone: "ok", title: "正在监听",
                     detail: "文件变化后会自动备份。" }
        return { glyph: "●", tone: "idle", title: "未启用", detail: "尚未开始实时监听。" }
    }

    readonly property var runState: page.runStateFor(realtime.phaseKey,
                                                    realtime.watchDegraded,
                                                    realtime.watchStateText)
    readonly property color runStateColor: {
        const tone = page.runState.tone
        if (tone === "ok")
            return theme.success
        if (tone === "busy")
            return theme.accent
        if (tone === "warning")
            return theme.warning
        if (tone === "error")
            return theme.error
        return theme.textDisabled
    }
    readonly property string runStateGlyph: page.runState.glyph
    readonly property string runStateTitle: page.runState.title
    readonly property string runStateDetail: page.runState.detail

    ScrollView {
        id: pageScroll
        objectName: "realtimePageScroll"
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
                text: "实时备份"
                font.pixelSize: 30
                font.weight: Font.DemiBold
                color: theme.textPrimary
            }

            // 副标题面向普通用户：只讲"它能帮我做什么"，不讲它怎么实现。
            // 架构层面的说明在下面的「技术详情」里。
            Text {
                objectName: "realtimeSubtitleText"
                Layout.fillWidth: true
                text: "文件发生变化后会自动创建备份，省去手动操作。程序关闭期间不会监听；"
                      + "重新打开后会自动同步这段时间的变化。"
                font.pixelSize: 17
                color: theme.textSecondary
                wrapMode: Text.WordWrap
                Layout.topMargin: -8
            }

            // ---------- 1. 常用设置 ----------
            AppCard {
                Layout.fillWidth: true
                Layout.topMargin: 4

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

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
                            text: page.draftEnabled ? "实时备份已启用" : "实时备份已停用"
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            color: theme.textPrimary
                        }

                        Text {
                            visible: page.draftDirty
                            text: "改动尚未保存"
                            font.pixelSize: 14
                            color: theme.warning
                        }

                        Item { Layout.fillWidth: true }

                        AppButton {
                            objectName: "realtimeEnabledToggle"
                            text: page.draftEnabled ? "停用" : "启用"
                            variant: page.draftEnabled ? "secondary" : "primary"
                            enabled: !realtime.libraryBusy
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
                            objectName: "realtimeSourceField"
                            Layout.fillWidth: true
                            enabled: !realtime.libraryBusy
                            placeholderText: "输入目录路径，或点击“选择目录”选择"
                            text: page.draftSource
                            onTextEdited: page.draftSource = text
                        }

                        AppButton {
                            objectName: "browseRealtimeSourceButton"
                            text: "选择目录"
                            iconName: "folder"
                            enabled: !realtime.libraryBusy
                            onClicked: {
                                sourceDialog.currentFolder = realtime.directoryDialogStartUrl(page.draftSource)
                                sourceDialog.open()
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "备份目录与备份仓库不能互相包含，否则备份写出的归档会被当成下一次变化。"
                        font.pixelSize: 14
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        text: "备份方式"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.topMargin: 2
                    }

                    AppComboBox {
                        id: strategyBox
                        objectName: "realtimeStrategyCombo"
                        implicitWidth: 200
                        enabled: !realtime.libraryBusy
                        model: page.strategyLabels
                        currentIndex: page.draftStrategyIndex
                        onActivated: page.draftStrategyIndex = currentIndex
                    }

                    Text {
                        Layout.fillWidth: true
                        text: page.strategyHelper
                        font.pixelSize: 14
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

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
                            objectName: "realtimeRetainField"
                            implicitWidth: 110
                            enabled: !realtime.libraryBusy
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
                        Layout.topMargin: 4
                        spacing: 12

                        AppButton {
                            objectName: "saveRealtimeButton"
                            text: "保存设置"
                            variant: "primary"
                            enabled: !realtime.libraryBusy
                            // 三个数字按**文本**交给 C++：QML 的 parseInt 会把
                            // "12abc" 悄悄变成 12，而 backupctl 会明确拒绝它。
                            // 解析规则只有一份，在共享核心里。
                            onClicked: realtime.saveConfigFromText(
                                page.draftEnabled,
                                page.draftSource,
                                page.draftDebounce,
                                page.draftMaxWait,
                                page.draftRetain,
                                page.packKeys[page.draftPackIndex],
                                page.compressionKeys[page.draftCompressionIndex],
                                ruleEditor.includeRuleTexts,
                                ruleEditor.excludeRuleTexts,
                                page.strategyKeys[page.draftStrategyIndex])
                        }

                        Item { Layout.fillWidth: true }
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
                            text: "延迟、打包、压缩与筛选规则，一般保持默认即可。"
                            font.pixelSize: 15
                            color: theme.textSecondary
                            elide: Text.ElideRight
                        }

                        AppButton {
                            objectName: "realtimeAdvancedToggle"
                            text: page.advancedExpanded ? "收起 ▾" : "展开 ▸"
                            variant: "flat"
                            onClicked: page.advancedExpanded = !page.advancedExpanded
                        }
                    }

                    // 折叠区。容器与区内的每个控件都显式跟随折叠状态：
                    // 收起时它们的 visible 都是 false（不只是"父级看不见"），
                    // 展开后可见可交互。折叠状态不持久化。
                    ColumnLayout {
                        id: advancedSection
                        objectName: "realtimeAdvancedSection"
                        Layout.fillWidth: true
                        spacing: 14
                        visible: page.advancedExpanded

                        // ---- 响应延迟 ----
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            // 主标签只用中文：英文术语（Debounce）退到「技术详情」
                            // 与源码注释里，普通用户不需要看见它。
                            Text {
                                text: "响应延迟"
                                font.pixelSize: 16
                                color: theme.textSecondary
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                AppTextField {
                                    id: debounceField
                                    objectName: "realtimeDebounceField"
                                    visible: page.advancedExpanded
                                    implicitWidth: 130
                                    enabled: !realtime.libraryBusy
                                    text: page.draftDebounce
                                    onTextEdited: page.draftDebounce = text
                                }
                                Text {
                                    text: "ms"
                                    font.pixelSize: 16
                                    color: theme.textSecondary
                                }
                                Item { Layout.fillWidth: true }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "文件停止变化多久后开始备份。默认 500 毫秒。"
                                font.pixelSize: 14
                                color: theme.textSecondary
                                wrapMode: Text.WordWrap
                            }
                        }

                        // ---- 最长等待 ----
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "最长等待"
                                font.pixelSize: 16
                                color: theme.textSecondary
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                AppTextField {
                                    id: maxWaitField
                                    objectName: "realtimeMaxWaitField"
                                    visible: page.advancedExpanded
                                    implicitWidth: 130
                                    enabled: !realtime.libraryBusy
                                    text: page.draftMaxWait
                                    onTextEdited: page.draftMaxWait = text
                                }
                                Text {
                                    text: "ms"
                                    font.pixelSize: 16
                                    color: theme.textSecondary
                                }
                                Item { Layout.fillWidth: true }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "文件持续写入时，最多等这么久就先备份一次。默认 5 秒。"
                                font.pixelSize: 14
                                color: theme.textSecondary
                                wrapMode: Text.WordWrap
                            }
                        }

                        // ---- 打包 / 压缩 ----
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 24

                            ColumnLayout {
                                spacing: 4

                                Text {
                                    text: "打包方式"
                                    font.pixelSize: 16
                                    color: theme.textSecondary
                                }
                                AppComboBox {
                                    id: packBox
                                    objectName: "realtimePackCombo"
                                    visible: page.advancedExpanded
                                    implicitWidth: 190
                                    enabled: !realtime.libraryBusy
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
                                    objectName: "realtimeCompressionCombo"
                                    visible: page.advancedExpanded
                                    implicitWidth: 210
                                    enabled: !realtime.libraryBusy
                                    model: page.compressionLabels
                                    currentIndex: page.draftCompressionIndex
                                    onActivated: page.draftCompressionIndex = currentIndex
                                }
                            }

                            Item { Layout.fillWidth: true }
                        }

                        // ---- 筛选规则 ----
                        //
                        // 以前这里是两个 raw DSL 输入框（"例如 ext:cpp;h" +
                        // "添加包含规则"），普通用户被要求自己写语法。现在换成
                        // 与备份页、自动备份页**同一个** FilterRuleEditor：
                        // 选条件类型、填取值，DSL 由共享 builder 生成。
                        FilterRuleEditor {
                            id: ruleEditor
                            ruleModel: realtimeFilterRuleModel
                            objectPrefix: "realtime"
                            busy: realtime.libraryBusy
                            heading: "筛选规则"
                            intro: "只有符合条件的文件会参与备份。如果同时命中包含和排除规则，以排除规则为准。"
                            Layout.fillWidth: true
                        }

                        // ---- 加密：一句弱提示，不再摆一个永远点不动的下拉框 ----
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: "加密"
                                font.pixelSize: 16
                                color: theme.textSecondary
                            }
                            Text {
                                objectName: "realtimeEncryptionText"
                                visible: page.advancedExpanded
                                text: "暂不支持"
                                font.pixelSize: 16
                                color: theme.textDisabled
                            }
                            Item { Layout.fillWidth: true }
                        }

                        // 说明来自控制器（控制器读的是核心里那句唯一来源），
                        // 页面不复制一份字面量。
                        Text {
                            objectName: "realtimeEncryptionNote"
                            visible: page.advancedExpanded
                            Layout.fillWidth: true
                            text: realtime.encryptionNote
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // ---------- 3. 运行状态（先给结论） ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Text {
                        text: "运行状态"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: page.runStateGlyph
                            font.pixelSize: 18
                            color: page.runStateColor
                        }
                        Text {
                            objectName: "realtimePhaseText"
                            Layout.fillWidth: true
                            text: page.runStateTitle
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            color: page.runStateColor
                            wrapMode: Text.WordWrap
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: page.runStateDetail !== ""
                        text: page.runStateDetail
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "realtimeLoadErrorText"
                        Layout.fillWidth: true
                        visible: realtime.loadError !== ""
                        text: realtime.loadError
                        font.pixelSize: 15
                        color: theme.error
                        wrapMode: Text.WordWrap
                    }

                    // 配置不可用是**持续状态**，与状态栏里那一句瞬时提示不同：
                    // 只要理由还在，这行就一直亮着。
                    Text {
                        objectName: "realtimeConfigErrorText"
                        Layout.fillWidth: true
                        visible: realtime.phaseKey === "config_error"
                        text: "实时备份已挂起：落盘配置不可用或源目录/仓库当前无法使用。"
                              + "程序不会自动修改它，也不会自动重试；请修正后重新保存。"
                        font.pixelSize: 15
                        color: theme.error
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ---------- 4. 技术详情（默认折叠） ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Text {
                            text: "技术详情"
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            color: theme.textSecondary
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "控制器给出的原始状态，排查问题时才需要看。"
                            font.pixelSize: 15
                            color: theme.textSecondary
                            elide: Text.ElideRight
                        }

                        AppButton {
                            objectName: "realtimeTechnicalToggle"
                            text: page.technicalExpanded ? "收起 ▾" : "展开 ▸"
                            variant: "flat"
                            onClicked: page.technicalExpanded = !page.technicalExpanded
                        }
                    }

                    ColumnLayout {
                        id: technicalSection
                        objectName: "realtimeTechnicalSection"
                        Layout.fillWidth: true
                        spacing: 6
                        visible: page.technicalExpanded

                        Text {
                            objectName: "realtimeSupportedModeText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: realtime.supportedModeText
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "realtimeRunScopeText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: "实时监听只在“本程序运行期间”生效：关掉程序就不再监听，"
                                  + "重开时会先做一次重新同步，把关掉那段时间的变化补上。"
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "realtimeRawPhaseText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: "控制器状态：" + realtime.phaseText
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "realtimeWatchText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: realtime.watchStateText
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 24

                            Text {
                                objectName: "realtimeWatchCountText"
                                visible: page.technicalExpanded
                                text: "监听目录数：" + realtime.watchCount
                                font.pixelSize: 14
                                color: theme.textSecondary
                            }

                            Text {
                                objectName: "realtimePendingCountText"
                                visible: page.technicalExpanded
                                text: "待处理事件数：" + realtime.pendingEventCount
                                font.pixelSize: 14
                                color: theme.textSecondary
                            }

                            Item { Layout.fillWidth: true }
                        }

                        Text {
                            objectName: "realtimePendingText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: realtime.pendingStateText
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "realtimeOverflowText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: realtime.overflowStateText
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "realtimeLastEventText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: realtime.lastEventText
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        // 英文术语只在这里出现一次：普通 UI 讲"响应延迟 / 最长等待"，
                        // 需要对着文档或源码排查的人在这一层能拿到原名。
                        Text {
                            objectName: "realtimeDelayTermText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: "响应延迟对应 Debounce（debounce_ms），最长等待对应 Max wait（max_wait_ms）。"
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "realtimeLastSnapshotText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: "最近一次产出：" + realtime.lastSnapshotText
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "realtimeRepositoryText"
                            visible: page.technicalExpanded
                            Layout.fillWidth: true
                            text: realtime.repositoryConfigured
                                  ? "备份仓库：" + realtime.repositoryPath
                                  : "备份仓库：尚未配置（请在设置页选择仓库目录）"
                            font.pixelSize: 14
                            color: realtime.repositoryConfigured ? theme.textSecondary : theme.error
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // ---------- 5. 最近备份 ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Text {
                            text: "最近备份"
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            color: theme.textSecondary
                        }

                        Item { Layout.fillWidth: true }

                        AppButton {
                            objectName: "refreshRealtimeSnapshotsButton"
                            text: "刷新"
                            iconName: "refresh"
                            enabled: !realtime.libraryBusy
                            onClicked: realtime.refreshSnapshots()
                        }
                    }

                    Text {
                        objectName: "realtimeSnapshotEmptyText"
                        Layout.fillWidth: true
                        visible: realtime.snapshots.length === 0
                        text: "还没有实时备份。启用后，文件发生变化时会在这里看到新的备份版本。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        objectName: "realtimeSnapshotList"
                        model: realtime.snapshots
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                // 来源 badge：文案直接用控制器给的分类（完整快照 /
                                // 增量基线 / 增量），页面不自己判断策略。
                                Rectangle {
                                    implicitWidth: badgeLabel.implicitWidth + 16
                                    implicitHeight: badgeLabel.implicitHeight + 6
                                    radius: 6
                                    color: theme.accentSoft

                                    Text {
                                        id: badgeLabel
                                        anchors.centerIn: parent
                                        text: modelData["kindText"]
                                        font.pixelSize: 13
                                        color: theme.accent
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData["createdText"] + "  ·  " + modelData["sizeText"]
                                    font.pixelSize: 15
                                    color: theme.textPrimary
                                    elide: Text.ElideRight
                                }

                                Text {
                                    visible: !modelData["verified"]
                                    text: modelData["verifiedText"]
                                    font.pixelSize: 14
                                    color: theme.error
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: modelData["archiveName"] + "  ·  " + modelData["basisText"]
                                      + "  ·  " + modelData["packText"] + " / "
                                      + modelData["compressionText"]
                                      + "  ·  事件 " + modelData["eventCount"]
                                font.pixelSize: 14
                                color: theme.textSecondary
                                elide: Text.ElideMiddle
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: modelData["diagnostic"] !== ""
                                text: modelData["diagnostic"]
                                font.pixelSize: 14
                                color: theme.error
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
            }

            // 实时页的状态来自 RealtimeController，只有这一页绑定它；pageScope
            // 仍然写全，让"临时提示只属于产生它的页面"这条契约在五处保持一致。
            StatusBanner {
                objectName: "realtimeStatusBanner"
                Layout.fillWidth: true
                pageScope: "realtime"
                scope: "realtime"
                kind: realtime.statusKind
                title: realtime.statusTitle
                message: realtime.statusMessage
            }

            Item { Layout.fillHeight: true }
        }
    }

    FolderDialog {
        id: sourceDialog
        objectName: "realtimeSourceFolderDialog"
        title: "选择实时备份的备份目录"
        onAccepted: {
            const chosen = realtime.localPathFromUrl(sourceDialog.selectedFolder)
            if (chosen !== "")
                page.draftSource = chosen
        }
    }
}

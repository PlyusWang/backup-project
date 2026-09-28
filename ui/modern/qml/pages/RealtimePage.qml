// RealtimePage.qml
//
// 实时备份页：文件事件触发 + 完整快照 / 增量策略。
//
// 这一页只做四件事：展示、编辑配置、点启停、展示最近实时快照。
// 它不直接调 inotify、不拼 repository 路径、不自己解析 Filter、不判断
// option support、也不自己执行 retention —— 全部来自 RealtimeController，
// 而 RealtimeController 背后是与 backupctl realtime 共用的同一份核心。
//
// 页面上每一个能点的东西都是真的：加密选择器是**置灰**的，因为它确实不可选
// （无人值守没有持久密钥来源），而不是"暂时藏起来"。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page

    // draft 语义与自动备份页一致：输入框里的是草稿，点"保存实时配置"才写进
    // 控制器。
    property bool draftEnabled: realtime.enabled
    property string draftSource: realtime.sourcePath
    property string draftDebounce: String(realtime.debounceMs)
    property string draftMaxWait: String(realtime.maxWaitMs)
    property string draftRetain: String(realtime.retainCount)
    property int draftStrategyIndex: 0
    property int draftPackIndex: 0
    property int draftCompressionIndex: 0
    property var draftInclude: []
    property var draftExclude: []
    property string draftIncludeInput: ""
    property string draftExcludeInput: ""

    readonly property var strategyKeys: ["full", "incremental"]
    readonly property var strategyLabels: ["完整备份", "增量备份"]
    readonly property var packKeys: ["mypack", "ustar", "fast-ustar"]
    readonly property var packLabels: ["MyPack", "USTAR", "Fast USTAR"]
    readonly property var compressionKeys: ["none", "huffman", "lzss-huffman"]
    readonly property var compressionLabels: ["不压缩", "Huffman", "LZSS + Huffman"]

    function syncFromController() {
        page.draftEnabled = realtime.enabled
        page.draftSource = realtime.sourcePath
        page.draftDebounce = String(realtime.debounceMs)
        page.draftMaxWait = String(realtime.maxWaitMs)
        page.draftRetain = String(realtime.retainCount)
        page.draftInclude = realtime.includeRules
        page.draftExclude = realtime.excludeRules
        page.draftStrategyIndex = Math.max(0, page.strategyKeys.indexOf(realtime.strategyKey))
        page.draftPackIndex = Math.max(0, page.packKeys.indexOf(realtime.packKey))
        page.draftCompressionIndex = Math.max(0, page.compressionKeys.indexOf(realtime.compressionKey))
    }

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

            Text {
                objectName: "realtimeSupportedModeText"
                Layout.fillWidth: true
                text: realtime.supportedModeText
                font.pixelSize: 17
                color: theme.textSecondary
                wrapMode: Text.WordWrap
                Layout.topMargin: -8
            }

            Text {
                objectName: "realtimeRunScopeText"
                Layout.fillWidth: true
                text: "实时监听只在“本程序运行期间”生效：关掉程序就不再监听，"
                      + "重开时会先做一次重新同步，把关掉那段时间的变化补上。"
                font.pixelSize: 15
                color: theme.textSecondary
                wrapMode: Text.WordWrap
            }

            // ---------- 实时配置 ----------
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
                            text: "启用实时备份"
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            color: theme.textSecondary
                        }

                        AppButton {
                            objectName: "realtimeEnabledToggle"
                            text: page.draftEnabled ? "已启用" : "已停用"
                            variant: page.draftEnabled ? "primary" : "secondary"
                            enabled: !realtime.libraryBusy
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
                            objectName: "realtimeSourceField"
                            Layout.fillWidth: true
                            enabled: !realtime.libraryBusy
                            placeholderText: "输入目录路径，或点击“浏览目录”选择"
                            text: page.draftSource
                            onTextEdited: page.draftSource = text
                        }

                        AppButton {
                            objectName: "browseRealtimeSourceButton"
                            text: "浏览目录"
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
                        text: "实时备份有它自己的源目录，不会跟随备份页上临时输入的路径；"
                              + "源目录与备份仓库不允许互相包含（否则备份写出的归档会变成下一次事件）。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        spacing: 10

                        Text {
                            text: "Debounce"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                        AppTextField {
                            id: debounceField
                            objectName: "realtimeDebounceField"
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

                        Text {
                            text: "Max wait"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                        AppTextField {
                            id: maxWaitField
                            objectName: "realtimeMaxWaitField"
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

                        Text {
                            text: "保留数量"
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
                            text: "份实时快照"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "Debounce 是事件合并窗口（100..60000 ms）；Max wait 是硬上限"
                              + "（500..300000 ms，且不小于 Debounce）——持续写入最迟在 Max wait"
                              + " 到期时形成一次检查点，不会被无限推迟。范围与判断全部来自共享核心。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        spacing: 10

                        Text {
                            text: "策略"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                        AppComboBox {
                            id: strategyBox
                            objectName: "realtimeStrategyCombo"
                            implicitWidth: 190
                            enabled: !realtime.libraryBusy
                            model: page.strategyLabels
                            currentIndex: page.draftStrategyIndex
                            onActivated: page.draftStrategyIndex = currentIndex
                        }

                        Text {
                            text: "打包"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                        AppComboBox {
                            id: packBox
                            objectName: "realtimePackCombo"
                            implicitWidth: 190
                            enabled: !realtime.libraryBusy
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
                            objectName: "realtimeCompressionCombo"
                            implicitWidth: 210
                            enabled: !realtime.libraryBusy
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
                        // 置灰，不是隐藏：它确实不可选，原因写在下一行。
                        AppComboBox {
                            objectName: "realtimeEncryptionCombo"
                            implicitWidth: 210
                            enabled: false
                            model: ["不加密"]
                            currentIndex: 0
                        }
                        Text {
                            objectName: "realtimeEncryptionText"
                            text: "不加密"
                            font.pixelSize: 16
                            color: theme.textSecondary
                        }
                    }

                    Text {
                        objectName: "realtimeEncryptionNote"
                        Layout.fillWidth: true
                        text: realtime.encryptionNote
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
                        text: "语法与备份页完全一致，规则由核心的 Filter 解析并校验；"
                              + "这里不做第二套解析。"
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
                                enabled: !realtime.libraryBusy
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
                            objectName: "realtimeIncludeField"
                            Layout.fillWidth: true
                            enabled: !realtime.libraryBusy
                            placeholderText: "例如 ext:cpp;h"
                            text: page.draftIncludeInput
                            onTextEdited: page.draftIncludeInput = text
                        }
                        AppButton {
                            objectName: "addRealtimeIncludeButton"
                            text: "添加 include"
                            enabled: !realtime.libraryBusy && page.draftIncludeInput !== ""
                            onClicked: {
                                const error = realtime.validateRule("include", page.draftIncludeInput)
                                if (error !== "") {
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
                            objectName: "realtimeExcludeField"
                            Layout.fillWidth: true
                            enabled: !realtime.libraryBusy
                            placeholderText: "例如 path:**/build/**"
                            text: page.draftExcludeInput
                            onTextEdited: page.draftExcludeInput = text
                        }
                        AppButton {
                            objectName: "addRealtimeExcludeButton"
                            text: "添加 exclude"
                            enabled: !realtime.libraryBusy && page.draftExcludeInput !== ""
                            onClicked: {
                                const error = realtime.validateRule("exclude", page.draftExcludeInput)
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
                        objectName: "realtimeInvalidRuleText"
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
                            objectName: "saveRealtimeButton"
                            text: "保存实时配置"
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
                                page.draftInclude,
                                page.draftExclude,
                                page.strategyKeys[page.draftStrategyIndex])
                        }

                        AppButton {
                            objectName: "refreshRealtimeSnapshotsButton"
                            text: "刷新快照列表"
                            iconName: "refresh"
                            enabled: !realtime.libraryBusy
                            onClicked: realtime.refreshSnapshots()
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
                        objectName: "realtimePhaseText"
                        Layout.fillWidth: true
                        text: "当前状态：" + realtime.phaseText
                        font.pixelSize: 16
                        color: theme.textPrimary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "realtimeWatchText"
                        Layout.fillWidth: true
                        text: realtime.watchStateText
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "realtimeWatchCountText"
                        Layout.fillWidth: true
                        text: "监听目录数：" + realtime.watchCount
                        font.pixelSize: 15
                        color: theme.textSecondary
                    }

                    Text {
                        objectName: "realtimePendingCountText"
                        Layout.fillWidth: true
                        text: "待处理事件数：" + realtime.pendingEventCount
                        font.pixelSize: 15
                        color: theme.textSecondary
                    }

                    Text {
                        objectName: "realtimePendingText"
                        Layout.fillWidth: true
                        text: realtime.pendingStateText
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "realtimeOverflowText"
                        Layout.fillWidth: true
                        text: realtime.overflowStateText
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "realtimeLastEventText"
                        Layout.fillWidth: true
                        text: realtime.lastEventText
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "realtimeLastSnapshotText"
                        Layout.fillWidth: true
                        text: "最近一次产出：" + realtime.lastSnapshotText
                        font.pixelSize: 15
                        color: theme.textPrimary
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "realtimeRepositoryText"
                        Layout.fillWidth: true
                        text: realtime.repositoryConfigured
                              ? "备份仓库：" + realtime.repositoryPath
                              : "备份仓库：尚未配置（请在设置页选择仓库目录）"
                        font.pixelSize: 15
                        color: realtime.repositoryConfigured ? theme.textSecondary : theme.error
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

            // ---------- 最近实时快照 ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "最近实时快照（只列带 .realtime 标记的那些）"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    Text {
                        objectName: "realtimeSnapshotEmptyText"
                        visible: realtime.snapshots.length === 0
                        text: "还没有实时触发创建的快照。手动备份与定时备份不会出现在这里，"
                              + "也不会被实时保留策略淘汰。"
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
                            Text {
                                Layout.fillWidth: true
                                text: modelData["createdText"] + "  ·  "
                                      + modelData["strategyText"] + "  ·  "
                                      + modelData["basisText"] + "（" + modelData["kindText"] + "）"
                                      + "  ·  " + modelData["packText"] + " / " + modelData["compressionText"]
                                      + "  ·  事件 " + modelData["eventCount"]
                                      + "  ·  " + modelData["sizeText"]
                                font.pixelSize: 15
                                color: theme.textPrimary
                                wrapMode: Text.WordWrap
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData["archiveName"]
                                      + (modelData["verified"] ? "" : "  ·  " + modelData["verifiedText"])
                                      + (modelData["diagnostic"] !== "" ? "  ·  " + modelData["diagnostic"] : "")
                                font.pixelSize: 14
                                color: modelData["verified"] ? theme.textSecondary : theme.error
                                elide: Text.ElideMiddle
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
        title: "选择实时备份的源目录"
        onAccepted: {
            const chosen = realtime.localPathFromUrl(sourceDialog.selectedFolder)
            if (chosen !== "")
                page.draftSource = chosen
        }
    }
}

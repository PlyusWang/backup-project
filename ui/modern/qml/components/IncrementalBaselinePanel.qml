import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

AppCard {
    id: baselinePanel
    required property var optionsPanel
    property string selectedFileName: ""

    Connections {
        target: controller
        function onBaselineCandidatesChanged() {
            baselinePanel.selectedFileName = ""
        }
    }
    Connections {
        target: baselinePanel.optionsPanel
        function onStrategyKeyChanged() { controller.invalidateBaselineCandidates() }
        function onPackKeyChanged() { controller.invalidateBaselineCandidates() }
        function onCompressionKeyChanged() { controller.invalidateBaselineCandidates() }
        function onEncryptionKeyChanged() { controller.invalidateBaselineCandidates() }
        function onPasswordChanged() { controller.invalidateBaselineCandidates() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Text {
            text: "增量基线"
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: theme.textPrimary
        }
        Text {
            Layout.fillWidth: true
            text: baselinePanel.selectedFileName.length > 0
                ? "已选择：" + baselinePanel.selectedFileName
                : "自动选择最近可验证且参数兼容的快照；没有可用基线时建立完整备份。"
            wrapMode: Text.Wrap
            font.pixelSize: 15
            color: theme.textSecondary
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            AppButton {
                objectName: "checkIncrementalBaselinesButton"
                text: controller.baselineScanBusy ? "正在验证…" : "检查可用基线"
                enabled: !controller.busy && !controller.baselineScanBusy
                onClicked: controller.refreshBaselineCandidates(
                    optionsPanel.packKey, optionsPanel.compressionKey,
                    optionsPanel.encryptionKey, optionsPanel.password)
            }
            AppButton {
                objectName: "useAutomaticBaselineButton"
                text: "恢复自动选择"
                enabled: baselinePanel.selectedFileName.length > 0 && !controller.busy
                onClicked: baselinePanel.selectedFileName = ""
            }
        }
        Text {
            Layout.fillWidth: true
            visible: controller.baselineScanError.length > 0
            text: controller.baselineScanError
            font.pixelSize: 15
            color: theme.error
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: controller.baselineScanBusy
            text: "正在读取归档并验证完整依赖链，较大的仓库可能需要一些时间。"
            color: theme.textSecondary
            font.pixelSize: 14
            wrapMode: Text.Wrap
        }
        ListView {
            id: candidateList
            objectName: "incrementalBaselineCandidates"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 420)
            visible: count > 0
            clip: true
            spacing: 12
            boundsBehavior: Flickable.StopAtBounds
            model: controller.baselineCandidates
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: ColumnLayout {
                id: candidateRow
                required property var modelData
                property bool showDetails: false
                width: candidateList.width - 16
                spacing: 5
                Text {
                    Layout.fillWidth: true
                    text: candidateRow.modelData.fileName
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: theme.textPrimary
                    wrapMode: Text.WrapAnywhere
                }
                Text {
                    Layout.fillWidth: true
                    text: "文件时间：" + candidateRow.modelData.modifiedTime + " · "
                        + candidateRow.modelData.kind + " · "
                        + candidateRow.modelData.encryption
                    font.pixelSize: 14
                    color: theme.textSecondary
                    wrapMode: Text.Wrap
                }
                Text {
                    Layout.fillWidth: true
                    text: candidateRow.modelData.reason
                    font.pixelSize: 14
                    color: theme.textSecondary
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    AppButton {
                        objectName: "selectIncrementalBaselineButton"
                        readonly property string candidateFileName: candidateRow.modelData.fileName
                        text: baselinePanel.selectedFileName === candidateRow.modelData.fileName
                            ? "已选择" : "选择此基线"
                        enabled: candidateRow.modelData.usable && !controller.busy
                        onClicked: baselinePanel.selectedFileName = candidateRow.modelData.fileName
                    }
                    AppButton {
                        text: candidateRow.showDetails ? "收起链信息" : "查看链信息"
                        onClicked: candidateRow.showDetails = !candidateRow.showDetails
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: candidateRow.showDetails
                    text: "Snapshot ID：" + (candidateRow.modelData.snapshotId || "不可验证")
                        + "\n父快照：" + (candidateRow.modelData.parentFileName || "无")
                        + "\nParent ID：" + (candidateRow.modelData.parentSnapshotId || "无")
                        + "\nLineage ID：" + (candidateRow.modelData.lineageId || "不可验证")
                        + "\nManifest SHA-256：" + (candidateRow.modelData.manifestDigest || "不可验证")
                    font.pixelSize: 13
                    color: theme.textSecondary
                    wrapMode: Text.WrapAnywhere
                }
            }
        }
    }
}

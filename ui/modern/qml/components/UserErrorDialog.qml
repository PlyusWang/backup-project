// UserErrorDialog.qml
//
// 用户主动操作失败时的唯一主要反馈。
//
// 结构固定为四段：标题 / 简短原因 / 下一步建议 / 技术详情（默认折叠）。
// 技术详情只给开发与运维看，可能包含路径、错误码和底层原因；它永远不含
// 口令、私钥、会话密钥或登录令牌——这一条由生产错误的 C++ 侧保证，
// 本组件只负责显示收起与展开。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dialog

    property string errorTitle: ""
    property string summary: ""
    property string suggestion: ""
    property string technical: ""
    property bool technicalExpanded: false

    anchors.centerIn: parent
    modal: true
    padding: 20
    closePolicy: Popup.CloseOnEscape
    // 宽度跟窗口走，但夹在 360..560 之间：太窄中文会逐字换行，
    // 太宽则一行文字过长。
    width: Math.min(560, Math.max(360, parent ? parent.width - 160 : 420))

    function showError(title, summaryText, suggestionText, technicalText) {
        dialog.errorTitle = title
        dialog.summary = summaryText
        dialog.suggestion = suggestionText
        dialog.technical = technicalText
        dialog.technicalExpanded = false
        dialog.open()
    }

    background: Rectangle {
        color: theme.surfaceElevated
        border.width: 1
        border.color: theme.border
        radius: 12
    }

    contentItem: ColumnLayout {
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            AppIcon {
                name: "warning"
                size: 20
                color: theme.error
            }

            Text {
                objectName: "userErrorTitle"
                Layout.fillWidth: true
                text: dialog.errorTitle
                color: theme.textPrimary
                font.pixelSize: 18
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
        }

        Text {
            objectName: "userErrorSummary"
            Layout.fillWidth: true
            text: dialog.summary
            color: theme.textPrimary
            font.pixelSize: 16
            wrapMode: Text.WordWrap
        }

        Text {
            objectName: "userErrorSuggestion"
            Layout.fillWidth: true
            visible: text.length > 0
            text: dialog.suggestion
            color: theme.textSecondary
            font.pixelSize: 15
            wrapMode: Text.WordWrap
        }

        AppButton {
            objectName: "userErrorTechnicalToggle"
            Layout.fillWidth: true
            visible: dialog.technical.length > 0
            variant: "flat"
            text: dialog.technicalExpanded ? "收起详情" : "查看详情"
            onClicked: dialog.technicalExpanded = !dialog.technicalExpanded
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(180, technicalText.implicitHeight + 20)
            visible: dialog.technicalExpanded
            color: theme.background
            radius: 8
            border.width: 1
            border.color: theme.border
            clip: true

            ScrollView {
                anchors.fill: parent
                anchors.margins: 10
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                TextEdit {
                    id: technicalText
                    objectName: "userErrorTechnical"
                    text: dialog.technical
                    readOnly: true
                    selectByMouse: true
                    cursorVisible: false
                    textFormat: TextEdit.PlainText
                    color: theme.textSecondary
                    font.pixelSize: 14
                    wrapMode: TextEdit.WrapAnywhere
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 10

            Item { Layout.fillWidth: true }

            AppButton {
                objectName: "userErrorClose"
                text: "关闭"
                variant: "primary"
                onClicked: dialog.close()
            }
        }
    }
}

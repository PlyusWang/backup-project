// StatusBanner.qml
//
// 操作状态反馈：等待 / 进行中 / 成功 / 失败四种。
// 失败时正文就是核心返回的 error_message 原文，不做二次包装。

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: banner

    property string kind: "idle"
    property string title: ""
    property string message: ""

    readonly property color tone: {
        if (kind === "success")
            return theme.success
        if (kind === "error")
            return theme.error
        if (kind === "running")
            return theme.accent
        return theme.textSecondary
    }
    readonly property string glyph: {
        if (kind === "success")
            return "check"
        if (kind === "error")
            return "warning"
        return "app"
    }

    implicitHeight: layout.implicitHeight + 26
    radius: 8
    color: kind === "idle" ? "transparent" : Qt.rgba(tone.r, tone.g, tone.b, theme.dark ? 0.14 : 0.09)
    border.width: kind === "idle" ? 0 : 1
    border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.35)
    visible: title !== "" || message !== ""

    RowLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 13
        spacing: 10

        AppIcon {
            name: banner.glyph
            size: 18
            color: banner.tone
            Layout.alignment: Qt.AlignTop
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3

            Text {
                text: banner.title
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: banner.kind === "idle" ? theme.textSecondary : banner.tone
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            Text {
                text: banner.message
                visible: banner.message !== ""
                font.pixelSize: 12
                color: theme.textSecondary
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
                // 错误里常带很长的绝对路径，允许选中复制，方便贴给别人看。
                textFormat: Text.PlainText
            }
        }
    }
}

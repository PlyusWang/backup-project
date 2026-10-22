// StatusBanner.qml
//
// 操作状态反馈：等待 / 进行中 / 成功 / 失败四种。
// 失败时正文是可以给用户看的中文说明；完整技术细节由错误对话框承载。

// 职责边界：纯展示组件——消息由外部（controller / remote / schedule 的 status*
// 属性）注入，本组件不请求、不超时、不自动清除；何时回到 idle 由生产方决定。
// kind 词表：idle / running / success / error；未知取值退化成中性观感，不报错。
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: banner

    property string kind: "idle"
    property string title: ""
    property string message: ""
    // 这条消息属于哪一页（"" = 全局消息），以及这个 banner 自己属于哪一页。
    property string scope: ""
    property string pageScope: ""
    readonly property bool inScope: scope === "" || scope === pageScope
    readonly property bool showsMessage:
        inScope && (title !== "" || message !== "")

    // tone 与 glyph 都是 kind 的纯函数。
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

    // 高度只由内容列决定。内容列显式给宽度（父宽 - 左右各 13），不四边锚定，
    // 因此不会出现 "高度 -> 布局 -> 高度" 的 polish 环。
    implicitHeight: contentColumn.implicitHeight + 26
    radius: 8
    color: kind === "idle"
        ? "transparent"
        : Qt.rgba(tone.r, tone.g, tone.b, theme.dark ? 0.14 : 0.09)
    border.width: kind === "idle" ? 0 : 1
    border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.35)
    visible: showsMessage

    ColumnLayout {
        id: contentColumn
        x: 13
        y: 13
        width: parent.width - 26
        spacing: 3

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            AppIcon {
                name: banner.glyph
                size: 18
                color: banner.tone
                Layout.alignment: Qt.AlignTop
            }

            Text {
                text: banner.title
                font.pixelSize: 16
                font.weight: Font.DemiBold
                color: banner.kind === "idle"
                    ? theme.textSecondary
                    : banner.tone
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // 错误正文常带很长的绝对路径，必须能选中复制。Text 本身不支持选择，
        // 所以用只读 TextEdit：背景透明、无边框，不选中时与普通文字一致。
        TextEdit {
            text: banner.message
            visible: banner.message !== ""
            readOnly: true
            selectByMouse: true
            selectByKeyboard: true
            persistentSelection: true
            cursorVisible: false
            // 强制纯文本：错误正文来自文件系统与解析器，属不可信内容，
            // 绝不能按富文本渲染。
            textFormat: TextEdit.PlainText
            font.pixelSize: 15
            color: theme.textSecondary
            selectionColor: theme.accent
            selectedTextColor: theme.surface
            Layout.fillWidth: true
            wrapMode: TextEdit.WrapAnywhere
        }
    }
}

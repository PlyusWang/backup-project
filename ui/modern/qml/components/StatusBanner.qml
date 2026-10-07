// StatusBanner.qml
//
// 操作状态反馈：等待 / 进行中 / 成功 / 失败四种。
// 失败时正文就是核心返回的 error_message 原文，不做二次包装。

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
    // 这条消息属于哪一页（"" = 全局消息：空闲基线、"正在备份/恢复"这类全局
    // 运行状态），以及这个 banner 自己属于哪一页。
    // severity（kind）与 scope 正交：颜色和图标只看 kind，
    // 显示在哪一页只看这两个 scope 是否相等
    // —— 四种 severity 因此走完全相同的生命周期。
    property string scope: ""
    property string pageScope: ""
    readonly property bool inScope: scope === "" || scope === pageScope
    // "这一页该不该显示这条消息"——只看 scope 与内容，
    // 不看所在的页面此刻是不是当前页。visible 读出来的是**有效可见性**
    // （非当前页整体不可见），所以自动化测试要断言"这页会不会显示"，
    // 读的是这一位。
    readonly property bool showsMessage:
        inScope && (title !== "" || message !== "")

    // tone 与 glyph 都是 kind 的纯函数：颜色和图标只从 kind 推导，与 scope、
    // 文本长度无关。新增 kind 只需在这两处加分支，其余绑定自动跟随。
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

    // 26 = 上下各 13 的 anchors.margins，必须与下面 layout 的 margins 保持
    // 一致，否则文字会被裁掉或底部多出一条空白。
    implicitHeight: layout.implicitHeight + 26
    radius: 8
    // 底色不用固定色板，而把 tone 按低透明度叠上去：新增 kind 不必再补一套
    // 主题色。深色取 0.14、浅色取 0.09——同样的 alpha 叠在深底上对比更弱，
    // 需要更高的不透明度才有同等可见度。
    color: kind === "idle"
        ? "transparent"
        : Qt.rgba(tone.r, tone.g, tone.b, theme.dark ? 0.14 : 0.09)
    border.width: kind === "idle" ? 0 : 1
    border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.35)
    visible: showsMessage

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

            // 标题用 WordWrap（按词折行）；正文不能照抄——错误信息里最常见的
            // 是一整条没有空格的绝对路径，WordWrap 会把它直接顶出卡片。
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

            // 错误正文常带很长的绝对路径，必须能选中复制。Text 本身不支持
            // 选择，所以用只读的 TextEdit：背景透明、无边框，字号与颜色不变，
            // 不选中时和一段普通文字看不出区别。
            TextEdit {
                text: banner.message
                visible: banner.message !== ""
                readOnly: true
                selectByMouse: true
                selectByKeyboard: true
                persistentSelection: true
                cursorVisible: false
                // 强制纯文本：error_message 来自文件系统与解析器，属不可信
                // 内容，绝不能按富文本渲染——正文里出现尖括号时 RichText 会
                // 去解析标签，甚至尝试加载外部资源。
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
}

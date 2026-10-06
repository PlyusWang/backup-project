// AppButton.qml
//
// 三种形态（primary / secondary / flat）共用一套状态样式：
// 现代感来自“同一套 hover / pressed 规则”，而不是每个按钮各写一套。

// 职责边界：只负责“按钮长什么样”，不接业务、不弹窗、不改 enabled——调用方用
// text / iconName / enabled 描述状态，点击语义留在 onClicked 里。
// variant 词表（只有这三个值有意义；拼错会静默退化成 secondary，没有告警）：
//   primary   强调色底 + 纯白字，用于每屏唯一的主操作
//   secondary 容器色底 + 1px 边框（默认值），用于次级操作
//   flat      完全透明、无边框，用于工具条与行内的图标按钮
import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: control

    property string variant: "secondary"
    property string iconName: ""
    property int iconSize: 16
    // hoverColor / pressedColor 是留给调用方的覆盖点（危险操作换成 error
    // 色调时用）；primary 忽略它们：主按钮的深浅变化必须与强调色成套。
    property color hoverColor: theme.hover
    property color pressedColor: theme.pressed
    // primary / flat 这两个派生位是所有样式的唯一判据：颜色、边框、留白都读
    // 它们，避免“什么算主按钮”在十几处绑定里各写一遍字符串比较。
    readonly property bool primary: variant === "primary"
    readonly property bool flat: variant === "flat"

    // 主按钮更高（46 vs 42）：层级差异在静态截图里也要看得出来，而不是只有
    // hover 才暴露。宽度下限 96 给短标签留着，flat 的横向留白更小（22）。
    implicitHeight: primary ? 46 : 42
    implicitWidth: Math.max(96, contentRoot.implicitWidth + (flat ? 22 : 40))
    padding: 0
    // hoverEnabled 必须显式打开：AbstractButton 默认不跟踪悬停，覆盖层依赖的
    // control.hovered 会恒为 false——没有 hover 反馈，而且不会报错。
    hoverEnabled: true

    background: Rectangle {
        radius: 9
        // 主按钮用强调色，次级按钮用容器色，flat 完全透明——
        // 三者的层级差异靠背景而不是靠边框粗细来区分。
        // 这里只留"常态底色"：hover / pressed 交给下面的固定色覆盖层，
        // 因为 "transparent" 和不透明色之间做 ColorAnimation 会逐分量插值出
        // "半透明黑"，浅色主题下鼠标划过就是那一闪的"黑一下"。
        color: {
            if (!control.enabled)
                return control.primary
                    ? theme.border
                    : (control.flat ? "transparent" : theme.surface)
            if (control.primary)
                return theme.accent
            return control.flat ? "transparent" : theme.surface
        }
        border.width: (control.primary || control.flat) ? 0 : 1
        border.color: theme.border

        // hover 覆盖层：固定色 + 只动画 opacity，
        // flat 从全透明淡入也不再经过黑色。
        // 自带一圈与 background 同色同宽的边框：
        // 覆盖层画在 background 边框之上，不补这一圈会把次级按钮的
        // 1px 描边盖掉（深色主题下描边比底色亮，最明显）。
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: control.primary ? theme.accentHover : control.hoverColor
            border.width: (control.primary || control.flat) ? 0 : 1
            border.color: theme.border
            opacity: control.enabled && control.hovered ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 110 } }
        }

        // pressed 覆盖层：画在 hover 之上，保证 pressed 优先于 hover。
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: control.primary ? theme.accentPressed : control.pressedColor
            border.width: (control.primary || control.flat) ? 0 : 1
            border.color: theme.border
            opacity: control.enabled && control.pressed ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 110 } }
        }
    }

    contentItem: Item {
        id: contentRoot
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 7

            AppIcon {
                visible: control.iconName !== ""
                name: control.iconName
                size: control.iconSize
                color: control.primary
                    ? "#ffffff"
                    : (control.enabled ? theme.textPrimary : theme.textDisabled)
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                id: label
                text: control.text
                anchors.verticalCenter: parent.verticalCenter
                font.pixelSize: 16
                font.weight: control.primary ? Font.DemiBold : Font.Normal
                color: {
                    if (!control.enabled)
                        return theme.textDisabled
                    if (control.primary)
                        return "#ffffff"
                    return control.flat && !control.hovered
                        ? theme.textSecondary
                        : theme.textPrimary
                }
            }
        }
    }
}

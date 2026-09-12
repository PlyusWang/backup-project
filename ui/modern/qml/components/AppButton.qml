// AppButton.qml
//
// 三种形态（primary / secondary / flat）共用一套状态样式：
// 现代感来自“同一套 hover / pressed 规则”，而不是每个按钮各写一套。

import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: control

    property string variant: "secondary"
    property string iconName: ""
    property int iconSize: 16
    property color hoverColor: theme.hover
    property color pressedColor: theme.pressed
    readonly property bool primary: variant === "primary"
    readonly property bool flat: variant === "flat"

    implicitHeight: primary ? 36 : 34
    implicitWidth: Math.max(80, contentRoot.implicitWidth + (flat ? 16 : 32))
    padding: 0
    hoverEnabled: true

    background: Rectangle {
        radius: 7
        // 主按钮用强调色，次级按钮用容器色，flat 完全透明——
        // 三者的层级差异靠背景而不是靠边框粗细来区分。
        color: {
            if (!control.enabled)
                return control.primary ? theme.border : (control.flat ? "transparent" : theme.surface)
            if (control.primary)
                return control.pressed ? theme.accentPressed : (control.hovered ? theme.accentHover : theme.accent)
            if (control.flat)
                return control.pressed ? control.pressedColor : (control.hovered ? control.hoverColor : "transparent")
            return control.pressed ? control.pressedColor : (control.hovered ? control.hoverColor : theme.surface)
        }
        border.width: (control.primary || control.flat) ? 0 : 1
        border.color: theme.border

        // 状态色变化时给一点点过渡，避免鼠标划过时颜色“跳”。
        Behavior on color { ColorAnimation { duration: 110 } }
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
                color: control.primary ? "#ffffff" : (control.enabled ? theme.textPrimary : theme.textDisabled)
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                id: label
                text: control.text
                anchors.verticalCenter: parent.verticalCenter
                font.pixelSize: 13
                font.weight: control.primary ? Font.DemiBold : Font.Normal
                color: {
                    if (!control.enabled)
                        return theme.textDisabled
                    if (control.primary)
                        return "#ffffff"
                    return control.flat && !control.hovered ? theme.textSecondary : theme.textPrimary
                }
            }
        }
    }
}

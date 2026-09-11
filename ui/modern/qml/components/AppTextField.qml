// AppTextField.qml
//
// 输入框：普通 / hover / focus / disabled 四态。
// 这里不做任何路径加工（trim、补斜杠等），用户填什么就存什么——
// Linux 下空格和大小写都是文件名的一部分，清洗反而会改坏路径。

import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: control

    implicitHeight: 36
    leftPadding: 11
    rightPadding: 11
    font.pixelSize: 13
    color: enabled ? theme.textPrimary : theme.textDisabled
    placeholderTextColor: theme.textSecondary
    selectionColor: theme.accent
    selectedTextColor: "#ffffff"
    selectByMouse: true
    hoverEnabled: true
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        radius: 7
        color: theme.surface
        // 聚焦时只换边框颜色，不加发光：发光在深色主题里最容易显得廉价。
        border.width: control.activeFocus ? 2 : 1
        border.color: {
            if (!control.enabled)
                return theme.border
            if (control.activeFocus)
                return theme.accent
            return control.hovered ? theme.textDisabled : theme.border
        }
        Behavior on border.color { ColorAnimation { duration: 110 } }
    }
}

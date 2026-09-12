// NavItem.qml
//
// 侧栏导航项。选中态用低透明度强调色 + 强调色文字 + 左侧细 indicator，
// 而不是“按钮底色 + 整圈边框”——后者一眼就是表单按钮，不像导航。

import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: control

    property string iconName: ""

    implicitHeight: 38
    padding: 0
    hoverEnabled: true

    background: Rectangle {
        radius: 7
        color: {
            if (control.checked)
                return theme.accentSoft
            return control.hovered ? theme.hover : "transparent"
        }
        Behavior on color { ColorAnimation { duration: 110 } }

        // 选中指示条：靠边框实现，不需要自绘，也不会在切换时让文字位移。
        Rectangle {
            width: 3
            height: parent.height - 14
            radius: 1.5
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: theme.accent
            visible: control.checked
        }
    }

    contentItem: Row {
        id: row
        spacing: 10
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.verticalCenter: parent.verticalCenter

        AppIcon {
            name: control.iconName
            size: 18
            color: control.checked ? theme.accent : theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: control.text
            font.pixelSize: 13
            font.weight: control.checked ? Font.DemiBold : Font.Normal
            color: control.checked ? theme.accent : theme.textPrimary
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}

// NavItem.qml
//
// 侧栏导航项。选中态用低透明度强调色 + 强调色文字 + 左侧细 indicator，
// 而不是“按钮底色 + 整圈边框”——后者一眼就是表单按钮，不像导航。

import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: control

    property string iconName: ""

    implicitHeight: 46
    padding: 0
    hoverEnabled: true

    background: Rectangle {
        radius: 7
        // 底色不参与颜色动画：所有随交互变化的面都拆成"固定主题色 + 只动画
        // opacity"的覆盖层。"transparent" 是 RGBA(0,0,0,0)，拿它和不透明色做
        // ColorAnimation 会逐分量插值出"半透明黑"——浅色主题下就是鼠标划过 /
        // 切换选中项时那一闪的"黑一下"。深色主题底色本就暗，所以几乎看不出。
        color: "transparent"

        // 选中底色。
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: theme.accentSoft
            opacity: control.checked ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 110 } }
        }

        // hover 底色：选中态刻意不给 hover 反馈（与改动前一致）。
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: theme.hover
            opacity: control.hovered && !control.checked ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 110 } }
        }

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
            font.pixelSize: 16
            font.weight: control.checked ? Font.DemiBold : Font.Normal
            color: control.checked ? theme.accent : theme.textPrimary
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}

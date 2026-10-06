// NavItem.qml
//
// 侧栏导航项。选中态用低透明度强调色 + 强调色文字 + 左侧细 indicator，
// 而不是“按钮底色 + 整圈边框”——后者一眼就是表单按钮，不像导航。

// 职责边界：只画“一个导航项现在长什么样”，不持有页面状态、不做跳转。选中与否由
// 调用方绑定 checked（Main.qml 里是 checked: root.currentPage === N），本组件
// 从不自己改 checked——“同时只有一项被选中”这个不变量由父级维护。
import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: control

    // 图标名对应 AppIcon 的 switch 表：空串什么都不画，拼错也只是留一个空位
    // 而不会报错，所以这里的字面量要和那张表保持一致。
    property string iconName: ""

    // 与 AppButton(primary) / AppTextField 同为 46，侧栏项与表单行同高。
    // padding 0 让 background 铺满整行，点击热区等于整行宽度。
    implicitHeight: 46
    padding: 0
    // 继承 AbstractButton 是为了拿到键盘 Space/Enter 激活与 focus 链，本文件
    // 只负责外观；因此点击一律接 onClicked，不要再另套一层 MouseArea。
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

    // contentItem 用 Row + 显式 leftMargin，而不是交给布局推导：导航项永远
    // 只有“图标 + 文字”两个成员，固定缩进更可控，不随文字长度漂移。
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

// SegmentedTabs.qml
//
// 分段控件：一个圆角容器里放若干个等宽分段，选中项用强调色，其余用容器的
// 中性底色。人工验收的结论是"登录 / 注册"以前只是两个各自独立的按钮，看起来
// 像可以同时按；分段控件把它们表达成"二选一"，一眼就能看出当前在哪一页。
//
// 它**不**新造视觉体系，每一个取值都来自设计系统里已有的东西：
//   * 容器底色 = AppButton 的 secondary 形态（theme.surface + theme.border）；
//   * 选中段   = AppButton 的 primary 形态（theme.accent + 白字 + DemiBold）；
//   * hover    = AppButton 用的同一个 theme.hover，并且只动画 opacity
//                （在 transparent 与不透明色之间动画颜色会在浅色主题下闪黑）；
//   * 圆角     = 与 AppButton 一致的 9（内层减掉边距 3）。
//
// 用法：
//     SegmentedTabs {
//         model: [{ "key": "login", "text": "登录" }, ...]
//         currentKey: "login"
//         onActivated: function (key) { ... }
//     }

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: control

    // [{ key: <string>, text: <string> }, ...]
    property var model: []
    property string currentKey: ""
    signal activated(string key)

    readonly property int segmentHeight: 42

    implicitHeight: segmentHeight
    implicitWidth: Math.max(200, row.implicitWidth + 2 * containerBorder)

    // 容器内边距：它同时决定内层分段要减掉多少圆角。
    readonly property int containerBorder: 3

    Rectangle {
        id: container
        anchors.fill: parent
        radius: 9
        color: theme.surface
        border.width: 1
        border.color: theme.border

        RowLayout {
            id: row
            anchors.fill: parent
            anchors.margins: control.containerBorder
            spacing: control.containerBorder

            Repeater {
                id: repeater
                model: control.model

                delegate: AbstractButton {
                    id: segment
                    required property var modelData

                    readonly property bool active:
                        String(modelData["key"]) === control.currentKey

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    enabled: control.enabled
                    hoverEnabled: true
                    padding: 0

                    background: Rectangle {
                        radius: 6
                        color: segment.active ? theme.accent : "transparent"

                        // hover 覆盖层：固定色 + 只动画 opacity（与 AppButton 同一条规则）。
                        Rectangle {
                            anchors.fill: parent
                            radius: parent.radius
                            color: theme.hover
                            opacity: (!segment.active && segment.hovered &&
                                      segment.enabled) ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 110 } }
                        }
                    }

                    contentItem: Text {
                        text: String(modelData["text"])
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 16
                        font.weight: segment.active ? Font.DemiBold : Font.Normal
                        color: {
                            if (!segment.enabled)
                                return theme.textDisabled
                            return segment.active ? "#ffffff" : theme.textPrimary
                        }
                    }

                    onClicked: control.activated(String(modelData["key"]))
                }
            }
        }
    }

    // 键盘可达性：左右方向键在分段之间移动，回车 / 空格选中。
    focus: true
    Keys.onLeftPressed: control.step(-1)
    Keys.onRightPressed: control.step(1)

    function step(delta) {
        if (control.model.length === 0)
            return
        let index = 0
        for (let i = 0; i < control.model.length; ++i) {
            if (String(control.model[i]["key"]) === control.currentKey)
                index = i
        }
        const next = (index + delta + control.model.length) % control.model.length
        control.activated(String(control.model[next]["key"]))
    }
}

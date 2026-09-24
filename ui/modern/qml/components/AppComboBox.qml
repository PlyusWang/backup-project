// AppComboBox.qml
//
// 下拉框的统一外观：与主题同一套 token（light / dark 都走 theme），
// 并且支持鼠标滚轮直接滚动弹出列表。
//
// 为什么单独成组件：规则编辑器里有 4 个下拉（字段、类型、运算符、单位），
// 逐个写 popup 样式会重复四遍；抽出来以后只有一处需要和主题对齐。

import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control

    // 尺寸体系：字大、行高、点击区域都按桌面可读性来定。
    implicitHeight: 46
    font.pixelSize: 17
    leftPadding: 14
    rightPadding: 14
    hoverEnabled: true

    // 未展开时的外框：和 AppTextField / AppCard 用同一套底色与圆角。
    background: Rectangle {
        radius: 9
        color: control.enabled ? theme.surface : theme.background
        border.width: control.activeFocus || control.popup.visible ? 2 : 1
        border.color: {
            if (!control.enabled)
                return theme.border
            if (control.popup.visible || control.activeFocus)
                return theme.accent
            return control.hovered ? theme.textDisabled : theme.border
        }
    }

    contentItem: Text {
        leftPadding: 0
        rightPadding: control.indicator.width + 6
        text: control.displayText
        font: control.font
        color: control.enabled ? theme.textPrimary : theme.textDisabled
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: control.width - width - 12
        y: control.topPadding + (control.availableHeight - height) / 2
        text: "▾"
        font.pixelSize: 16
        color: control.enabled ? theme.textSecondary : theme.textDisabled
    }

    // 弹出列表：背景、边框、圆角、hover / selected 全部跟随主题。
    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(contentList.contentHeight + 12, 320)
        padding: 6

        background: Rectangle {
            radius: 10
            color: theme.surfaceElevated
            border.width: 1
            border.color: theme.border
        }

        contentItem: ListView {
            id: contentList
            clip: true
            implicitHeight: contentHeight + 12
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            flickDeceleration: 2200

            // 鼠标停在下拉列表上时，滚轮必须能滚动：列表自己处理滚轮，
            // 不把事件冒泡给后面的页面。
            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function (event) {
                    const max = Math.max(0, contentList.contentHeight - contentList.height)
                    contentList.contentY = Math.max(0, Math.min(max, contentList.contentY - event.angleDelta.y))
                    event.accepted = true
                }
            }

            ScrollBar.vertical: ScrollBar {
                policy: contentList.contentHeight > contentList.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            }
        }

        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 }
        }
    }

    // 每一行：行高、padding、hover、选中态都用主题色，字比默认大。
    delegate: ItemDelegate {
        id: row
        required property var modelData
        required property int index

        width: control.width - 12
        implicitHeight: 44
        highlighted: control.highlightedIndex === index

        contentItem: Text {
            leftPadding: 10
            text: row.modelData
            font.pixelSize: 17
            color: theme.textPrimary
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: 7
            color: {
                if (row.highlighted)
                    return theme.hover
                if (row.hovered)
                    return theme.hover
                return "transparent"
            }
        }
    }
}

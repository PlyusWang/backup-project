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

    // 每一行：行高、padding 与**三种互不相同的状态**。
    //
    // 人工验收：鼠标移过某一项之后，把它移开（甚至移出下拉菜单）那块灰底不会
    // 消失。根因是这里曾经把两个不同的概念混成了同一块灰色：
    //
    //   row.highlighted —— 绑到 control.highlightedIndex，这是 Qt 的**常驻**索引：
    //                      下拉一打开就被设成当前已选择项（键盘导航的起点），
    //                      ↑/↓ 移动它，它也不会因为鼠标离开而回到 -1。
    //   row.hovered     —— 真正的"指针此刻在这一行上"。
    //
    // 两个分支返回同一个 theme.hover，于是"当前已选择的那一行"从下拉打开的那一
    // 刻起就是灰的。用户先划过它、再移开鼠标，看到的自然是一块"移不掉"的灰——
    // 那块灰根本不是鼠标点亮的，而是 highlightedIndex 一直在那儿。
    //
    // 现在三种状态各走各的通道，任何一个都不会借用另一个的表现：
    //
    //   鼠标 hover —— 只有指针真的停在这一行上才出现（row.hovered），指针一走
    //                 立刻恢复透明。它是唯一使用 theme.hover 灰底的状态。
    //   已选择项  —— 文字换成强调色 + 加粗，右侧一个勾号。不占任何底色，因此
    //                 不可能被看成"残留的 hover"。
    //   键盘导航  —— ↑/↓ 移开之后才点亮（row.keyboardHighlighted），用另一种
    //                 颜色（accentSoft）。它和 hover 的灰在视觉上不是同一样东西。
    delegate: ItemDelegate {
        id: row

        // headless 自检按这两个名字找行与它的底：--combo-hover-test 会真的把
        // 指针移到某一行上，再移开，断言灰底严格跟着指针来去。
        objectName: "comboItemRow"
        required property var modelData
        required property int index

        width: control.width - 12
        implicitHeight: 44
        // 显式打开 hover：默认值来自系统 style hint，headless 环境下不一定为真，
        // 而这里的全部语义都建立在"指针真的在这一行上"。
        hoverEnabled: true

        readonly property bool isSelected: control.currentIndex === index
        // 键盘导航高亮：highlightedIndex 是常驻索引，所以只在它**不等于**当前已
        // 选择项时才算"用户正在用键盘导航"。下拉刚打开时它等于 currentIndex，
        // 那一行已经由"已选择"的样式表达了，不该再叠一层底色。
        readonly property bool keyboardHighlighted: control.highlightedIndex === index
                                                   && !row.isSelected
        // ItemDelegate.highlighted 仍然是"键盘 / 焦点高亮"的语义，不能拿它当
        // 鼠标 hover 用（Basic 样式会用它换文字色）。指针在的时候由 hover 说话。
        highlighted: row.keyboardHighlighted && !row.hovered

        contentItem: Item {
            Text {
                id: rowLabel
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.rightMargin: 26
                anchors.verticalCenter: parent.verticalCenter
                leftPadding: 10
                text: row.modelData
                font.pixelSize: 17
                font.weight: row.isSelected ? Font.DemiBold : Font.Normal
                color: row.isSelected ? theme.accent : theme.textPrimary
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            // 已选择项的标记。它和 hover 的灰底是两条独立通道，"鼠标走了还留着
            // 一块灰"在结构上不再可能发生。
            Text {
                objectName: "comboItemCheck"
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                visible: row.isSelected
                text: "✓"
                font.pixelSize: 16
                color: theme.accent
            }
        }

        background: Rectangle {
            objectName: "comboItemBackground"
            radius: 7
            // 固定色之间直接切换，不做颜色动画：transparent 与不透明色之间插值会
            // 逐分量经过"半透明黑"，浅色主题下就是上一轮已经修掉的那一闪。
            color: {
                if (row.hovered)
                    return theme.hover
                if (row.keyboardHighlighted)
                    return theme.accentSoft
                return "transparent"
            }
        }
    }
}

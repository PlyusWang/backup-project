// AppComboBox.qml
// 统一下拉框：当前候选项只使用一套交互高亮，已选值另显示勾选标记。

import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control

    implicitHeight: 46
    font.pixelSize: 17
    leftPadding: 14
    rightPadding: 14
    hoverEnabled: true
    // 滚轮在下面统一处理，避免原生处理与自定义处理重复切换两次。
    wheelEnabled: false

    // Popup 里只有一个“当前可操作项”：鼠标移动和键盘移动都写这里。
    // currentIndex 仍表示已经提交的值，Escape 关闭时不会修改它。
    property int activeIndex: -1
    property real wheelRemainder: 0
    readonly property int pageStep: Math.max(1,
        Math.floor(contentList.height / 44) - 1)

    function selectIndex(index) {
        if (index < 0 || index >= control.count)
            return
        const changed = control.currentIndex !== index
        control.currentIndex = index
        if (changed)
            control.activated(index)
    }

    function navigate(index) {
        if (control.count === 0)
            return
        const next = Math.max(0, Math.min(control.count - 1, index))
        if (control.popup.visible) {
            control.activeIndex = next
            contentList.positionViewAtIndex(next, ListView.Contain)
        } else {
            control.selectIndex(next)
        }
    }

    function handleKey(event) {
        if (!control.enabled || control.count === 0)
            return
        if (event.modifiers & (Qt.AltModifier | Qt.ControlModifier
                               | Qt.MetaModifier))
            return
        const index = control.popup.visible ? control.activeIndex
                                            : control.currentIndex
        switch (event.key) {
        case Qt.Key_Up:
            control.navigate(index - 1)
            break
        case Qt.Key_Down:
            control.navigate(index + 1)
            break
        case Qt.Key_Home:
            control.navigate(0)
            break
        case Qt.Key_End:
            control.navigate(control.count - 1)
            break
        case Qt.Key_PageUp:
            control.navigate(index - control.pageStep)
            break
        case Qt.Key_PageDown:
            control.navigate(index + control.pageStep)
            break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (!control.popup.visible) {
                control.popup.open()
            } else {
                const next = control.activeIndex
                control.popup.close()
                control.selectIndex(next)
            }
            break
        case Qt.Key_Escape:
            if (!control.popup.visible)
                return
            control.popup.close()
            break
        default:
            return
        }
        event.accepted = true
    }

    Keys.priority: Keys.BeforeItem
    Keys.onPressed: function (event) { control.handleKey(event) }
    // ComboBox 的原生 Enter 释放处理也会切换 popup；自定义按下处理后必须
    // 同时消费释放，否则同一次按键会先打开又关闭。
    Keys.onReleased: function (event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
            event.accepted = true
    }
    onActiveFocusChanged: {
        if (!activeFocus)
            wheelRemainder = 0
    }

    WheelHandler {
        enabled: control.enabled && !control.popup.visible
                 && (control.hovered || control.activeFocus)
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) {
            const delta = event.angleDelta.y !== 0
                ? event.angleDelta.y / 120 : event.pixelDelta.y / 40
            if (delta === 0) {
                event.accepted = false
                return
            }
            control.wheelRemainder += delta
            const steps = control.wheelRemainder > 0
                ? Math.floor(control.wheelRemainder)
                : Math.ceil(control.wheelRemainder)
            if (steps !== 0) {
                control.selectIndex(Math.max(0, Math.min(control.count - 1,
                    control.currentIndex - steps)))
                control.wheelRemainder -= steps
            }
            // 到达边界时也消费事件，避免用户正在选值却滚动了后面的页面。
            event.accepted = true
        }
    }

    background: Rectangle {
        radius: 9
        color: control.enabled ? theme.surface : theme.background
        border.width: control.activeFocus || control.popup.visible ? 2 : 1
        border.color: !control.enabled ? theme.border
            : (control.popup.visible || control.activeFocus ? theme.accent
               : (control.hovered ? theme.textDisabled : theme.border))
    }

    contentItem: Text {
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

    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(contentList.contentHeight + 12, 320)
        padding: 6
        onOpened: {
            control.activeIndex = Math.max(0, control.currentIndex)
            contentList.positionViewAtIndex(control.activeIndex,
                                            ListView.Contain)
        }
        onClosed: {
            control.activeIndex = -1
        }
        background: Rectangle {
            radius: 10
            color: theme.surfaceElevated
            border.width: 1
            border.color: theme.border
        }

        contentItem: ListView {
            id: contentList
            objectName: "comboPopupList"
            clip: true
            implicitHeight: Math.min(contentHeight, 308)
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.activeIndex
            boundsBehavior: Flickable.StopAtBounds
            flickDeceleration: 2200
            Keys.priority: Keys.BeforeItem
            Keys.onPressed: function (event) { control.handleKey(event) }
            Keys.onReleased: function (event) {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                    event.accepted = true
            }

            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function (event) {
                    const delta = event.pixelDelta.y !== 0
                        ? event.pixelDelta.y : event.angleDelta.y
                    if (delta === 0) {
                        event.accepted = false
                        return
                    }
                    control.activeIndex = Math.max(0, Math.min(
                        control.count - 1,
                        control.activeIndex + (delta > 0 ? 1 : -1)))
                    const max = Math.max(0,
                        contentList.contentHeight - contentList.height)
                    contentList.contentY = Math.max(0, Math.min(max,
                        contentList.contentY - delta))
                    event.accepted = true
                }
            }
            ScrollBar.vertical: ScrollBar {
                policy: contentList.contentHeight > contentList.height
                    ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            }
        }
    }

    delegate: ItemDelegate {
        id: row
        objectName: "comboItemRow"
        required property var modelData
        required property int index
        width: control.width - 12
        implicitHeight: 44
        hoverEnabled: true
        readonly property bool isSelected: control.currentIndex === index
        readonly property bool active:
            control.popup.visible && control.activeIndex === index
        highlighted: row.active
        onHoveredChanged: {
            if (row.hovered) {
                control.activeIndex = row.index
                contentList.positionViewAtIndex(row.index, ListView.Contain)
            }
        }
        onClicked: {
            control.selectIndex(row.index)
            control.popup.close()
        }

        contentItem: Item {
            Text {
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
            color: "transparent"
            Rectangle {
                objectName: "comboItemActiveLayer"
                anchors.fill: parent
                radius: parent.radius
                // 鼠标移动会更新 activeIndex；悬停填充不能再留下第二个高亮行。
                color: row.active ? theme.accentSoft : "transparent"
                border.width: row.active ? 2 : 0
                border.color: theme.accent
            }
        }
    }
}

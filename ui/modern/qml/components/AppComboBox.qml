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

    // ---- 输入方式（纯 presentation 状态，不放进任何控制器）----
    //
    // 人工验收第二轮：灰框仍然残留。这次它不在 hover 分支上，而在"键盘高亮"
    // 分支上 —— 因为 control.highlightedIndex **不能证明高亮是键盘来的**：
    // 鼠标划过一行之后，Qt 也会把 highlightedIndex 留在那一行上（运行期探针：
    // 指针移出 popup 之后 highlightedIndex=2、row.hovered=false、isSelected=false，
    // 那一行照样被画成一块底色）。
    //
    // 也就是说："highlightedIndex === index" 既可能是键盘，也可能是鼠标留下的
    // 残影——只凭它画任何东西，都会把鼠标留下的索引重新变成一块移不掉的底色。
    //
    // 所以键盘高亮必须再有一个**明确的输入方式闸门**：只有用户真的按了导航键，
    // 才允许它亮；任何一次真实的指针活动都把闸门关掉。
    property bool keyboardNavigationActive: false

    // 只"观察"这些键，不拦截：见 popup 里的 Keys.onPressed。
    function isNavigationKey(key) {
        return key === Qt.Key_Up || key === Qt.Key_Down
                || key === Qt.Key_Home || key === Qt.Key_End
                || key === Qt.Key_PageUp || key === Qt.Key_PageDown
    }

    // 指针一旦真的动过，输入方式就是鼠标。键盘高亮立刻让位给 hover。
    function notePointerActivity() {
        control.keyboardNavigationActive = false
    }

    // 键盘导航当前停在哪一行 —— 问**正在处理键盘的那个对象**。
    //
    // 运行期实测（Qt 6.4.2）：popup 的 ListView 自己处理 ↑/↓，移动的是它自己的
    // currentIndex（2 -> 3）；而 control.highlightedIndex 在同一时刻纹丝不动，
    // 它只被"鼠标划过某一行"改写。两者根本不是同一个东西：
    //
    //   鼠标划过第 4 行  -> highlightedIndex = 4   （指针留下的残影）
    //   ↑/↓ 移动一行    -> ListView.currentIndex 变  （真正的键盘位置）
    //
    // 所以键盘高亮读的是 ListView 的 currentIndex，而且只在输入方式闸门开着时
    // 才生效；闸门关着的时候，它停在哪一行都不画任何东西。
    readonly property int keyboardRowIndex: contentList.currentIndex

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

        // 打开/关闭都不是"用户在键盘导航"：即使 Qt 自动把 highlightedIndex
        // 设成 currentIndex，那也只是键盘导航的起点，不是一次导航动作。
        // 当前已选择项已经由 check + 强调色文字表达了，不需要再来一块底色。
        onOpened: control.keyboardNavigationActive = false
        onClosed: control.keyboardNavigationActive = false

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

            // 只观察输入方式，不接管 Qt 的导航：
            //   * Keys 的处理器在该控件自己的按键处理**之前**跑；
            //   * 这里只记下"用户是按键盘来的"，然后把事件原样放回去
            //     （accepted = false），ListView 自己的 ↑ / ↓ / Home / End
            //     行为一字未改，也没有抢焦点。
            Keys.onPressed: function (event) {
                if (control.isNavigationKey(event.key))
                    control.keyboardNavigationActive = true
                event.accepted = false
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

    // 每一行：行高、padding 与**三种互相独立的状态**。
    //
    // 两轮人工验收踩的是同一个坑的两种形态，根因都是"用一个残影当输入状态用"：
    //
    //   第一轮：background 里 row.highlighted（= highlightedIndex === index）
    //           与 row.hovered 返回同一块 theme.hover。打开下拉时 Qt 就把
    //           highlightedIndex 设成当前已选择项，于是那一行从打开起就是灰的，
    //           鼠标移开当然不会消失。
    //   第二轮：把那条分支换成"键盘高亮 + accentSoft"之后仍然残留 ——
    //           因为 highlightedIndex **不能证明高亮是键盘来的**：鼠标划过一行，
    //           Qt 也会把 highlightedIndex 留在那一行上（运行期探针实测：
    //           指针移出 popup 后 highlightedIndex=2、row.hovered=false、
    //           isSelected=false，那一行照样被画出一块底色）。
    //
    // 结论：highlightedIndex 只能回答"Qt 当前把哪一行当作 highlighted row"，
    // 回答不了"这次高亮是谁产生的"。所以键盘高亮必须再串一个**输入方式闸门**
    // （control.keyboardNavigationActive，只有真的按了导航键才为真，任何一次
    // 指针活动立刻关掉）。三种状态因此各走各的通道：
    //
    //   鼠标 hover —— row.hovered，指针一走立刻归零。唯一使用 theme.hover 的状态。
    //   已选择项  —— 强调色文字 + 加粗 + 右侧勾号。不占任何底色，
    //                 所以永远不可能被看成"残留的高亮"。
    //   键盘导航  —— keyboardNavigationActive && highlightedIndex === index
    //                 && 不是已选择项 && 指针不在这一行上，用 accentSoft。
    //                 闸门关着的时候，highlightedIndex 停在哪一行都不画东西。
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
        // 键盘导航高亮：**必须**同时满足"用户真的在用键盘导航"这个输入方式闸门。
        // 只看 highlightedIndex 是不够的 —— 鼠标划过一行之后 Qt 也会把它留在
        // 那一行上，那时 row.hovered 已经是 false，只凭索引就会留下一块移不掉的
        // 底色（上一轮就是这么错的）。
        readonly property bool keyboardHighlighted: control.keyboardNavigationActive
                                                   && control.keyboardRowIndex === index
                                                   && !row.isSelected
                                                   && !row.hovered
        // ItemDelegate.highlighted 仍然是"键盘 / 焦点高亮"的语义，不能拿它当
        // 鼠标 hover 用（Basic 样式会用它换文字色）。指针在的时候由 hover 说话。
        highlighted: row.keyboardHighlighted

        // 任何一次真实的指针活动都说明"现在是鼠标在操作"：闸门关掉，
        // 键盘高亮立刻让位给严格跟随指针的 hover。
        onHoveredChanged: if (row.hovered) control.notePointerActivity()
        onPressed: control.notePointerActivity()

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

        // 两种高亮各是一个**固定颜色 + opacity** 的覆盖层，底色永远是透明的。
        // 不做颜色插值：transparent 与不透明色之间做 ColorAnimation 会逐分量经过
        // "半透明黑"，浅色主题下就是已经修掉的那一下黑闪。
        //
        // 优先级：鼠标 hover > 键盘导航 > 无。两者互斥（keyboardHighlighted 里
        // 已经带 !row.hovered），所以谁在上面都不会打架。
        background: Rectangle {
            objectName: "comboItemBackground"
            radius: 7
            color: "transparent"

            // 唯一的灰。只有指针真的停在这一行上时才亮，指针一走就是 0。
            Rectangle {
                objectName: "comboItemHoverLayer"
                anchors.fill: parent
                radius: parent.radius
                color: theme.hover
                opacity: row.hovered ? 1 : 0
            }

            // 键盘导航：另一种颜色，而且只有在"用户真的在用键盘导航"时才亮。
            Rectangle {
                objectName: "comboItemKeyboardLayer"
                anchors.fill: parent
                radius: parent.radius
                color: theme.accentSoft
                opacity: row.keyboardHighlighted ? 1 : 0
            }
        }
    }
}

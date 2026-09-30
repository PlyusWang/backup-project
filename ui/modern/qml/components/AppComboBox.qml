// AppComboBox.qml
//
// 下拉框的统一外观：与主题同一套 token（light / dark 都走 theme），
// 并且支持鼠标滚轮直接滚动弹出列表。
//
// 为什么单独成组件：规则编辑器里有 4 个下拉（字段、类型、运算符、单位），
// 逐个写 popup 样式会重复四遍；抽出来以后只有一处需要和主题对齐 ——
// 下拉行的三种状态（已选择 / 鼠标 hover / 键盘光标）也因此只在这里定义一次。

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
    // popup 刚打开时 Qt 会把它自己的 currentIndex 同步过来，那不是"用户在导航"。
    // 在这一次同步结束之前，不许从导航结果里推断键盘模式。
    property bool suppressNavigationInference: true
    // 此刻是否有 delegate 真的 hovered。
    //
    // 这一条就足够把"Qt 因为 hover 顺手改了 popup currentIndex"挡在推断之外：
    // delegate 的 onHoveredChanged 与 Qt 更新 currentIndex 都在同一次 hover 事件里
    // 完成，而 notePointerActivity() 还会在同一轮把已经打开的键盘模式清掉，
    // 所以既不会误判、也不需要任何延时窗口。
    property bool pointerHovering: false

    // 指针一旦真的动过，输入方式就是鼠标：键盘高亮立刻让位给 hover，
    // 并且在一小段窗口期内不再从导航结果推断键盘模式
    // （Qt 的 hover 也会顺带改动 popup 的 currentIndex，
    //   那个变化必须归到鼠标头上，不能算成键盘导航）。
    function notePointerActivity() {
        control.keyboardNavigationActive = false
    }

    // ---- 键盘模式的唯一来源：**Qt 导航的结果**，不是按键事件 ----
    //
    // 真实桌面实测（xcb 窗口，焦点在 ComboBox 上——也就是用户点开下拉之后的
    // 真实状态）：
    //
    //   C1: after Down    cur=0  hl=1  listCur=1   kbnav=0
    //   C2: after Down    cur=0  hl=2  listCur=2   kbnav=0
    //   C3: after Up      cur=0  hl=1  listCur=1   kbnav=0
    //
    // 按键是送到 **ComboBox** 上的（真实点击之后焦点在它身上），不是送到 popup 的
    // ListView 上。所以"监听按键"这条路在真实桌面上收不到事件，键盘高亮永远不亮
    // —— 这正是人工验收看到的"能导航、但没有任何可见高亮"。
    //
    // 而 Qt 导航的结果是可靠的：popup ListView 的 currentIndex 一步一步跟着走。
    // 所以这里改成**观察导航结果**：
    //
    //   1. popup 开着；
    //   2. 不是打开时的初次同步；
    //   3. 没有指针活动、也没有任何一行正被 hover；
    //   4. popup ListView 的 currentIndex 变了；
    //   => 用户在用键盘导航。
    //
    // 这样不需要抢任何按键，也不会碰 Qt 自己的 ↑/↓ / Home / End / Enter / Esc。
    function noteNavigationResult() {
        if (!control.popup.visible) return
        if (control.suppressNavigationInference) return
        if (control.pointerHovering) return
        control.keyboardNavigationActive = true
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

        // 打开/关闭都不是"用户在键盘导航"：即使 Qt 自动把 currentIndex 同步过来，
        // 那也只是键盘导航的起点，不是一次导航动作。当前已选择项已经由
        // check + 强调色文字表达了，不需要再来一块底色。
        onOpened: {
            control.keyboardNavigationActive = false
            control.pointerHovering = false
            control.suppressNavigationInference = true
            // 初次同步（currentIndex 被设成当前项）发生在这个调用之前/同一轮里，
            // 下一轮事件循环再放开推断 —— 不用任何 sleep。
            Qt.callLater(function () {
                control.suppressNavigationInference = false
            })
        }
        onClosed: {
            control.keyboardNavigationActive = false
            control.pointerHovering = false
            control.suppressNavigationInference = true
        }

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

            // 只观察 Qt 导航的**结果**，不接管它的任何按键处理。
            // 真正的判定在 control.noteNavigationResult() 里（见文件上方的说明）。
            onCurrentIndexChanged: control.noteNavigationResult()

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
    // 三轮人工验收踩的是同一个坑的三种形态，根因都是"用一个残影当输入状态用"：
    //
    //   第一轮：background 里 row.highlighted（= highlightedIndex === index）
    //           与 row.hovered 返回同一块 theme.hover。打开下拉时 Qt 就把
    //           highlightedIndex 设成当前已选择项，于是那一行从打开起就是灰的，
    //           鼠标移开当然不会消失。
    //   第二轮：换成"键盘高亮 + accentSoft"之后仍然残留 —— 因为 highlightedIndex
    //           **不能证明高亮是键盘来的**：鼠标划过一行，Qt 也会把
    //           highlightedIndex 留在那一行上（实测：指针移出 popup 后
    //           highlightedIndex=4、row.hovered=false，那一行照样被画出一块底色）。
    //   第三轮：改成监听 Keys.onPressed 之后，真实桌面里**键盘高亮永远不亮** ——
    //           因为按键送到的是 ComboBox（真实点击之后焦点在它身上），
    //           不是 popup 的 ListView；监听按键这条路在真实焦点链上收不到事件。
    //
    // 三轮下来结论只有一句：**不要监听"用户有没有按键"，要观察"Qt 把键盘位置
    // 移到了哪里"**。真实桌面实测：↑/↓ 会让 popup ListView 的 currentIndex
    // 一步一步跟着走（0 -> 1 -> 2 -> 1），那才是键盘位置的唯一可靠来源。
    //
    // 三种状态因此各走各的通道：
    //
    //   鼠标 hover —— row.hovered，指针一走立刻归零。唯一使用 theme.hover 的状态。
    //   已选择项  —— 强调色文字 + 加粗 + 右侧勾号。不占任何底色，
    //                 所以永远不可能被看成"残留的高亮"。
    //   键盘光标  —— keyboardNavigationActive（由导航结果推断，见文件上方）
    //                 && popup ListView 的 currentIndex === index，用 accentSoft。
    //                 它与"已选择项"**可以叠加**：selected 表示"这个值已选中"，
    //                 keyboard 表示"现在按 Enter 会选它"，两个概念不冲突。
    //                 闸门关着的时候，currentIndex 停在哪一行都不画东西。
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
        // 键盘光标：只要闸门开着、且 Qt 的键盘位置就在这一行，就有反馈。
        // **不排除已选择项** —— 键盘导航回当前值时，用户同样需要看见"按 Enter
        // 会选中哪一行"。两个概念是独立的：selected 表示"这个值已经选中"，
        // keyboard 表示"现在按 Enter 会选它"。
        readonly property bool keyboardHighlighted: control.keyboardNavigationActive
                                                   && control.keyboardRowIndex === index
        // ItemDelegate.highlighted 仍然是"键盘 / 焦点高亮"的语义，不能拿它当
        // 鼠标 hover 用（Basic 样式会用它换文字色）。指针在的时候由 hover 说话。
        highlighted: row.keyboardHighlighted

        // 任何一次真实的指针活动都说明"现在是鼠标在操作"：闸门关掉，
        // 键盘高亮立刻让位给严格跟随指针的 hover。
        onHoveredChanged: {
            control.pointerHovering = row.hovered
            if (row.hovered)
                control.notePointerActivity()
        }
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
        // 优先级：鼠标 hover > 键盘导航 > 无。两个覆盖层都可能是 1
        // （鼠标正好停在键盘光标那一行上），所以顺序是刻意的：
        // 键盘层先画、hover 层后画，后者在上 —— "鼠标永远是最后说话的那一个"。
        background: Rectangle {
            objectName: "comboItemBackground"
            radius: 7
            color: "transparent"

            // 键盘光标：只有在"Qt 真的把键盘位置移到这里"时才亮。
            Rectangle {
                objectName: "comboItemKeyboardLayer"
                anchors.fill: parent
                radius: parent.radius
                color: theme.accentSoft
                opacity: row.keyboardHighlighted ? 1 : 0
            }

            // 唯一的灰。只有指针真的停在这一行上时才亮，指针一走就是 0。
            // 画在键盘层之上，所以两者同时亮时看到的是 hover。
            Rectangle {
                objectName: "comboItemHoverLayer"
                anchors.fill: parent
                radius: parent.radius
                color: theme.hover
                opacity: row.hovered ? 1 : 0
            }
        }
    }
}

// RuleCard.qml
//
// 一条筛选规则的卡片：负责展示，并在点击时通过注入的 ruleModelRef 直接调用
// 上移 / 下移 / 删除（改完之后由面板的 rulesChanged 统一同步状态与刷新预览）。
//
// 之所以单独成组件：委托作用域里引用所属组件的根 id（panel.）会让 Qt 6.4 的静态
// 检查工具报假告警。卡片用 required property 显式声明依赖（index / ruleData /
// ruleModelRef），不引用父级成员，依赖方向清楚，也不必扩大静态检查的放行范围。

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: card

    // Repeater / ListView 会把模型下标注入到这个 required 属性；
    // 这样委托里不需要引用隐式的 index（静态检查的已知局限点）。
    required property int index
    required property var ruleData
    property bool busy: false
    property int totalRules: 0
    // 规则模型由面板注入：卡片自己完成上移 / 下移 / 删除，这样委托作用域里
    // 不需要引用父级成员（静态检查的已知局限点，也让依赖方向更清楚）。
    required property var ruleModelRef


    Layout.fillWidth: true
    implicitHeight: column.implicitHeight + 12
    radius: 4
    color: theme.hover

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        spacing: 2

        Text {
            Layout.fillWidth: true
            text: (card.ruleData.action === "include" ? "[Include] " : "[Exclude] ") + card.ruleData.summary
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: theme.textPrimary
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            text: card.ruleData.dsl
            font.pixelSize: 11
            font.family: "monospace"
            color: theme.textSecondary
        }

        RowLayout {
            spacing: 6

            AppButton {
                text: "上移"
                enabled: !card.busy && card.index > 0
                onClicked: card.ruleModelRef.moveRule(card.index, -1)
            }

            AppButton {
                text: "下移"
                enabled: !card.busy && card.index < card.totalRules - 1
                onClicked: card.ruleModelRef.moveRule(card.index, 1)
            }

            AppButton {
                text: "删除"
                enabled: !card.busy
                onClicked: card.ruleModelRef.removeRule(card.index)
            }
        }
    }
}

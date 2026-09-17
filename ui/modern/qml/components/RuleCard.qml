// RuleCard.qml
//
// 一条筛选规则的卡片：只负责展示与发出动作信号，不触碰规则模型。
//
// 之所以单独成组件：委托作用域里引用所属组件的根 id（panel.）会让 Qt 6.4 的
// 静态检查工具报 Unqualified access 假告警。把卡片独立出来后，移动 / 删除
// 的动作在面板自己的作用域里处理，既不需要放行，也让依赖方向更清楚。

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

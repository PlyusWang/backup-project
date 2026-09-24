// RuleCard.qml
//
// 一条规则的卡片。数据由 delegate 在边界处就拆成显式字符串传进来
// （actionText / summaryText / dslText / detailText），卡片内部不解析任何
// 来源不明的 var QVariantMap。
//
// 布局：正文在上、操作按钮在下并右对齐；卡片高度完全由正文 implicitHeight 决定。
// 正文的三行 Text 都带 objectName，供 headless 测试读取真实 text/宽高，
// 用来防止"模型里有字符串、屏幕上却是一片空白"的回归。

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: card

    objectName: "ruleCard"
    required property int ruleIndex
    required property string actionText
    required property string summaryText
    required property string dslText
    property string detailText: ""
    required property var ruleModelRef
    property bool busy: false
    property int totalRules: 0

    readonly property int cardPadding: 14
    readonly property bool isInclude: card.actionText !== "exclude"

    Layout.fillWidth: true
    Layout.preferredHeight: implicitHeight
    implicitHeight: body.implicitHeight + cardPadding * 2
    radius: 8
    color: theme.hover

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: card.cardPadding
        spacing: 6

        Text {
            id: primaryText
            objectName: "rulePrimaryText"
            Layout.fillWidth: true
            text: (card.isInclude ? "Include" : "Exclude")
                  + (card.detailText.length > 0 ? " · " + card.detailText : "")
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: card.isInclude ? theme.textPrimary : theme.warning
            wrapMode: Text.WordWrap
        }

        Text {
            id: summaryTextItem
            objectName: "ruleSummaryText"
            Layout.fillWidth: true
            text: card.summaryText
            font.pixelSize: 16
            color: theme.textPrimary
            wrapMode: Text.WordWrap
        }

        Text {
            id: dslTextItem
            objectName: "ruleDslText"
            Layout.fillWidth: true
            text: card.actionText + " " + card.dslText
            font.pixelSize: 15
            font.family: "monospace"
            color: theme.textSecondary
            wrapMode: Text.WordWrap
        }

        RowLayout {
            id: actionRow
            objectName: "ruleActionRow"
            Layout.fillWidth: true
            spacing: 10

            Item { Layout.fillWidth: true }

            AppButton {
                text: "上移"
                enabled: !card.busy && card.ruleIndex > 0
                onClicked: card.ruleModelRef.moveRule(card.ruleIndex, -1)
            }

            AppButton {
                text: "下移"
                enabled: !card.busy && card.ruleIndex < card.totalRules - 1
                onClicked: card.ruleModelRef.moveRule(card.ruleIndex, 1)
            }

            AppButton {
                text: "删除"
                variant: "flat"
                enabled: !card.busy
                onClicked: card.ruleModelRef.removeRule(card.ruleIndex)
            }
        }
    }
}

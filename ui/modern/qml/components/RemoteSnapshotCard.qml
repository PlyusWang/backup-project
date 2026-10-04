// RemoteSnapshotCard.qml
//
// 云端备份列表里的一行。它只做三件事：展示、发意图、把"这一条能不能恢复"
// 如实说清楚。
//
//   * 主视觉是用户看得懂的：名称、大小、创建时间，加上 PR #21 的链信息
//     （完整 / 增量、代数、父快照前 12 位）；
//   * 编号与摘要属于技术细节，放在页面里默认折叠的"技术详情"里；
//   * 主操作三种类型都叫"恢复"（PR #21 UI closure 第二轮）：完整备份 / 增量
//     备份走链（自动取回整条依赖链）；原始归档（lineage 为空）下载那一个 blob
//     之后按本地备份格式独立恢复。内部实现是两条路，但那是**类型 badge** 与
//     说明行要说的事——按钮只表达用户的意图，不表达"这次能不能成"。
//     "下载归档"与"删除"是次级操作：前者只是把云端那个 blob 取回本地。
//   * 三类的主类型 badge 必须是三个词（原始归档 / 完整备份 / 增量备份）：
//     原始归档既没有父也没有"代数"可言，所以那两项在它这一行**不显示**。
//     以前原始归档显示成"完整 + 代数 0"，用户会以为它是一份完整备份、
//     只是不知道为什么不能恢复。
//
// 它**不**做任何类型判断：类型、代数、父、按钮文案全部来自控制器
// （presentation model），这一层只负责显示与发意图。

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: card

    property string snapshotId: ""
    property string nameText: ""
    property string sizeText: ""
    property string createdText: ""
    // ---- PR #21 链信息 ----
    property string kindText: ""      // "原始归档" / "完整备份" / "增量备份"
    property string kindKey: ""       // "raw" / "full" / "incremental"
    // 原始归档：不显示"代数"（它没有链）；主操作与产品级一样是"恢复"。
    property bool rawArchive: false
    property bool generationVisible: true
    property int generation: 0
    property string parentShort: ""   // 只有增量才有
    // 这一条是什么（raw 才有一句话说明；产品级为空）。
    property string typeNote: ""
    property string restoreLabel: "恢复"
    property bool restorable: true
    property string restoreHint: ""   // 不能恢复时的一句话
    property bool busy: false

    signal restoreRequested(string snapshotId, string name)
    signal downloadRequested(string snapshotId, string name)
    signal deleteRequested(string snapshotId, string name)

    radius: 8
    color: theme.surface
    border.width: 1
    border.color: theme.border
    implicitHeight: layout.implicitHeight + 26

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 13
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Text {
                objectName: "remoteSnapshotName"
                Layout.fillWidth: true
                text: card.nameText
                font.pixelSize: 16
                font.weight: Font.DemiBold
                color: theme.textPrimary
                wrapMode: Text.WrapAnywhere
            }

            AppButton {
                objectName: "remoteSnapshotRestoreButton"
                // 文案来自控制器：三种类型的按钮都是"恢复"，由 badge 说明
                // 这一条是什么类型、恢复会走哪条路。
                text: card.restoreLabel
                variant: "primary"
                enabled: !card.busy && card.restorable
                onClicked: card.restoreRequested(card.snapshotId, card.nameText)
            }

            AppButton {
                objectName: "remoteSnapshotDownloadButton"
                text: "下载归档"
                enabled: !card.busy
                onClicked: card.downloadRequested(card.snapshotId, card.nameText)
            }

            AppButton {
                objectName: "remoteSnapshotDeleteButton"
                text: "删除"
                enabled: !card.busy
                onClicked: card.deleteRequested(card.snapshotId, card.nameText)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            // 主类型 badge：三种类型三种外观（原始归档是带强调色的标签，
            // 完整 / 增量是中性标签，增量再靠"代数 + 父"区分）。
            Rectangle {
                id: kindBadge
                objectName: "remoteSnapshotKindBadge"
                radius: 6
                color: card.rawArchive
                       ? theme.accentSoft
                       : (card.kindKey === "incremental"
                          ? theme.surfaceElevated : theme.surface)
                border.width: 1
                border.color: card.rawArchive ? theme.accent : theme.border
                implicitWidth: kindLabel.implicitWidth + 16
                implicitHeight: kindLabel.implicitHeight + 8

                Text {
                    id: kindLabel
                    objectName: "remoteSnapshotKindText"
                    anchors.centerIn: parent
                    text: card.kindText
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    color: card.rawArchive ? theme.accent : theme.textPrimary
                }
            }

            // "代数"只对产品链成员有意义：原始归档没有链，这里不显示"代数 0"
            // ——那是一个编出来的语义。
            Text {
                objectName: "remoteSnapshotGenerationText"
                visible: card.generationVisible
                text: "代数 " + card.generation
                font.pixelSize: 15
                color: theme.textSecondary
            }

            Text {
                objectName: "remoteSnapshotParentText"
                visible: card.kindKey === "incremental" && card.parentShort !== ""
                text: "父 " + card.parentShort
                font.pixelSize: 15
                color: theme.textSecondary
            }

            Text {
                objectName: "remoteSnapshotSize"
                text: card.sizeText
                font.pixelSize: 15
                color: theme.textSecondary
            }

            Text {
                objectName: "remoteSnapshotTime"
                text: card.createdText
                font.pixelSize: 15
                color: theme.textSecondary
            }

            Item { Layout.fillWidth: true }
        }

        // 这一条"是什么"：只有原始归档需要解释（它是手动上传的归档，不参与
        // 远端增量链），产品级两类的语义由 badge 与"代数 / 父"说清了。
        Text {
            objectName: "remoteSnapshotTypeNote"
            Layout.fillWidth: true
            visible: card.typeNote !== ""
            text: card.typeNote
            font.pixelSize: 14
            color: theme.textSecondary
            wrapMode: Text.WrapAnywhere
        }

        Text {
            objectName: "remoteSnapshotRestoreHint"
            Layout.fillWidth: true
            visible: !card.restorable && card.restoreHint !== ""
            text: card.restoreHint
            font.pixelSize: 14
            color: theme.textSecondary
            wrapMode: Text.WrapAnywhere
        }
    }
}

// RemoteSnapshotCard.qml
//
// 云端备份列表里的一行。它只做三件事：展示、发意图、把"这一条能不能恢复"
// 如实说清楚。
//
//   * 主视觉是用户看得懂的：名称、大小、创建时间，加上 PR #21 的链信息
//     （完整 / 增量、代数、父快照前 12 位）；
//   * 编号与摘要属于技术细节，放在页面里默认折叠的"技术详情"里；
//   * 主操作是"恢复"（产品级链恢复）；"下载归档"与"删除"是次级操作——
//     前者是低层 raw 操作（取回一个 blob），与"恢复一份远端备份"不是一回事；
//   * 原始归档上传的条目（不属于远端备份链）不能做链恢复：按钮禁用，
//     并在卡片上直接说明原因，而不是等用户点了之后才报"不是 BPSNAP1"。
//
// 它**不**做任何链判断：能不能恢复来自服务端元数据（控制器已经算好
// restorable / restoreHint），这一层只负责显示与禁用它。

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: card

    property string snapshotId: ""
    property string nameText: ""
    property string sizeText: ""
    property string createdText: ""
    // ---- PR #21 链信息 ----
    property string kindText: ""      // "完整" / "增量"
    property string kindKey: ""       // "full" / "incremental"
    property int generation: 0
    property string parentShort: ""   // 增量才有
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
                text: "恢复"
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

            Rectangle {
                id: kindBadge
                objectName: "remoteSnapshotKindBadge"
                radius: 6
                color: card.kindKey === "incremental"
                       ? theme.surfaceElevated : theme.surface
                border.width: 1
                border.color: theme.border
                implicitWidth: kindLabel.implicitWidth + 16
                implicitHeight: kindLabel.implicitHeight + 8

                Text {
                    id: kindLabel
                    objectName: "remoteSnapshotKindText"
                    anchors.centerIn: parent
                    text: card.kindText
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    color: theme.textPrimary
                }
            }

            Text {
                objectName: "remoteSnapshotGenerationText"
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

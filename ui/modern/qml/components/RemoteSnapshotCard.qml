// RemoteSnapshotCard.qml
//
// 云端备份列表里的一行。它只做两件事：展示，和"发意图"。
//
//   * 展示的三样东西是用户看得懂的：名称、大小、创建时间；
//   * 编号与摘要属于技术细节，放在页面里默认折叠的"技术详情"里，
//     不在这里当主视觉；
//   * 下载与删除都只发信号，真正动网络的调用由页面转交给
//     RemoteController（页面再转交给共享的 RemoteArchiveClient）。

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: card

    property string snapshotId: ""
    property string nameText: ""
    property string sizeText: ""
    property string createdText: ""
    property bool busy: false

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
                objectName: "remoteSnapshotDownloadButton"
                text: "下载"
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
    }
}

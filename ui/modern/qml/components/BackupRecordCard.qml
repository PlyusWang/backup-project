// BackupRecordCard.qml
//
// 备份管理页里的一条记录。数据在 delegate 边界处就拆成显式的基本类型传进来
// （fileNameText / sizeText / modifiedText / recognized / formatVersion /
// entryCountText / diagnosticText），卡片内部不解析来源不明的 QVariantMap。
//
// 卡片是自洽的：恢复目录选择与删除确认都在这一个文件里完成，最后由它调用
// controller 的 Q_INVOKABLE。这样 delegate 里不需要引用任何外层 id，
// 静态检查与运行期的作用域都干净。
//
// 文案边界（archive v0.1 的事实）：
//   * 只能叫“文件修改时间”——那是归档文件自身在文件系统上的 mtime，
//     v0.1 的归档里根本没有 created_at 这个字段；
//   * recognizedArchive 只表示“全局 header 被当前实现认得”，
//     所以这里写“已识别归档格式”，绝不写“校验通过 / 完整 / 健康”。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

Rectangle {
    id: card

    objectName: "backupRecordCard"
    required property string fileNameText
    required property string sizeText
    required property string modifiedText
    required property bool recognized
    required property int formatVersion
    required property string entryCountText
    required property string diagnosticText
    property bool busy: false

    readonly property int cardPadding: 14

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
            objectName: "backupRecordName"
            Layout.fillWidth: true
            text: card.fileNameText
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: theme.textPrimary
            wrapMode: Text.WordWrap
        }

        Text {
            objectName: "backupRecordMeta"
            Layout.fillWidth: true
            text: card.sizeText + " · 文件修改时间 " + card.modifiedText
            font.pixelSize: 15
            color: theme.textSecondary
            wrapMode: Text.WordWrap
        }

        Text {
            objectName: "backupRecordRecognition"
            Layout.fillWidth: true
            text: card.recognized
                  ? "已识别归档格式 · v" + card.formatVersion + " · " + card.entryCountText + " 个条目"
                  : "无法识别归档头"
            font.pixelSize: 15
            color: card.recognized ? theme.textPrimary : theme.warning
            wrapMode: Text.WordWrap
        }

        Text {
            objectName: "backupRecordRecognitionNote"
            Layout.fillWidth: true
            visible: card.recognized
            text: "恢复时仍会执行完整归档校验。"
            font.pixelSize: 14
            color: theme.textSecondary
            wrapMode: Text.WordWrap
        }

        // 坏掉的 .bak 也要把原因摊开给用户看，而不是只写一句"损坏"。
        TextEdit {
            objectName: "backupRecordDiagnostic"
            Layout.fillWidth: true
            visible: !card.recognized && card.diagnosticText.length > 0
            text: card.diagnosticText
            readOnly: true
            selectByMouse: true
            cursorVisible: false
            textFormat: TextEdit.PlainText
            font.pixelSize: 14
            color: theme.warning
            wrapMode: TextEdit.WrapAnywhere
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Item { Layout.fillWidth: true }

            AppButton {
                objectName: "backupRecordRestore"
                text: "恢复"
                variant: "primary"
                iconName: "restore"
                // 不认得 header 就不给恢复入口。但控制器并不依赖这层 UI 防护：
                // Resolve 会校验名字，真正的完整校验在 preflight 里。
                enabled: !card.busy && card.recognized
                onClicked: {
                    restoreDialog.currentFolder = controller.directoryDialogStartUrl("")
                    restoreDialog.open()
                }
            }

            AppButton {
                objectName: "backupRecordDelete"
                text: "删除"
                variant: "flat"
                iconName: "trash"
                // 坏掉的 .bak 也能删，否则它会永远留在列表里。
                enabled: !card.busy
                onClicked: deleteDialog.open()
            }
        }
    }

    // 恢复目标目录：选完之后立刻发起恢复，界面不保存"待恢复"状态。
    FolderDialog {
        id: restoreDialog
        objectName: "managedRestoreDialog"
        title: "选择恢复目录"
        onAccepted: {
            const destination = controller.localPathFromUrl(restoreDialog.selectedFolder)
            if (destination !== "" && card.fileNameText !== "")
                controller.startManagedRestore(card.fileNameText, destination)
        }
    }

    // 删除确认。确认框不是安全边界，文件名校验仍然由 Catalog 自己做。
    Dialog {
        id: deleteDialog
        objectName: "deleteConfirmDialog"
        anchors.centerIn: parent
        modal: true
        padding: 0

        background: Rectangle {
            color: theme.surfaceElevated
            border.width: 1
            border.color: theme.border
            radius: 10
        }

        contentItem: ColumnLayout {
            spacing: 12

            Text {
                text: "删除备份"
                color: theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Text {
                Layout.preferredWidth: 320
                text: "确定要从备份仓库中删除 " + card.fileNameText + " 吗？此操作不可撤销。"
                color: theme.textSecondary
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Item { Layout.fillWidth: true }

                AppButton {
                    text: "取消"
                    onClicked: deleteDialog.close()
                }

                AppButton {
                    objectName: "deleteConfirmButton"
                    text: "删除"
                    variant: "primary"
                    onClicked: {
                        deleteDialog.close()
                        controller.deleteBackup(card.fileNameText)
                    }
                }
            }
        }
    }
}

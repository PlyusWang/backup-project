// BackupRecordCard.qml
//
// 备份管理页里的一条记录。数据在 delegate 边界处就拆成显式的基本类型传进来
// （fileNameText / sizeText / modifiedText / recognized / formatVersion /
// entryCountText / diagnosticText / packMethodText / …），
// 卡片内部不解析来源不明的 QVariantMap。
//
// 卡片是自洽的：恢复目录选择、加密备份的密码输入与删除确认
// 都在这一个文件里完成，最后由它调用 controller 的 Q_INVOKABLE。
// 这样 delegate 里不需要引用任何外层 id，
// 静态检查与运行期的作用域都干净。
//
// 文案边界（archive 的事实）：
//   * 只能叫“文件修改时间”——那是归档文件自身在文件系统上的 mtime，
//     v0.1 的归档里根本没有 created_at 这个字段；
//   * recognizedArchive 只表示“全局 header 被当前实现认得”，
//     所以这里写“已识别归档格式”，绝不写任何校验结论、也不写“完整 / 健康”；
//     列表这一次只看了 header，没有做完整校验，文案不能替核心下结论；
//   * legacy v0.1 记录没有 pipeline 字段（hasPipelineMethods 为 false）：
//     一条算法名都不显示，免得让人以为它也被 v2 管道处理过；
//   * v2 记录的三段算法名一律用 C++ 给的展示文案拼，界面不解释枚举数字，
//     也不自己编算法名 —— 真值来源只有一处；
//   * 快照种类与依赖链状态同样全部来自 C++：完整 / 增量、父快照名、
//     "能不能恢复"以及不能恢复的原因。界面只做连接，不自己推断；
//   * 断链时禁用恢复按钮只是 UX，让用户不必点一次才知道；核心的
//     preflight 没有因此削弱，绕过界面的调用方依旧 fail closed。
//
// 密码边界（加密恢复）：
//   * 密码只活在这一张卡片里（restorePassword），提交后立刻清空，
//     不进控制器全局状态、不进状态栏、不进提示气泡、不写进文件名；
//   * 目标目录同样只暂存在卡片本地（pendingRestoreDestination），
//     只在“选完目录 → 输密码”这段间隙里有效，对话框一关就清掉。

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
    // v2 容器才有 pipeline 元数据；legacy 记录上这三个展示文案是空的。
    required property bool hasPipelineMethods
    required property string packMethodText
    required property string compressionMethodText
    required property string encryptionMethodText
    // 加密备份恢复时需要密码。它只决定交互：恢复能不能成由核心裁决。
    required property bool passwordRequired
    // 快照种类与依赖链状态。这几个值来自 BackupCatalog 的廉价判断
    // （只确认父文件还在），不是完整校验；所以它们只用来把已知事实
    // 提前讲清楚，绝不替代核心的 preflight。
    required property string recordKind
    required property bool isDelta
    required property string parentFileName
    required property bool chainRestorable
    required property string chainDiagnostic
    // 来源（手动备份 / 定时备份）与计划快照的变化摘要。
    // 两者都来自 ScheduleStore，QML 不解析文件名也不自己推断。
    required property string originText
    required property string scheduledChangesText
    property bool busy: false

    // 卡片本地的加密恢复状态。两者都在密码对话框关闭时清空。
    property string pendingRestoreDestination: ""
    property string restorePassword: ""

    readonly property int cardPadding: 14

    Layout.fillWidth: true
    Layout.preferredHeight: implicitHeight
    implicitHeight: body.implicitHeight + cardPadding * 2
    radius: 8
    color: theme.hover

    // 选完目标目录之后的唯一分流点：未加密记录直接恢复；加密记录先把目标目录
    // 暂存在卡片本地，等密码确认后再发起。写成函数有两个原因 ——
    // "目标目录为空就不做事"这个判断只有一处，而且恢复流程可以被无显示环境下的
    // 测试直接驱动（平台目录对话框在 offscreen 下点不动）。
    function beginRestore(destination) {
        if (destination === "" || card.fileNameText === "")
            return
        if (!card.passwordRequired) {
            controller.startManagedRestore(card.fileNameText, destination)
            return
        }
        card.pendingRestoreDestination = destination
        restorePasswordDialog.open()
    }

    // 清掉卡片本地的目标目录与密码。密码输入框自己的 text 也要清：
    // 它不绑定 restorePassword，用户输入之后只改属性会在框里留下旧值。
    function clearRestoreSecrets() {
        card.restorePassword = ""
        card.pendingRestoreDestination = ""
        restorePasswordInput.text = ""
    }

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

        // legacy v0.1 仍然只说“已识别归档格式 · v1 · N 个条目”，
        // 只在末尾补一个 Legacy v0.1 标记，绝不显示任何算法名。
        Text {
            objectName: "backupRecordRecognition"
            Layout.fillWidth: true
            text: card.recognized
                  ? "已识别归档格式 · v" + card.formatVersion + " · "
                    + card.entryCountText + " 个条目"
                    + (card.hasPipelineMethods ? "" : " · Legacy v0.1")
                  : "无法识别归档头"
            font.pixelSize: 15
            color: card.recognized ? theme.textPrimary : theme.warning
            wrapMode: Text.WordWrap
        }

        // 快照种类。父快照名就用仓库里的文件名 —— 它在列表里本来
        // 就看得到，不是把内部格式术语摊给用户。
        Text {
            objectName: "backupRecordSnapshotKind"
            Layout.fillWidth: true
            text: card.isDelta
                  ? (card.parentFileName !== ""
                     ? "增量备份 · 父快照 " + card.parentFileName
                     : "增量备份")
                  : "完整备份"
            font.pixelSize: 15
            color: theme.textPrimary
            wrapMode: Text.WordWrap
        }

        // 依赖链断了：恢复一定会失败，所以在卡片上提前说清楚，用户不必
        // 点一次"恢复"、等 preflight 报错才知道。这层只是把已知事实讲
        // 出来，真正的裁决仍然在核心，禁用按钮不构成任何安全保证。
        Text {
            objectName: "backupRecordChainBroken"
            Layout.fillWidth: true
            visible: card.isDelta && !card.chainRestorable
            text: "依赖链不可恢复"
            font.pixelSize: 15
            font.weight: Font.DemiBold
            color: theme.warning
            wrapMode: Text.WordWrap
        }

        // 原因原样来自核心（"父快照缺失: xxx" / "没有声明父快照"），
        // 界面不自己编，也不把它翻译成另一套说法。
        TextEdit {
            objectName: "backupRecordChainDiagnostic"
            Layout.fillWidth: true
            visible: card.isDelta && !card.chainRestorable
                     && card.chainDiagnostic.length > 0
            text: card.chainDiagnostic
            readOnly: true
            selectByMouse: true
            cursorVisible: false
            textFormat: TextEdit.PlainText
            font.pixelSize: 14
            color: theme.warning
            wrapMode: TextEdit.WrapAnywhere
        }

        // v2 记录的三个算法名：全部来自 C++，这里只做连接。
        // legacy 记录连这一步连接都不做，卡片里不会留下任何算法名文本。
        Text {
            objectName: "backupRecordPipeline"
            Layout.fillWidth: true
            visible: card.hasPipelineMethods
            text: card.hasPipelineMethods
                  ? card.packMethodText + " · " + card.compressionMethodText
                    + " · " + card.encryptionMethodText
                  : ""
            font.pixelSize: 15
            color: theme.textPrimary
            wrapMode: Text.WordWrap
        }

        // 来源 + 计划变化摘要。变化摘要只有 managed 的计划快照才有，
        // 手动备份这一栏是空的 —— 不显示"0 新增 0 修改"这种没有意义的噪声。
        Text {
            objectName: "backupRecordOrigin"
            Layout.fillWidth: true
            text: card.originText
                  + (card.scheduledChangesText !== ""
                    ? "  ·  " + card.scheduledChangesText
                    : "")
            font.pixelSize: 15
            color: theme.textSecondary
            wrapMode: Text.WordWrap
        }

        // 加密备份的事实，不是校验结论：恢复时需要用户提供密码。
        Text {
            objectName: "backupRecordPasswordRequired"
            Layout.fillWidth: true
            visible: card.passwordRequired
            text: "需要密码恢复"
            font.pixelSize: 15
            color: theme.textPrimary
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
                // 不认得 header、或者依赖链已经断了，都不给恢复入口 ——
                // 后一种情况恢复一定失败，让用户点了再报错没有意义。
                // 控制器并不依赖这层 UI 防护：Resolve 会校验名字，真正的
                // 完整校验（含整条依赖链）在 preflight 里，CLI 或任何绕过
                // 界面的调用方照样 fail closed。
                enabled: !card.busy && card.recognized && card.chainRestorable
                onClicked: {
                    restoreDialog.currentFolder =
                        controller.directoryDialogStartUrl("")
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

    // 恢复目标目录。selectedFolder 由平台目录对话框写入，这里只负责把 URL
    // 还原成本地路径（中文、空格、# 与 % 都靠 localPathFromUrl，
    // 不手写前缀裁剪），剩下的分流交给 beginRestore()。
    FolderDialog {
        id: restoreDialog
        objectName: "managedRestoreDialog"
        title: "选择恢复目录"
        onAccepted: card.beginRestore(
                controller.localPathFromUrl(restoreDialog.selectedFolder))
    }

    // 加密备份的密码对话框。密码是一次性的输入：不进任何全局状态，
    // 提交后立刻清空；取消（含 Esc 与点窗口外关闭）时同样清空。
    Dialog {
        id: restorePasswordDialog
        objectName: "restorePasswordDialog"
        anchors.centerIn: parent
        modal: true
        // 自定义 Dialog 默认没有内容边距，
        // 标题 / 正文 / 输入框 / 按钮会贴着边框。
        // 统一给 18，与 AppCard 的内边距同一个量级；
        // background 的圆角与描边不动。
        padding: 18

        // 关闭路径不止“取消”一种，所以清空挂在 onClosed 上，
        // 保证任何一种关法都不会把输入留在界面上。
        onClosed: card.clearRestoreSecrets()

        background: Rectangle {
            color: theme.surfaceElevated
            border.width: 1
            border.color: theme.border
            radius: 10
        }

        contentItem: ColumnLayout {
            spacing: 12

            Text {
                text: "恢复加密备份"
                color: theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Text {
                Layout.preferredWidth: 320
                text: "此备份已加密，需要密码才能恢复。"
                color: theme.textSecondary
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            AppTextField {
                id: restorePasswordInput
                objectName: "restorePasswordField"
                Layout.preferredWidth: 320
                echoMode: TextInput.Password
                placeholderText: "密码"
                enabled: !card.busy
                onTextEdited: card.restorePassword = text
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Item { Layout.fillWidth: true }

                AppButton {
                    objectName: "restorePasswordCancel"
                    text: "取消"
                    onClicked: {
                        card.clearRestoreSecrets()
                        restorePasswordDialog.close()
                    }
                }

                AppButton {
                    objectName: "restorePasswordConfirm"
                    text: "恢复"
                    variant: "primary"
                    // 空密码不给提交；操作进行中同样不给提交
                    // （一次只跑一个操作）。
                    // 密码错一次之后这个按钮照样可点：重试只是重新走一遍恢复。
                    enabled: card.restorePassword.length > 0 && !card.busy
                    onClicked: {
                        controller.startManagedRestoreWithPassword(
                            card.fileNameText,
                            card.pendingRestoreDestination,
                            card.restorePassword)
                        // 密码只服务这一次调用：先清掉本地副本，再关对话框。
                        card.clearRestoreSecrets()
                        restorePasswordDialog.close()
                    }
                }
            }
        }
    }

    // 删除确认。确认框不是安全边界，文件名校验仍然由 Catalog 自己做。
    Dialog {
        id: deleteDialog
        objectName: "deleteConfirmDialog"
        anchors.centerIn: parent
        modal: true
        // 同 restorePasswordDialog：这是同一套自定义 Dialog 样式，
        // 边距保持一致。
        padding: 18

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
                text: "确定要从备份仓库中删除 " + card.fileNameText
                    + " 吗？此操作不可撤销。"
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

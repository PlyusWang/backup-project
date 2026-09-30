// RemotePage.qml
//
// 远程备份页：把本机的备份存到云端服务器，也可以随时取回。
//
// 这一页只做四件事：展示、把输入交给 RemoteController、点按钮、展示进度。
// 它**不**自己开 socket、不拼协议帧、不解析归档、不重新打包压缩加密——
// 那些全部在共享的 RemoteArchiveClient 里，与 backupctl remote 是同一份实现。
//
// 页面按"用户先要看什么"分四层：
//   1. 连接服务器 —— 地址 / 端口 / 用户名 / 密码，注册与登录
//   2. 上传备份   —— 选一个本机的 .bak，传到云端
//   3. 云端备份   —— 列表（名称 / 大小 / 时间）+ 下载 / 删除
//   4. 技术详情   —— 默认折叠：编号、摘要、最近一次失败的技术原因
//
// 主界面不出现协议术语：没有 BPNET1 / PBKDF2 / HMAC / SQLite / opcode /
// frame / request_id，也没有原始 token。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page
    objectName: "remotePage"

    // ---------- 草稿 ----------
    // 输入框里的是草稿：只有点"登录 / 注册 / 上传 / 下载"时才交给控制器。
    // 用户每敲一个字符就改 endpoint 会让"上一个真正生效的地址"和"正在编辑的
    // 地址"混在一起。密码草稿只活在这一页的内存里，既不落盘也没有回读接口。
    property string draftHost: ""
    property string draftPort: ""
    property string draftUser: ""
    property string draftPassword: ""
    property string draftUploadPath: ""
    property string draftUploadName: ""
    property string draftDownloadPath: ""
    // 待确认的删除目标：确认对话框关闭之前一个字节都不会动。
    property string pendingDeleteId: ""
    property string pendingDeleteName: ""
    // 最近一次发起下载的目标：出错之后"覆盖并重新下载"要用同一个。
    property string pendingDownloadId: ""
    property bool technicalExpanded: false

    // 真正的互斥在控制器里（busy_ 在提交任务之前同步置位）；这里只是可见性。
    readonly property bool canOperate: remote.authenticated && !remote.busy
    readonly property int rowCount: remote.snapshots.length

    Component.onCompleted: {
        page.draftHost = remote.host
        page.draftPort = remote.portText
        page.draftUser = remote.username
    }

    // ---------- 从列表发起的动作 ----------
    // 删除永远先经过确认对话框：一次误点不能直接删掉云端的备份。
    function requestDelete(snapshotId, name) {
        page.pendingDeleteId = snapshotId
        page.pendingDeleteName = name
        deleteDialog.open()
    }

    function confirmDelete() {
        const target = page.pendingDeleteId
        deleteDialog.close()
        page.pendingDeleteId = ""
        page.pendingDeleteName = ""
        if (target !== "")
            remote.deleteSnapshot(target)
    }

    function requestDownload(snapshotId, name) {
        page.pendingDownloadId = snapshotId
        page.pendingDownloadName = name
        if (page.draftDownloadPath === "") {
            downloadDialog.currentFolder = remote.fileDialogStartUrl(controller.repositoryPath)
            downloadDialog.selectedFile = String(downloadDialog.currentFolder) + "/" + remote.suggestedDownloadName(name)
            downloadDialog.open()
            return
        }
        remote.downloadArchive(snapshotId, page.draftDownloadPath, false)
    }

    // 技术详情：默认折叠。这里是唯一允许出现编号、摘要与原始原因的地方。
    function technicalText() {
        const lines = []
        lines.push("服务器：" + remote.host + ":" + remote.portText)
        lines.push("账号：" + (remote.username === "" ? "（未填写）" : remote.username))
        lines.push("会话：" + remote.sessionText)
        lines.push("服务器只监听本机回环地址，客户端通过部署时配置的安全通道访问它。")
        if (remote.diagnosticText !== "")
            lines.push("最近一次失败的技术原因：" + remote.diagnosticText)
        for (let i = 0; i < remote.snapshots.length; ++i) {
            const item = remote.snapshots[i]
            lines.push("云端备份编号：" + item["id"]
                       + "（" + item["name"] + "，SHA-256 前 12 位 " + item["sha256Short"] + "）")
        }
        return lines.join("\n")
    }

    ScrollView {
        id: pageScroll
        objectName: "remotePageScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        Component.onCompleted: {
            if (pageScroll.contentItem)
                pageScroll.contentItem.boundsBehavior = Flickable.StopAtBounds
        }
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: column
            x: Math.max(32, (pageScroll.availableWidth - width) / 2)
            y: 20
            width: Math.min(pageScroll.availableWidth - 64, 1400)
            spacing: 16

            Text {
                text: "远程备份"
                font.pixelSize: 30
                font.weight: Font.DemiBold
                color: theme.textPrimary
            }

            Text {
                Layout.fillWidth: true
                text: "把本机的备份存到云端服务器，需要时可以再取回来。"
                font.pixelSize: 17
                color: theme.textSecondary
                Layout.topMargin: -8
            }

            // ---------- 1. 连接服务器 ----------
            AppCard {
                Layout.fillWidth: true
                Layout.topMargin: 4

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "连接服务器"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "服务器地址"
                                font.pixelSize: 15
                                color: theme.textSecondary
                            }

                            AppTextField {
                                id: hostField
                                objectName: "remoteHostField"
                                Layout.fillWidth: true
                                enabled: !remote.busy
                                placeholderText: "例如 127.0.0.1"
                                text: page.draftHost
                                onTextEdited: page.draftHost = text
                            }
                        }

                        ColumnLayout {
                            Layout.preferredWidth: 170
                            spacing: 4

                            Text {
                                text: "端口"
                                font.pixelSize: 15
                                color: theme.textSecondary
                            }

                            AppTextField {
                                id: portField
                                objectName: "remotePortField"
                                Layout.fillWidth: true
                                enabled: !remote.busy
                                placeholderText: "18765"
                                text: page.draftPort
                                onTextEdited: page.draftPort = text
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "用户名"
                                font.pixelSize: 15
                                color: theme.textSecondary
                            }

                            AppTextField {
                                id: userField
                                objectName: "remoteUserField"
                                Layout.fillWidth: true
                                enabled: !remote.busy
                                placeholderText: "字母、数字、点、下划线或减号"
                                text: page.draftUser
                                onTextEdited: page.draftUser = text
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "密码"
                                font.pixelSize: 15
                                color: theme.textSecondary
                            }

                            AppTextField {
                                id: passwordField
                                objectName: "remotePasswordField"
                                Layout.fillWidth: true
                                enabled: !remote.busy
                                // 密码永远不明文常显：不回读、不落盘、不进日志。
                                echoMode: TextInput.Password
                                placeholderText: "登录或注册时使用"
                                text: page.draftPassword
                                onTextEdited: page.draftPassword = text
                                onAccepted: {
                                    if (!remote.busy)
                                        remote.login(page.draftHost, page.draftPort, page.draftUser, page.draftPassword)
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                        spacing: 12

                        AppButton {
                            objectName: "remoteRegisterButton"
                            text: "注册"
                            enabled: !remote.busy
                            onClicked: remote.registerAccount(page.draftHost, page.draftPort, page.draftUser, page.draftPassword)
                        }

                        AppButton {
                            objectName: "remoteLoginButton"
                            text: "登录"
                            variant: "primary"
                            enabled: !remote.busy
                            onClicked: remote.login(page.draftHost, page.draftPort, page.draftUser, page.draftPassword)
                        }

                        AppButton {
                            objectName: "remoteLogoutButton"
                            text: "退出登录"
                            enabled: remote.authenticated && !remote.busy
                            onClicked: {
                                remote.logoutLocal()
                                // 退出之后这一页也不再留着口令草稿。
                                page.draftPassword = ""
                            }
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        objectName: "remoteSessionText"
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        text: remote.sessionText
                        font.pixelSize: 15
                        color: remote.authenticated ? theme.success : theme.textSecondary
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }

            // ---------- 2. 上传 ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "上传备份到云端"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        AppTextField {
                            id: uploadPathField
                            objectName: "remoteUploadPathField"
                            Layout.fillWidth: true
                            enabled: !remote.busy
                            placeholderText: "选择本机上的一个备份文件（.bak）"
                            text: page.draftUploadPath
                            onTextEdited: page.draftUploadPath = text
                        }

                        AppButton {
                            objectName: "remoteUploadBrowseButton"
                            text: "选择本地备份"
                            iconName: "folder"
                            enabled: !remote.busy
                            onClicked: {
                                uploadDialog.currentFolder = remote.fileDialogStartUrl(controller.repositoryPath)
                                uploadDialog.open()
                            }
                        }
                    }

                    AppTextField {
                        id: uploadNameField
                        objectName: "remoteUploadNameField"
                        Layout.fillWidth: true
                        enabled: !remote.busy
                        placeholderText: "云端名称（留空就用文件名）"
                        text: page.draftUploadName
                        onTextEdited: page.draftUploadName = text
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "上传的是备份工具自己生成的 .bak 文件；云端只负责保存和取回，不会重新打包、压缩或加密。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WrapAnywhere
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                        spacing: 12

                        AppButton {
                            objectName: "remoteUploadButton"
                            text: "上传"
                            variant: "primary"
                            enabled: page.canOperate
                            onClicked: remote.uploadArchive(page.draftUploadPath, page.draftUploadName)
                        }

                        Text {
                            objectName: "remoteUploadHint"
                            Layout.fillWidth: true
                            text: remote.authenticated ? "" : "登录之后才能上传。"
                            font.pixelSize: 15
                            color: theme.textSecondary
                            wrapMode: Text.WrapAnywhere
                        }
                    }
                }
            }

            // ---------- 3. 云端备份 ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Text {
                            text: "云端备份"
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            color: theme.textSecondary
                        }

                        Item { Layout.fillWidth: true }

                        AppButton {
                            objectName: "remoteRefreshButton"
                            text: "刷新"
                            iconName: "refresh"
                            enabled: page.canOperate
                            onClicked: remote.refreshList()
                        }
                    }

                    Text {
                        objectName: "remoteListSummary"
                        Layout.fillWidth: true
                        text: remote.listSummary
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WrapAnywhere
                    }

                    Text {
                        objectName: "remoteListEmptyText"
                        Layout.fillWidth: true
                        visible: page.rowCount === 0
                        text: remote.authenticated
                              ? (remote.listLoaded
                                 ? "云端还没有备份。点上面的“上传”把第一份备份放上去。"
                                 : "点“刷新”读取云端备份列表。")
                              : "登录之后可以查看云端备份。"
                        font.pixelSize: 16
                        color: theme.textSecondary
                        wrapMode: Text.WrapAnywhere
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: page.rowCount > 0
                        spacing: 8

                        AppTextField {
                            id: downloadPathField
                            objectName: "remoteDownloadTargetField"
                            Layout.fillWidth: true
                            enabled: !remote.busy
                            placeholderText: "下载保存到哪里（点右边的按钮选择）"
                            text: page.draftDownloadPath
                            onTextEdited: page.draftDownloadPath = text
                        }

                        AppButton {
                            objectName: "remoteDownloadBrowseButton"
                            text: "选择保存位置"
                            iconName: "folder"
                            enabled: !remote.busy
                            onClicked: {
                                downloadDialog.currentFolder = remote.fileDialogStartUrl(controller.repositoryPath)
                                downloadDialog.selectedFile = String(downloadDialog.currentFolder) + "/" + remote.suggestedDownloadName(page.pendingDownloadName)
                                downloadDialog.open()
                            }
                        }
                    }

                    Repeater {
                        id: snapshotRepeater
                        objectName: "remoteSnapshotRepeater"
                        model: remote.snapshots

                        delegate: RemoteSnapshotCard {
                            required property var modelData
                            Layout.fillWidth: true
                            snapshotId: String(modelData["id"] || "")
                            nameText: String(modelData["name"] || "")
                            sizeText: String(modelData["sizeText"] || "")
                            createdText: String(modelData["createdText"] || "")
                            busy: remote.busy
                            onDownloadRequested: function (snapshotId, name) { page.requestDownload(snapshotId, name) }
                            onDeleteRequested: function (snapshotId, name) { page.requestDelete(snapshotId, name) }
                        }
                    }
                }
            }

            // ---------- 传输进度 ----------
            // 上传与下载共用一条：direction 与字节数都来自网络层的真实回调。
            RowLayout {
                objectName: "remoteProgressRow"
                Layout.fillWidth: true
                visible: remote.transferActive
                spacing: 12

                Text {
                    objectName: "remoteProgressText"
                    text: remote.transferPhaseText + "  " + remote.progressText
                    font.pixelSize: 15
                    color: theme.textSecondary
                }

                ProgressBar {
                    id: transferBar
                    objectName: "remoteProgressBar"
                    Layout.fillWidth: true
                    Layout.maximumWidth: 360
                    from: 0
                    to: 1
                    value: remote.progressRatio
                    implicitHeight: 8

                    background: Rectangle {
                        radius: 4
                        color: theme.hover
                        border.width: 1
                        border.color: theme.border
                    }

                    contentItem: Item {
                        Rectangle {
                            width: transferBar.visualPosition * parent.width
                            height: parent.height
                            radius: 4
                            color: theme.accent
                        }
                    }
                }

                Text {
                    objectName: "remoteProgressRatioText"
                    text: Math.round(remote.progressRatio * 100) + "%"
                    font.pixelSize: 15
                    color: theme.textSecondary
                }
            }

            StatusBanner {
                objectName: "remoteStatusBanner"
                Layout.fillWidth: true
                pageScope: "remote"
                scope: remote.statusScope
                kind: remote.statusKind
                title: remote.statusTitle
                message: remote.statusMessage
            }

            // 目标已存在时只多给一个明确的动作，而不是常驻一个"覆盖"开关。
            AppButton {
                objectName: "remoteOverwriteButton"
                Layout.fillWidth: false
                visible: remote.lastErrorKind === "target-exists"
                text: "覆盖并重新下载"
                enabled: page.canOperate && page.pendingDownloadId !== ""
                onClicked: remote.downloadArchive(page.pendingDownloadId, page.draftDownloadPath, true)
            }

            // ---------- 4. 技术详情（默认折叠）----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true

                        AppButton {
                            objectName: "remoteTechnicalToggle"
                            variant: "flat"
                            iconName: "settings"
                            text: page.technicalExpanded ? "收起技术详情" : "技术详情"
                            onClicked: page.technicalExpanded = !page.technicalExpanded
                        }

                        Item { Layout.fillWidth: true }
                    }

                    TextEdit {
                        objectName: "remoteTechnicalText"
                        Layout.fillWidth: true
                        visible: page.technicalExpanded
                        text: page.technicalText()
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true
                        cursorVisible: false
                        textFormat: TextEdit.PlainText
                        font.pixelSize: 14
                        color: theme.textSecondary
                        selectionColor: theme.accent
                        selectedTextColor: theme.surface
                        wrapMode: TextEdit.WrapAnywhere
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }
    }

    // 选择本地备份。只负责"帮忙填"：填完之后路径仍然可以手改。
    FileDialog {
        id: uploadDialog
        objectName: "remoteUploadFileDialog"
        title: "选择要上传的备份文件"
        fileMode: FileDialog.OpenFile
        nameFilters: ["备份文件 (*.bak)", "所有文件 (*)"]
        onAccepted: page.draftUploadPath = remote.localPathFromUrl(uploadDialog.selectedFile)
    }

    FileDialog {
        id: downloadDialog
        objectName: "remoteDownloadFileDialog"
        title: "选择下载保存位置"
        fileMode: FileDialog.SaveFile
        nameFilters: ["备份文件 (*.bak)", "所有文件 (*)"]
        onAccepted: {
            const chosen = remote.localPathFromUrl(downloadDialog.selectedFile)
            if (chosen === "")
                return
            page.draftDownloadPath = chosen
            if (page.pendingDownloadId !== "")
                remote.downloadArchive(page.pendingDownloadId, chosen, false)
        }
    }

    // 删除确认。默认焦点在"取消"上：回车不会误删。
    Dialog {
        id: deleteDialog
        objectName: "remoteDeleteDialog"
        anchors.centerIn: parent
        modal: true
        padding: 18
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: theme.surfaceElevated
            border.width: 1
            border.color: theme.border
            radius: 10
        }

        contentItem: ColumnLayout {
            spacing: 12

            Text {
                text: "删除云端备份"
                color: theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Text {
                Layout.preferredWidth: 340
                text: "确定要删除云端的这一份备份吗？删除之后只能重新上传。\n\n"
                      + page.pendingDeleteName
                color: theme.textSecondary
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Item { Layout.fillWidth: true }

                AppButton {
                    objectName: "remoteDeleteCancelButton"
                    text: "取消"
                    onClicked: {
                        page.pendingDeleteId = ""
                        page.pendingDeleteName = ""
                        deleteDialog.close()
                    }
                }

                AppButton {
                    objectName: "remoteDeleteConfirmButton"
                    text: "确认删除"
                    variant: "primary"
                    enabled: !remote.busy
                    onClicked: page.confirmDelete()
                }
            }
        }
    }
}

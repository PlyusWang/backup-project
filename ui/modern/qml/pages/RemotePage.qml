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
    // 服务器身份指纹（server-key）：连接之前必须有的"我要连的是哪一台服务器"。
    // 它不是口令——公钥/指纹可以公开、可以抄进部署文档——但它是必填项：
    // 不填时客户端拒绝连接（不做"第一次见到谁就信谁"）。
    property string draftServerKeyPin: ""
    // 注册标签页的两个口令草稿（登录标签页继续用 draftPassword）。
    property string draftRegisterPassword: ""
    property string draftConfirmPassword: ""
    // 注销账户对话框：再次输入当前密码 + 逐字输入当前账户名。
    property string draftDeletePassword: ""
    property string draftDeleteName: ""
    // 账户区域当前标签页：0 = 登录，1 = 注册。
    property int accountTab: 0
    // 两个密码框都填了、但不一样：注册页据此显示红色错误（改一个字符就自动重算，
    // 所以错误会随用户修改立刻消失或更新）。
    readonly property bool registerPasswordMismatch:
        page.draftRegisterPassword !== "" && page.draftConfirmPassword !== "" &&
        page.draftRegisterPassword !== page.draftConfirmPassword
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
        // 调用方（CLI / 自检 harness）已经给过一个指纹就回填进来，界面上看到的
        // 永远是"现在真的会用哪一个"，而不是一个空框。
        page.draftServerKeyPin = remote.serverKeyPin
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

            // ---------- 1. 账户（登录 / 注册两个标签页）----------
            //
            // 人工验收的结论：用户名 / 密码 / 注册 / 登录 / 退出登录全堆在同一块
            // 里，用户分不清"我现在是在登录还是在注册"，注册也只有一个密码框。
            // 现在拆成两个标签页；已登录时整块换成账户卡片。
            AppCard {
                Layout.fillWidth: true
                Layout.topMargin: 4

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "服务器账户"
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
                                // 输入一改，上一次的错误就不再成立：立刻收起来，
                                // 免得旧原因挂在新输入上。
                                onTextEdited: {
                                    page.draftHost = text
                                    remote.clearLoginError()
                                    remote.clearRegisterError()
                                }
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
                                onTextEdited: {
                                    page.draftPort = text
                                    remote.clearLoginError()
                                    remote.clearRegisterError()
                                }
                            }
                        }
                    }

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
                            onTextEdited: {
                                page.draftUser = text
                                remote.clearLoginError()
                                remote.clearRegisterError()
                            }
                        }
                    }

                    // 服务器身份指纹：连接设置的一部分（地址 / 端口 / 用户名 /
                    // 指纹）。指纹错了就是一个输入问题，"应用"之后原因写在这
                    // 张表单自己的错误行里，不弹对话框，也不占用页面底部横幅。
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        spacing: 4

                        Text {
                            text: "服务器身份指纹（server-key）"
                            font.pixelSize: 15
                            color: theme.textSecondary
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            AppTextField {
                                id: serverKeyPinField
                                objectName: "remoteServerKeyPinField"
                                Layout.fillWidth: true
                                enabled: !remote.busy
                                placeholderText: "例如 sha256:0123…（服务器管理员给出）"
                                text: page.draftServerKeyPin
                                onTextEdited: {
                                    page.draftServerKeyPin = text
                                    remote.clearServerKeyPinError()
                                }
                                onAccepted: {
                                    if (!remote.busy)
                                        remote.setServerKeyPin(page.draftServerKeyPin)
                                }
                            }

                            AppButton {
                                objectName: "remoteServerKeyPinApplyButton"
                                text: "应用"
                                enabled: !remote.busy
                                onClicked: remote.setServerKeyPin(page.draftServerKeyPin)
                            }
                        }

                        // 指纹不是密码：这句话解释它为什么可以贴在这里，也说明它
                        // 只用来确认"连到的确实是你的服务器"。
                        Text {
                            text: "公钥指纹不是密码：它只用来确认连到的确实是你的服务器。"
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WrapAnywhere
                        }

                        Text {
                            objectName: "remoteServerKeyPinError"
                            Layout.fillWidth: true
                            visible: remote.serverKeyPinError !== ""
                            text: remote.serverKeyPinError
                            color: theme.error
                            font.pixelSize: 15
                            wrapMode: Text.WrapAnywhere
                        }
                    }

                    // ---------- 未登录：登录 / 注册两个标签页 ----------
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        visible: !remote.authenticated
                        spacing: 8

                        // 登录 / 注册是一个分段控件（二选一），不是两个独立按钮。
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            SegmentedTabs {
                                objectName: "remoteAccountTabs"
                                Layout.preferredWidth: 260
                                enabled: !remote.busy
                                currentKey: page.accountTab === 0 ? "login" : "register"
                                model: [
                                    { "key": "login", "text": "登录" },
                                    { "key": "register", "text": "注册" }
                                ]
                                onActivated: function (key) {
                                    page.accountTab = (key === "login") ? 0 : 1
                                }
                            }

                            Item { Layout.fillWidth: true }
                        }

                        // ---- 登录标签：密码只输一次 ----
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: page.accountTab === 0
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
                                placeholderText: "登录密码"
                                text: page.draftPassword
                                onTextEdited: {
                                    page.draftPassword = text
                                    remote.clearLoginError()
                                }
                                onAccepted: {
                                    if (!remote.busy)
                                        remote.login(page.draftHost, page.draftPort, page.draftUser, page.draftPassword)
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                spacing: 12

                                AppButton {
                                    objectName: "remoteLoginButton"
                                    text: "登录"
                                    variant: "primary"
                                    enabled: !remote.busy
                                    onClicked: remote.login(page.draftHost, page.draftPort, page.draftUser, page.draftPassword)
                                }

                                Text {
                                    objectName: "remoteReachabilityText"
                                    Layout.fillWidth: true
                                    // 只有真的试过一次连接之后才有内容：没试过
                                    // 的时候这一行根本不出现，也就不会有人把
                                    // "还没有连接"读成"服务器挂了"。
                                    visible: remote.serverReachabilityText !== ""
                                    text: remote.serverReachabilityText
                                    font.pixelSize: 15
                                    color: theme.textSecondary
                                    wrapMode: Text.WrapAnywhere
                                }
                            }

                            // 登录失败的原因就写在这张表单下面：用户是在这里点的
                            // "登录"，反馈也必须在这里看到——不再只出现在页面底部
                            // 的横幅里（那样看起来就像"点了没反应"）。
                            Text {
                                objectName: "remoteLoginError"
                                Layout.fillWidth: true
                                visible: remote.loginError !== ""
                                text: remote.loginError
                                color: theme.error
                                font.pixelSize: 15
                                wrapMode: Text.WrapAnywhere
                            }
                        }

                        // ---- 注册标签：两个密码框，必须完全一致 ----
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: page.accountTab === 1
                            spacing: 4

                            Text {
                                text: "密码"
                                font.pixelSize: 15
                                color: theme.textSecondary
                            }

                            AppTextField {
                                id: registerPasswordField
                                objectName: "remoteRegisterPasswordField"
                                Layout.fillWidth: true
                                enabled: !remote.busy
                                echoMode: TextInput.Password
                                placeholderText: "至少 8 个字符"
                                text: page.draftRegisterPassword
                                onTextEdited: {
                                    page.draftRegisterPassword = text
                                    remote.clearRegisterError()
                                }
                            }

                            Text {
                                text: "确认密码"
                                font.pixelSize: 15
                                color: theme.textSecondary
                                Layout.topMargin: 4
                            }

                            AppTextField {
                                id: registerConfirmField
                                objectName: "remoteRegisterConfirmField"
                                Layout.fillWidth: true
                                enabled: !remote.busy
                                // 两个密码框都必须是密码回显模式：确认密码不是
                                // "再看一眼明文"的地方。
                                echoMode: TextInput.Password
                                placeholderText: "再输入一次"
                                text: page.draftConfirmPassword
                                onTextEdited: {
                                    page.draftConfirmPassword = text
                                    remote.clearRegisterError()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                spacing: 12

                                AppButton {
                                    objectName: "remoteRegisterButton"
                                    text: "注册"
                                    variant: "primary"
                                    enabled: !remote.busy
                                    onClicked: remote.registerAccount(page.draftHost, page.draftPort, page.draftUser, page.draftRegisterPassword, page.draftConfirmPassword)
                                }

                                // 正常状态只给一句弱化的辅助文字；真的不一致时换成
                                // 红色提示。控制器已经报了错就让位——同一件事只说一遍。
                                Text {
                                    objectName: "remoteRegisterHint"
                                    Layout.fillWidth: true
                                    visible: !page.registerPasswordMismatch &&
                                             remote.registerError === ""
                                    text: "请再次输入密码以确认。"
                                    font.pixelSize: 15
                                    color: theme.textSecondary
                                    wrapMode: Text.WrapAnywhere
                                }

                                Text {
                                    objectName: "remoteRegisterMismatch"
                                    Layout.fillWidth: true
                                    visible: page.registerPasswordMismatch &&
                                             remote.registerError === ""
                                    text: "两次输入的密码不一致"
                                    font.pixelSize: 15
                                    color: theme.error
                                    wrapMode: Text.WrapAnywhere
                                }
                            }

                            // 注册被拒的原因写在这张表单下面（服务端说"该用户名已被
                            // 使用"也在这里），不再只出现在页面底部的横幅里。
                            Text {
                                objectName: "remoteRegisterError"
                                Layout.fillWidth: true
                                visible: remote.registerError !== ""
                                text: remote.registerError
                                color: theme.error
                                font.pixelSize: 15
                                wrapMode: Text.WrapAnywhere
                            }
                        }
                    }

                    // ---------- 已登录：账户卡片 ----------
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        visible: remote.authenticated
                        spacing: 4

                        Text {
                            objectName: "remoteAccountText"
                            Layout.fillWidth: true
                            text: "当前账户：" + remote.username
                            font.pixelSize: 17
                            color: theme.textPrimary
                        }

                        Text {
                            objectName: "remoteAccountStateText"
                            Layout.fillWidth: true
                            text: "状态：已登录"
                            font.pixelSize: 15
                            color: theme.success
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            spacing: 12

                            AppButton {
                                objectName: "remoteLogoutButton"
                                text: "退出登录"
                                enabled: !remote.busy
                                onClicked: {
                                    remote.logoutLocal()
                                    // 退出之后这一页也不再留着口令草稿。
                                    page.draftPassword = ""
                                }
                            }

                            AppButton {
                                objectName: "remoteDeleteAccountButton"
                                text: "注销账户"
                                enabled: !remote.busy
                                onClicked: {
                                    page.draftDeletePassword = ""
                                    page.draftDeleteName = ""
                                    remote.clearDeleteAccountError()
                                    deleteAccountDialog.open()
                                }
                            }

                            Item { Layout.fillWidth: true }
                        }
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

    // 注册成功 / 登录成功 / 注销成功之后的界面清理。
    //
    // 放在这里而不是按钮的 onClicked 里：这三件事的结果是异步回来的，
    // 只有操作真的结束了才知道该清哪些草稿、该不该切标签页。
    Connections {
        target: remote

        function onOperationFinished(kind, succeeded) {
            if (!succeeded)
                return
            if (kind === "register") {
                // 推荐行为：切回「登录」标签、保留刚注册的用户名、不保存密码。
                page.draftRegisterPassword = ""
                page.draftConfirmPassword = ""
                page.accountTab = 0
            } else if (kind === "login") {
                page.draftPassword = ""
            } else if (kind === "delete-account") {
                // 只有成功才走到这里（失败在上面 return 掉了）：关对话框、
                // 清掉三层口令草稿、回到登录标签。
                page.draftPassword = ""
                page.draftDeletePassword = ""
                page.draftDeleteName = ""
                page.accountTab = 0
                deleteAccountDialog.close()
            }
        }
    }

    // 注销账户。这是一个不可撤销的服务端删除，所以确认文案必须说清楚后果，
    // 而且要求"再次输入当前密码" + "逐字输入当前账户名"两件事都做对。
    Dialog {
        id: deleteAccountDialog
        objectName: "remoteDeleteAccountDialog"
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
                text: "注销账户"
                color: theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Text {
                Layout.preferredWidth: 420
                text: "注销账户会永久删除该账户以及全部云端备份，此操作无法撤销。\n\n"
                      + "当前账户：" + remote.username
                color: theme.textSecondary
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            AppTextField {
                id: deletePasswordField
                objectName: "remoteDeletePasswordField"
                Layout.fillWidth: true
                enabled: !remote.busy
                echoMode: TextInput.Password
                placeholderText: "再次输入当前密码"
                text: page.draftDeletePassword
                onTextEdited: {
                    page.draftDeletePassword = text
                    remote.clearDeleteAccountError()
                }
            }

            AppTextField {
                id: deleteNameField
                objectName: "remoteDeleteNameField"
                Layout.fillWidth: true
                enabled: !remote.busy
                placeholderText: "输入账户名以确认：" + remote.username
                text: page.draftDeleteName
                onTextEdited: {
                    page.draftDeleteName = text
                    remote.clearDeleteAccountError()
                }
            }

            // 失败原因就显示在这里（对话框内部）：用户是在这个对话框里点的
            // "确认注销"，结果也必须在这里看到——不能关掉对话框之后无事发生。
            Text {
                objectName: "remoteDeleteAccountError"
                Layout.fillWidth: true
                Layout.preferredWidth: 420
                visible: remote.deleteAccountError !== ""
                text: remote.deleteAccountError
                color: theme.error
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Item { Layout.fillWidth: true }

                AppButton {
                    objectName: "remoteDeleteAccountCancelButton"
                    text: "取消"
                    onClicked: {
                        page.draftDeletePassword = ""
                        page.draftDeleteName = ""
                        deleteAccountDialog.close()
                    }
                }

                AppButton {
                    objectName: "remoteDeleteAccountConfirmButton"
                    text: "确认注销"
                    variant: "primary"
                    enabled: !remote.busy
                    onClicked: {
                        // 只提交，不关对话框：注销是异步的，而且失败必须留在
                        // 对话框里显示原因。关闭由操作真正成功之后（下面
                        // Connections 的 delete-account 分支）来做。
                        remote.deleteAccount(page.draftDeletePassword, page.draftDeleteName)
                    }
                }
            }
        }
    }
}

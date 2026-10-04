// RemotePage.qml
//
// 远程备份页：把本机的备份存到云端服务器，也可以随时取回。
//
// 这一页只做四件事：展示、把输入交给 RemoteController、点按钮、展示进度。
// 它**不**自己开 socket、不拼协议帧、不解析归档、不重新打包压缩加密——
// 那些全部在共享的 RemoteArchiveClient 里，与 backupctl remote 是同一份实现。
//
// 页面按"用户先要看什么"分五层：
//   1. 连接服务器 —— 地址 / 端口 / 用户名 / 密码，注册与登录（含服务器身份指纹）
//   2. 远端备份   —— 选一个**源目录**，完整或增量备份到远端（产品级能力，
//                    与 backupctl remote backup 共用同一套 core）
//   3. 云端备份   —— 列表（名称 / 类型 / 代数 / 父 / 大小 / 时间）
//                    + 恢复（产品级链恢复）/ 下载归档 / 删除。
//                    列表里同时可能出现**三类**对象，它们一眼可分：
//                      [原始归档]  手动上传的归档：没有链、没有代数、没有父；
//                                  主操作是"恢复"（下载后按本地格式独立
//                                  恢复），不是链恢复。
//                      [完整备份]  产品链根：代数 0，主操作是"恢复"。
//                      [增量备份]  产品链成员：代数 N + 父快照前 12 位。
//   4. 高级       —— 上传 / 下载**原始归档**（低层 raw 操作，与上面的产品级
//                    备份是两件事，刻意分开放）
//   5. 技术详情   —— 默认折叠：编号、摘要、最近一次失败的技术原因
//
// "恢复"与"下载归档"的区别是这一页的重点之一：恢复会自动解析并下载整条依赖
// 链、逐成员校验、原子发布到目标目录；下载归档只是把云端那一个 blob 取回来。
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
    // ---- PR #22 连接方式 ----
    // 客户端怎么到达服务端。"ssh" = 由本程序管理一条 SSH 安全通道（默认，
    // 因为当前部署的服务端只监听它自己的回环地址）；"direct" = 直连（高级，
    // 逻辑与 PR #21 完全一样）。
    property string draftConnectionMode: "ssh"
    // ~/.ssh/config 里的别名（当前部署是 aliyun-ecs）或 user@host。
    property string draftSshHost: ""
    // 空 = 自动挑一个空闲回环端口。**不**默认写死 18765：那个端口很可能已经
    // 被用户自己开的隧道占着，写死就会撞车。
    property string draftSshLocalPort: ""
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

    // ---- PR #21 产品级远端备份 ----
    // 这一页只收集"源目录 + 策略"和"目标快照 + 目标目录"；能不能续链、父是
    // 谁、代数、lineage、要不要 bootstrap 缓存，全部由共享 core 决定。
    property string draftBackupSource: ""
    // 0 = 完整（默认），1 = 增量。增量在没有可信基线时会由 core 自动改成
    // 完整基线，页面按控制器回来的**实际类型**显示。
    property int backupStrategy: 0
    // 待恢复的目标：目录对话框选完之后才真正发起恢复。
    property string pendingRestoreId: ""
    property string pendingRestoreName: ""
    property string draftRestorePath: ""
    // ---- 原始归档的"恢复"（与产品级链恢复是两套实现，但对用户是同一个动作）----
    // 原始归档没有链：先把那一个 blob 下载下来，再按本地备份格式独立恢复。
    // 第一段只问目标目录；**只有** core 明确说"这份备份加密了"之后才出现密码
    // 输入框（第二段）。目标目录与密码都只活在这个对话框里；密码不落盘、不回读、
    // 不进日志，界面上永远是掩码。
    property string pendingRawRestoreId: ""
    property string pendingRawRestoreName: ""
    property string draftRawRestorePath: ""
    property string draftRawRestorePassword: ""

    // 真正的互斥在控制器里（busy_ 在提交任务之前同步置位）；这里只是可见性。
    readonly property bool canOperate: remote.authenticated && !remote.busy
    readonly property int rowCount: remote.snapshots.length
    // 输入框里显示的指纹与**已经生效**的指纹不一致 = 尚未应用。
    // 这条绑定就是"不要让输入框显示 A、控制器实际用 B 而界面毫无提示"的
    // 那一条提示（§4）。
    readonly property bool pinDirty:
        page.draftServerKeyPin.trim() !== remote.appliedServerKeyPin
    readonly property bool sshMode: page.draftConnectionMode === "ssh"
    // 官方云端：身份与地址都来自编译进二进制的 profile，界面上不需要任何输入。
    readonly property bool officialMode: page.draftConnectionMode === "official"
    // 通道状态的颜色：建好了是绿的，失败是红的，正在动是黄的，没启动是灰的。
    readonly property color tunnelColor:
        remote.tunnelState === "ready" ? theme.success
        : remote.tunnelState === "failed" ? theme.error
        : (remote.tunnelState === "starting" || remote.tunnelState === "stopping")
            ? theme.warning
            : theme.textSecondary
    // 通道状态左边那个圆点：只有"已建立"才实心。
    readonly property string tunnelDot:
        remote.tunnelState === "ready" ? "●" : "○"

    Component.onCompleted: {
        page.draftHost = remote.host
        page.draftPort = remote.portText
        page.draftUser = remote.username
        // 调用方（CLI / 自检 harness）已经给过一个指纹就回填进来，界面上看到的
        // 永远是"现在真的会用哪一个"，而不是一个空框。
        page.draftServerKeyPin = remote.serverKeyPin
        // 连接方式与 SSH 参数也回填：调用方（自检 / 命令行）已经配好的值要
        // 出现在页面上，用户看到的永远是"现在真的会用哪一个"。
        page.draftConnectionMode = remote.connectionMode
        page.draftSshHost = remote.sshHost
        page.draftSshLocalPort = remote.sshLocalPort
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

    // 恢复一份远端备份。
    //
    // 两种类型走**两条不同的路**，所以对话框也不同：
    //   * 产品级（完整备份 / 增量备份）：选目标目录 -> 自动取回整条依赖链再恢复；
    //   * 原始归档（lineage 为空）：选目标目录（+ 加密时才需要的密码）->
    //     下载那一个 blob、按本地格式独立恢复。
    // 判断只看控制器给的类型，不按名字猜。
    function requestRestore(snapshotId, name, isRawArchive) {
        if (isRawArchive) {
            page.requestRawRestore(snapshotId, name)
            return
        }
        page.pendingRestoreId = snapshotId
        page.pendingRestoreName = name
        restoreDialog.currentFolder = remote.fileDialogStartUrl(page.draftRestorePath)
        restoreDialog.open()
    }

    // 原始归档：打开"恢复"对话框（第一段：只问目标目录）。密码留空表示"没填"
    // ——只有 core 真的说这份归档需要密码，界面才会要它，这里不做任何猜测。
    function requestRawRestore(snapshotId, name) {
        page.pendingRawRestoreId = snapshotId
        page.pendingRawRestoreName = name
        // 口令永远不预填（它只活在这一页的内存里）；目标目录保留上一次的选择，
        // 与产品级恢复对话框同一条行为：用户改了目录也会被记住。
        page.draftRawRestorePassword = ""
        rawRestorePasswordField.text = ""
        rawRestoreFolderDialog.currentFolder =
            remote.fileDialogStartUrl(page.draftRawRestorePath)
        rawRestoreDialog.open()
    }

    // 对话框的确认按钮：第一段是"开始恢复"，第二段是"继续恢复（用这次输入的
    // 密码）"。两段是同一次交互——第二段不会重新下载那份归档。
    function confirmRawRestore() {
        if (remote.rawRestoreAwaitingPassword) {
            const password = page.draftRawRestorePassword
            if (password === "")
                return
            // 输入框立刻清空：口令不在界面上多留一刻。提交的是上面这一份拷贝。
            page.draftRawRestorePassword = ""
            rawRestorePasswordField.text = ""
            remote.restoreRawArchiveWithPassword(password)
            return
        }
        const target = page.pendingRawRestoreId
        const destination = page.draftRawRestorePath
        if (target === "" || destination === "")
            return
        remote.restoreRawArchive(target, destination, "")
    }

    // 关闭对话框并让控制器放下这次交互（临时归档立刻删掉）。取消 / 成功 /
    // 致命失败三条路都走它。
    function closeRawRestoreDialog(abandonInteraction) {
        page.draftRawRestorePassword = ""
        rawRestorePasswordField.text = ""
        page.pendingRawRestoreId = ""
        page.pendingRawRestoreName = ""
        if (abandonInteraction)
            remote.cancelRawRestore()
        rawRestoreDialog.close()
    }

    // 策略当前值（SegmentedTabs 用的键）。
    function backupStrategyKey() {
        return page.backupStrategy === 1 ? "incremental" : "full"
    }

    // 技术详情：默认折叠。这里是唯一允许出现编号、摘要与原始原因的地方。
    function technicalText() {
        const lines = []
        lines.push("服务器：" + remote.host + ":" + remote.portText)
        lines.push("账号：" + (remote.username === "" ? "（未填写）" : remote.username))
        lines.push("会话：" + remote.sessionText)
        lines.push("连接方式：" + (page.sshMode ? "SSH 安全通道" : "直接连接"))
        if (page.sshMode) {
            lines.push("SSH 主机：" + (remote.sshHost === "" ? "（未填写）" : remote.sshHost))
            lines.push("通道状态：" + remote.tunnelState + "（" + remote.tunnelStateText + "）")
            lines.push("本地端点：" + (remote.tunnelLocalEndpointText === ""
                                       ? "（尚未分配）" : remote.tunnelLocalEndpointText))
            if (remote.tunnelReady)
                lines.push("远端端点：" + remote.tunnelRemoteEndpointText)
            lines.push("通道进程：" + (remote.tunnelOwnedByApp
                                       ? "由本程序启动（退出时会自动结束）"
                                       : (remote.tunnelExternalReuse
                                          ? "复用已存在的本地监听者（本程序不会结束它）"
                                          : "本程序没有启动任何进程")))
            if (remote.tunnelDiagnosticText !== "")
                lines.push("ssh 诊断输出：" + remote.tunnelDiagnosticText)
        }
        // 这一行必须与**当前**连接方式一致：直连模式下写"通过 SSH 安全通道访问"
        // 就是在技术详情里说假话（本轮审查抓到的 D2）。
        lines.push(page.sshMode
                   ? "服务端只监听它自己的回环地址；客户端通过上面这条 SSH 安全通道访问它。"
                   : "直连模式：客户端直接连接上面的服务器地址与端口（没有经过任何隧道）。")
        if (remote.diagnosticText !== "")
            lines.push("最近一次失败的技术原因：" + remote.diagnosticText)
        // core 给出的“为什么这次不是增量”的原始理由（英文）：属于诊断，
        // 只出现在这张默认折叠的卡片里，不出现在结论那一行。
        if (remote.backupBaselineReason !== "")
            lines.push("最近一次基线的核心原因：" + remote.backupBaselineReason)
        for (let i = 0; i < remote.snapshots.length; ++i) {
            const item = remote.snapshots[i]
            let line = "云端备份编号：" + item["id"]
                       + "（" + item["name"] + "，SHA-256 前 12 位 " + item["sha256Short"]
                       + "，类型 " + item["kind"] + "，代数 " + item["generation"]
            if (item["kind"] === "incremental")
                line += "，父 " + item["parentId"]
            line += "）"
            lines.push(line)
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

                    // ---------- 连接方式（PR #22）----------
                    // 服务端只监听它自己的回环地址，所以"客户端怎么到达它"必须
                    // 是产品的一部分，而不是用户脑子里的一条备注。
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        spacing: 6

                        Text {
                            text: "连接方式"
                            font.pixelSize: 15
                            color: theme.textSecondary
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            SegmentedTabs {
                                objectName: "remoteConnectionModeTabs"
                                Layout.preferredWidth: 420
                                enabled: !remote.busy
                                currentKey: page.draftConnectionMode
                                model: [
                                    { "key": "official", "text": "官方云端" },
                                    { "key": "ssh", "text": "SSH 安全通道（推荐）" },
                                    { "key": "direct", "text": "直接连接（高级）" }
                                ]
                                onActivated: function (key) {
                                    page.draftConnectionMode = key
                                    remote.setConnectionMode(key)
                                    // 换模式不动已经建立的通道，也不联网：
                                    // 这是配置动作。页面把状态原样显示出来。
                                    page.draftSshHost = remote.sshHost
                                    page.draftSshLocalPort = remote.sshLocalPort
                                }
                            }

                            Item { Layout.fillWidth: true }
                        }
                    }

                    // ---------- 官方云端（仅官方模式）----------
                    // 这一块**故意**只有一个标题和一句话：官方云端就是
                    // 不需要用户填任何东西，所以界面上也不该出现任何输入框。
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.officialMode
                        spacing: 6

                        Text {
                            objectName: "remoteOfficialCloudName"
                            text: remote.officialCloudName
                            font.pixelSize: 18
                            font.bold: true
                            color: theme.textSecondary
                        }

                        Text {
                            text: "服务器身份由官方根签发的证书自动校验；不需要配置隧道，也不需要核对任何指纹。"
                            wrapMode: Text.WordWrap
                            color: theme.textSecondary
                        }
                    }

                    // ---------- SSH 安全通道（仅 SSH 模式）----------
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.sshMode
                        spacing: 6

                        // 两个输入框各有自己的标签。
                        //
                        // 人工验收的结论：把"本地端口（留空 = 自动）"整句塞进
                        // placeholder 会被 200px 的框截断，用户根本读不到"留空"
                        // 是什么意思。所以标签写"本地端口（可选）"，placeholder
                        // 只留"留空则自动选择"，并把框加宽到放得下整句提示。
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Text {
                                    text: "SSH 主机"
                                    font.pixelSize: 15
                                    color: theme.textSecondary
                                }

                                AppTextField {
                                    id: sshHostField
                                    objectName: "remoteSshHostField"
                                    Layout.fillWidth: true
                                    enabled: !remote.busy
                                    placeholderText: "例如 aliyun-ecs（~/.ssh/config 里的别名）"
                                    text: page.draftSshHost
                                    onTextEdited: {
                                        page.draftSshHost = text
                                        remote.sshHost = text
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.preferredWidth: 240
                                spacing: 4

                                Text {
                                    text: "本地端口（可选）"
                                    font.pixelSize: 15
                                    color: theme.textSecondary
                                }

                                AppTextField {
                                    id: sshLocalPortField
                                    objectName: "remoteSshLocalPortField"
                                    Layout.fillWidth: true
                                    enabled: !remote.busy
                                    placeholderText: "留空则自动选择"
                                    text: page.draftSshLocalPort
                                    onTextEdited: {
                                        page.draftSshLocalPort = text
                                        remote.sshLocalPort = text
                                    }
                                }
                            }
                        }

                        // 这段话只回答一个问题：上面两个框和下面两个框是什么关系。
                        // 它们是同一条链路的**两端**，不是要填两套服务器地址。
                        Text {
                            Layout.fillWidth: true
                            text: "连接链路：本机自动端口 → SSH 主机 → 远端服务地址和端口。"
                                  + "本地端口留空时由程序自动选择。"
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WrapAnywhere
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "不会修改任何网络配置，也不会弱化 SSH 主机密钥校验。"
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WrapAnywhere
                        }

                        // 状态行：普通用户只需要看懂这一句。
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 2
                            spacing: 8

                            Text {
                                objectName: "remoteTunnelDot"
                                text: page.tunnelDot
                                font.pixelSize: 16
                                color: page.tunnelColor
                            }

                            Text {
                                objectName: "remoteTunnelStateText"
                                Layout.fillWidth: true
                                text: remote.tunnelStateText
                                font.pixelSize: 16
                                font.weight: Font.DemiBold
                                color: page.tunnelColor
                                wrapMode: Text.WrapAnywhere
                            }
                        }

                        Text {
                            objectName: "remoteTunnelEndpointText"
                            Layout.fillWidth: true
                            visible: remote.tunnelReady
                            text: remote.tunnelOwnedByApp
                                      ? ("本地 " + remote.tunnelLocalEndpointText
                                         + "  →  " + remote.tunnelRemoteEndpointText
                                         + "（本程序启动的 ssh，退出时会自动结束）")
                                      : ("本地 " + remote.tunnelLocalEndpointText
                                         + " 上已有一条隧道（不是本程序启动的，"
                                         + "退出时不会结束它）")
                            font.pixelSize: 14
                            color: theme.textSecondary
                            wrapMode: Text.WrapAnywhere
                        }

                        // 失败原因：一句可以照着做的中文，而不是"网络错误"。
                        Text {
                            objectName: "remoteTunnelFailureText"
                            Layout.fillWidth: true
                            visible: remote.tunnelFailureText !== ""
                            text: remote.tunnelFailureText
                            font.pixelSize: 15
                            color: theme.error
                            wrapMode: Text.WrapAnywhere
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 2
                            spacing: 12

                            AppButton {
                                objectName: "remoteEnsureConnectionButton"
                                text: "建立连接"
                                variant: "primary"
                                enabled: !remote.busy
                                onClicked: remote.ensureConnection(page.draftHost,
                                                                   page.draftPort)
                            }

                            AppButton {
                                objectName: "remoteStopTunnelButton"
                                text: "关闭安全通道"
                                enabled: !remote.busy && remote.tunnelState !== "stopped"
                                onClicked: remote.stopTunnel()
                            }

                            Item { Layout.fillWidth: true }
                        }
                    }

                    // 只有 SSH 模式需要这句话：直连模式下这两个框就是客户端要
                    // 连的地方，不存在"先登录谁、再连谁"的歧义。
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        visible: page.sshMode
                        text: "SSH 登录到服务器后，将连接这里填写的服务地址和端口。"
                        font.pixelSize: 14
                        color: theme.textSecondary
                        wrapMode: Text.WrapAnywhere
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                // 同一个输入框，两种含义：SSH 模式下它是"隧道
                                // 那头的服务在哪"，直连模式下它就是客户端要连的
                                // 地方。标签跟着模式走，避免用户按错误的含义填。
                                text: page.sshMode ? "远端服务地址" : "服务器地址"
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
                            Layout.preferredWidth: 190
                            spacing: 4

                            Text {
                                text: page.sshMode ? "远端服务端口" : "端口"
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
                                    // 旧结论不能挂在新输入上：错误行与"✓ 已应用"
                                    // 都立刻收起来，下面重新显示"尚未应用"。
                                    remote.clearServerKeyPinError()
                                    remote.clearPinApplyState()
                                }
                                onAccepted: {
                                    if (!remote.busy)
                                        remote.applyServerKeyPin(page.draftServerKeyPin)
                                }
                            }

                            AppButton {
                                objectName: "remoteServerKeyPinApplyButton"
                                text: "应用"
                                enabled: !remote.busy
                                // PR #22：这里以前只是 setServerKeyPin，返回值
                                // 被丢掉了 —— 用户点完"应用"界面上什么都不发生。
                                // 现在它写一行看得见的结果。
                                onClicked: remote.applyServerKeyPin(page.draftServerKeyPin)
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

                        // "应用"的结果：必须是**看得见**的（PR #22 修掉的
                        // 那个人工验收问题）。三种可能各有各的一句话：
                        // 已应用 / 已是当前值 / 尚未应用（输入框改过但没提交）。
                        Text {
                            objectName: "remoteServerKeyPinApplied"
                            Layout.fillWidth: true
                            visible: remote.pinApplyMessage !== ""
                            text: remote.pinApplyMessage
                            color: theme.success
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            wrapMode: Text.WrapAnywhere
                        }

                        Text {
                            objectName: "remoteServerKeyPinDirty"
                            Layout.fillWidth: true
                            // 输入框里是 A、真正生效的是 B 的时候必须说出来。
                            visible: page.pinDirty && remote.serverKeyPinError === ""
                                     && remote.pinApplyMessage === ""
                            text: "尚未应用：登录 / 注册会直接采用这里填的指纹并自动生效。"
                            color: theme.warning
                            font.pixelSize: 14
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
                                        remote.loginWithPin(page.draftHost, page.draftPort,
                                                            page.draftUser, page.draftPassword,
                                                            page.draftServerKeyPin)
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
                                    // PR #22：登录前先把**当前输入框里的**
                                    // 指纹自动提交掉。用户不需要记住"先应用再登录"。
                                    onClicked: remote.loginWithPin(page.draftHost, page.draftPort,
                                                                   page.draftUser, page.draftPassword,
                                                                   page.draftServerKeyPin)
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
                                    onClicked: remote.registerAccountWithPin(page.draftHost, page.draftPort,
                                                                             page.draftUser, page.draftRegisterPassword,
                                                                             page.draftConfirmPassword,
                                                                             page.draftServerKeyPin)
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

            // ---------- 2. 远端备份（产品级）----------
            // 对应 backupctl remote backup：把一个**目录**备份到远端。
            // 这一块只收集"源目录 + 策略"；能不能续链、父是谁、代数、lineage
            // 全部由共享 core 决定。用户选的策略与实际产出的类型不一定相同
            // （没有可信基线时会重建完整基线），所以下面那一行结论说的是实际值。
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "远端备份"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "把一个目录备份到远端。增量只上传变化的部分；云端还没有可续的链时会先自动建一份完整基线。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WrapAnywhere
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        spacing: 8

                        AppTextField {
                            id: backupSourceField
                            objectName: "remoteBackupSourceField"
                            Layout.fillWidth: true
                            enabled: !remote.busy
                            readOnly: true
                            placeholderText: "选择要备份到远端的目录"
                            text: page.draftBackupSource
                        }

                        AppButton {
                            objectName: "remoteBackupSourceBrowseButton"
                            text: "选择源目录"
                            iconName: "folder"
                            enabled: !remote.busy
                            onClicked: {
                                backupSourceDialog.currentFolder = remote.fileDialogStartUrl(page.draftBackupSource)
                                backupSourceDialog.open()
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        spacing: 12

                        Text {
                            text: "策略"
                            font.pixelSize: 15
                            color: theme.textSecondary
                        }

                        SegmentedTabs {
                            objectName: "remoteBackupStrategyTabs"
                            model: [
                                { "key": "full", "text": "完整" },
                                { "key": "incremental", "text": "增量" }
                            ]
                            currentKey: page.backupStrategyKey()
                            onActivated: function (key) {
                                page.backupStrategy = (key === "incremental") ? 1 : 0
                                // 上一次的结论不能挂在新策略上。
                                remote.clearBackupSummary()
                            }
                        }

                        Text {
                            objectName: "remoteBackupStrategyHint"
                            Layout.fillWidth: true
                            text: page.backupStrategy === 1
                                  ? "增量：云端没有可续的链时会自动先建完整基线。"
                                  : "完整：每次都创建一份完整的基线快照。"
                            font.pixelSize: 15
                            color: theme.textSecondary
                            wrapMode: Text.WrapAnywhere
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                        spacing: 12

                        AppButton {
                            objectName: "remoteBackupButton"
                            text: "开始远端备份"
                            variant: "primary"
                            enabled: page.canOperate && page.draftBackupSource !== ""
                            onClicked: remote.backupRemote(page.draftBackupSource, page.backupStrategy === 1)
                        }

                        Text {
                            objectName: "remoteBackupHint"
                            Layout.fillWidth: true
                            text: remote.authenticated
                                  ? (page.draftBackupSource === ""
                                     ? "先点“选择源目录”挑一个要备份的目录。"
                                     : "")
                                  : "登录之后才能开始远端备份。"
                            font.pixelSize: 15
                            color: theme.textSecondary
                            wrapMode: Text.WrapAnywhere
                        }
                    }

                    // 长操作进行中的一行状态：进度回调到达之前（连接、握手、
                    // 重新生成材料包）也有东西可看，而不是只看到一排灰掉的按钮。
                    // 内容来自控制器的 busyAction，不是画上去的假进度。
                    Text {
                        objectName: "remoteBusyText"
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        visible: remote.busy
                        text: remote.busyAction
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WrapAnywhere
                    }

                    // 最近一次远端备份的**实际**结论：完整 / 增量 / 没有变化。
                    // 这一行不会因为别的操作被顶掉（与页面底部的临时提示不同）。
                    Text {
                        objectName: "remoteBackupSummary"
                        Layout.fillWidth: true
                        visible: remote.backupSummary !== ""
                        text: remote.backupSummary
                        font.pixelSize: 15
                        color: theme.textPrimary
                        wrapMode: Text.WrapAnywhere
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

                    // 两种恢复机制的一句话区别。列表里同时存在"完整备份 /
                    // 增量备份"（产品链）与"原始归档"（手动上传的归档）时，
                    // 这句话让用户不用点开就知道两个按钮不是一回事。
                    Text {
                        objectName: "remoteRestoreMechanismHint"
                        Layout.fillWidth: true
                        visible: page.rowCount > 0
                        text: "三种备份都点「恢复」：完整备份 / 增量备份会自动取回整条依赖链；原始归档会先下载、校验，再按本地备份格式独立恢复。"
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
                                 ? "云端还没有备份。选好源目录后点上面的“开始远端备份”；原始归档用最下面的“高级：原始归档”上传。"
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
                            placeholderText: "下载归档保存到哪里（原始归档，点右边的按钮选择）"
                            text: page.draftDownloadPath
                            onTextEdited: page.draftDownloadPath = text
                        }

                        AppButton {
                            objectName: "remoteDownloadBrowseButton"
                            text: "选择归档保存位置"
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
                            kindText: String(modelData["kindText"] || "")
                            kindKey: String(modelData["kind"] || "")
                            rawArchive: modelData["rawArchive"] === true
                            generationVisible: modelData["generationVisible"] !== false
                            generation: Number(modelData["generation"] || 0)
                            parentShort: String(modelData["parentShort"] || "")
                            typeNote: String(modelData["typeNote"] || "")
                            restoreLabel: String(modelData["restoreLabel"] || "恢复")
                            restorable: modelData["restorable"] !== false
                            restoreHint: String(modelData["restoreHint"] || "")
                            busy: remote.busy
                            onRestoreRequested: function (snapshotId, name) {
                                page.requestRestore(snapshotId, name, modelData["rawArchive"] === true)
                            }
                            onDownloadRequested: function (snapshotId, name) { page.requestDownload(snapshotId, name) }
                            onDeleteRequested: function (snapshotId, name) { page.requestDelete(snapshotId, name) }
                        }
                    }
                }
            }

            // ---------- 4. 高级：原始归档 ----------
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: "上传原始归档（高级）"
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
                        objectName: "remoteUploadExplanation"
                        Layout.fillWidth: true
                        text: "这是低层操作：把一个已经存在的 .bak 原样放到云端，与上面的“远端备份”不是一回事——它不参与增量链，也不会出现在链恢复里。上传之后，列表里那一条是【原始归档】：要取回来就用它的“恢复”（下载后按本地格式独立恢复）或“下载归档”。"
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

    // 选择远端备份的源目录（产品级备份）。
    FolderDialog {
        id: backupSourceDialog
        objectName: "remoteBackupSourceFolderDialog"
        title: "选择要备份到远端的目录"
        onAccepted: {
            const chosen = remote.localPathFromUrl(backupSourceDialog.selectedFolder)
            if (chosen === "")
                return
            page.draftBackupSource = chosen
            // 换了源目录，上一次的结论（完整 / 增量 / 无变化）就不再适用。
            remote.clearBackupSummary()
        }
    }

    // 链恢复的目标目录：选完立刻发起恢复。用户只需要选"恢复到哪里"，
    // 依赖链的解析与下载全部由 core 负责。
    FolderDialog {
        id: restoreDialog
        objectName: "remoteRestoreFolderDialog"
        title: "选择恢复到哪个目录"
        onAccepted: {
            const chosen = remote.localPathFromUrl(restoreDialog.selectedFolder)
            if (chosen === "")
                return
            page.draftRestorePath = chosen
            const target = page.pendingRestoreId
            page.pendingRestoreId = ""
            if (target !== "")
                remote.restoreSnapshot(target, chosen)
        }
    }

    // 原始归档恢复的目标目录。选完只填进对话框，**不**立刻发起：用户还要
    // 点一次"恢复"才是明确意图。
    FolderDialog {
        id: rawRestoreFolderDialog
        objectName: "remoteRawRestoreFolderDialog"
        title: "选择恢复到哪个目录"
        onAccepted: {
            const chosen = remote.localPathFromUrl(rawRestoreFolderDialog.selectedFolder)
            if (chosen !== "")
                page.draftRawRestorePath = chosen
        }
    }

    // 恢复一份原始归档。对话框分两段，分界线是**控制器回来的事实**，不是猜测：
    //
    //   第一段（!rawRestoreDialog.passwordStage）：只问恢复到哪。用户点"恢复"
    //     就是明确意图，程序立刻下载 -> 校验 SHA-256 -> 按内容识别 -> 交给既有
    //     的本地恢复核心。没有加密的归档到这里就结束了，全程看不到密码框。
    //   第二段（rawRestoreDialog.passwordStage）：core 明确回来说"这份备份是
    //     加密的、需要密码"之后才出现。输入框是掩码，密码不进日志、不落盘；
    //     重输密码用的是**同一份**已经下载并校验过的字节。
    //
    // 这一页只说三件事：恢复到哪、要不要密码、会走哪条路。它**不**解析归档、
    // 不解密、不解压：那些全部在共享的本地恢复核心（core）里，与 CLI 的
    // backupctl restore / GUI 的本地恢复是同一份实现。
    Dialog {
        id: rawRestoreDialog
        objectName: "remoteRawRestoreDialog"
        anchors.centerIn: parent
        modal: true
        padding: 18
        closePolicy: Popup.CloseOnEscape

        readonly property bool passwordStage: remote.rawRestoreAwaitingPassword

        // 关闭（Esc / 点对话框外面）也是一次"取消"：不能把"正等着输密码"的
        // 交互和它的临时归档留在后台。cancelRawRestore 是幂等的——会话已经
        // 结束（成功 / 致命失败）时它什么都不做；忙碌时它也不会去碰后台线程
        // 正在用的那个会话。
        onClosed: {
            if (remote.rawRestoreAwaitingPassword && !remote.busy)
                remote.cancelRawRestore()
        }

        background: Rectangle {
            color: theme.surfaceElevated
            border.width: 1
            border.color: theme.border
            radius: 10
        }

        contentItem: ColumnLayout {
            spacing: 10

            Text {
                objectName: "remoteRawRestoreTitle"
                text: rawRestoreDialog.passwordStage
                      ? "输入恢复密码"
                      : "恢复原始归档"
                color: theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            // ---------- 第一段：目标目录 ----------
            Text {
                objectName: "remoteRawRestoreDialogText"
                visible: !rawRestoreDialog.passwordStage
                Layout.preferredWidth: 430
                text: "目标：" + page.pendingRawRestoreName + "\n\n"
                      + "会先下载这份归档并校验 SHA-256，再按本地备份格式恢复。"
                      + "原始归档不属于远端增量链：它没有父快照，也不会自动取回别的对象。"
                color: theme.textSecondary
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                visible: !rawRestoreDialog.passwordStage

                AppTextField {
                    id: rawRestoreTargetField
                    objectName: "remoteRawRestoreTargetField"
                    Layout.fillWidth: true
                    enabled: !remote.busy
                    placeholderText: "恢复到哪个目录（必填）"
                    text: page.draftRawRestorePath
                    onTextEdited: page.draftRawRestorePath = text
                }

                AppButton {
                    objectName: "remoteRawRestoreBrowseButton"
                    text: "选择恢复位置"
                    iconName: "folder"
                    enabled: !remote.busy
                    onClicked: rawRestoreFolderDialog.open()
                }
            }

            Text {
                objectName: "remoteRawRestoreBusyText"
                visible: !rawRestoreDialog.passwordStage && remote.busy
                text: "正在下载并校验这份归档…"
                color: theme.textSecondary
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            // ---------- 第二段：密码 ----------
            Text {
                objectName: "remoteRawRestorePasswordPrompt"
                visible: rawRestoreDialog.passwordStage
                Layout.preferredWidth: 430
                text: "此备份已加密，请输入恢复密码。\n\n"
                      + "归档已经下载并通过 SHA-256 校验，输错密码可以直接重输，"
                      + "不会重新下载。"
                color: theme.textSecondary
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "remoteRawRestoreDestinationEcho"
                visible: rawRestoreDialog.passwordStage
                Layout.fillWidth: true
                text: "恢复到：" + remote.rawRestoreDestinationText
                color: theme.textSecondary
                font.pixelSize: 15
                wrapMode: Text.WrapAnywhere
            }

            AppTextField {
                id: rawRestorePasswordField
                objectName: "remoteRawRestorePasswordField"
                visible: rawRestoreDialog.passwordStage
                Layout.fillWidth: true
                enabled: !remote.busy
                echoMode: TextInput.Password
                placeholderText: "恢复密码"
                text: page.draftRawRestorePassword
                onTextEdited: page.draftRawRestorePassword = text
            }

            Text {
                objectName: "remoteRawRestorePasswordError"
                visible: rawRestoreDialog.passwordStage && text !== ""
                Layout.preferredWidth: 430
                text: remote.rawRestorePasswordError
                color: theme.error
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Item { Layout.fillWidth: true }

                AppButton {
                    objectName: "remoteRawRestoreCancelButton"
                    text: "取消"
                    enabled: !remote.busy
                    onClicked: page.closeRawRestoreDialog(true)
                }

                AppButton {
                    objectName: "remoteRawRestoreConfirmButton"
                    text: rawRestoreDialog.passwordStage ? "继续恢复" : "恢复"
                    variant: "primary"
                    enabled: !remote.busy
                             && (rawRestoreDialog.passwordStage
                                 ? page.draftRawRestorePassword !== ""
                                 : page.draftRawRestorePath !== "")
                    onClicked: page.confirmRawRestore()
                }
            }
        }
    }

    // 一次原始归档恢复结束时收尾：还需要密码就留在对话框里切到第二段，
    // 其余情况（成功 / 致命失败）关掉对话框——成功与失败都写在页面横幅上。
    Connections {
        target: remote

        function onOperationFinished(kind, succeeded) {
            if (kind !== "restore-raw")
                return
            if (remote.rawRestoreAwaitingPassword) {
                // 用户上一次输的密码（如果有）立刻清掉：换一段重新输入。
                page.draftRawRestorePassword = ""
                rawRestorePasswordField.text = ""
                return
            }
            page.closeRawRestoreDialog(false)
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

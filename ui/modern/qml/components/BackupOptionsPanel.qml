// BackupOptionsPanel.qml
//
// 备份页的「高级选项」折叠面板：打包格式 / 压缩 / 加密，以及加密时才出现的密码输入。
//
// 为什么默认收起：此前的产品行为是"选一个源目录、点开始备份"，
// 默认取值（mypack + 不压缩 + 不加密）必须与那套行为完全一致 ——
// 收起时只剩一行摘要，不展开就看不到也用不到任何新选项，默认产物不变。
//
// 分工：QML 只持有"用户选了哪一个"的字符串键（mypack / ustar / none …），
// 键到枚举的映射、密码的校验与实际使用全部在 C++ 控制器里。
// 界面不解释数字枚举，也不自己拼算法展示文案（备份记录里的展示文案由 C++ 给出）。

import QtQuick
import QtQuick.Layouts

Item {
    id: panel

    objectName: "backupOptionsPanel"

    // 折叠状态：默认收起。
    property bool expanded: false

    // 当前选择：冻结的字符串键，控制器拿到的永远是这几个键之一。
    property string packKey: "mypack"
    property string compressionKey: "none"
    property string encryptionKey: "none"

    // 密码只在加密时才有意义；直接暴露输入框自己的 text，界面里不留第二份副本。
    // 这两条是 readonly 绑定，不是 property alias：清 passwordField.text
    // 就等于清掉对外暴露的 password / confirmPassword。
    readonly property string password: passwordField.text
    readonly property string confirmPassword: confirmField.text

    // "当前输入合不合法"与"该不该把错误显示出来"是两件事，所以分开：
    // validationMessage 一直实时算，这个开关只在用户真的点过一次"开始备份"之后才置真。
    // 于是刚选上 AES、还没输入时不会先红一片；而成功提交后 clearPasswords()
    // 会把它复位，红色提示随之消失 —— 那一次的密码是程序主动清的，不是用户输错了。
    property bool passwordValidationRequested: false

    // 显示名与键分开放：下拉里给人看的是显示名，交给 C++ 的始终是键。
    readonly property var packLabels: ["MyPack", "USTAR", "Fast USTAR"]
    readonly property var packKeys: ["mypack", "ustar", "fast-ustar"]
    readonly property var compressionLabels: ["不压缩", "Huffman", "LZSS + Huffman"]
    readonly property var compressionKeys: ["none", "huffman", "lzss-huffman"]
    // 加密的显示顺序固定为：不加密 -> AES -> DES。DES 是课程用的旧算法，
    // 名字后面必须挂着这个标记，免得有人在真实数据上误选它。
    readonly property var encryptionLabels: ["不加密",
                                             "AES-256-CTR + HMAC-SHA256",
                                             "DES-CBC + HMAC-SHA256（教学 / 旧算法）"]
    readonly property var encryptionKeys: ["none",
                                           "aes-256-ctr-hmac-sha256",
                                           "des-cbc-hmac-sha256"]

    readonly property int packIndex: Math.max(0, panel.packKeys.indexOf(panel.packKey))
    readonly property int compressionIndex: Math.max(0, panel.compressionKeys.indexOf(panel.compressionKey))
    readonly property int encryptionIndex: Math.max(0, panel.encryptionKeys.indexOf(panel.encryptionKey))

    // 收起时的一行摘要：直接用当前选中项的显示名拼，和下拉里看到的完全一致。
    readonly property string summaryText: panel.packLabels[panel.packIndex] + " · "
                                          + panel.compressionLabels[panel.compressionIndex] + " · "
                                          + panel.encryptionLabels[panel.encryptionIndex]

    // 每个算法的说明只在它被选中时显示。刻意不写"最快""压缩率最高"这类
    // 没有前提条件的断言：Fast USTAR 的差别在 I/O 实现，不是对所有文件都更快。
    readonly property string packHelper: {
        if (panel.packKey === "ustar")
            return "标准 TAR/USTAR 格式，便于互操作；mtime 为秒级。"
        if (panel.packKey === "fast-ustar")
            return "与 USTAR 生成相同标准格式，使用更高吞吐的 I/O 实现。"
        return "本项目扩展格式，可保存纳秒级 mtime 与完整扩展元数据。"
    }
    readonly property string compressionHelper: {
        if (panel.compressionKey === "huffman")
            return "基于字符频率的无损压缩。"
        if (panel.compressionKey === "lzss-huffman")
            return "先利用重复片段，再进行 Huffman 编码；通常更适合重复较多的数据。"
        return ""
    }

    readonly property bool encryptionSelected: panel.encryptionKey !== "none"
    // "手写实现 + 未经专业审计"这两件事必须写在用户做选择的地方，而不是只写在文档里：
    // 选了加密却不知道这一点，是这一页最大的风险。
    readonly property string encryptionDisclaimer: panel.encryptionSelected
                                                   ? "本项目的密码学实现为课程手写实现，未经专业密码学审计，不应用于真实敏感数据。"
                                                   : ""
    readonly property bool legacyEncryptionSelected: panel.encryptionKey === "des-cbc-hmac-sha256"

    // 密码规则只有两条：非空、两次一致。长度 / 复杂度是核心没有的要求，
    // 界面不自己发明，也不假装检查过。
    readonly property bool passwordControlsVisible: panel.encryptionSelected
    readonly property string validationMessage: {
        if (!panel.passwordControlsVisible)
            return ""
        if (panel.password.length === 0)
            return "密码不能为空"
        if (panel.password !== panel.confirmPassword)
            return "两次输入的密码不一致"
        return ""
    }
    readonly property bool passwordAcceptable: !panel.passwordControlsVisible
                                               || panel.validationMessage.length === 0

    // 清的是输入框本身 —— password / confirmPassword 是它 text 的 readonly 绑定，
    // 所以不存在"清了一处、另一处还留着"的可能。
    //
    // 顺带把"已请求过校验"复位：清空是成功提交之后的动作，不是用户输错了，
    // 复位后那行红色提示立刻消失，而不是跳成"密码不能为空"。
    function clearPasswords() {
        passwordField.text = ""
        confirmField.text = ""
        panel.passwordValidationRequested = false
    }

    // 用户按下"开始备份"时才请求校验。单独给一个函数而不是让 BackupPage 直接写属性，
    // 是为了让"什么算一次提交尝试"只有面板自己知道。
    function requestPasswordValidation() {
        panel.passwordValidationRequested = true
    }

    onEncryptionKeyChanged: {
        if (panel.encryptionKey === "none") {
            // 切回"不加密"：留着上一次的密码没有用处，还会让
            // "这一次备份到底有没有用密码"变得含糊。
            panel.clearPasswords()
        } else {
            // 切到（或切换到另一种）加密方式时只复位"已尝试提交"，
            // 不清已经输入的密码 —— 清不清密码是既有行为，本轮不扩大语义。
            // 重点是别把上一次的红色错误继承过来。
            panel.passwordValidationRequested = false
        }
    }

    implicitHeight: card.implicitHeight

    AppCard {
        id: card
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        padding: 14

        ColumnLayout {
            id: body
            anchors.fill: parent
            spacing: 10

            // ---------- 头部：一行摘要 + 展开 / 收起 ----------
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Text {
                    text: "高级选项"
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    color: theme.textSecondary
                }

                Text {
                    objectName: "backupOptionsSummary"
                    Layout.fillWidth: true
                    text: panel.summaryText
                    font.pixelSize: 16
                    color: theme.textPrimary
                    // 连 DES 的长名字算进来后仍可能超出窄窗口，这里用省略号收尾：
                    // 宁可少显示几个字，也不让页面横向溢出。
                    elide: Text.ElideRight
                }

                AppButton {
                    objectName: "backupOptionsToggle"
                    text: panel.expanded ? "收起高级选项" : "展开高级选项"
                    variant: "flat"
                    onClicked: panel.expanded = !panel.expanded
                }
            }

            // ---------- 展开区：三个选择器 ----------
            // 都受 controller.busy 约束：算法是这一次备份的参数，
            // 跑到一半再换没有任何意义，只会让界面和产物对不上。
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 12
                visible: panel.expanded

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        text: "打包格式"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    // 控件宽度统一按最长的那个显示名取（DES 那一条约 390px）：
                    // 加密方式必须在收起状态下也能完整读到“教学 / 旧算法”这几个字，
                    // 被省略号截掉的警示等于没有。
                    AppComboBox {
                        objectName: "packSelector"
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        model: panel.packLabels
                        currentIndex: panel.packIndex
                        enabled: !controller.busy
                        onActivated: panel.packKey = panel.packKeys[currentIndex]
                    }

                    Text {
                        objectName: "packHelperText"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: panel.packHelper
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        text: "压缩方式"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    AppComboBox {
                        objectName: "compressionSelector"
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        model: panel.compressionLabels
                        currentIndex: panel.compressionIndex
                        enabled: !controller.busy
                        onActivated: panel.compressionKey = panel.compressionKeys[currentIndex]
                    }

                    Text {
                        objectName: "compressionHelperText"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: panel.compressionHelper
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        text: "加密方式"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    AppComboBox {
                        objectName: "encryptionSelector"
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        model: panel.encryptionLabels
                        currentIndex: panel.encryptionIndex
                        enabled: !controller.busy
                        onActivated: panel.encryptionKey = panel.encryptionKeys[currentIndex]
                    }

                    // 只要选了加密，这段话就必须出现，且始终在密码输入框上方：
                    // 用户是先看到它、再决定要不要把密码交给这个实现。
                    Text {
                        objectName: "encryptionDisclaimer"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: panel.encryptionDisclaimer
                        font.pixelSize: 15
                        color: theme.warning
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        objectName: "legacyEncryptionNote"
                        Layout.fillWidth: true
                        visible: panel.legacyEncryptionSelected
                        text: "DES 仅用于课程演示与兼容性测试。"
                        font.pixelSize: 15
                        color: theme.warning
                        wrapMode: Text.WordWrap
                    }
                }

                // ---------- 密码：只在加密时出现 ----------
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    visible: panel.passwordControlsVisible

                    Text {
                        text: "密码"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    AppTextField {
                        id: passwordField
                        objectName: "passwordField"
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        echoMode: TextInput.Password
                        placeholderText: "本次备份使用的密码"
                        enabled: !controller.busy
                    }

                    Text {
                        text: "确认密码"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    AppTextField {
                        id: confirmField
                        objectName: "confirmPasswordField"
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        echoMode: TextInput.Password
                        placeholderText: "再输入一次"
                        enabled: !controller.busy
                    }

                    // 提示只在"用户真的点过一次开始备份"之后才出现：刚选上 AES 还没输入时
                    // 不该先报错，成功提交清空密码之后也不该立刻跳成"密码不能为空"。
                    // 一旦请求过校验，文案仍然实时跟随 validationMessage 更新，
                    // 用户边改边看到它从"不能为空"变成"两次不一致"再消失。
                    Text {
                        objectName: "passwordValidationText"
                        Layout.fillWidth: true
                        visible: panel.passwordValidationRequested
                                 && panel.validationMessage.length > 0
                        text: panel.validationMessage
                        font.pixelSize: 15
                        color: theme.error
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}

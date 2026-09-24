// FilterEditorPanel.qml
//
// 可视化 Filter 规则编辑器（备份页内嵌：宽屏左右两栏，窄屏上下堆叠）。
//
// 分工与依赖方向：
//   main.cpp 注册 filterRuleModel
//     -> OperationPage 显式注入 ruleModel
//       -> 本面板只在自己的根节点读取 model，把结果放进本地属性
//         -> 子项一律绑定本地属性，从不直接访问 model
//
// 这样做的原因是 QML 的创建顺序：嵌套子项的绑定会在父级注入的属性赋值之前
// 求值，直接写 model.x 会产生 “Cannot read property 'x' of null” 的运行期告警。
// 面板根在 model 就绪后同步一次，并在 model 的信号里再同步，因此首帧数据就是
// 正确的，不需要在每处绑定上散落 null 判断。

import QtQuick
import QtQuick.Layouts

import "../components"

Item {
    id: panel

    // 由 OperationPage 注入的规则模型（见上）。
    required property var ruleModel

    // 本地派生状态：子项只读这些。
    property var ruleList: []
    property var previewList: []
    property bool previewBusy: false
    property bool previewTruncated: false
    property int previewShown: 0
    property int previewLimit: 300
    property string previewSource: ""
    property string summaryLine: ""
    property string dslPreview: ""
    property string errorText: ""
    property string cliLine: ""

    // 表单状态（当前正在编辑的规则）
    property string formAction: "include"
    property string formField: "ext"
    property string formPattern: ""
    property string formExtensions: ""
    property string formType: "file"
    property string formCompare: ">="
    property string formUnit: "KB"
    property string formSizeLowText: "1"
    property string formSizeHighText: "10"
    property string formError: ""

    readonly property bool wide: width >= 760

    function syncFromModel() {
        if (!ruleModel)
            return
        panel.ruleList = panel.ruleModel.rules
        panel.previewList = panel.ruleModel.previewItems
        panel.previewBusy = panel.ruleModel.previewBusy
        panel.previewTruncated = panel.ruleModel.previewTruncated
        panel.previewShown = panel.ruleModel.previewShown
        panel.previewLimit = panel.ruleModel.previewLimit
        panel.summaryLine = panel.ruleModel.summaryText
        panel.dslPreview = panel.ruleModel.dslText
        panel.errorText = panel.ruleModel.lastError
        panel.cliLine = panel.ruleModel.cliArguments()
        panel.previewSource = ruleModel.previewSource
    }

    function formMap() {
        return {
            "action": panel.formAction,
            "field": panel.formField,
            "pattern": panel.formPattern,
            "extensions": panel.formExtensions,
            "type": panel.formType,
            "compare": panel.formCompare,
            "unit": panel.formUnit,
            "sizeLow": panel.sizeLowValue(),
            "sizeHigh": panel.sizeHighValue()
        }
    }

    // 表单校验同样交给 C++（builder + 真实 Filter 裁决），QML 不自判语法。
    function isNonNegativeInt(text) {
        return /^[0-9]+$/.test(text)
    }

    function sizeLowValue() {
        return panel.isNonNegativeInt(panel.formSizeLowText) ? parseInt(panel.formSizeLowText) : 0
    }

    function sizeHighValue() {
        return panel.isNonNegativeInt(panel.formSizeHighText) ? parseInt(panel.formSizeHighText) : 0
    }

    function refreshFormError() {
        if (!ruleModel) {
            panel.formError = ""
            return
        }
        if (panel.formField === "size") {
            if (!panel.isNonNegativeInt(panel.formSizeLowText)) {
                panel.formError = "size 的数值必须是非负整数（当前是 " + panel.formSizeLowText + "）"
                return
            }
            if (panel.formCompare === ".." && !panel.isNonNegativeInt(panel.formSizeHighText)) {
                panel.formError = "size 区间的上界必须是非负整数（当前是 " + panel.formSizeHighText + "）"
                return
            }
        }
        panel.formError = panel.ruleModel.validateForm(panel.formMap())
    }

    function resetForm(action) {
        panel.formAction = action
        panel.formField = "ext"
        panel.formPattern = ""
        panel.formExtensions = ""
        panel.formType = "file"
        panel.formCompare = ">="
        panel.formUnit = "KB"
        panel.formSizeLowText = "1"
        panel.formSizeHighText = "10"
        if (ruleModel)
            panel.ruleModel.clearError()
        panel.refreshFormError()
    }

    function refreshPreview() {
        if (!ruleModel || controller.sourcePath.length === 0)
            return
        panel.ruleModel.requestPreview(controller.sourcePath, "")
        panel.syncFromModel()
    }

    implicitHeight: layout.implicitHeight

    onRuleModelChanged: syncFromModel()

    // 规则变化后：同步本地状态，并刷新预览（latest-request-wins 由模型负责）。
    function onRulesChanged() {
        panel.syncFromModel()
        panel.refreshPreview()
    }

    // 用显式信号连接，而不是 QML 的 Connections 元素：
    // 静态检查工具的 6.4 版 qmltypes 解析不了 Connections（以及随之而来的 target），
    // 会连锁出一批假告警；显式 connect 行为等价，也不引入新的告警。
    // ruleModel 由父级在创建期注入，Component.onCompleted 时已经就绪（见同步逻辑）。
    Component.onCompleted: {
        panel.syncFromModel()
        if (panel.ruleModel) {
            panel.ruleModel.rulesChanged.connect(panel.onRulesChanged)
            panel.ruleModel.previewChanged.connect(panel.syncFromModel)
            panel.ruleModel.lastErrorChanged.connect(panel.syncFromModel)
        }
    }

    GridLayout {
        id: layout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        columns: panel.wide ? 2 : 1
        columnSpacing: 12
        rowSpacing: 12

        // ---------------- 左：文件预览（真实 Filter 判定 + 后台扫描） ----------------
        AppCard {
            Layout.fillWidth: true
            Layout.preferredWidth: panel.wide ? Math.max(360, panel.width * 0.36) : 0
            padding: 14
            Layout.alignment: Qt.AlignTop

            ColumnLayout {
                anchors.fill: parent
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Text {
                        text: "文件预览"
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.fillWidth: true
                    }

                    AppButton {
                        text: "刷新预览"
                        enabled: !controller.busy && controller.sourcePath.length > 0
                        onClicked: panel.refreshPreview()
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 18
                    color: theme.textSecondary
                    text: {
                        if (panel.previewBusy)
                            return "扫描中…"
                        if (controller.sourcePath.length === 0)
                            return "选定源目录后，这里会显示应用当前规则的结果。"
                        if (panel.previewShown === 0)
                            return "尚未生成预览，点“刷新预览”即可。"
                        if (panel.previewSource.length > 0 && panel.previewSource !== controller.sourcePath)
                            return "共 " + panel.previewShown + " 项（结果对应 " + panel.previewSource + "，源目录已改，请刷新）。"
                        if (panel.previewTruncated)
                            return "仅预览前 " + panel.previewLimit + " 项（目录过大时只显示开头部分）。"
                        return "共 " + panel.previewShown + " 项。"
                    }
                }

                ListView {
                    id: previewList
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.previewShown === 0 ? 60 : Math.min(320, previewList.contentHeight)
                    clip: true
                    model: panel.previewList

                    delegate: ColumnLayout {
                        width: ListView.view ? ListView.view.width : 0
                        spacing: 0

                        Text {
                            Layout.fillWidth: true
                            text: modelData.isDirectory ? "📁 " + modelData.path : modelData.path
                            font.pixelSize: 17
                            color: modelData.included ? theme.textPrimary : theme.textSecondary
                            elide: Text.ElideMiddle
                        }

                        Text {
                            Layout.fillWidth: true
                            text: modelData.tag + (modelData.size.length > 0 ? " · " + modelData.size : "")
                            font.pixelSize: 17
                            color: modelData.included ? theme.textSecondary : theme.accent
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        // ---------------- 右：规则编辑 ----------------
        AppCard {
            Layout.fillWidth: true
            Layout.preferredWidth: panel.wide ? Math.max(420, panel.width * 0.64 - 14) : 0
            Layout.alignment: Qt.AlignTop

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Text {
                    text: "过滤规则（可选）"
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    color: theme.textSecondary
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 16
                    color: theme.textSecondary
                    text: "未设置过滤规则时，将备份源目录中的全部文件与目录结构；若某项同时命中 Include 与 Exclude，则以 Exclude 为准。"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppButton {
                        text: "添加 Include 规则"
                        enabled: !controller.busy
                        onClicked: {
                            panel.resetForm("include")
                            editor.visible = true
                        }
                    }

                    AppButton {
                        text: "添加 Exclude 规则"
                        enabled: !controller.busy
                        onClicked: {
                            panel.resetForm("exclude")
                            editor.visible = true
                        }
                    }

                    AppButton {
                        text: "清空"
                        enabled: !controller.busy && panel.ruleList.length > 0
                        onClicked: {
                            if (ruleModel)
                                panel.ruleModel.clearRules()
                        }
                    }
                }

                Repeater {
                    model: panel.ruleList

                    delegate: RuleCard {
                        required property int index
                        required property var modelData

                        ruleIndex: index
                        actionText: String(modelData["action"] || "")
                        summaryText: String(modelData["summary"] || "")
                        dslText: String(modelData["dsl"] || "")
                        detailText: String(modelData["detail"] || "")
                        ruleModelRef: panel.ruleModel
                        busy: controller.busy
                        totalRules: panel.ruleList.length
                    }
                }

                // 编辑表单：字段 + 各字段专属控件，用户不需要记语法
                ColumnLayout {
                    id: editor
                    Layout.fillWidth: true
                    spacing: 6
                    visible: false

                    Text {
                        Layout.fillWidth: true
                        text: panel.formAction === "include" ? "新建 Include 规则" : "新建 Exclude 规则"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textPrimary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        AppComboBox {
                            Layout.preferredWidth: 120
                            model: ["ext", "name", "path", "stem", "type", "size"]
                            currentIndex: Math.max(0, model.indexOf(panel.formField))
                            onActivated: {
                                panel.formField = currentText
                                panel.refreshFormError()
                            }
                        }

                        AppTextField {
                            Layout.fillWidth: true
                            visible: panel.formField === "name" || panel.formField === "path" || panel.formField === "stem"
                            placeholderText: panel.formField === "path" ? "如 **/build/**" : "如 *.txt"
                            text: panel.formPattern
                            onTextEdited: {
                                panel.formPattern = text
                                panel.refreshFormError()
                            }
                        }

                        AppTextField {
                            Layout.fillWidth: true
                            visible: panel.formField === "ext"
                            placeholderText: "扩展名，用分号分隔，如 txt;md"
                            text: panel.formExtensions
                            onTextEdited: {
                                panel.formExtensions = text
                                panel.refreshFormError()
                            }
                        }

                        AppComboBox {
                            visible: panel.formField === "type"
                            Layout.preferredWidth: 120
                            model: ["file", "folder"]
                            currentIndex: panel.formType === "folder" ? 1 : 0
                            onActivated: {
                                panel.formType = currentText
                                panel.refreshFormError()
                            }
                        }

                        AppComboBox {
                            visible: panel.formField === "size"
                            Layout.preferredWidth: 80
                            model: ["<", "<=", ">", ">=", ".."]
                            currentIndex: Math.max(0, model.indexOf(panel.formCompare))
                            onActivated: {
                                panel.formCompare = currentText
                                panel.refreshFormError()
                            }
                        }

                        AppTextField {
                            visible: panel.formField === "size"
                            Layout.preferredWidth: 80
                            text: panel.formSizeLowText
                            onTextEdited: {
                                panel.formSizeLowText = text
                                panel.refreshFormError()
                            }
                        }

                        AppTextField {
                            visible: panel.formField === "size" && panel.formCompare === ".."
                            Layout.preferredWidth: 80
                            text: panel.formSizeHighText
                            onTextEdited: {
                                panel.formSizeHighText = text
                                panel.refreshFormError()
                            }
                        }

                        AppComboBox {
                            visible: panel.formField === "size"
                            Layout.preferredWidth: 80
                            model: ["B", "KB", "MB", "GB"]
                            currentIndex: Math.max(0, model.indexOf(panel.formUnit))
                            onActivated: {
                                panel.formUnit = currentText
                                panel.refreshFormError()
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 15
                        color: theme.accent
                        text: panel.formError
                        visible: text.length > 0
                    }

                    RowLayout {
                        spacing: 8

                        AppButton {
                            text: "添加规则"
                            variant: "primary"
                            enabled: !controller.busy && panel.formError.length === 0
                            onClicked: {
                                if (!ruleModel)
                                    return
                                if (panel.ruleModel.addRule(panel.formMap())) {
                                    panel.resetForm(panel.formAction)
                                    editor.visible = false
                                }
                            }
                        }

                        AppButton {
                            text: "取消"
                            onClicked: {
                                panel.resetForm(panel.formAction)
                                editor.visible = false
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 15
                        color: theme.textSecondary
                        text: "通配符：* 匹配任意字符但不跨 /，? 匹配一个字符，** 可以跨 /。"
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 17
                    color: theme.textPrimary
                    text: panel.summaryLine
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 16
                    font.family: "monospace"
                    color: theme.textSecondary
                    text: panel.dslPreview.length > 0 ? panel.dslPreview : "（还没有规则）"
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 15
                    color: theme.accent
                    text: panel.errorText
                    visible: text.length > 0
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 16
                    font.family: "monospace"
                    color: theme.textSecondary
                    visible: panel.ruleList.length > 0
                    text: "CLI 等价参数：" + panel.cliLine
                }
            }
        }
    }
}

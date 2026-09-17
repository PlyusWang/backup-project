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
import QtQuick.Controls
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
    property int formSizeLow: 1
    property int formSizeHigh: 10
    property int editingIndex: -1
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
            "sizeLow": panel.formSizeLow,
            "sizeHigh": panel.formSizeHigh
        }
    }

    // 表单校验同样交给 C++（builder + 真实 Filter 裁决），QML 不自判语法。
    function refreshFormError() {
        if (!ruleModel) {
            panel.formError = ""
            return
        }
        panel.formError = panel.ruleModel.validateForm(panel.formMap())
    }

    function resetForm(action) {
        panel.editingIndex = -1
        panel.formAction = action
        panel.formField = "ext"
        panel.formPattern = ""
        panel.formExtensions = ""
        panel.formType = "file"
        panel.formCompare = ">="
        panel.formUnit = "KB"
        panel.formSizeLow = 1
        panel.formSizeHigh = 10
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

    // 用显式信号连接，而不是 QML 的 Connections 元素：
    // 静态检查工具的 6.4 版 qmltypes 解析不了 Connections（以及随之而来的 target），
    // 会连锁出一批假告警；显式 connect 行为等价，也不引入新的告警。
    // ruleModel 由父级在创建期注入，Component.onCompleted 时已经就绪（见同步逻辑）。
    Component.onCompleted: {
        panel.syncFromModel()
        if (panel.ruleModel) {
            panel.ruleModel.rulesChanged.connect(panel.syncFromModel)
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
            Layout.preferredWidth: panel.wide ? 340 : 0
            Layout.alignment: Qt.AlignTop

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: "文件预览"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                        Layout.fillWidth: true
                    }

                    AppButton {
                        text: "刷新"
                        enabled: !controller.busy && controller.sourcePath.length > 0
                        onClicked: panel.refreshPreview()
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: theme.textSecondary
                    text: {
                        if (panel.previewBusy)
                            return "扫描中…"
                        if (controller.sourcePath.length === 0)
                            return "填写源目录后可预览筛选结果。"
                        if (panel.previewShown === 0)
                            return "还没有预览结果，点“刷新”。"
                        if (panel.previewTruncated)
                            return "仅预览前 " + panel.previewLimit + " 项（目录过大时只显示开头部分）。"
                        return "共 " + panel.previewShown + " 项。"
                    }
                }

                ListView {
                    id: previewList
                    Layout.fillWidth: true
                    Layout.preferredHeight: 260
                    clip: true
                    model: panel.previewList

                    delegate: ColumnLayout {
                        width: ListView.view ? ListView.view.width : 0
                        spacing: 0

                        Text {
                            Layout.fillWidth: true
                            text: modelData.isDirectory ? "📁 " + modelData.path : modelData.path
                            font.pixelSize: 12
                            color: modelData.included ? theme.textPrimary : theme.textSecondary
                            elide: Text.ElideMiddle
                        }

                        Text {
                            Layout.fillWidth: true
                            text: modelData.tag + (modelData.size.length > 0 ? " · " + modelData.size : "")
                            font.pixelSize: 10
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
            Layout.preferredWidth: panel.wide ? 520 : 0
            Layout.alignment: Qt.AlignTop

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Text {
                    text: "过滤规则（可选）"
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: theme.textSecondary
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: theme.textSecondary
                    text: "没有规则时按 PR #8 行为备份全部内容；exclude 优先于 include。"
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
                            panel.refreshPreview()
                        }
                    }
                }

                Repeater {
                    model: panel.ruleList

                    delegate: Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: ruleColumn.implicitHeight + 12
                        radius: 4
                        color: theme.hover

                        ColumnLayout {
                            id: ruleColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 6
                            spacing: 2

                            Text {
                                Layout.fillWidth: true
                                text: (modelData.action === "include" ? "[Include] " : "[Exclude] ") + modelData.summary
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                color: theme.textPrimary
                                wrapMode: Text.WordWrap
                            }

                            Text {
                                Layout.fillWidth: true
                                text: modelData.dsl
                                font.pixelSize: 11
                                font.family: "monospace"
                                color: theme.textSecondary
                            }

                            RowLayout {
                                spacing: 6

                                AppButton {
                                    text: "上移"
                                    enabled: !controller.busy && index > 0
                                    onClicked: {
                                        panel.ruleModel.moveRule(index, -1)
                                        panel.refreshPreview()
                                    }
                                }

                                AppButton {
                                    text: "下移"
                                    enabled: !controller.busy && index < panel.ruleList.length - 1
                                    onClicked: {
                                        panel.ruleModel.moveRule(index, 1)
                                        panel.refreshPreview()
                                    }
                                }

                                AppButton {
                                    text: "删除"
                                    enabled: !controller.busy
                                    onClicked: {
                                        panel.ruleModel.removeRule(index)
                                        panel.refreshPreview()
                                    }
                                }
                            }
                        }
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
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        color: theme.textPrimary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        ComboBox {
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

                        ComboBox {
                            visible: panel.formField === "type"
                            Layout.preferredWidth: 120
                            model: ["file", "folder"]
                            currentIndex: panel.formType === "folder" ? 1 : 0
                            onActivated: panel.formType = currentText
                        }

                        ComboBox {
                            visible: panel.formField === "size"
                            Layout.preferredWidth: 80
                            model: ["<", "<=", ">", ">=", ".."]
                            currentIndex: Math.max(0, model.indexOf(panel.formCompare))
                            onActivated: panel.formCompare = currentText
                        }

                        AppTextField {
                            visible: panel.formField === "size"
                            Layout.preferredWidth: 80
                            text: String(panel.formSizeLow)
                            onTextEdited: panel.formSizeLow = parseInt(text) || 0
                        }

                        AppTextField {
                            visible: panel.formField === "size" && panel.formCompare === ".."
                            Layout.preferredWidth: 80
                            text: String(panel.formSizeHigh)
                            onTextEdited: panel.formSizeHigh = parseInt(text) || 0
                        }

                        ComboBox {
                            visible: panel.formField === "size"
                            Layout.preferredWidth: 80
                            model: ["B", "KB", "MB", "GB"]
                            currentIndex: Math.max(0, model.indexOf(panel.formUnit))
                            onActivated: panel.formUnit = currentText
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        color: theme.accent
                        text: panel.formError
                        visible: text.length > 0
                    }

                    RowLayout {
                        spacing: 8

                        AppButton {
                            text: panel.editingIndex >= 0 ? "保存为新规则" : "添加规则"
                            variant: "primary"
                            enabled: !controller.busy && panel.formError.length === 0
                            onClicked: {
                                if (!ruleModel)
                                    return
                                if (panel.ruleModel.addRule(panel.formMap())) {
                                    if (panel.editingIndex >= 0) {
                                        panel.ruleModel.removeRule(panel.editingIndex)
                                        panel.editingIndex = -1
                                    }
                                    panel.resetForm(panel.formAction)
                                    editor.visible = false
                                    panel.syncFromModel()
                                    panel.refreshPreview()
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
                        font.pixelSize: 11
                        color: theme.textSecondary
                        text: "通配符：* 匹配任意字符但不跨 /，? 匹配一个字符，** 可以跨 /。"
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: theme.textPrimary
                    text: panel.summaryLine
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    font.family: "monospace"
                    color: theme.textSecondary
                    text: panel.dslPreview.length > 0 ? panel.dslPreview : "（还没有规则）"
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: theme.accent
                    text: panel.errorText
                    visible: text.length > 0
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    font.family: "monospace"
                    color: theme.textSecondary
                    visible: panel.ruleList.length > 0
                    text: "CLI 等价参数：" + panel.cliLine
                }
            }
        }
    }
}

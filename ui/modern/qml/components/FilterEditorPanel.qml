// FilterEditorPanel.qml
//
// 备份页（Manual Backup）的筛选面板：左边是文件预览，右边是共享的规则编辑器。
//
// 规则编辑器本身在 FilterRuleEditor.qml —— 计划页与实时页用的是同一个组件，
// 三处看到的筛选配置方式因此必然一致。这个文件只剩两件这里独有的事：
//
//   1. 布局（宽屏左右两栏，窄屏上下堆叠）；
//   2. 文件预览：调用 FilterRuleModel 的共享遍历，展示真实 Filter 的判定结果。
//
// 分工与依赖方向：
//   main.cpp 注册 filterRuleModel
//     -> BackupPage 显式注入 ruleModel
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

    // 由 BackupPage 注入的规则模型（见上）。
    required property var ruleModel

    // 本地派生状态：子项只读这些。
    property var previewList: []
    property bool previewBusy: false
    property bool previewTruncated: false
    property int previewShown: 0
    property int previewLimit: 300
    property int previewIncluded: 0
    property string previewSource: ""

    readonly property bool wide: width >= 760

    function syncPreview() {
        if (!panel.ruleModel)
            return
        panel.previewList = panel.ruleModel.previewItems
        panel.previewBusy = panel.ruleModel.previewBusy
        panel.previewTruncated = panel.ruleModel.previewTruncated
        panel.previewShown = panel.ruleModel.previewShown
        panel.previewLimit = panel.ruleModel.previewLimit
        panel.previewIncluded = panel.ruleModel.previewIncluded
        panel.previewSource = panel.ruleModel.previewSource
    }

    function refreshPreview() {
        if (!panel.ruleModel || controller.sourcePath.length === 0)
            return
        panel.ruleModel.requestPreview(controller.sourcePath, "")
        panel.syncPreview()
    }

    // 规则变化后：刷新预览（latest-request-wins 由模型负责）。规则列表本身的
    // 显示由 FilterRuleEditor 自己同步。
    function onRulesChanged() {
        panel.refreshPreview()
    }

    implicitHeight: layout.implicitHeight

    // 用显式信号连接，而不是 QML 的 Connections 元素：
    // 静态检查工具的 6.4 版 qmltypes 解析不了 Connections（以及随之而来的 target），
    // 会连锁出一批假告警；显式 connect 行为等价，也不引入新的告警。
    // ruleModel 由父级在创建期注入，Component.onCompleted 时已经就绪（见同步逻辑）。
    Component.onCompleted: {
        panel.syncPreview()
        if (panel.ruleModel) {
            panel.ruleModel.rulesChanged.connect(panel.onRulesChanged)
            panel.ruleModel.previewChanged.connect(panel.syncPreview)
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
                            return "共 " + panel.previewIncluded + " 项会进入归档；列表只显示前 " + panel.previewLimit + " 个预览条目（完整执行与备份一致的筛选遍历，被排除的目录不进入其子树）。"
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

        // ---------------- 右：规则编辑（共享组件） ----------------
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
                    text: "未设置过滤规则时，将备份源目录中的全部文件与目录结构；若某项同时命中包含与排除规则，则以排除规则为准。"
                }

                FilterRuleEditor {
                    ruleModel: panel.ruleModel
                    objectPrefix: "filter"
                    busy: controller.busy
                    heading: ""
                    intro: ""
                    showCliLine: true
                    Layout.fillWidth: true
                }
            }
        }
    }
}

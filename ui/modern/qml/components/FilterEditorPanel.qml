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
    // ---- 镜像属性：ruleModel 的同值副本，子项只绑定它们（原因见文件头） ----
    // previewList 的记录格式由 FilterRuleModel 生成，delegate 按字段名读取：
    //   { path: 绝对路径, isDirectory: bool, included: bool,
    //     size: 已格式化的可读文本（如 "12 KB"）, tag: 中文判定说明 }
    property var previewList: []
    property bool previewBusy: false
    property bool previewTruncated: false
    property int previewShown: 0
    // previewShown 只是“列表里有几行”（≤ previewLimit）；previewIncluded 是全量
    // 遍历里真正会进归档的条目数，可以远大于列表长度。两者都会出现在文案里，
    // 混用会让“共 N 项”随显示窗口变化，看起来像丢了文件。
    property int previewLimit: 300
    property int previewIncluded: 0
    property string previewSource: ""

    // 断点 760：两栏最小宽度是 360 + 420，再加 12 的列间距才放得下；更窄就改成
    // 上下堆叠（GridLayout 在 columns = 1 时自动换行），而不是挤成两条窄缝。
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

    // 主动重扫。源目录为空时不请求：模型对空路径只会报“请先填写源目录”，而这里
    // 想要的是“还没到能预览的时候”，不该让用户看到一条无谓的报错。
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
    // 静态检查工具的 6.4 版 qmltypes 解析不了 Connections
    // （以及随之而来的 target），会连锁出一批假告警；
    // 显式 connect 行为等价，也不引入新的告警。
    // ruleModel 由父级在创建期注入，
    // Component.onCompleted 时已经就绪（见同步逻辑）。
    // 连接不显式断开：接收者就是本面板，对象销毁时 Qt 会自动断开以它为接收者的
    // 连接；而本块每个实例只执行一次，不存在重复连接的问题。
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

        // ----------- 左：文件预览（真实 Filter 判定 + 后台扫描） -----------
        AppCard {
            Layout.fillWidth: true
            Layout.preferredWidth: panel.wide
                ? Math.max(360, panel.width * 0.36)
                : 0
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
                        enabled: !controller.busy
                            && controller.sourcePath.length > 0
                        onClicked: panel.refreshPreview()
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 18
                    color: theme.textSecondary
                    // 状态行是**有序**判断，顺序不能重排：
                    //   扫描中 -> 未选源目录 -> 还没生成过 -> 结果过期
                    //   -> 被截断（给出全量数字）-> 正常条数
                    // “过期”必须排在“截断”之前：过期结果里的数字已经不可信。
                    text: {
                        if (panel.previewBusy)
                            return "扫描中…"
                        if (controller.sourcePath.length === 0)
                            return "选定源目录后，这里会显示"
                                + "应用当前规则的结果。"
                        if (panel.previewShown === 0)
                            return "尚未生成预览，点“刷新预览”即可。"
                        if (panel.previewSource.length > 0
                            && panel.previewSource !== controller.sourcePath)
                            return "共 " + panel.previewShown
                                + " 项（结果对应 " + panel.previewSource
                                + "，源目录已改，请刷新）。"
                        if (panel.previewTruncated)
                            return "共 " + panel.previewIncluded
                                + " 项会进入归档；列表只显示前 "
                                + panel.previewLimit + " 个预览条目"
                                + "（完整执行与备份一致的筛选遍历，"
                                + "被排除的目录不进入其子树）。"
                        return "共 " + panel.previewShown + " 项。"
                    }
                }

                ListView {
                    id: previewList
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.previewShown === 0
                        ? 60
                        : Math.min(320, previewList.contentHeight)
                    clip: true
                    model: panel.previewList

                    delegate: ColumnLayout {
                        width: ListView.view ? ListView.view.width : 0
                        spacing: 0

                        Text {
                            Layout.fillWidth: true
                            text: modelData.isDirectory
                                ? "📁 " + modelData.path
                                : modelData.path
                            font.pixelSize: 17
                            color: modelData.included
                                ? theme.textPrimary
                                : theme.textSecondary
                            elide: Text.ElideMiddle
                        }

                        Text {
                            Layout.fillWidth: true
                            text: modelData.tag
                                + (modelData.size.length > 0
                                    ? " · " + modelData.size
                                    : "")
                            font.pixelSize: 17
                            color: modelData.included
                                ? theme.textSecondary
                                : theme.accent
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        // ---------------- 右：规则编辑（共享组件） ----------------
        AppCard {
            Layout.fillWidth: true
            Layout.preferredWidth: panel.wide
                ? Math.max(420, panel.width * 0.64 - 14)
                : 0
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
                    text: "未设置过滤规则时，"
                            + "将备份源目录中的全部文件与目录结构；"
                            + "若某项同时命中包含与排除规则，"
                            + "则以排除规则为准。"
                }

                // 规则编辑器是三个页面共享的组件（BackupPage / SchedulePage /
                // RealtimePage），只传模型与前缀：同一份 QML 被实例化多次，
                // 内部控件靠 objectPrefix 区分，取值必须互不相同。
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

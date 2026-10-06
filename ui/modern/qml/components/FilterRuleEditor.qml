// FilterRuleEditor.qml
//
// **三个页面共用**的可视化筛选规则编辑器。
//
// 问题是"界面好看了，但操作方式仍然偏开发者"：普通用户被要求自己
// 写 ext:cpp;h、path:**/build/**、size:<1MB。这个编辑器把"配置筛选条件"变成
// "选条件、填取值"，语法由程序生成。
//
// 结构：
//
//   FilterRuleEditor（本文件，唯一的规则编辑界面）
//        ├── BackupPage      （FilterEditorPanel 里，模型接了
//        │                     BackupController）
//        ├── SchedulePage    （高级设置里，模型只用来生成 DSL，
//        │                     保存时再交给控制器）
//        └── RealtimePage    （高级设置里，同上）
//
// 语法裁决始终只有一条路：表单 -> FilterRuleModel -> FilterRuleBuilder ->
// Filter::AddRule。本文件不实现第二套 parser，也不判断任何一条规则的合法性：
// 它只负责把用户选的东西原样打包成 form map 交给模型。
//
// 关于 objectName：同一个组件被实例化三次，内部控件必须靠 objectPrefix 区分，
// 否则自动化测试 findChild 拿到的永远是第一个页面的那个。

// 数据流与职责（给改动者）：用户操作 -> 本地表单属性 -> formMap() ->
// FilterRuleModel（C++）-> FilterRuleBuilder -> Filter::AddRule；界面只显示
// 模型回传的 summary / dsl / error，从不自己拼规则文本、也不判断合法性。
// 这套单向流是"三个页面共用一套语义"的前提，任何"在 QML 里先校验一下"的
// 改动都会破坏它。
//
// 失败语义：模型返回 false 时规则列表原样不动（整体失败，不部分应用），原因在
// lastError 里，界面只负责显示。QML 侧唯一的前置拒绝是"高级规则不能为空"。
//
// 生命周期：ruleModel 由页面注入、比本组件活得久；信号连接在 onCompleted 建立、
// 没有显式断开，因此这里不要持有需要析构的资源。
import QtQuick
import QtQuick.Layouts

import "../components"

ColumnLayout {
    id: editor

    // 由页面注入的规则模型（见文件头）。子项一律绑定本地的 ruleList / options，
    // 不直接读 ruleModel：QML 里嵌套子项的绑定会先于父级注入的属性求值。
    required property var ruleModel

    // 页面注入的"正在忙"标志（备份 / 预览扫描进行中）：为真时禁用所有编辑入口，
    // 避免用户在一次操作中途改配置。
    property bool busy: false
    // 每个页面一个前缀（filter / schedule / realtime），
    // 用来生成唯一的 objectName。
    property string objectPrefix: ""
    property string heading: "筛选规则"
    property string intro: "只有符合包含规则、"
        + "且没有命中排除规则的文件会被备份。"
    // 高级入口的说明文字。是否展示由调用方决定（三个页面都要，所以默认开）。
    property bool showCliLine: false

    // 规则列表 / 摘要 / 预览（本地派生状态）
    property var ruleList: []
    property string summaryLine: ""
    property string dslPreview: ""
    property string errorText: ""
    property string cliLine: ""

    // 条件类型 / 取值表单的选项表，来自共享 builder（见 FilterRuleModel::
    // editorOptions）。界面不自己维护一份"有哪些条件"的清单。
    property var options: ({})
    property var fieldKeys: []
    property var fieldLabels: []
    property var fieldHints: []
    property var typeKeys: []
    property var typeLabels: []
    property var sizeCompareKeys: []
    property var sizeCompareLabels: []
    property var idCompareKeys: []
    property var idCompareLabels: []
    property var sizeUnitKeys: []
    property var sizeUnitLabels: []
    property var mtimeKeys: []
    property var mtimeLabels: []

    // ---- 表单状态（当前正在编辑的那一条规则）----
    //
    // 所有取值都以**文本**收集：数字的解析、范围与溢出裁决全部在 C++ 侧
    // （FilterRuleModel + FilterRuleBuilder + 真实 Filter），QML 不 parseInt、
    // 也不判断"这条规则能不能成立"。
    property string formAction: "include"
    property string formField: "ext"
    property string formPattern: ""
    property string formExtensions: ""
    property string formType: "file"
    property string formCompare: "<"
    property string formUnit: "MB"
    property string formSizeLowText: "1"
    property string formSizeHighText: "10"
    property string formUidCompare: "eq"
    property string formUidText: "0"
    property string formUidHighText: "0"
    property string formGidCompare: "eq"
    property string formGidText: "0"
    property string formGidHighText: "0"
    property string formUser: ""
    property string formGroup: ""
    property string formMtimeKind: "today"
    property string formDaysBackText: "7"
    property string formDateLow: ""
    property string formDateHigh: ""
    // 表单错误由 C++ 的 validateForm 写入（见 refreshFormError）。它与页面级的
    // errorText 是两条通道，分别对应"这条新规则"和"整个规则列表"。
    property string formError: ""

    // 新建规则的表单默认收起：
    // 点"添加包含规则 / 添加排除规则"才展开（不是弹窗）。
    property bool builderVisible: false
    // 表单是内联展开而不是弹窗：校验失败时它保持打开，用户改完可以直接再提交；
    // 只有提交成功或点"取消"才会收起。
    // 高级 DSL 默认收起：普通用户不需要它，熟悉语法的人能力一点没少。
    property bool advancedExpanded: false
    property string advancedAction: "include"

    // objectName 的统一构造：同一份 QML 被三个页面实例化，只有 objectPrefix
    // 不同，否则自动化测试 findChild 永远拿到第一个页面的那个控件。
    function nameOf(suffix) {
        return editor.objectPrefix + suffix
    }

    // 把"当前取值"映射成 ComboBox 的 currentIndex。找不到时退回 0 而不是 -1：
    // -1 会让下拉显示空白；模型未就绪（keys 为空）时同样退回 0。
    function keyIndex(keys, key) {
        const index = keys ? keys.indexOf(key) : -1
        return index < 0 ? 0 : index
    }

    // ---- 与模型同步 ---------------------------------------------------------
    // 模型是唯一真值来源，本地属性只是它的快照：每次 rulesChanged 或
    // lastError 变化都整体重取（规则条数是几十级别，不做增量 diff）。
    // cliArguments() 只在需要显示 CLI 等价参数时才调用；这些属性只用于显示，
    // 任何写操作都必须回到模型。
    function syncFromModel() {
        if (!editor.ruleModel)
            return
        editor.ruleList = editor.ruleModel.rules
        editor.summaryLine = editor.ruleModel.summaryText
        editor.dslPreview = editor.ruleModel.dslText
        editor.errorText = editor.ruleModel.lastError
        editor.cliLine = editor.showCliLine
            ? editor.ruleModel.cliArguments()
            : ""
    }

    // 把 C++ 的选项表摊平成"键数组 + 标签数组"（ComboBox 要标签，逻辑要用键）。
    // 键与标签同索引，只能整体重取；表来自共享 builder，界面不维护第二份清单。
    function syncOptions() {
        if (!editor.ruleModel)
            return
        const table = editor.ruleModel.editorOptions()
        editor.options = table
        editor.fieldKeys = table.fields.map(function (item) { return item.key })
        editor.fieldLabels =
            table.fields.map(function (item) { return item.label })
        editor.fieldHints =
            table.fields.map(function (item) { return item.hint })
        editor.typeKeys = table.types.map(function (item) { return item.key })
        editor.typeLabels =
            table.types.map(function (item) { return item.label })
        editor.sizeCompareKeys =
            table.sizeCompares.map(function (item) { return item.key })
        editor.sizeCompareLabels =
            table.sizeCompares.map(function (item) { return item.label })
        editor.idCompareKeys =
            table.idCompares.map(function (item) { return item.key })
        editor.idCompareLabels =
            table.idCompares.map(function (item) { return item.label })
        editor.sizeUnitKeys =
            table.sizeUnits.map(function (item) { return item.key })
        editor.sizeUnitLabels =
            table.sizeUnits.map(function (item) { return item.label })
        editor.mtimeKeys =
            table.mtimeKinds.map(function (item) { return item.key })
        editor.mtimeLabels =
            table.mtimeKinds.map(function (item) { return item.label })
    }

    // 当前条件类型的那句填写说明（来自共享 builder 的表，
    // 不在 QML 里另写一份）。
    // 当前条件类型的填写说明，与 fieldKeys 同索引；越界时给空串而不是报错。
    readonly property string fieldHint: {
        const index = editor.keyIndex(editor.fieldKeys, editor.formField)
        return editor.fieldHints.length > index ? editor.fieldHints[index] : ""
    }

    // uid / gid 共用一个比较方式控件，所以这里抽出的"当前字段对应的那个取值"。
    readonly property string idCompare: editor.formField === "uid"
        ? editor.formUidCompare
        : editor.formGidCompare

    // 表单 -> C++ 的唯一载荷。键名必须与 DraftFromForm 读取的键逐字一致：
    // 拼错一个键不会报错，只会让那个取值被当成空串（静默丢字段）。
    // 所有值都是字符串，数字解析、范围与溢出裁决全部在 C++ 侧。
    function formMap() {
        return {
            "action": editor.formAction,
            "field": editor.formField,
            "pattern": editor.formPattern,
            "extensions": editor.formExtensions,
            "type": editor.formType,
            "compare": editor.formCompare,
            "unit": editor.formUnit,
            "sizeLowText": editor.formSizeLowText,
            "sizeHighText": editor.formSizeHighText,
            "uid_compare": editor.formUidCompare,
            "uid": editor.formUidText,
            "uid_high": editor.formUidHighText,
            "gid_compare": editor.formGidCompare,
            "gid": editor.formGidText,
            "gid_high": editor.formGidHighText,
            "user": editor.formUser,
            "group": editor.formGroup,
            "mtime_kind": editor.formMtimeKind,
            "days_back": editor.formDaysBackText,
            "date_low": editor.formDateLow,
            "date_high": editor.formDateHigh
        }
    }

    // 表单校验同样交给 C++（builder + 真实 Filter 裁决），QML 不自判语法。
    // 校验同样交给 C++（builder + 真实 Filter 裁决）：formError 非空时
    // 提交按钮禁用，所以这里是"这条规则能不能添加"的唯一守卫。
    function refreshFormError() {
        if (!editor.ruleModel) {
            editor.formError = ""
            return
        }
        editor.formError = editor.ruleModel.validateForm(editor.formMap())
    }

    // 这一条规则"将会是什么样"的人话预览。仍然由 C++ 生成，界面只显示。
    // stamp 是显式依赖清单：绑定只对求值期间真正读到的属性建立依赖，这里把每个
    // 输入字段读一遍，保证任一取值变化都会重算。新增字段时必须同步加进来，否则
    // 会出现"改了值但预览不动"。注意 uid_high / gid_high / date_low / date_high
    // 不在清单里，属可疑遗漏（本次作业只报告，未改动）。
    readonly property string formSummary: {
        if (!editor.ruleModel || !editor.builderVisible)
            return ""
        // 依赖 formError 是为了在任一取值变化后重算（formMap 里每个字段都会
        // 触发它）：这一行只是展示，重算成本可以忽略。
        const stamp = editor.formError + "|" + editor.formField
                + "|" + editor.formPattern + "|" + editor.formExtensions
                + "|" + editor.formType
                + "|" + editor.formCompare + "|" + editor.formUnit
                + "|" + editor.formSizeLowText + "|" + editor.formSizeHighText
                + "|" + editor.formUidText + "|" + editor.formGidText
                + "|" + editor.formUser + "|" + editor.formGroup
                + "|" + editor.formMtimeKind + "|" + editor.formDaysBackText
        return editor.ruleModel.summaryForForm(editor.formMap())
    }

    // 复位到"新建一条规则"的默认取值。属性声明处的初始化只在组件创建时发生
    // 一次；取消或提交之后必须显式回到这套值，否则上一条的输入会留在表单里
    // 被误当成新规则。同时清掉模型的 lastError，避免旧错误挂在界面上。
    function resetForm(action) {
        editor.formAction = action
        editor.formField = "ext"
        editor.formPattern = ""
        editor.formExtensions = ""
        editor.formType = "file"
        editor.formCompare = "<"
        editor.formUnit = "MB"
        editor.formSizeLowText = "1"
        editor.formSizeHighText = "10"
        editor.formUidCompare = "eq"
        editor.formUidText = "0"
        editor.formUidHighText = "0"
        editor.formGidCompare = "eq"
        editor.formGidText = "0"
        editor.formGidHighText = "0"
        editor.formUser = ""
        editor.formGroup = ""
        editor.formMtimeKind = "today"
        editor.formDaysBackText = "7"
        editor.formDateLow = ""
        editor.formDateHigh = ""
        if (editor.ruleModel)
            editor.ruleModel.clearError()
        editor.refreshFormError()
    }

    // 打开表单 = 复位 + 展开；action 决定这次加的是包含还是排除规则。
    function openBuilder(action) {
        editor.resetForm(action)
        editor.builderVisible = true
    }

    // 模型返回 true 才收起表单并复位（防止连点重复添加同一条）；返回 false
    // 保留用户输入，错误文本由 lastErrorChanged -> syncFromModel 送进界面。
    function submitForm() {
        if (!editor.ruleModel)
            return
        if (editor.ruleModel.addRule(editor.formMap())) {
            editor.resetForm(editor.formAction)
            editor.builderVisible = false
        }
    }

    // 空输入在 QML 侧提前拒绝：空 DSL 交给 C++ 可能被解释成一条匹配一切的规则，
    // 那是"多备份了东西"而不是"少配了一条"。成功才清空输入框，失败保留原文。
    function submitAdvanced() {
        if (!editor.ruleModel)
            return
        if (advancedField.text.length === 0) {
            advancedErrorText.text = "请先填写规则"
            return
        }
        if (editor.ruleModel.addAdvancedRule(editor.advancedAction,
                                            advancedField.text)) {
            advancedErrorText.text = ""
            advancedField.text = ""
        } else {
            advancedErrorText.text = editor.ruleModel.lastError
        }
    }

    // ---- 给页面用的入口 -----------------------------------------------------
    //
    // 计划页 / 实时页的配置是"草稿 + 保存"，所以它们把规则读回去交给自己的
    // 控制器；备份页的模型直接挂在 BackupController 上，不需要这一步。
    //
    // 为什么是"绑定属性"而不是函数：页面上的"改动尚未保存"标记要让绑定跟着
    // 规则列表一起重算，函数调用不建立依赖。这里只**过滤**模型给出的 dsl 字段
    // （规则文本本来就是模型算好的），不解析、也不重新拼任何语法。
    // 给页面用的只读视图：从模型给出的 dsl 字段过滤，不重新拼任何语法。action
    // 缺省（未定义）按包含处理，与模型的默认动作一致；顺序保持列表顺序，
    // 这样页面拿它做草稿指纹时是稳定的。
    readonly property var includeRuleTexts: {
        const out = []
        for (let i = 0; i < editor.ruleList.length; ++i) {
            const item = editor.ruleList[i]
            if (String(item["action"]) !== "exclude")
                out.push(String(item["dsl"]))
        }
        return out
    }

    // 与 includeRuleTexts 对称：只按 action === "exclude" 过滤。
    readonly property var excludeRuleTexts: {
        const out = []
        for (let i = 0; i < editor.ruleList.length; ++i) {
            const item = editor.ruleList[i]
            if (String(item["action"]) === "exclude")
                out.push(String(item["dsl"]))
        }
        return out
    }

    // 规则列表的文本指纹：页面拿它做"草稿是否已经保存"的比较。
    // 页面用它判断"草稿是否已经保存"。两个列表之间用 \u0000 分隔，避免
    // "include 的尾串 + exclude 的首串"拼出与另一种划分相同的字符串，把改动
    // 误判成未改动。
    readonly property string rulesSignature: editor.includeRuleTexts.join("\n")
        + "\u0000" + editor.excludeRuleTexts.join("\n")

    // 用落盘配置里的规则整体替换当前列表。校验仍然走共享 builder。
    // 用落盘配置里的规则整体替换当前列表（载入与手工添加走同一套校验）。
    // 返回 false 时列表原样不动——绝不部分应用，也不静默跳过读不懂的规则。
    function loadRules(includeList, excludeList) {
        if (!editor.ruleModel)
            return true
        const ok = editor.ruleModel.setRules(includeList, excludeList)
        editor.syncFromModel()
        return ok
    }

    // 规则变化后同步本地状态。预览的刷新由备份页自己负责（只有那一页有预览）。
    // 显式 connect 的槽：规则变化后只同步本地快照，预览刷新由备份页自己做。
    function handleRulesChanged() {
        editor.syncFromModel()
    }

    // 用显式信号连接而不是 QML 的 Connections 元素：6.4 的 qmltypes 解析不了
    // Connections（以及随之而来的 target），会连锁出一批假告警；显式 connect
    // 行为等价，也不引入新的告警。
    // ruleModel 是 required property，注入发生在子项创建之后，所以不能只依赖
    // Component.onCompleted：这里再同步一次，注入顺序变化也不会漏。
    onRuleModelChanged: {
        syncFromModel()
        syncOptions()
    }

    // 首次同步 + 建立信号连接；用 connect 而不是 Connections 的原因见上。
    // 没有对应的 disconnect：ruleModel 由页面持有、生命周期长于本组件；
    // 若将来模型可能先于编辑器销毁，需要在这里补 onDestruction 断开。
    Component.onCompleted: {
        editor.syncFromModel()
        editor.syncOptions()
        editor.refreshFormError()
        if (editor.ruleModel) {
            editor.ruleModel.rulesChanged.connect(editor.handleRulesChanged)
            editor.ruleModel.lastErrorChanged.connect(editor.syncFromModel)
        }
    }

    // ---- 1. 标题与说明 ----
    Text {
        Layout.fillWidth: true
        visible: editor.heading.length > 0
        text: editor.heading
        font.pixelSize: 16
        font.weight: Font.DemiBold
        color: theme.textSecondary
    }

    Text {
        Layout.fillWidth: true
        visible: editor.intro.length > 0
        text: editor.intro
        font.pixelSize: 15
        color: theme.textSecondary
        wrapMode: Text.WordWrap
    }

    // ---- 2. 添加 / 清空 ----
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 4
        spacing: 8

        AppButton {
            objectName: editor.nameOf("AddIncludeRuleButton")
            text: "添加包含规则"
            enabled: !editor.busy
            onClicked: editor.openBuilder("include")
        }

        AppButton {
            objectName: editor.nameOf("AddExcludeRuleButton")
            text: "添加排除规则"
            enabled: !editor.busy
            onClicked: editor.openBuilder("exclude")
        }

        AppButton {
            objectName: editor.nameOf("ClearRulesButton")
            text: "清空"
            enabled: !editor.busy && editor.ruleList.length > 0
            onClicked: {
                if (editor.ruleModel)
                    // 清空交给模型统一处理：它重建列表并发 rulesChanged；
                    // 界面不做本地删除，避免"界面空了但模型还留着规则"。
                    editor.ruleModel.clearRules()
            }
        }

        Item { Layout.fillWidth: true }
    }

    // ---- 3. 已有规则：主行是人话，DSL 降级成次要信息 ----
    // 规则卡片列表。modelData 是 FilterRuleModel 生成的 QVariantMap，键名
    // （action / summary / dsl / detail / conditionLabel…）属于 C++ 侧契约。
    // 每条规则一张卡片：主行是人话摘要，DSL 折到次要位置——普通用户不该被语法
    // 吓到，但排查问题时又能看到真正生效的文本。
    Repeater {
        model: editor.ruleList

        delegate: RuleCard {
            required property int index
            required property var modelData

            ruleIndex: index
            actionText: String(modelData["action"] || "")
            actionLabel: String(modelData["actionLabel"] || "")
            conditionLabel: String(modelData["conditionLabel"] || "")
            summaryText: String(modelData["summary"] || "")
            dslText: String(modelData["dsl"] || "")
            detailText: String(modelData["detail"] || "")
            ruleModelRef: editor.ruleModel
            busy: editor.busy
            totalRules: editor.ruleList.length
        }
    }

    // ---- 4. 新建规则：选条件、填取值，语法由程序生成 ----
    // 表单里有哪些条件、每个条件的比较方式与单位，全部来自 FilterRuleModel::
    // editorOptions()；这里不硬编码任何条件名。
    ColumnLayout {
    // 表单整体（标题、取值行、提示、预览、错误、按钮）一起显隐，所以它是
    // 一个独立的 ColumnLayout，而不是散落在页面里的若干控件。
        id: builder
        objectName: editor.nameOf("RuleBuilderForm")
        Layout.fillWidth: true
        Layout.topMargin: 4
        spacing: 8
        visible: editor.builderVisible

        Text {
            objectName: editor.nameOf("RuleBuilderTitle")
            Layout.fillWidth: true
            text: editor.formAction === "include"
                ? "新建包含规则"
                : "新建排除规则"
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: theme.textPrimary
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: "条件类型"
                font.pixelSize: 16
                color: theme.textSecondary
            }

            AppComboBox {
                id: fieldBox
                objectName: editor.nameOf("RuleFieldCombo")
                Layout.preferredWidth: 180
                enabled: !editor.busy
                model: editor.fieldLabels
                currentIndex:
                    editor.keyIndex(editor.fieldKeys, editor.formField)
                onActivated: {
                    editor.formField = editor.fieldKeys[currentIndex]
                    editor.refreshFormError()
                }
            }

            Item { Layout.fillWidth: true }
        }

        // 取值控件按条件类型切换：不同条件用不同控件，用户不需要记任何格式。
        // 实现方式是同一行里放全部控件、用 visible 切换，而不是 Loader /
        // StackLayout：每个控件都绑同一组表单属性，RowLayout 会自动跳过
        // 不可见项，不需要手工调整间距。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // 文件名 / 路径 / 主文件名：一个通配符模式
            AppTextField {
                objectName: editor.nameOf("RulePatternField")
                Layout.fillWidth: true
                visible: editor.formField === "name"
                         || editor.formField === "path"
                         || editor.formField === "stem"
                enabled: !editor.busy
                placeholderText: editor.formField === "path" ? "如 **/build/**"
                                : editor.formField === "stem" ? "如 report*"
                                                              : "如 *.log"
                text: editor.formPattern
                onTextEdited: {
                    editor.formPattern = text
                    editor.refreshFormError()
                }
            }

            // 文件扩展名：分号分隔，不需要点号
            AppTextField {
                objectName: editor.nameOf("RuleExtensionField")
                Layout.fillWidth: true
                visible: editor.formField === "ext"
                enabled: !editor.busy
                placeholderText: "如 txt;md;pdf"
                text: editor.formExtensions
                onTextEdited: {
                    editor.formExtensions = text
                    editor.refreshFormError()
                }
            }

            // 文件类型：只能是下拉，绝不让用户输入 type:file
            AppComboBox {
                objectName: editor.nameOf("RuleTypeCombo")
                visible: editor.formField === "type"
                Layout.preferredWidth: 180
                enabled: !editor.busy
                model: editor.typeLabels
                currentIndex: editor.keyIndex(editor.typeKeys, editor.formType)
                onActivated: {
                    editor.formType = editor.typeKeys[currentIndex]
                    editor.refreshFormError()
                }
            }

            // 文件大小：比较方式 + 数值 + 单位（用户永远不写 size:<1MB）
            // 只有比较方式是 ".." 时才有上界输入框；单位对区间两端同时生效
            // （同一个 unit 字段），用户不能给上下界各写一个单位。
            AppComboBox {
                objectName: editor.nameOf("RuleSizeCompareCombo")
                visible: editor.formField === "size"
                Layout.preferredWidth: 150
                enabled: !editor.busy
                model: editor.sizeCompareLabels
                currentIndex:
                    editor.keyIndex(editor.sizeCompareKeys, editor.formCompare)
                onActivated: {
                    editor.formCompare = editor.sizeCompareKeys[currentIndex]
                    editor.refreshFormError()
                }
            }

            AppTextField {
                objectName: editor.nameOf("RuleSizeValueField")
                visible: editor.formField === "size"
                Layout.preferredWidth: 110
                enabled: !editor.busy
                placeholderText: "如 1"
                text: editor.formSizeLowText
                onTextEdited: {
                    editor.formSizeLowText = text
                    editor.refreshFormError()
                }
            }

            AppTextField {
                objectName: editor.nameOf("RuleSizeHighField")
                visible: editor.formField === "size"
                         && editor.formCompare === ".."
                Layout.preferredWidth: 110
                enabled: !editor.busy
                placeholderText: "上界"
                text: editor.formSizeHighText
                onTextEdited: {
                    editor.formSizeHighText = text
                    editor.refreshFormError()
                }
            }

            AppComboBox {
                objectName: editor.nameOf("RuleSizeUnitCombo")
                visible: editor.formField === "size"
                Layout.preferredWidth: 100
                enabled: !editor.busy
                model: editor.sizeUnitLabels
                currentIndex:
                    editor.keyIndex(editor.sizeUnitKeys, editor.formUnit)
                onActivated: {
                    editor.formUnit = editor.sizeUnitKeys[currentIndex]
                    editor.refreshFormError()
                }
            }

            // 用户 / 用户组 ID：数字，不是名字（核心目前没有 NSS 解析能力）
            // 记录的是数值 uid / gid，所以这一组必须填数字；按名字筛选
            // 用下面的 user / group 条件。
            AppComboBox {
                objectName: editor.nameOf("RuleIdCompareCombo")
                visible: editor.formField === "uid"
                         || editor.formField === "gid"
                Layout.preferredWidth: 150
                enabled: !editor.busy
                model: editor.idCompareLabels
                currentIndex:
                    editor.keyIndex(editor.idCompareKeys, editor.idCompare)
                onActivated: {
                    if (editor.formField === "uid")
                        editor.formUidCompare =
                            editor.idCompareKeys[currentIndex]
                    else
                        editor.formGidCompare =
                            editor.idCompareKeys[currentIndex]
                    editor.refreshFormError()
                }
            }

            AppTextField {
                objectName: editor.nameOf("RuleIdValueField")
                visible: editor.formField === "uid"
                         || editor.formField === "gid"
                Layout.preferredWidth: 130
                enabled: !editor.busy
                placeholderText: "如 1000"
                text: editor.formField === "uid"
                    ? editor.formUidText
                    : editor.formGidText
                onTextEdited: {
                    if (editor.formField === "uid")
                        editor.formUidText = text
                    else
                        editor.formGidText = text
                    editor.refreshFormError()
                }
            }

            AppTextField {
                objectName: editor.nameOf("RuleIdHighField")
                visible: (editor.formField === "uid"
                         || editor.formField === "gid")
                         && editor.idCompare === "range"
                Layout.preferredWidth: 130
                enabled: !editor.busy
                placeholderText: "区间上界"
                text: editor.formField === "uid"
                    ? editor.formUidHighText
                    : editor.formGidHighText
                onTextEdited: {
                    if (editor.formField === "uid")
                        editor.formUidHighText = text
                    else
                        editor.formGidHighText = text
                    editor.refreshFormError()
                }
            }

            // 用户名 / 用户组名：精确匹配，不支持通配符
            // 与 uid / gid 互补：按名字筛选时用这一组，不必自己查数值 ID。
            AppTextField {
                objectName: editor.nameOf("RuleNameValueField")
                Layout.fillWidth: true
                visible: editor.formField === "user"
                         || editor.formField === "group"
                enabled: !editor.busy
                placeholderText: editor.formField === "user"
                    ? "如 alice"
                    : "如 staff"
                text: editor.formField === "user"
                    ? editor.formUser
                    : editor.formGroup
                onTextEdited: {
                    if (editor.formField === "user")
                        editor.formUser = text
                    else
                        editor.formGroup = text
                    editor.refreshFormError()
                }
            }

            // 修改时间：形态下拉 + 天数 / 日期（日期格式由 C++ 裁决）
            // 三种形态共用两个输入框：last_days 用天数，day / day_range 用
            // 日期；格式与区间关系（起始 <= 结束）都由 builder 校验。
            AppComboBox {
                objectName: editor.nameOf("RuleMtimeKindCombo")
                visible: editor.formField === "mtime"
                Layout.preferredWidth: 160
                enabled: !editor.busy
                model: editor.mtimeLabels
                currentIndex:
                    editor.keyIndex(editor.mtimeKeys, editor.formMtimeKind)
                onActivated: {
                    editor.formMtimeKind = editor.mtimeKeys[currentIndex]
                    editor.refreshFormError()
                }
            }

            AppTextField {
                objectName: editor.nameOf("RuleMtimeDaysField")
                visible: editor.formField === "mtime"
                         && editor.formMtimeKind === "last_days"
                Layout.preferredWidth: 120
                enabled: !editor.busy
                placeholderText: "如 7"
                text: editor.formDaysBackText
                onTextEdited: {
                    editor.formDaysBackText = text
                    editor.refreshFormError()
                }
            }

            AppTextField {
                objectName: editor.nameOf("RuleMtimeDateField")
                visible: editor.formField === "mtime"
                         && (editor.formMtimeKind === "day"
                             || editor.formMtimeKind === "day_range")
                Layout.preferredWidth: 160
                enabled: !editor.busy
                placeholderText: "YYYY-MM-DD"
                text: editor.formDateLow
                onTextEdited: {
                    editor.formDateLow = text
                    editor.refreshFormError()
                }
            }

            AppTextField {
                objectName: editor.nameOf("RuleMtimeDateHighField")
                visible: editor.formField === "mtime"
                         && editor.formMtimeKind === "day_range"
                Layout.preferredWidth: 160
                enabled: !editor.busy
                placeholderText: "结束日期 YYYY-MM-DD"
                text: editor.formDateHigh
                onTextEdited: {
                    editor.formDateHigh = text
                    editor.refreshFormError()
                }
            }

            Item {
                Layout.fillWidth: true
                visible: editor.formField === "type"
                         || editor.formField === "size"
                         || editor.formField === "uid"
                         || editor.formField === "gid"
            }
        }

        Text {
            objectName: editor.nameOf("RuleFieldHintText")
            Layout.fillWidth: true
            text: editor.fieldHint
            font.pixelSize: 15
            color: theme.textSecondary
            wrapMode: Text.WordWrap
        }

        // 程序负责语法：这一行把"用户选的东西会变成什么规则"用人话复述一遍。
        // 预览与错误分开显示：前者是"会添加什么"，后者是"为什么不能添加"；
        // 两者都由 C++ 生成，界面不做二次加工。
        Text {
            objectName: editor.nameOf("RuleFormSummaryText")
            Layout.fillWidth: true
            visible: text.length > 0
            text: editor.formSummary.length > 0
                  ? "将添加：" + editor.formSummary
                  : ""
            font.pixelSize: 15
            color: theme.textSecondary
            wrapMode: Text.WordWrap
        }

        Text {
            objectName: editor.nameOf("RuleFormErrorText")
            Layout.fillWidth: true
            visible: text.length > 0
            text: editor.formError
            font.pixelSize: 15
            color: theme.accent
            wrapMode: Text.WordWrap
        }

        RowLayout {
            spacing: 8

            AppButton {
            // enabled 直接绑定 C++ 的校验结果：formError 非空就点不动，因此
            // 不存在"先提交、再被拒绝"的往返；busy 表示页面正在跑备份。
                objectName: editor.nameOf("RuleSubmitButton")
                text: "添加规则"
                variant: "primary"
                enabled: !editor.busy && editor.formError.length === 0
                onClicked: editor.submitForm()
            }

            AppButton {
                objectName: editor.nameOf("RuleCancelButton")
                text: "取消"
                onClicked: {
                    editor.resetForm(editor.formAction)
                    editor.builderVisible = false
                }
            }
        }
    }

    // ---- 5. 高级规则（完整 DSL）----
    //
    // 可视化表单每条规则只填一个条件；DSL 允许一条规则里写多个条件（AND），
    // 而多条 --include 之间是 OR —— 两者并不等价。默认收起，但能力一点没少。
    // 高级规则与可视化规则存在同一个 rules 列表里：删除 / 上移 / 下移一视同仁，
    // 区别只在卡片显示原文而非人话摘要；校验同样走 Filter::AddRule。
    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: 4
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Text {
                text: "高级规则"
                font.pixelSize: 16
                font.weight: Font.DemiBold
                color: theme.textSecondary
            }

            Text {
                Layout.fillWidth: true
                text: "熟悉筛选语法时可以直接写完整规则。"
                font.pixelSize: 15
                color: theme.textSecondary
                elide: Text.ElideRight
            }

            AppButton {
                objectName: editor.nameOf("AdvancedRulesToggle")
                text: editor.advancedExpanded ? "收起 ▾" : "展开 ▸"
                variant: "flat"
                onClicked: editor.advancedExpanded = !editor.advancedExpanded
            }
        }

        ColumnLayout {
            id: advancedSection
            objectName: editor.nameOf("AdvancedRulesSection")
            Layout.fillWidth: true
            spacing: 6
            visible: editor.advancedExpanded

            Text {
                Layout.fillWidth: true
                text: "一条高级规则可以同时包含多个条件，"
                        + "用空格分隔，全部满足时才命中。"
                font.pixelSize: 15
                color: theme.textSecondary
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                AppComboBox {
                    id: advancedActionBox
                    objectName: editor.nameOf("AdvancedActionCombo")
                    Layout.preferredWidth: 110
                    enabled: !editor.busy
                    model: ["包含", "排除"]
                    onActivated: editor.advancedAction =
                        currentIndex === 1 ? "exclude" : "include"
                }

                AppTextField {
                    id: advancedField
                    objectName: editor.nameOf("AdvancedRuleField")
                    Layout.fillWidth: true
                    enabled: !editor.busy
                    placeholderText: "如 name:*.txt size:<1MB"
                    onTextEdited: advancedErrorText.text = ""
                }

                AppButton {
                    // 失败只写本地 advancedErrorText，不走模型的错误通道；
                    // 成功才清空输入。两条通道分开，是为了让高级规则错在哪里
                    // 不覆盖表单错误。
                    objectName: editor.nameOf("AddAdvancedRuleButton")
                    text: "添加高级规则"
                    enabled: !editor.busy
                    onClicked: editor.submitAdvanced()
                }
            }

            Text {
                id: advancedErrorText
                objectName: editor.nameOf("AdvancedRuleErrorText")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 15
                color: theme.accent
                visible: text.length > 0
            }
        }
    }

    // 生效规则的只读回显：summary 是一句人话，dsl 是交给 Filter 的文本。
    // 两者都来自模型（rules / summaryText / dslText）；页面保存时取每条规则
    // 的 dsl，而不是这一行拼出来的串。
    // ---- 6. 当前生效的规则（一句人话 + 等价 DSL）----
    Text {
        objectName: editor.nameOf("RuleSummaryText")
        Layout.fillWidth: true
        Layout.topMargin: 2
        text: editor.summaryLine
        font.pixelSize: 16
        color: theme.textPrimary
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: editor.nameOf("RuleDslPreviewText")
        Layout.fillWidth: true
        text: editor.dslPreview.length > 0 ? "DSL：" + editor.dslPreview : ""
        visible: text.length > 0
        font.pixelSize: 14
        font.family: "monospace"
        color: theme.textSecondary
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: editor.nameOf("RuleErrorText")
        Layout.fillWidth: true
        visible: text.length > 0
        text: editor.errorText
        font.pixelSize: 15
        color: theme.accent
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: editor.nameOf("RuleCliText")
        Layout.fillWidth: true
        // CLI 等价参数只在调用方要求时显示（高级入口）；它来自模型的
        // cliArguments()，与真正执行备份时的参数同源，方便抄到命令行复现。
        visible: editor.showCliLine && editor.ruleList.length > 0
        text: "CLI 等价参数：" + editor.cliLine
        font.pixelSize: 14
        font.family: "monospace"
        color: theme.textSecondary
        wrapMode: Text.WordWrap
    }
}

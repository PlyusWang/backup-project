// AppTextField.qml
//
// 输入框：普通 / hover / focus / disabled 四态。
// 这里不做任何路径加工（trim、补斜杠等），用户填什么就存什么——
// Linux 下空格和大小写都是文件名的一部分，清洗反而会改坏路径。

// 职责边界：只提供“外观 + 原生输入行为”，不做校验、不 trim、不改写内容，也不
// 上报错误——是否合法由调用方判断（FilterRuleEditor 走 C++ 侧的表单校验）。
// 状态与外观的对应关系（全部由控件自身状态推出，外部只给 enabled 与文本）：
//   disabled -> 固定 theme.border 边框，无 hover / focus 反馈
//   normal   -> 1px 边框；hover 时边框转为 theme.textDisabled
//   focused  -> 2px 主题强调色边框
import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: control

    // 46 与 AppButton 的主按钮同高：输入框和按钮常在同一行（路径 + 浏览），
    // 行高对齐不靠调用方补 margin。左右 padding 11 让光标与文字不贴边框。
    implicitHeight: 46
    leftPadding: 11
    rightPadding: 11
    font.pixelSize: 17
    color: enabled ? theme.textPrimary : theme.textDisabled
    placeholderTextColor: theme.textSecondary
    selectionColor: theme.accent
    selectedTextColor: "#ffffff"
    selectByMouse: true
    hoverEnabled: true
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        radius: 9
        color: theme.surface
        // 聚焦时只换边框颜色，不加发光：发光在深色主题里最容易显得廉价。
        // 聚焦用加粗边框而不是发光/阴影：边框画在背景矩形自己的边界内，
        // 控件尺寸与文字位置都不变，一行里的多个输入框不会因聚焦互相挤动。
        border.width: control.activeFocus ? 2 : 1
        border.color: {
            if (!control.enabled)
                return theme.border
            if (control.activeFocus)
                return theme.accent
            return control.hovered ? theme.textDisabled : theme.border
        }
        Behavior on border.color { ColorAnimation { duration: 110 } }
    }
}

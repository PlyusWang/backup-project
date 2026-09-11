// AppCard.qml
//
// 卡片只做三件事：圆角、1px 细边框、容器底色。
// 刻意不加阴影：Qt 6.4.2 上没有稳定的轻量阴影实现，
// 层次交给底色明度差 + 边框 + 留白，代价小且不会有渲染问题。

import QtQuick

Rectangle {
    id: card

    // radius 直接用 Rectangle 自带的属性，调用方写 AppCard { radius: ... } 就能覆盖；
    // 不需要再声明一个同名属性（那会被 QML 判成“同一属性被赋值两次”）。
    property int padding: 20

    default property alias content: inner.data

    color: theme.surface
    border.width: 1
    border.color: theme.border
    radius: 10

    // 高度 = 内容自己的隐式高度 + 上下内边距。
    // 内容是用 anchors.fill 铺满 inner 的，所以这里不能读 childrenRect：
    // 那会形成 卡片高度 → inner 高度 → 内容实际高度 → 卡片高度 的循环依赖，
    // QML 破环时取 0，卡片就只剩内边距那么高，内容会溢出到卡片外面。
    // 读内容项的 implicitHeight 不参与这个环，显式给了高度的调用方也不受影响。
    implicitHeight: {
        let content_height = 0
        const items = inner.children
        for (let i = 0; i < items.length; ++i) {
            if (items[i].visible)
                content_height = Math.max(content_height, items[i].implicitHeight)
        }
        return content_height + padding * 2
    }

    Item {
        id: inner
        anchors.fill: parent
        anchors.margins: card.padding
    }
}


// AppIcon.qml
//
// 极简线性图标：全部用 Canvas 手绘，不下载任何图标包，也不用 emoji。
// 之所以不用 SVG 文件，是因为 QML 的 Image 不能方便地按主题重新着色；
// 直接用 Canvas 画就能跟随 theme 的颜色绑定，切主题不需要换图片。

// 绘制生命周期：属性变化 -> requestPaint() -> onPaint 一次性重画全部路径。
// Canvas 没有局部重绘的概念，所以每个 case 都要自己 beginPath，不能依赖
// 上一条路径的残留状态。
//
// 所有 case 共用的前提：坐标系已被 ctx.scale 缩放到 18x18，颜色来自 color
// 属性，线宽来自 size —— case 内部只允许出现路径坐标，不再改这些设置。
import QtQuick

Canvas {
    id: icon

    // 唯一输入。默认空串什么都不画，所以先把 AppIcon 放上去当占位是安全的；
    // 拼错名字只会在界面上留一个空位，不会有任何报错。color / size 有默认值，
    // 调用点通常只写 name。
    property string name: ""
    property color color: theme.textPrimary
    property int size: 18

    // width 与 height 都绑到同一个 size，两个方向的缩放系数因此相同 ——
    // 即使调用点只写了 size，图标也不会被拉变形。
    width: size
    height: size
    antialiasing: true

    // 三个属性都必须显式驱动重画：onPaint 不会因为属性变化自动重跑，少了
    // 任何一条绑定，切主题或换名字时都会保留上一帧的像素。
    // 颜色或图标名变化时重画；Canvas 不会自动跟踪依赖。
    onColorChanged: requestPaint()
    onNameChanged: requestPaint()
    onSizeChanged: requestPaint()
    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()
        ctx.clearRect(0, 0, width, height)
        ctx.strokeStyle = color
        ctx.fillStyle = color
        // 线宽随尺寸缩放但有下限 1.2：更细的线在小尺寸下会被抗锯齿抹成
        // 一层灰雾，"图标一变小就消失"是这里最容易踩的坑。
        ctx.lineWidth = Math.max(1.2, size / 12)
        ctx.lineCap = "round"
        ctx.lineJoin = "round"

        // 统一在 18x18 的逻辑坐标系里描述路径，再按实际尺寸缩放，
        // 这样同一个图标在 16 / 18 / 20 px 下比例一致。
        // save/restore 包住整套缩放：Canvas 的上下文状态在多次 onPaint 之间
        // 是保留的，不还原会让缩放逐次累乘，图标越来越小。
        ctx.save()
        ctx.scale(width / 18, height / 18)

        switch (name) {
        case "app":
            ctx.beginPath()
            ctx.moveTo(9, 2.5)
            ctx.lineTo(15, 6)
            ctx.lineTo(15, 13)
            ctx.lineTo(3, 13)
            ctx.lineTo(3, 6)
            ctx.closePath()
            ctx.stroke()
            break
        case "home":
            ctx.beginPath()
            ctx.moveTo(2.8, 8.6)
            ctx.lineTo(9, 3.4)
            ctx.lineTo(15.2, 8.6)
            ctx.moveTo(4.8, 8.2)
            ctx.lineTo(4.8, 14.6)
            ctx.lineTo(13.2, 14.6)
            ctx.lineTo(13.2, 8.2)
            ctx.stroke()
            break
        case "backup":
            ctx.beginPath()
            ctx.moveTo(9, 3)
            ctx.lineTo(9, 11)
            ctx.moveTo(5.6, 7.8)
            ctx.lineTo(9, 11.2)
            ctx.lineTo(12.4, 7.8)
            ctx.moveTo(3.2, 14.4)
            ctx.lineTo(14.8, 14.4)
            ctx.stroke()
            break
        case "restore":
            ctx.beginPath()
            ctx.moveTo(9, 11.2)
            ctx.lineTo(9, 3.2)
            ctx.moveTo(5.6, 6.6)
            ctx.lineTo(9, 3.2)
            ctx.lineTo(12.4, 6.6)
            ctx.moveTo(3.2, 14.4)
            ctx.lineTo(14.8, 14.4)
            ctx.stroke()
            break
        case "folder":
            ctx.beginPath()
            ctx.moveTo(2.8, 5.4)
            ctx.lineTo(7.2, 5.4)
            ctx.lineTo(8.6, 7.2)
            ctx.lineTo(15.2, 7.2)
            ctx.lineTo(15.2, 13.6)
            ctx.lineTo(2.8, 13.6)
            ctx.closePath()
            ctx.stroke()
            break
        case "clock":
            // 表盘 + 时针分针。"定时"的语义就在这两根针上：
            // 画成"一个圆里加个点"在 16px 下认不出是什么。
            ctx.beginPath()
            ctx.arc(9, 9, 6.2, 0, Math.PI * 2)
            ctx.stroke()
            ctx.beginPath()
            ctx.moveTo(9, 9)
            ctx.lineTo(9, 4.9)
            ctx.stroke()
            ctx.beginPath()
            ctx.moveTo(9, 9)
            ctx.lineTo(12.3, 10.7)
            ctx.stroke()
            break
        // 八条射线按 45 度均分，端点用 cos/sin 现算，圆心 (9,9) 就是 18x18
        // 逻辑坐标系的中心。
        case "sun":
            ctx.beginPath()
            ctx.arc(9, 9, 3.4, 0, Math.PI * 2)
            ctx.stroke()
            for (var i = 0; i < 8; ++i) {
                var angle = i * Math.PI / 4
                ctx.beginPath()
                ctx.moveTo(9 + Math.cos(angle) * 5.4, 9 + Math.sin(angle) * 5.4)
                ctx.lineTo(9 + Math.cos(angle) * 7.2, 9 + Math.sin(angle) * 7.2)
                ctx.stroke()
            }
            break
        case "moon":
            // 月牙 = 外圆减掉一个右移的等半径圆：外弧走左半边，内弧反向咬回来，
            // 两条弧交在上下两个尖点上，闭合后填充就是一轮弯月。
            // 之前只画了一条 0.35π~1.55π 的弧，缺口是张开的，看着像个 C。
            // 这里用填充而不是描边：16px 下描边版两条弧之间只剩 2~3px，
            // 看上去像一对括号；填充版在这个尺寸上才认得出是月亮。
            var moon_radius = 6.8
            var moon_gap = 5.2
            var moon_half_gap = moon_gap / 2
            var cusp = Math.atan2(
                Math.sqrt(
                    moon_radius * moon_radius - moon_half_gap * moon_half_gap),
                moon_half_gap)
            // 月牙左右不对称，外圆圆心右移半个宽度，整体才是居中的。
            var moon_center_x = 9 + (moon_radius - moon_half_gap) / 2
            ctx.beginPath()
            ctx.arc(
                moon_center_x, 9, moon_radius, cusp, Math.PI * 2 - cusp, false)
            ctx.arc(moon_center_x + moon_gap, 9, moon_radius, Math.PI + cusp,
                    Math.PI - cusp, true)
            ctx.closePath()
            ctx.fill()
            break
        case "check":
            ctx.beginPath()
            ctx.moveTo(4, 9.4)
            ctx.lineTo(7.4, 12.8)
            ctx.lineTo(14, 5.6)
            ctx.stroke()
            break
        case "warning":
            ctx.beginPath()
            ctx.moveTo(9, 3.2)
            ctx.lineTo(15.2, 14.4)
            ctx.lineTo(2.8, 14.4)
            ctx.closePath()
            ctx.stroke()
            ctx.beginPath()
            ctx.moveTo(9, 7.4)
            ctx.lineTo(9, 10.6)
            ctx.stroke()
            ctx.beginPath()
            ctx.arc(9, 12.6, 0.7, 0, Math.PI * 2)
            ctx.fill()
            break
        case "minimize":
            ctx.beginPath()
            ctx.moveTo(4.5, 12.5)
            ctx.lineTo(13.5, 12.5)
            ctx.stroke()
            break
        case "maximize":
            ctx.strokeRect(4.8, 4.8, 8.4, 8.4)
            break
        // 最大化按钮的"还原"态：前景矩形 + 右上角露出的一条边，表示两个
        // 叠放的窗口。它与 "maximize" 由调用方按窗口状态二选一
        // （Main.qml 里的标题栏按钮）。
        case "restore-window":
            ctx.strokeRect(3.6, 6.4, 8, 8)
            ctx.beginPath()
            ctx.moveTo(6.4, 6.2)
            ctx.lineTo(6.4, 3.8)
            ctx.lineTo(14.2, 3.8)
            ctx.lineTo(14.2, 11.6)
            ctx.lineTo(11.8, 11.6)
            ctx.stroke()
            break
        case "settings":
            // 齿轮：一个中心圆 + 八根径向短齿。
            // 18x18 下再画齿形轮廓就糊成一团了。
            ctx.beginPath()
            ctx.arc(9, 9, 3.1, 0, Math.PI * 2)
            ctx.stroke()
            for (var s = 0; s < 8; ++s) {
                var sa = s * Math.PI / 4
                ctx.beginPath()
                ctx.moveTo(9 + Math.cos(sa) * 4.7, 9 + Math.sin(sa) * 4.7)
                ctx.lineTo(9 + Math.cos(sa) * 6.7, 9 + Math.sin(sa) * 6.7)
                ctx.stroke()
            }
            break
        case "refresh":
            // 一段留缺口的圆弧 + 起点处的小箭头：缺口让方向可读，
            // 箭头只是加强它，不额外画第二段弧。
            var refresh_radius = 5.4
            var refresh_start = Math.PI * 0.42
            ctx.beginPath()
            ctx.arc(9, 9, refresh_radius, refresh_start, Math.PI * 1.92)
            ctx.stroke()
            var tip_x = 9 + Math.cos(refresh_start) * refresh_radius
            var tip_y = 9 + Math.sin(refresh_start) * refresh_radius
            ctx.beginPath()
            ctx.moveTo(tip_x - 2.3, tip_y - 0.4)
            ctx.lineTo(tip_x, tip_y)
            ctx.lineTo(tip_x + 0.4, tip_y - 2.4)
            ctx.stroke()
            break
        case "trash":
            // 盖子、提手、桶身三段。桶身用开口梯形而不是矩形：
            // 直上直下的桶在 16px 下和"删除"两个字一样没有辨识度。
            ctx.beginPath()
            ctx.moveTo(3.4, 5.4)
            ctx.lineTo(14.6, 5.4)
            ctx.moveTo(7.2, 5.4)
            ctx.lineTo(7.2, 3.6)
            ctx.lineTo(10.8, 3.6)
            ctx.lineTo(10.8, 5.4)
            ctx.stroke()
            ctx.beginPath()
            ctx.moveTo(5.2, 5.4)
            ctx.lineTo(6.0, 14.4)
            ctx.lineTo(12.0, 14.4)
            ctx.lineTo(12.8, 5.4)
            ctx.stroke()
            break
        case "close":
            ctx.beginPath()
            ctx.moveTo(5.2, 5.2)
            ctx.lineTo(12.8, 12.8)
            ctx.moveTo(12.8, 5.2)
            ctx.lineTo(5.2, 12.8)
            ctx.stroke()
            break
        case "cloud":
            // 一朵云：远程备份在国际惯例里就是它，比画一台服务器更容易在
            // 16px 下认出来。三段等半径半圆共切线 + 一条底边，一个闭合路径，
            // 不需要任何填充。
            ctx.beginPath()
            ctx.moveTo(4.6, 13.2)
            ctx.lineTo(11.2, 13.2)
            ctx.arc(11.2, 11.0, 2.2, Math.PI * 0.5, -Math.PI * 0.5, true)
            ctx.arc(9.0, 8.8, 2.2, 0, Math.PI, true)
            ctx.arc(6.8, 11.0, 2.2, -Math.PI * 0.5, Math.PI * 0.5, true)
            ctx.closePath()
            ctx.stroke()
            break
        // 认不出的名字落到这里：什么都不画。新增图标只加 case 是不够的，
        // 调用点也要跟着加；拼错名字不会有任何报错。
        default:
            break
        }
        // 与上面的 ctx.save() 配对：restore 之后上下文回到进入 onPaint 时的
        // 状态，下一帧从干净状态开始。
        ctx.restore()
    }
}

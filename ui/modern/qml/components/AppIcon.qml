// AppIcon.qml
//
// 极简线性图标：全部用 Canvas 手绘，不下载任何图标包，也不用 emoji。
// 之所以不用 SVG 文件，是因为 QML 的 Image 不能方便地按主题重新着色；
// 直接用 Canvas 画就能跟随 theme 的颜色绑定，切主题不需要换图片。

import QtQuick

Canvas {
    id: icon

    property string name: ""
    property color color: theme.textPrimary
    property int size: 18

    width: size
    height: size
    antialiasing: true

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
        ctx.lineWidth = Math.max(1.2, size / 12)
        ctx.lineCap = "round"
        ctx.lineJoin = "round"

        // 统一在 18x18 的逻辑坐标系里描述路径，再按实际尺寸缩放，
        // 这样同一个图标在 16 / 18 / 20 px 下比例一致。
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
            ctx.beginPath()
            ctx.arc(10, 9, 5.2, Math.PI * 0.35, Math.PI * 1.55, false)
            ctx.stroke()
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
        case "close":
            ctx.beginPath()
            ctx.moveTo(5.2, 5.2)
            ctx.lineTo(12.8, 12.8)
            ctx.moveTo(12.8, 5.2)
            ctx.lineTo(5.2, 12.8)
            ctx.stroke()
            break
        default:
            break
        }
        ctx.restore()
    }
}

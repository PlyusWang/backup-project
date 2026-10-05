#!/usr/bin/env python3
# make-icon.py —— 生成 AppImage / .desktop 用的图标。
#
# 这里**没有新画任何品牌标记**：多边形的每一条坐标都照抄仓库里已有的
# ui/modern/qml/components/AppIcon.qml 中 case "app" 的路径（18x18 逻辑坐标系），
# 颜色用 ui/modern/app_theme.cpp 里的 accent（#0f6cbd）。本脚本只是把项目自己的
# 矢量标记导出成一个 PNG 文件 —— 仓库里没有现成的图片资产，而 AppImage 的
# appimagetool 必须有图标文件才能出包。
#
#   python3 packaging/client/make-icon.py <输出.png> [尺寸]
#
# 需要 python3-pil（Debian/Ubuntu: python3-pil）；缺了就直接失败，不静默降级。
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    sys.stderr.write("ERROR: 需要 python3-pil（apt-get install -y python3-pil）\n")
    sys.exit(1)

# AppIcon.qml case "app"：18x18 逻辑坐标系里的闭合多边形
PATH_POINTS = [(9, 2.5), (15, 6), (15, 13), (3, 13), (3, 6)]
ACCENT = (0x0F, 0x6C, 0xBD, 0xFF)
SUPERSAMPLE = 4

def main() -> int:
    if len(sys.argv) < 2:
        sys.stderr.write("用法: make-icon.py <输出.png> [尺寸]\n")
        return 2
    out = sys.argv[1]
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    scale = size * SUPERSAMPLE / 18.0
    stroke = max(1.2, 18.0 / 12.0) * scale          # 与 AppIcon.qml 同一公式
    img = Image.new("RGBA", (size * SUPERSAMPLE, size * SUPERSAMPLE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    pts = [(x * scale, y * scale) for (x, y) in PATH_POINTS]
    draw.line(pts + [pts[0]], fill=ACCENT, width=int(round(stroke)), joint="curve")
    # 端点补圆，等价于 AppIcon.qml 的 lineCap = "round"
    r = stroke / 2.0
    for (x, y) in pts:
        draw.ellipse([x - r, y - r, x + r, y + r], fill=ACCENT)
    img.resize((size, size), Image.LANCZOS).save(out)
    print("icon: %s (%dx%d, glyph from ui/modern/qml/components/AppIcon.qml, accent #0f6cbd)" % (out, size, size))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())

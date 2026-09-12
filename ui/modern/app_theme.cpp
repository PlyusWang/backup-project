// app_theme.cpp
//
// 只做两件事：给出两套调色板，以及把用户选择通过 QSettings 记住。
// 这里刻意不放任何布局或尺寸，视觉组件的间距由 QML 自己控制。

#include "app_theme.h"

#include <QSettings>

namespace backup_modern {

namespace {

// QSettings 键名固定，改名会让老用户的选择失效。
const char kDarkModeKey[] = "appearance/modern_dark";

// accent 用偏冷的现代蓝：比纯蓝灰一点，长时间看不刺眼，也不照抄任何产品色板。
// 写调色板时用 Rgb() 而不是 #rrggbb 字符串：拼错了编译期就能发现。
QColor Rgb(int r, int g, int b) { return QColor(r, g, b); }

}  // namespace

// 读上次的选择，读不到就用浅色。
// 这里不跟随桌面环境的深浅色设置：6.4.2 上要拿到这个信息得多引一层依赖，
// 而工具本身的主题选择是可以记住的，下次启动沿用即可。
AppTheme::AppTheme(QObject* parent) : QObject(parent) {
  QSettings settings;
  dark_ = settings.value(kDarkModeKey, false).toBool();
}

// 发一次 changed() 就够了：QML 里所有引用 theme.xxx 的绑定会一起重算。
void AppTheme::setDark(bool dark) {
  if (dark_ == dark) {
    return;
  }
  dark_ = dark;
  QSettings settings;
  settings.setValue(kDarkModeKey, dark_);
  emit changed();
}

void AppTheme::toggle() { setDark(!dark_); }

// 浅色不是纯白打底：窗口给一点冷灰，白卡片才能靠明度差自然浮起来，
// 也不至于在显示器上白得发晃。
AppTheme::Palette AppTheme::MakeLightPalette() {
  Palette palette;
  // 窗口用微冷灰而不是纯白，白卡片才能靠明度差自然浮起来。
  palette.background = Rgb(0xf3, 0xf5, 0xf8);
  palette.surface = Rgb(0xff, 0xff, 0xff);
  palette.surface_elevated = Rgb(0xff, 0xff, 0xff);
  palette.sidebar = Rgb(0xfa, 0xfb, 0xfd);
  palette.text_primary = Rgb(0x1f, 0x23, 0x28);
  palette.text_secondary = Rgb(0x5f, 0x6b, 0x7a);
  palette.text_disabled = Rgb(0xa5, 0xae, 0xba);
  palette.border = Rgb(0xe1, 0xe6, 0xec);
  palette.hover = Rgb(0xea, 0xef, 0xf5);
  palette.pressed = Rgb(0xdf, 0xe6, 0xef);
  palette.accent = Rgb(0x0f, 0x6c, 0xbd);
  palette.accent_hover = Rgb(0x11, 0x5e, 0xa3);
  palette.accent_pressed = Rgb(0x0d, 0x4f, 0x8a);
  palette.accent_soft = QColor(0x0f, 0x6c, 0xbd, 28);
  palette.success = Rgb(0x14, 0x7a, 0x3f);
  palette.error = Rgb(0xb4, 0x2b, 0x25);
  palette.warning = Rgb(0x9a, 0x62, 0x00);
  return palette;
}

// 深色不压到纯黑：纯黑配亮蓝会显得刺眼，中性灰更接近现代桌面工具，
// 长时间盯着也不累。
AppTheme::Palette AppTheme::MakeDarkPalette() {
  Palette palette;
  palette.background = Rgb(0x1b, 0x1c, 0x20);
  palette.surface = Rgb(0x23, 0x25, 0x2a);
  palette.surface_elevated = Rgb(0x2a, 0x2c, 0x32);
  palette.sidebar = Rgb(0x1f, 0x21, 0x25);
  palette.text_primary = Rgb(0xe4, 0xe7, 0xeb);
  palette.text_secondary = Rgb(0x9b, 0xa3, 0xad);
  palette.text_disabled = Rgb(0x63, 0x6b, 0x75);
  palette.border = Rgb(0x33, 0x36, 0x3c);
  palette.hover = Rgb(0x2e, 0x31, 0x37);
  palette.pressed = Rgb(0x36, 0x39, 0x40);
  palette.accent = Rgb(0x4c, 0x9a, 0xe8);
  palette.accent_hover = Rgb(0x66, 0xac, 0xf0);
  palette.accent_pressed = Rgb(0x3d, 0x86, 0xd0);
  palette.accent_soft = QColor(0x4c, 0x9a, 0xe8, 46);
  palette.success = Rgb(0x4f, 0xc0, 0x84);
  palette.error = Rgb(0xf0, 0x7b, 0x74);
  palette.warning = Rgb(0xe0, 0xa9, 0x4c);
  return palette;
}

}  // namespace backup_modern

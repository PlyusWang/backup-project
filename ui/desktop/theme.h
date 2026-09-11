// theme.h
//
// 桌面 GUI 的主题定义集中在这里：颜色表、两套 preset、QSS
// 生成和用户选择的读写。 之所以要集中，是因为样式一旦散成满仓库的
// setStyleSheet("...") 字符串， 改一次配色就得靠搜索，以后想加 Accent Color
// 也没有落笔的地方。

#ifndef BACKUP_PROJECT_UI_DESKTOP_THEME_H_
#define BACKUP_PROJECT_UI_DESKTOP_THEME_H_

#include <QString>

namespace backup_gui {

// 一套主题用到的颜色。字段按用途命名而不是按色值命名，
// hover_background 是导航/次级按钮悬停时的浅背景，accent_soft 是强调色
// 的低透明度版本（导航选中态用它，既不抢眼又能看出被选中）。
// 这样新增主题时只要重新填一遍这张表，QSS 本身不用动。
// 颜色存成 QString 而不是 QColor：它们的最终去处是 QSS 文本，
// 保持字符串形态可以直接拼进去，不必每一处再转换一次。
struct ThemeColors {
  QString window_background;
  QString sidebar_background;
  QString card_background;
  QString input_background;
  QString text_primary;
  QString text_secondary;
  QString border;
  QString hover_background;
  QString accent_soft;
  QString accent;
  QString accent_hover;
  QString error;
  QString success;
};

// 第一版只做浅色和深色两套，不做“跟随系统”，也不做自定义调色板。
enum class ThemeKind { kLight, kDark };

// 状态标题的几种状态，颜色统一从主题里取，不在页面里写死。
enum class StatusKind { kIdle, kRunning, kSuccess, kError };

ThemeColors LightTheme();
ThemeColors DarkTheme();

// 主题选择通过 QSettings 落盘，下次启动沿用上一次的选择。
ThemeKind LoadThemeKind();
void SaveThemeKind(ThemeKind kind);

// 把颜色表翻译成整个应用的 QSS。占位符用 @name 而不是 QString::arg 的 %1，
// 因为这里的替换点有十来个，序号很容易错位。
QString BuildStyleSheet(const ThemeColors& colors);

// 状态标题的颜色。“成功是什么绿、失败是什么红”只在这里定义一次。
QString StatusColor(const ThemeColors& colors, StatusKind kind);

}  // namespace backup_gui

#endif  // BACKUP_PROJECT_UI_DESKTOP_THEME_H_

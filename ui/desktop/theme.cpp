// theme.cpp
//
// 具体色值和 QSS 都在这个文件里。GUI 其它部分只使用 ThemeColors 和
// BuildStyleSheet 的结果，不自己写颜色常量。

#include "theme.h"

#include <QSettings>

namespace backup_gui {

namespace {

// 浅色：白底 + 极浅灰窗口背景 + 蓝色强调色，尽量接近系统控件的观感。
// 窗口背景比卡片略深一点，卡片才靠“更白”自然浮起来，而不是靠阴影——
// 第一版刻意不用阴影，层次交给边框和明度差。
ThemeColors MakeLightColors() {
  ThemeColors colors;
  colors.window_background = "#f6f7f9";
  colors.sidebar_background = "#ffffff";
  colors.card_background = "#ffffff";
  colors.input_background = "#ffffff";
  colors.text_primary = "#1f2328";
  colors.text_secondary = "#6b7280";
  colors.border = "#e3e6ea";
  colors.accent = "#2f6feb";
  colors.accent_hover = "#2559c9";
  colors.error = "#c0392b";
  colors.success = "#1f8b4c";
  return colors;
}

// 深色：背景压到中性灰而不是纯黑，避免与文字对比过强、看着发脏。
// 卡片比窗口亮一档、输入框比卡片暗一档，这样三层结构在深色下依然分得清。
ThemeColors MakeDarkColors() {
  ThemeColors colors;
  colors.window_background = "#1e2024";
  colors.sidebar_background = "#26282d";
  colors.card_background = "#2b2e33";
  colors.input_background = "#22242a";
  colors.text_primary = "#e6e8eb";
  colors.text_secondary = "#9aa0a8";
  colors.border = "#3a3d44";
  colors.accent = "#4c8dff";
  colors.accent_hover = "#6ba1ff";
  colors.error = "#ff7b72";
  colors.success = "#4ac37c";
  return colors;
}

// QSettings 的键名固定下来，避免以后有人顺手改名导致老用户设置失效。
// 配置文件落在哪里由 QApplication 的 organizationName / applicationName 决定，
// 所以 main.cpp 里必须先设置好这两个名字，否则会写到别的目录去。
const char kThemeSettingKey[] = "appearance/theme";

}  // namespace

// 两套 preset 用函数返回而不是全局常量：调用方拿到的是副本，
// 以后要做“在预设基础上微调 accent”时也不会互相污染。
ThemeColors LightTheme() { return MakeLightColors(); }

ThemeColors DarkTheme() { return MakeDarkColors(); }

// 第一次启动默认浅色：不预设用户偏好，浅色在投影和截图里也更稳妥。
ThemeKind LoadThemeKind() {
  QSettings settings;
  const QString value = settings.value(kThemeSettingKey, "light").toString();
  return value == "dark" ? ThemeKind::kDark : ThemeKind::kLight;
}

void SaveThemeKind(ThemeKind kind) {
  QSettings settings;
  settings.setValue(kThemeSettingKey,
                    kind == ThemeKind::kDark ? "dark" : "light");
}

// 用 switch 而不是查表：状态只有四种，写全之后编译器还能帮忙
// 检查新增状态时有没有漏掉分支。
QString StatusColor(const ThemeColors& colors, StatusKind kind) {
  switch (kind) {
    case StatusKind::kSuccess:
      return colors.success;
    case StatusKind::kError:
      return colors.error;
    case StatusKind::kRunning:
      return colors.accent;
    case StatusKind::kIdle:
      break;
  }
  return colors.text_primary;
}

QString BuildStyleSheet(const ThemeColors& colors) {
  // QSS 写成带占位符的模板：样式结构固定在下面这段文本里，
  // 具体颜色在函数末尾统一替换，避免把颜色散进十几个小字符串。
  QString sheet = QString::fromUtf8(R"QSS(
/* 先把底色和文字色全局定下来：深色主题下只要漏掉某一个控件，
   它就会退回系统默认的浅色，于是出现黑底黑字。 */
QWidget {
  background-color: @window;
  color: @text;
  font-size: 14px;
}

/* ---- 左侧导航 ---- */
/* 侧栏只画右边框，和内容区之间用一条细线分开，
   比整块阴影更安静，也不会在深色下糊成一片。 */
#Sidebar {
  background-color: @sidebar;
  border-right: 1px solid @border;
}
/* 侧栏标题：加粗一点就够，不做 Logo、也不塞图标。 */
#AppTitle {
  font-size: 17px;
  font-weight: 600;
  color: @text;
}
/* 导航按钮平时是“透明底 + 左对齐文字”，只有悬停和选中才给底色，
   这样侧栏看起来干净，也不会出现一排边框。 */
QPushButton#NavButton {
  text-align: left;
  padding: 9px 14px;
  border: 1px solid transparent;
  border-radius: 6px;
  background-color: transparent;
  color: @text;
}
QPushButton#NavButton:hover {
  background-color: @window;
}
/* 选中项用卡片底色 + 强调色文字：比整块反白更克制，也和卡片层级一致。 */
QPushButton#NavButton:checked {
  background-color: @card;
  border: 1px solid @border;
  color: @accent;
  font-weight: 600;
}

/* ---- 右侧内容 ---- */
/* 右侧大标题：层级只靠字号和字重表达，不加下划线、色块之类的装饰。 */
#PageTitle {
  font-size: 22px;
  font-weight: 600;
}
/* 副标题用次要文字色，让“说明”和“内容”自然拉开距离。 */
#PageSubtitle {
  color: @muted;
}
/* 字段名 500 字重：比正文强调一点，又不至于看起来像小标题。 */
#FieldLabel {
  font-weight: 500;
}
/* 卡片用 1px 边框 + 10px 圆角表达层级：圆角只做轻微收边，
   不做胶囊形，避免整个界面看起来像放大版手机 App。 */
#Card {
  background-color: @card;
  border: 1px solid @border;
  border-radius: 10px;
}
#StatusTitle {
  font-size: 15px;
  font-weight: 600;
}
#StatusMessage {
  color: @muted;
}

/* 输入框比卡片再暗/亮一档，聚焦时换成强调色边框，
   这样“现在焦点在哪个输入框”一眼就能看出来。 */
QLineEdit {
  background-color: @input;
  border: 1px solid @border;
  border-radius: 6px;
  padding: 7px 10px;
  selection-background-color: @accent;
  selection-color: #ffffff;
}
/* 聚焦只换边框颜色，不加发光或阴影：边框变化本身已经足够明显。 */
QLineEdit:focus {
  border: 1px solid @accent;
}

/* 次级按钮（“选择”）：卡片底色 + 细边框，悬停时把边框换成强调色。 */
QPushButton {
  background-color: @card;
  border: 1px solid @border;
  border-radius: 6px;
  padding: 7px 16px;
  color: @text;
}
QPushButton:hover {
  border-color: @accent;
}
/* 禁用态只压暗文字、保留边框形状，避免按钮在布局里“看起来消失”。 */
QPushButton:disabled {
  color: @muted;
}
/* 主按钮用独立的 objectName 而不是 :default 伪状态：
   一个页面只有一个主操作，用名字表达意图比依赖默认按钮状态更直观。 */
QPushButton#PrimaryButton {
  background-color: @accent;
  border: 1px solid @accent;
  color: #ffffff;
  font-weight: 600;
  padding: 9px 22px;
}
QPushButton#PrimaryButton:hover {
  background-color: @accent_hover;
  border-color: @accent_hover;
}
/* 主按钮禁用时降级成边框色：一眼能看出不可点，但不会太刺眼。 */
QPushButton#PrimaryButton:disabled {
  background-color: @border;
  border-color: @border;
  color: @muted;
}

/* 进度条固定高度：不确定进度的动画块不能让状态卡片忽高忽低跳动。 */
QProgressBar {
  background-color: @input;
  border: 1px solid @border;
  border-radius: 5px;
  min-height: 8px;
  max-height: 8px;
}
/* 进度块用强调色；不确定进度时由 Qt 自己让这一块来回滚动。 */
QProgressBar::chunk {
  background-color: @accent;
  border-radius: 4px;
}
)QSS");
  // 替换顺序有讲究：@accent_hover 必须排在 @accent 前面。
  // 反过来的话，先替换 @accent 会把 @accent_hover 吃掉一半，
  // 模板里就会残留 "_hover" 这样的尾巴，QSS 直接失效。
  sheet.replace("@window", colors.window_background);
  sheet.replace("@sidebar", colors.sidebar_background);
  sheet.replace("@card", colors.card_background);
  sheet.replace("@input", colors.input_background);
  sheet.replace("@text", colors.text_primary);
  sheet.replace("@muted", colors.text_secondary);
  sheet.replace("@border", colors.border);
  sheet.replace("@accent_hover", colors.accent_hover);
  sheet.replace("@accent", colors.accent);
  return sheet;
}

}  // namespace backup_gui

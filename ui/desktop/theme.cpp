// theme.cpp
//
// 具体色值和 QSS 都在这个文件里。GUI 其它部分只使用 ThemeColors 和
// BuildStyleSheet 的结果，不自己写颜色常量。

#include "theme.h"

#include <QSettings>

namespace backup_gui {

namespace {

// 浅色：浅灰页面 + 白卡片 + 蓝色强调色。
// 页面比卡片暗一点点就够了，层次靠这点明度差和细边框，不靠阴影。
ThemeColors MakeLightColors() {
  ThemeColors colors;
  colors.window_background = "#f5f6f8";
  colors.sidebar_background = "#ffffff";
  colors.card_background = "#ffffff";
  colors.input_background = "#fbfbfc";
  colors.text_primary = "#1f2328";
  colors.text_secondary = "#6b7280";
  colors.border = "#e4e7eb";
  colors.hover_background = "#eef1f5";
  colors.accent_soft = "rgba(47, 111, 235, 12%)";
  colors.accent = "#2f6feb";
  colors.accent_hover = "#2559c9";
  colors.error = "#c0392b";
  colors.success = "#1f8b4c";
  return colors;
}

// 深色：背景压到中性灰而不是纯黑，避免与文字对比过强、看着发脏。
// 卡片比页面亮一档、输入框比卡片暗一档，三层关系在深色下依然分得清。
ThemeColors MakeDarkColors() {
  ThemeColors colors;
  colors.window_background = "#1e2024";
  colors.sidebar_background = "#25272c";
  colors.card_background = "#2b2e34";
  colors.input_background = "#23252b";
  colors.text_primary = "#e6e8eb";
  colors.text_secondary = "#9aa0a8";
  colors.border = "#383b42";
  colors.hover_background = "#31343a";
  colors.accent_soft = "rgba(76, 141, 255, 18%)";
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
/* 这里只统一字体和前景色，绝不能再写 background-color。
   之前写成 QWidget { background-color: ... } 时，QLabel 这类子类也会跟着
   绘制背景，于是每行文字后面都拖出一条横向色带，界面看着像默认表单工具。 */
QWidget {
  color: @text;
  font-size: 14px;
}

/* 页面底色交给容器：主窗口、中央区域、页面根节点、页面栈。
   其余控件不写背景就是透明的，父级底色自然透出来。 */
QMainWindow, #CentralArea, #PageRoot, QStackedWidget {
  background-color: @window;
}

/* 文字控件一律透明：让文字直接浮在卡片上，而不是压在一个矩形块里。 */
QLabel {
  background-color: transparent;
}

/* ---- 左侧导航 ---- */
/* 侧栏只画右边框，和内容区之间用一条细线分开，比阴影安静。 */
#Sidebar {
  background-color: @sidebar;
  border-right: 1px solid @border;
}
/* 标题只靠字号和字重，不做 Logo、不塞图标。 */
#AppTitle {
  font-size: 17px;
  font-weight: 600;
}
/* 导航项平时完全透明；左侧留 3px 透明边框占位，
   选中态换成正的 indicator 时文字不会左右跳动。 */
QPushButton#NavButton {
  text-align: left;
  padding: 9px 14px;
  border: none;
  border-left: 3px solid transparent;
  border-radius: 6px;
  background-color: transparent;
  color: @text;
}
QPushButton#NavButton:hover {
  background-color: @hover;
}
/* 选中态用低透明度强调色 + 强调色文字，不用“卡片底色 + 整圈边框”：
   后者看起来像被按下的表单按钮，不像导航项。 */
QPushButton#NavButton:checked {
  background-color: @accent_soft;
  border-left: 3px solid @accent;
  color: @accent;
  font-weight: 600;
}
/* 主题切换按钮：轻量 flat 风格，无强边框，只有 hover 时给一点背景。 */
QPushButton#FlatButton {
  text-align: left;
  padding: 9px 14px;
  border: 1px solid transparent;
  border-radius: 6px;
  background-color: transparent;
  color: @muted;
}
QPushButton#FlatButton:hover {
  background-color: @hover;
  color: @text;
}

/* ---- 右侧内容 ---- */
/* 大标题层级只靠字号和字重，不加下划线、色块之类的装饰。 */
#PageTitle {
  font-size: 23px;
  font-weight: 600;
}
/* 副标题用次要文字色，让“说明”和“内容”自然拉开距离。 */
#PageSubtitle {
  color: @muted;
}
/* 字段名 500 字重：比正文强调一点，又不至于像小标题。 */
#FieldLabel {
  font-weight: 500;
}
/* 卡片：1px 边框 + 9px 圆角，层次靠底色明度差和留白，不用阴影。 */
#Card {
  background-color: @card;
  border: 1px solid @border;
  border-radius: 9px;
}
#StatusTitle {
  font-size: 15px;
  font-weight: 600;
}
#StatusMessage {
  color: @muted;
}

/* 输入框比卡片再暗/亮一档，聚焦时换成强调色边框，
   这样“焦点在哪个输入框”一眼就能看出来。 */
QLineEdit {
  background-color: @input;
  border: 1px solid @border;
  border-radius: 6px;
  padding: 8px 10px;
  min-height: 20px;
  selection-background-color: @accent;
  selection-color: #ffffff;
}
/* 聚焦只换边框颜色，不加发光或阴影。 */
QLineEdit:focus {
  border: 1px solid @accent;
}

/* 次级按钮（“选择”）：卡片底色 + 细边框，hover 只轻微加深背景。 */
QPushButton {
  background-color: @card;
  border: 1px solid @border;
  border-radius: 6px;
  padding: 8px 16px;
  min-height: 20px;
  color: @text;
}
QPushButton:hover {
  background-color: @hover;
}
/* 禁用态只压暗文字、保留形状，避免按钮在布局里“看起来消失”。 */
QPushButton:disabled {
  color: @muted;
}
/* 主按钮用独立 objectName 而不是 :default 伪状态：
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
/* 主按钮禁用时降级成边框色：看得出不可点，但不刺眼。 */
QPushButton#PrimaryButton:disabled {
  background-color: @border;
  border-color: @border;
  color: @muted;
}

/* 进度条固定细高度；间距写在 margin-top 上，
   这样它隐藏时不占位，出现时才把状态卡片撑开一点。 */
QProgressBar {
  background-color: @input;
  border: 1px solid @border;
  border-radius: 5px;
  margin-top: 10px;
  min-height: 6px;
  max-height: 6px;
}
/* 进度块用强调色；不确定进度时由 Qt 自己让这一块来回滚动。 */
QProgressBar::chunk {
  background-color: @accent;
  border-radius: 4px;
}
)QSS");
  // 替换顺序有讲究：@accent_soft 和 @accent_hover 都必须排在 @accent 前面。
  // 反过来的话，先替换 @accent 会把它们吃掉一半，
  // 模板里就会残留 "_soft"、"_hover" 这样的尾巴，QSS 直接失效。
  sheet.replace("@window", colors.window_background);
  sheet.replace("@sidebar", colors.sidebar_background);
  sheet.replace("@card", colors.card_background);
  sheet.replace("@input", colors.input_background);
  sheet.replace("@text", colors.text_primary);
  sheet.replace("@muted", colors.text_secondary);
  sheet.replace("@border", colors.border);
  sheet.replace("@hover", colors.hover_background);
  sheet.replace("@accent_soft", colors.accent_soft);
  sheet.replace("@accent_hover", colors.accent_hover);
  sheet.replace("@accent", colors.accent);
  return sheet;
}

}  // namespace backup_gui

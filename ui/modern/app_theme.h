// app_theme.h
//
// 现代 GUI 的设计系统：所有颜色 token 和 Light / Dark 两套取值都集中在这里。
// 做成 C++ 的 QObject 而不是 QML singleton，是因为 QML singleton 需要
// qmldir + import 配合，在 qrc 场景下容易出问题；做成上下文属性之后，
// 任何 QML 文件都能直接写 theme.accent，切主题时靠 NOTIFY 自动刷新绑定。
// 这一层只有颜色，不含尺寸与间距：按钮多高、卡片留白多少由 QML 决定，
// 免得设计 token 和布局实现耦合成一个文件。

#ifndef BACKUP_PROJECT_UI_MODERN_APP_THEME_H_
#define BACKUP_PROJECT_UI_MODERN_APP_THEME_H_

#include <QColor>
#include <QObject>

namespace backup_modern {

// token 命名对齐设计稿：背景 / 容器 / 侧栏 / 文字 / 边框 / 交互态 / 语义色。
// QML 只认这些名字，换配色时不需要动任何 QML 文件。
// QML 里的颜色一律来自这里，页面和组件不写死任何色值。
// 切主题时所有绑定一起刷新，不需要挨个页面去改。
class AppTheme : public QObject {
  Q_OBJECT
  // 所有 token 共用一个 changed() 通知：切主题时整套配色一起变，
  // 拆成十几个独立信号既没意义，也多一堆要维护的连接。
  // dark 是唯一带 WRITE 的属性，其余 token 都是只读的派生值。
  Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY changed)
  Q_PROPERTY(QColor background READ background NOTIFY changed)
  Q_PROPERTY(QColor surface READ surface NOTIFY changed)
  Q_PROPERTY(QColor surfaceElevated READ surfaceElevated NOTIFY changed)
  Q_PROPERTY(QColor sidebar READ sidebar NOTIFY changed)
  Q_PROPERTY(QColor textPrimary READ textPrimary NOTIFY changed)
  Q_PROPERTY(QColor textSecondary READ textSecondary NOTIFY changed)
  Q_PROPERTY(QColor textDisabled READ textDisabled NOTIFY changed)
  Q_PROPERTY(QColor border READ border NOTIFY changed)
  Q_PROPERTY(QColor hover READ hover NOTIFY changed)
  Q_PROPERTY(QColor pressed READ pressed NOTIFY changed)
  Q_PROPERTY(QColor accent READ accent NOTIFY changed)
  Q_PROPERTY(QColor accentHover READ accentHover NOTIFY changed)
  Q_PROPERTY(QColor accentPressed READ accentPressed NOTIFY changed)
  Q_PROPERTY(QColor accentSoft READ accentSoft NOTIFY changed)
  Q_PROPERTY(QColor success READ success NOTIFY changed)
  Q_PROPERTY(QColor error READ error NOTIFY changed)
  Q_PROPERTY(QColor warning READ warning NOTIFY changed)

 public:
  explicit AppTheme(QObject* parent = nullptr);

  // dark() 只读，setDark() 由 QML 通过属性写入；两个入口最后都汇到
  // 同一条“值真的变了才发信号”的路径上。
  bool dark() const { return dark_; }
  void setDark(bool dark);
  // 侧栏的主题开关直接调它；写成 Q_INVOKABLE 才能在 QML 的 onClicked 里调到。
  Q_INVOKABLE void toggle();

  // getter 全部从“当前生效的那套 Palette”取，QML 侧看到的永远只有一个来源，
  // 不会出现某些 token 已经切了、某些还是旧值的情况。
  QColor background() const { return palette().background; }
  QColor surface() const { return palette().surface; }
  QColor surfaceElevated() const { return palette().surface_elevated; }
  QColor sidebar() const { return palette().sidebar; }
  QColor textPrimary() const { return palette().text_primary; }
  QColor textSecondary() const { return palette().text_secondary; }
  QColor textDisabled() const { return palette().text_disabled; }
  QColor border() const { return palette().border; }
  QColor hover() const { return palette().hover; }
  QColor pressed() const { return palette().pressed; }
  QColor accent() const { return palette().accent; }
  QColor accentHover() const { return palette().accent_hover; }
  QColor accentPressed() const { return palette().accent_pressed; }
  QColor accentSoft() const { return palette().accent_soft; }
  QColor success() const { return palette().success; }
  QColor error() const { return palette().error; }
  QColor warning() const { return palette().warning; }

 signals:
  // 只有这一个信号：切主题时发一次，所有 QML 绑定自动重算。
  void changed();

 private:
  // 一套完整的颜色取值。加第三套主题时只需要再写一个 Palette 构造函数。
  struct Palette {
    QColor background;
    QColor surface;
    QColor surface_elevated;
    QColor sidebar;
    QColor text_primary;
    QColor text_secondary;
    QColor text_disabled;
    QColor border;
    QColor hover;
    QColor pressed;
    QColor accent;
    QColor accent_hover;
    QColor accent_pressed;
    QColor accent_soft;
    QColor success;
    QColor error;
    QColor warning;
  };

  // 两套取值由这两个静态函数生成，AppTheme 构造时各算一次；
  // 第三套主题（比如跟随系统的自动模式）加一个函数就够了。
  static Palette MakeLightPalette();
  static Palette MakeDarkPalette();
  const Palette& palette() const {
    return dark_ ? dark_palette_ : light_palette_;
  }

  // 两套调色板在构造时各生成一次，切主题只是换一个引用，不重算颜色。
  bool dark_ = false;
  Palette light_palette_ = MakeLightPalette();
  Palette dark_palette_ = MakeDarkPalette();
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_APP_THEME_H_

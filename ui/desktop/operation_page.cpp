// operation_page.cpp
//
// 页面本体：搭界面、选择目录、把任务丢给
// QtConcurrent，然后在任务结束时更新状态。

#include "operation_page.h"

#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <string>

#include "backup_engine.h"

namespace backup_gui {

namespace {

// 路径选择器的起始目录。空输入时退回家目录，避免对话框停在奇怪的位置。
QString StartDirectoryFor(const QLineEdit* edit) {
  return edit->text().isEmpty() ? QDir::homePath() : edit->text();
}

}  // namespace

// 构造顺序是：先搭控件、再连 watcher、最后套主题和初始状态。
// 反过来的话，SetStatus 会在状态控件还没建出来时就被调用。
OperationPage::OperationPage(OperationKind kind, QWidget* parent)
    : QWidget(parent), kind_(kind) {
  BuildLayout();
  // watcher 以 this 作为上下文对象：页面被销毁时 Qt 会自动断开，
  // 后台任务即使还在跑也不会回调到已经释放的控件上。
  // 用 lambda + 上下文对象（this）而不是自定义槽：少一个 Q_OBJECT 依赖，
  // 页面销毁时 Qt 又会自动断开这条连接，后台任务跑完也不会回调到野指针上。
  connect(
      &watcher_, &QFutureWatcher<OperationResult>::finished, this, [this]() {
        // 先把“进行中”的视觉收掉，再更新结果文字，
        // 避免出现“进度条还在转，但下面已经写成功”的中间状态。
        progress_->setVisible(false);
        SetControlsEnabled(true);
        // finished 之后 result() 才保证可用；提前调用会阻塞主线程等任务结束。
        const OperationResult result = watcher_.result();
        if (result.succeeded) {
          SetStatus(StatusKind::kSuccess, SucceededTitle(), SucceededMessage());
        } else {
          // 核心给的错误信息原样显示：它已经说明了是哪个路径、什么原因，
          // 在这里改写成“操作失败，请重试”反而会把有用的信息丢掉。
          SetStatus(StatusKind::kError, tr("操作失败"), result.error_message);
        }
      });
  ApplyTheme(LightTheme());
  SetStatus(StatusKind::kIdle, tr("等待操作"), IdleMessage());
}

void OperationPage::BuildLayout() {
  // 页面根部是竖直布局：标题、说明、操作卡片、状态卡片自上而下。
  // 末尾留一个 stretch，窗口变高时多出来的空间落在底部，
  // 而不是把几个控件之间的距离一起拽开。
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(32, 28, 32, 28);
  root->setSpacing(18);

  // 大标题 + 一行说明是右侧内容区的固定开头，两个页面保持一致，
  // 用户在两个页面之间切换时视觉起点不会跳。
  auto* title = new QLabel(WindowTitleText(), this);
  title->setObjectName("PageTitle");
  root->addWidget(title);

  auto* subtitle = new QLabel(SubtitleText(), this);
  subtitle->setObjectName("PageSubtitle");
  root->addWidget(subtitle);

  // 操作卡片把两条路径和主按钮放在一起：它们是同一次操作的输入，
  // 和下面的状态区分开之后，状态变化不会牵动输入区的位置。
  auto* card = new QWidget(this);
  card->setObjectName("Card");
  auto* card_layout = new QVBoxLayout(card);
  card_layout->setContentsMargins(22, 22, 22, 22);
  card_layout->setSpacing(14);

  // 两条路径的结构完全一样，用循环生成，省得维护两份几乎相同的代码。
  const QString labels[2] = {FirstFieldLabel(), SecondFieldLabel()};
  QLineEdit* edits[2] = {nullptr, nullptr};
  for (int index = 0; index < 2; ++index) {
    auto* label = new QLabel(labels[index], card);
    label->setObjectName("FieldLabel");
    card_layout->addWidget(label);

    auto* row = new QHBoxLayout();
    row->setSpacing(10);
    auto* edit = new QLineEdit(card);
    // 输入框保持可编辑：备份仓库允许是一个尚不存在的路径，
    // 而 QFileDialog 只能方便地选已存在的目录，不能把核心支持的能力卡掉。
    edit->setPlaceholderText(tr("可直接输入路径，也可以点击右侧按钮选择"));
    // stretch 给输入框：路径通常很长，按钮只需要自然宽度。
    edit->setMinimumWidth(240);
    row->addWidget(edit, 1);

    auto* choose = new QPushButton(tr("选择"), card);
    connect(choose, &QPushButton::clicked, this,
            [this, edit]() { ChooseDirectory(edit); });
    row->addWidget(choose);

    card_layout->addLayout(row);
    edits[index] = edit;
  }
  first_edit_ = edits[0];
  second_edit_ = edits[1];

  // 主按钮单独一行、左对齐：它是最重要的操作，但不需要占满整行宽度。
  action_button_ = new QPushButton(ActionButtonText(), card);
  action_button_->setObjectName("PrimaryButton");
  action_button_->setCursor(Qt::PointingHandCursor);
  connect(action_button_, &QPushButton::clicked, this,
          [this]() { StartOperation(); });

  auto* action_row = new QHBoxLayout();
  action_row->addWidget(action_button_);
  action_row->addStretch(1);
  card_layout->addSpacing(4);
  card_layout->addLayout(action_row);

  root->addWidget(card);

  // 状态卡片单独成块：标题给结论（等待/进行中/成功/失败），
  // 正文给细节，失败时直接把核心的 error_message 原文放在这里。
  auto* status_card = new QWidget(this);
  status_card->setObjectName("Card");
  auto* status_layout = new QVBoxLayout(status_card);
  status_layout->setContentsMargins(22, 18, 22, 18);
  status_layout->setSpacing(10);

  status_title_ = new QLabel(status_card);
  status_title_->setObjectName("StatusTitle");
  status_layout->addWidget(status_title_);

  status_message_ = new QLabel(status_card);
  status_message_->setObjectName("StatusMessage");
  // 错误信息里常常是很长的绝对路径：允许换行、允许选中复制，
  // 并且限定最小宽度不要让长路径把窗口越撑越宽。
  // 错误信息里常带很长的绝对路径：允许换行、允许用鼠标选中复制，
  // 用户才能把原因完整贴给别人，而不是只看到被截断的半句话。
  status_message_->setWordWrap(true);
  status_message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  status_message_->setMinimumWidth(200);
  status_layout->addWidget(status_message_);

  // 进度条平时隐藏，只在任务运行期间出现，结束后立刻收起。
  progress_ = new QProgressBar(status_card);
  // range 0..0 就是“不确定进度”：核心目前没有进度回调，
  // 与其编一个假的百分比，不如明确告诉用户“在跑，但不知道还剩多少”。
  progress_->setRange(0, 0);
  progress_->setTextVisible(false);
  progress_->setVisible(false);
  status_layout->addWidget(progress_);

  root->addWidget(status_card);
  root->addStretch(1);
}

// 页面只保存一份颜色副本，整体 QSS 由主窗口统一设置。
// 这样切主题时不会出现“输入框已经换色、状态文字还是旧颜色”的错位。
void OperationPage::ApplyTheme(const ThemeColors& colors) {
  colors_ = colors;
  ApplyStatusColors();
}

void OperationPage::ApplyStatusColors() {
  if (status_title_ == nullptr) {
    return;
  }
  // 状态颜色随状态变化，写在 QSS 里需要动态属性 + 重新 polish，
  // 直接按当前状态取一次主题颜色更简单，也不会漏掉刷新。
  status_title_->setStyleSheet(
      QString("color: %1;").arg(StatusColor(colors_, status_kind_)));
}

void OperationPage::ChooseDirectory(QLineEdit* target) {
  // 目录选择器只是“帮你填”：填完仍然可以手改，不在这里做任何路径规范化，
  // 路径是否合法由核心判断，GUI 不复制一套校验规则。
  const QString chosen = QFileDialog::getExistingDirectory(
      this, tr("选择目录"), StartDirectoryFor(target));
  if (chosen.isEmpty()) {
    return;  // 用户取消，保持原输入不动。
  }
  // 选择结果原样写入：不做 trim、不补斜杠，Linux 下空格是合法文件名。
  target->setText(chosen);
}

void OperationPage::StartOperation() {
  // 双保险：正常路径下主按钮已经被禁用，但快捷键或程序化调用仍可能走到这里，
  // 一旦重复提交就会有两个复制任务同时写同一个仓库。
  if (IsRunning()) {
    return;
  }

  OperationRequest request;
  request.kind = kind_;
  request.first_path = first_edit_->text();
  request.second_path = second_edit_->text();
  if (request.first_path.isEmpty() || request.second_path.isEmpty()) {
    // 只检查“有没有填”。路径对不对、拓扑合不合法，全部交给 BackupEngine 判断，
    // GUI 不再复制一套路径校验逻辑。
    SetStatus(StatusKind::kError, tr("操作失败"), tr("请先填写两个路径。"));
    return;
  }

  // 先把界面锁住再启动任务：否则用户可能在任务已经开跑之后再点一次，
  // 两次复制同时写同一个仓库，结果谁也说不清。
  SetControlsEnabled(false);
  SetStatus(StatusKind::kRunning, RunningTitle(), RunningMessage());
  progress_->setVisible(true);
  // QtConcurrent 走全局线程池，主线程只等 finished 信号，
  // 所以大目录复制期间窗口依然能拖动、能响应事件。
  // 传函数指针 + 按值拷贝的 request：后台线程拿到的是自己的副本，
  // 不需要加锁，也不会读到界面线程正在修改的数据。
  watcher_.setFuture(QtConcurrent::run(&OperationPage::RunOperation, request));
}

OperationResult OperationPage::RunOperation(const OperationRequest& request) {
  // 这个函数跑在后台线程：只创建引擎、调一次接口，绝不触碰任何 QWidget。
  // QString 到 std::string 只在这里转换一次，核心接口上不再反复转。
  backupproject::BackupEngine engine;
  std::string error_message;
  const std::string first = request.first_path.toStdString();
  const std::string second = request.second_path.toStdString();

  OperationResult result;
  if (request.kind == OperationKind::kBackup) {
    result.succeeded = engine.Backup(first, second, &error_message);
  } else {
    result.succeeded = engine.Restore(first, second, &error_message);
  }
  if (!result.succeeded) {
    result.error_message = QString::fromStdString(error_message);
  }
  return result;
}

// 状态更新只走这一个入口：标题、正文、颜色一起改，
// 避免各处零散赋值导致“文字说成功、颜色还是红的”这类不同步。
void OperationPage::SetStatus(StatusKind kind, const QString& title,
                              const QString& message) {
  status_kind_ = kind;
  status_title_->setText(title);
  status_message_->setText(message);
  ApplyStatusColors();
}

// 只锁输入框和主按钮，不动左侧导航：任务在跑时用户仍然可以切到另一页看看。
void OperationPage::SetControlsEnabled(bool enabled) {
  first_edit_->setEnabled(enabled);
  second_edit_->setEnabled(enabled);
  action_button_->setEnabled(enabled);
}

bool OperationPage::IsRunning() const { return watcher_.isRunning(); }

// 下面这组文案按操作类型分支。集中放在一处的好处是改措辞只改一个地方，
// 也不会出现两个页面用词不一致的情况。
QString OperationPage::WindowTitleText() const {
  return kind_ == OperationKind::kBackup ? tr("备份") : tr("恢复");
}

QString OperationPage::SubtitleText() const {
  return kind_ == OperationKind::kBackup ? tr("把一个目录完整备份到指定仓库。")
                                         : tr("从备份仓库恢复目录树。");
}

QString OperationPage::FirstFieldLabel() const {
  return kind_ == OperationKind::kBackup ? tr("源目录") : tr("备份仓库");
}

QString OperationPage::SecondFieldLabel() const {
  return kind_ == OperationKind::kBackup ? tr("备份仓库") : tr("恢复目录");
}

QString OperationPage::ActionButtonText() const {
  return kind_ == OperationKind::kBackup ? tr("开始备份") : tr("开始恢复");
}

QString OperationPage::RunningTitle() const {
  return kind_ == OperationKind::kBackup ? tr("正在备份……") : tr("正在恢复……");
}

QString OperationPage::RunningMessage() const {
  return tr("正在复制目录，完成前请勿关闭窗口。");
}

QString OperationPage::SucceededTitle() const {
  return kind_ == OperationKind::kBackup ? tr("备份完成") : tr("恢复完成");
}

QString OperationPage::SucceededMessage() const {
  return kind_ == OperationKind::kBackup ? tr("源目录已写入备份仓库。")
                                         : tr("备份仓库已恢复到目标目录。");
}

QString OperationPage::IdleMessage() const {
  return kind_ == OperationKind::kBackup
             ? tr("选择源目录和备份仓库后，点击“开始备份”。")
             : tr("选择备份仓库和恢复目录后，点击“开始恢复”。");
}

}  // namespace backup_gui

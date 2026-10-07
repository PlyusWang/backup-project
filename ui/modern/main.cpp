// main.cpp
//
// 现代 QML GUI 的入口。除正常启动外还带几个开发期开关：
//   --smoke-test 建引擎、建窗口、遍历七个页面、换主题后退出
//   --screenshot <目录>                 七个页面 × 两套主题渲染成 PNG
//   --self-test <源> <备份文件> <恢复目录> [--include R] [--exclude R]
//                                       真跑一次 direct archive 备份 + 恢复
//   --repository-test <源> <仓库> <恢复目录>
//                                       走 ConfigManager + BackupCatalog 的
//                                       repository-driven 产品链路端到端验证
//   --backup-options-test               验证算法选项 / 密码校验 / 加密恢复 /
//                                       目录字段 / 密码不落盘（真实控制器路径）
//   --schedule-test                     验证自动备份页的控制器链路：保存计划、
//                                       立即运行、无变化跳过、变化建快照、
//                                       retention、0600 权限、与 CLI 同源路径
//   --preview-test <源> [--include R]... [--exclude R]...
//                                       把 Manual Backup 的筛选预览按 CLI 的
//                                       格式打出来，供脚本与
//                                       `backupctl preview` 逐行对比
//   --config-file <路径>                指定配置文件（测试隔离真实用户配置）
//   --schedule-file <路径>              指定计划存储文件（测试隔离真实计划）
//   --schedule-show                     把控制器读到的计划配置打成 key=value，
//                                       用来证明 GUI 与 CLI 读的是同一份 store
//   --realtime-test                     验证实时备份页的控制器链路：写配置并
//                                       逐字段读回、attach watcher、resync 触发
//                                       快照、文件事件触发快照、列出实时快照，
//                                       以及运行期改仓库后跟上新仓库 / 非法仓库
//                                       只降级不写盘 / 未启用时不启动 watcher
//   --realtime-show                     把控制器读到的实时配置打成 key=value，
//                                       用来证明 GUI 与 CLI 读的是同一份 store
//   --realtime-file <路径> 指定实时存储文件（测试隔离真实实时配置）
//   --remote-test                       验证远程备份页的控制器链路：真的起一个
//                                       backup-server，走注册 / 登录 / 上传真实
//                                       归档 / 列表 / 下载（含默认不覆盖）/
//                                       删除 （含确认路径）/
//                                       退出登录；并断言密码框回显 模式、口令与
//                                       token 不落盘、忙碌时冲突请求
//                                       被拒、页面提示不外泄
//   --remote-smoke <地址> <端口> <用户名>
//                                       对着真实远端（阿里云 ECS 上只监听
//                                       127.0.0.1:18765 的 backup-server，经
//                                       SSH 隧道转发）走一次 GUI 路径：注册 /
//                                       登录 / 列表 / 上传真实归档 / 下载比对 /
//                                       删除。 口令只从环境变量
//                                       BACKUP_REMOTE_PASSWORD 读，不进 argv
//   --remote-acceptance <输出目录> <地址> <端口> <用户名>
//                                       无人值守 GUI 最终验收：抓真实
//                                       窗口截图（浅色 / 深色 / 窄窗口）、读
//                                       关键控件的真实几何并断言不重叠不越界，
//                                       并把完整 / 增量 / 回退成完整基线 /
//                                       无变化 / 原始归档 / 冷缓存链恢复 /
//                                       错 pin / 空闲重连 + RESUME / 忙碌
//                                       逐个真的走一遍。口令与指纹走环境变量
//                                       BACKUP_REMOTE_PASSWORD /
//                                       BACKUP_REMOTE_PIN，不进 argv
//   --path-test                         验证本地路径与 URL 互转不丢字符
//   --close-guard-test                  验证任务进行中关窗会被拦下：手动备份、
//                                       实时触发、计划评估三位 writer 都要在
//                                       飞时被拒绝、结束后放行
//   --gui-contract-test                 验证首页三张卡片的按钮几何，以及
//                                       "临时提示只属于产生它的页面"这条契约
//   --incremental-test <源> <仓库>      GUI/CLI parity：走真实控制器
//                                       入口跑 baseline / no-change / delta /
//                                       依赖链恢复，按固定格式打印结果
//   --combo-hover-test                  共享下拉的 hover 残留回归：真的把指针
//                                       移到某一行、再移走，
//                                       断言灰底严格跟着指针来去，
//                                       关掉重开也不留痕迹
//   --filter-ux-test                    三页 parity：同一个普通表单输入
//                                       （条件类型 + 取值）在备份页 /
//                                       自动备份页 / 实时备份页生成同一条
//                                       DSL，非法输入三处 得到同一句来自共享
//                                       builder 的原因
//   --native-frame                      退回系统原生标题栏（Wayland 兜底）
//
// 这些开关让没有显示器的环境也能验证界面：离屏平台插件把窗口真正建出来，
// 自检再切一遍页面、换一次主题、跑一次备份恢复，不需要人盯着屏幕。
//
// 自检的统一约定（改这些自检之前先读这一段）：
//
//   * 退出码：0 = 全部断言通过；1 = 有断言失败或环境不满足（临时目录建不出来、
//     目标控件找不到）；2 = 命令行用法错误（缺参数、参数解析失败）。脚本只依赖
//     退出码判断成败，人工排查时再看 stderr。
//   * stdout 是给脚本解析的接口：key=value 行、passed=/failed= 汇总行、与
//     backupctl 逐字比对的输出格式都算契约，改措辞等于改接口。
//   * stderr 只放失败原因，而且取核心 / 控制器给出的原文，不翻译也不包装 ——
//     脚本看到的失败原因与用户在界面上看到的是同一句话。
//   * 数据隔离：输入一律造在临时目录里，存储位置由 --config-file /
//     --schedule-file / --realtime-file 覆盖；远端自检用随机账号并在结束前
//     注销。任何一条自检都不许读写用户的真实配置与真实仓库。
//   * 断言只打在真实入口上：QML 属性绑定、真实按钮的 clicked 信号、控制器
//     公开方法。谁在自检里复制一份实现，测出来的就只是那份复制品。
//   * 线程模型：自检自身全程在 GUI 线程（QML 对象只能在 GUI 线程访问），
//     备份 / 恢复由控制器派发到 QtConcurrent；等任务结束一律走
//     waitForIdle() / waitForCatalogIdle() 或带超时的 processEvents 循环，
//     不 sleep 猜时间，也不把事件循环占死。
//   * 几何与颜色断言前先等动画结束（WaitForAnimation）：过渡帧的位置与
//     透明度都没稳定，直接断言会得到随机失败的结论。

#include <qqml.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMap>
#include <QMetaObject>
#include <QPointF>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>

#include "app_paths.h"
#include "app_theme.h"
#include "application_instance_lock.h"
#include "archive_pipeline.h"
#include "backup_controller.h"
#include "config_manager.h"
#include "dev_harnesses.h"
#include "filter_rule_model.h"
#include "incremental_delta.h"
#include "operation_gate.h"
#include "realtime_controller.h"
#include "remote_controller.h"
#include "remote_incremental.h"
#include "schedule_controller.h"
#include "schedule_frequency.h"
#include "schedule_store.h"
#include "scheduler_lock.h"
#include "server_profile.h"
namespace {
int g_qml_warnings = 0;

// QML 的运行期问题（binding loop、类型错误、模块缺失……）都以 Qt warning 发出。
// 这里显式计数：--smoke-test / --screenshot 一旦出现 QML 警告就直接判失败，
// 免得靠人去一屏日志里翻。
// 分流之后两类信息一眼可辨：QML 问题计进 g_qml_warnings 决定退出码，
// 其它警告只打印，不会误伤。
void MessageHandler(QtMsgType type, const QMessageLogContext& context,
                    const QString& message) {
  Q_UNUSED(context);
  const bool is_warning =
      type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg;
  if (!is_warning) {
    std::fprintf(stdout, "%s\n", qPrintable(message));
    return;
  }
  // 判据只能是消息文本：Qt 把 binding loop、类型错误、模块缺失都归在 Warning，
  // 没有可用的独立类别。宁可宽进（普通警告被误计），也不能漏 —— 漏掉一条
  // binding loop 就等于这一轮自检白跑。
  const bool looks_like_qml =
      message.contains(QStringLiteral(".qml")) ||
      message.contains(QStringLiteral("QML")) ||
      message.contains(QStringLiteral("Binding loop")) ||
      message.contains(QStringLiteral("is not installed"));
  if (looks_like_qml) {
    ++g_qml_warnings;
    std::fprintf(stderr, "[qml-warning] %s\n", qPrintable(message));
  } else {
    std::fprintf(stderr, "[warning] %s\n", qPrintable(message));
  }
  if (type == QtFatalMsg) {
    std::abort();
  }
}

// 配置文件路径：正常启动由 Qt 按应用名算出 AppConfigLocation，
// 显式给了 --config-file 时用调用方指定的那一个（自动测试据此隔离真实配置）。
//
// 这里刻意不猜 HOME、不写死 ~/.config、不假设运行在 Ubuntu 上：
// 路径的来源只有 QStandardPaths 和命令行这两个。
QString ResolveConfigFilePath(const QStringList& arguments) {
  const int index = arguments.indexOf(QStringLiteral("--config-file"));
  if (index >= 0 && index + 1 < arguments.size()) {
    return arguments.at(index + 1);
  }
  // 默认位置与 backupctl 严格同源：两边走的都是 app_paths.h 里那一份实现。
  // 这样"GUI 保存的计划 CLI 读不到"在结构上就不可能发生，
  // 而不是靠两处各自记得写对同一个字符串。
  return QString::fromStdString(backupproject::DefaultConfigFilePath());
}

// 计划存储文件：默认位置同样来自 app_paths.h，--schedule-file 只用于测试隔离。
QString ResolveScheduleFilePath(const QStringList& arguments) {
  const int index = arguments.indexOf(QStringLiteral("--schedule-file"));
  if (index >= 0 && index + 1 < arguments.size()) {
    return arguments.at(index + 1);
  }
  return QString::fromStdString(backupproject::DefaultScheduleFilePath());
}

// 实时备份存储文件：默认位置同样来自 app_paths.h（backupctl realtime 读的就是
// 这一份），--realtime-file 只用于测试隔离。
QString ResolveRealtimeFilePath(const QStringList& arguments) {
  const int index = arguments.indexOf(QStringLiteral("--realtime-file"));
  if (index >= 0 && index + 1 < arguments.size()) {
    return arguments.at(index + 1);
  }
  return QString::fromStdString(backupproject::DefaultRealtimeFilePath());
}

// 它验的是桥加核心这一整条链路：先备份再恢复，任何一步失败
// 就把核心的原文错误打到 stderr 并以非 0 退出。
// --self-test 可以带 --include / --exclude：这些规则和界面点"添加"时走的是
// 同一条路径（BackupController::addFilterRule → 同一个 C++ Filter）。
// 规则非法就直接报错退出，不会跑出一个"看起来成功"的备份。
int ApplyFilterArguments(backup_modern::BackupController* controller,
                         const QStringList& arguments) {
  for (int index = 0; index < arguments.size(); ++index) {
    const QString option = arguments.at(index);
    if (option != QStringLiteral("--include") &&
        option != QStringLiteral("--exclude")) {
      continue;
    }
    if (index + 1 >= arguments.size()) {
      std::fprintf(stderr, "%s 需要一个规则参数\n", qPrintable(option));
      return 1;
    }
    const QString action = (option == QStringLiteral("--include"))
                               ? QStringLiteral("include")
                               : QStringLiteral("exclude");
    if (!controller->addFilterRule(action, arguments.at(index + 1))) {
      std::fprintf(stderr, "规则无效: %s\n",
                   qPrintable(controller->statusMessage()));
      return 1;
    }
    ++index;
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  // 这两个名字同时决定 QSettings 与 QStandardPaths 的落盘位置，
  // 所以必须在读主题、解析配置文件路径之前设置好。
  // 名字来自共享的 app_paths.h：CLI 侧的默认路径就是按这两个常量算出来的，
  // 写死字符串会让"GUI 与 CLI 严格同路径"这条约束只靠约定维持。
  QCoreApplication::setApplicationName(
      QString::fromLatin1(backupproject::kAppApplicationName));
  QCoreApplication::setOrganizationName(
      QString::fromLatin1(backupproject::kAppOrganizationName));
  // 固定用 Basic 风格：不跟随发行版的 GTK/GNOME 主题，
  // 否则同一份 QML 在不同 Linux 上会长得完全不一样。
  // 样式名必须和 QML 里 import 的 QtQuick.Controls.Basic 对得上。
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  // 开关解析保持最朴素的形式：出现即生效，顺序无关，
  // 不需要参数解析库，也不会因为多一个没见过的参数就退出。
  const QStringList arguments = QCoreApplication::arguments();
  const bool smoke_test = arguments.contains(QStringLiteral("--smoke-test"));
  const bool native_frame =
      arguments.contains(QStringLiteral("--native-frame"));
  const bool path_test = arguments.contains(QStringLiteral("--path-test"));
  const bool close_guard_test =
      arguments.contains(QStringLiteral("--close-guard-test"));
  const bool gui_contract_test =
      arguments.contains(QStringLiteral("--gui-contract-test"));
  // 三个页面"同一个普通表单输入 -> 同一条 DSL"的 parity 自检（见
  // RunFilterUxTest）。它需要真实 QML 对象，所以和 gui-contract 一样在窗口
  // 建好之后才分派，也属于自检模式（配置隔离照常生效）。
  const bool filter_ux_test =
      arguments.contains(QStringLiteral("--filter-ux-test"));
  // 共享 AppComboBox 的 hover 残留回归（见 RunComboHoverTest）。它要真的把指针
  // 移到一个 popup 行上，所以同样在窗口建好之后才分派。
  const bool combo_hover_test =
      arguments.contains(QStringLiteral("--combo-hover-test"));
  const int incremental_test_index =
      arguments.indexOf(QStringLiteral("--incremental-test"));
  const int screenshot_index =
      arguments.indexOf(QStringLiteral("--screenshot"));
  const int screenshot_remote_index =
      arguments.indexOf(QStringLiteral("--screenshot-remote"));
  const int official_acceptance_index =
      arguments.indexOf(QStringLiteral("--official-acceptance"));
  const int self_test_index = arguments.indexOf(QStringLiteral("--self-test"));
  const int repository_test_index =
      arguments.indexOf(QStringLiteral("--repository-test"));
  const int config_file_index =
      arguments.indexOf(QStringLiteral("--config-file"));
  const int schedule_file_index =
      arguments.indexOf(QStringLiteral("--schedule-file"));
  const bool realtime_test =
      arguments.contains(QStringLiteral("--realtime-test"));
  const bool remote_test = arguments.contains(QStringLiteral("--remote-test"));
  const int remote_acceptance_index =
      arguments.indexOf(QStringLiteral("--remote-acceptance"));
  const int remote_smoke_index =
      arguments.indexOf(QStringLiteral("--remote-smoke"));
  const int realtime_file_index =
      arguments.indexOf(QStringLiteral("--realtime-file"));
  const bool backup_options_test =
      arguments.contains(QStringLiteral("--backup-options-test"));
  const bool schedule_test =
      arguments.contains(QStringLiteral("--schedule-test"));
  const bool schedule_show =
      arguments.contains(QStringLiteral("--schedule-show"));
  const bool realtime_show =
      arguments.contains(QStringLiteral("--realtime-show"));
  const int preview_test_index =
      arguments.indexOf(QStringLiteral("--preview-test"));

  // 需要参数的开关：参数没跟上就是用法错误，明确说清楚并以 2 退出，
  // 而不是悄悄退化成默认行为（那会让测试以为它隔离了配置，其实没有）。
  if (config_file_index >= 0 && config_file_index + 1 >= arguments.size()) {
    std::fprintf(stderr, "--config-file 需要一个配置文件路径参数\n");
    return 2;
  }
  if (schedule_file_index >= 0 && schedule_file_index + 1 >= arguments.size()) {
    std::fprintf(stderr, "--schedule-file 需要一个计划存储文件路径参数\n");
    return 2;
  }
  if (realtime_file_index >= 0 && realtime_file_index + 1 >= arguments.size()) {
    std::fprintf(stderr, "--realtime-file 需要一个实时存储文件路径参数\n");
    return 2;
  }
  if (repository_test_index >= 0 &&
      repository_test_index + 3 >= arguments.size()) {
    std::fprintf(
        stderr,
        "--repository-test 需要三个参数: <源目录> <备份仓库> <恢复目录>\n");
    return 2;
  }
  if (self_test_index >= 0 && self_test_index + 3 >= arguments.size()) {
    std::fprintf(stderr,
                 "--self-test 需要三个参数: <源目录> <备份文件> <恢复目录>\n");
    return 2;
  }
  if (preview_test_index >= 0 && preview_test_index + 1 >= arguments.size()) {
    std::fprintf(stderr, "--preview-test 需要一个源目录参数\n");
    return 2;
  }

  // ---- 全应用单实例 ----
  //
  // 位置是硬要求：必须在构造任何 controller / 读任何持久状态之前。
  // AppTheme 的构造函数会读 QSettings，BackupController 会读 config.json，
  // ScheduleController 会读 schedule.json —— "先读一遍再发现已经有另一个
  // 实例"等于并发访问已经发生，那把锁也就白拿了。
  //
  // 锁路径与 backupctl 走同一个函数（app_paths.h），所以 GUI 与 CLI 的默认
  // 位置不可能漂移；产品只允许一个 GUI 或一个 CLI。
  std::string application_lock_path;
  std::string application_lock_error;
  backupproject::ApplicationInstanceLock application_lock;
  if (!backupproject::DefaultApplicationInstanceLockPath(
          &application_lock_path, &application_lock_error)) {
    std::fprintf(stderr, "%s\n", application_lock_error.c_str());
    return 1;
  }
  const backupproject::ApplicationInstanceStatus application_lock_status =
      application_lock.Acquire(application_lock_path, &application_lock_error);
  if (application_lock_status !=
      backupproject::ApplicationInstanceStatus::kAcquired) {
    // 第二个 GUI：明确说清楚，然后退出。绝不建第二套 controller。
    std::fprintf(stderr, "%s\n", application_lock_error.c_str());
    return application_lock_status ==
                   backupproject::ApplicationInstanceStatus::kAlreadyRunning
               ? backupproject::kApplicationAlreadyRunningExitCode
               : 1;
  }

  qInstallMessageHandler(MessageHandler);

  backup_modern::AppTheme theme;
  // 配置路径在这里定型：正常启动是 AppConfigLocation/config.json，
  // 自动测试用 --config-file 指到临时目录，绝不读写真实用户配置。
  //
  // 但"自检模式没给路径就退化成真实用户 profile"确实污染过用户配置：
  // --realtime-test 之类会把 /tmp/backup-gui-modern-XXXXXX/repository 写进
  // ~/.config/backup-project/backup-gui-modern/config.json，之后用户不带参数
  // 正常启动（例如 Demo 的 run.sh）就会看到一个早已被删掉的临时仓库。
  // 所以自检 / 抓图模式下，只要有哪条路径没被显式指定，就把它重定向到本次
  // 进程专属的临时目录，并在 stderr 说明。显式参数永远优先；正常启动
  // （没有任何自检开关）行为完全不变。
  const bool self_check_mode =
      smoke_test || path_test || close_guard_test || gui_contract_test ||
      preview_test_index >= 0 || incremental_test_index >= 0 ||
      screenshot_index >= 0 || screenshot_remote_index >= 0 ||
      self_test_index >= 0 || repository_test_index >= 0 || realtime_test ||
      backup_options_test || schedule_test || filter_ux_test ||
      combo_hover_test || remote_test || remote_acceptance_index >= 0 ||
      remote_smoke_index >= 0;
  QString config_file_path = ResolveConfigFilePath(arguments);
  QString schedule_file_path = ResolveScheduleFilePath(arguments);
  QString realtime_file_path = ResolveRealtimeFilePath(arguments);
  if (self_check_mode) {
    // static：目录必须活到进程结束（controller 全程读写这三份文件），
    // 析构时自动清理。放在 if 里是为了让正常启动根本不建临时目录。
    static QTemporaryDir self_check_profile;
    if (!self_check_profile.isValid()) {
      std::fprintf(stderr, "[self-check] 无法创建隔离配置目录\n");
      return 1;
    }
    if (config_file_index < 0) {
      config_file_path =
          self_check_profile.filePath(QStringLiteral("config.json"));
    }
    if (schedule_file_index < 0) {
      schedule_file_path =
          self_check_profile.filePath(QStringLiteral("schedule.json"));
    }
    if (realtime_file_index < 0) {
      realtime_file_path =
          self_check_profile.filePath(QStringLiteral("realtime.json"));
    }
    // 这行只是给人看的提示：stderr 被重定向时（自动化 / parity 测试）它不能
    // 抢占首行 —— 否则真实业务错误不再是第一条 stderr，观察到的错误契约就变了。
    // 提示与配置隔离行为无关，隔离本身照旧生效。
    if (::isatty(::fileno(stderr)) != 0) {
      std::fprintf(
          stderr,
          "[self-check] 隔离配置目录 %s（显式给出的存储路径仍然优先）\n",
          self_check_profile.path().toLocal8Bit().constData());
    }
  }
  // 一个进程内"同一时刻只有一个会改动持久状态的业务操作"的共享闸门。
  // 两个控制器拿到的是同一个对象：手动备份/恢复/删除/改仓库与"后台评估 +
  // 保存计划"互相排斥，由 C++ 保证，而不是靠 QML 把按钮置灰。
  backup_modern::OperationGate operation_gate;
  backup_modern::BackupController controller(config_file_path, &operation_gate);
  // 计划存储文件与配置走同一套默认位置策略（见 app_paths.h）：
  // backupctl schedule show 读到的就是这一份。路径已在上面解析并定型
  // （自检模式下未显式指定时指向隔离目录）。
  // 定时备份的桥。它自己不做任何业务判断，全部转发给共享核心；
  // 同时订阅 controller.busy，保证手动备份与计划备份不会同时写盘。
  //
  // 声明顺序不是随意的：QObject 上下文属性必须在 QML 引擎**之前**构造、
  // 在它**之后**析构。反过来（引擎先析构）会让析构期间的绑定重算拿到一个
  // 已经变成 null 的 schedule，冒出一屏 "Cannot read property of null"。
  backup_modern::ScheduleController schedule_controller(
      schedule_file_path, config_file_path, &controller, &operation_gate);
  // 删除归档之后的计划状态同步走这条直接连接，而不是信号：BackupController
  // 会在自己的删除闸门持有期内同步调用它，中间不给后台评估留窗口。
  controller.SetArchiveDeletedObserver(&schedule_controller);
  // 实时备份的桥。它与手动 / 计划共用同一个 operation_gate，共用同一个仓库，
  // 也用同一份 realtime.json（backupctl realtime 读的就是这一份）。
  // 它**不**自己取任何 application lock：GUI 主进程已经在启动时按 per-UID
  // 持有了那把锁，flock 绑在 open file description 上，再取一次只会把自己
  // 判成"另一个实例正在运行"。
  // realtime_file_path 同样已在上面解析并定型（自检模式下未显式指定时
  // 指向隔离目录）。
  // 第三个参数是 BackupController：实时控制器订阅它的 repositoryPathChanged，
  // 这样设置页把仓库从 A 改成 B 之后，watcher、overlap 校验与快照列表都会跟着
  // 换到 B，旧仓库不会再收到任何新快照（与 ScheduleController 同一种接法）。
  backup_modern::RealtimeController realtime_controller(
      realtime_file_path, config_file_path, &controller, &operation_gate);
  // 远程备份的桥。它背后是与 backupctl remote **共用**的 RemoteArchiveClient，
  // 自己不碰 socket、不碰协议帧。它**不**占用 operation_gate：那把闸门保护的是
  // "同一时刻只有一个本地仓库 writer"，而远程网络 I/O 不改动本地仓库的任何
  // 持久状态（上传只读一个用户选定的 .bak，下载写的是用户指定的新路径）。
  backup_modern::RemoteController remote_controller;
  // 三个页面的筛选规则编辑器共用同一个 presentation 组件，但每个页面有**自己**
  // 的规则列表：备份页的模型直接挂在 BackupController 上（编辑即生效），计划页
  // 与实时页是"草稿 + 保存"，所以它们的模型不带落点（构造参数为 nullptr），
  // 只在本地生成 DSL 与摘要，保存时由页面把列表交给各自的控制器。
  //
  // 语法裁决仍然只有一条路：任一模型 -> FilterRuleBuilder -> Filter::AddRule。
  backup_modern::FilterRuleModel filter_rule_model(&controller);
  backup_modern::FilterRuleModel schedule_filter_rule_model(nullptr);
  backup_modern::FilterRuleModel realtime_filter_rule_model(nullptr);

  QQmlApplicationEngine engine;
  // 用上下文属性而不是注册 QML 类型：QML 侧直接写 theme.accent /
  // controller.busy， 不需要任何 import 声明，也就不会碰到模块路径问题。
  engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
  engine.rootContext()->setContextProperty(QStringLiteral("controller"),
                                           &controller);
  engine.rootContext()->setContextProperty(QStringLiteral("filterRuleModel"),
                                           &filter_rule_model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("scheduleFilterRuleModel"), &schedule_filter_rule_model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("realtimeFilterRuleModel"), &realtime_filter_rule_model);
  engine.rootContext()->setContextProperty(QStringLiteral("schedule"),
                                           &schedule_controller);
  engine.rootContext()->setContextProperty(QStringLiteral("realtime"),
                                           &realtime_controller);
  engine.rootContext()->setContextProperty(QStringLiteral("remote"),
                                           &remote_controller);
  // 窗口用不用系统边框由 C++ 决定、QML 只读：窗口标志必须在窗口创建时定下来，
  // 之后再改会出现“已经画了一帧才换边框”的闪动。
  engine.rootContext()->setContextProperty(QStringLiteral("useNativeFrame"),
                                           native_frame);
  engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

  if (engine.rootObjects().isEmpty()) {
    std::fprintf(stderr, "QML 根对象创建失败\n");
    return 1;
  }
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  if (window == nullptr) {
    std::fprintf(stderr, "根对象不是 QQuickWindow\n");
    return 1;
  }
  // 先把窗口显示出来再往下走：--self-test 与 --screenshot 都依赖一个
  // 已经建好的窗口，提前返回会让这两条路径测的不是真实情况。
  window->show();
  WaitForAnimation(200);

  // 页面建好之后再启动计划：此时 QML 已经绑好了 schedule 的属性，
  // 第一步读盘的结果能直接反映到界面上。
  //
  // 自检模式刻意不自动启动 runner：自检要自己控制每一步（从空 store 开始、
  // 手动触发评估），自动 tick 会和它抢同一份状态。
  if (remote_acceptance_index >= 0) {
    if (remote_acceptance_index + 4 >= arguments.size()) {
      std::fprintf(stderr,
                   "--remote-acceptance 需要 <输出目录> <地址> <端口> <用户名>"
                   "（口令走 BACKUP_REMOTE_PASSWORD、指纹走 "
                   "BACKUP_REMOTE_PIN）\n");
      return 2;
    }
    return RunRemoteAcceptance(window, &remote_controller, &controller, &theme,
                               arguments.at(remote_acceptance_index + 1),
                               arguments.at(remote_acceptance_index + 2),
                               arguments.at(remote_acceptance_index + 3),
                               arguments.at(remote_acceptance_index + 4));
  }
  if (remote_test) {
    return RunRemoteTest(window, &remote_controller, &controller, &theme,
                         config_file_path, schedule_file_path,
                         realtime_file_path);
  }
  if (remote_smoke_index >= 0) {
    if (remote_smoke_index + 3 >= arguments.size()) {
      std::fprintf(stderr, "--remote-smoke 需要 <地址> <端口> <用户名>\n");
      return 2;
    }
    // 口令只从环境变量读：argv 对同机用户可见，不能用来传口令。
    const QString smoke_password =
        qEnvironmentVariable("BACKUP_REMOTE_PASSWORD");
    if (smoke_password.isEmpty()) {
      std::fprintf(stderr,
                   "--remote-smoke 需要环境变量 BACKUP_REMOTE_PASSWORD"
                   "（口令不进 argv）\n");
      return 2;
    }
    return RunRemoteSmoke(&remote_controller, &controller,
                          arguments.at(remote_smoke_index + 1),
                          arguments.at(remote_smoke_index + 2),
                          arguments.at(remote_smoke_index + 3), smoke_password);
  }
  if (schedule_test) {
    return RunScheduleTest(&schedule_controller, &controller, config_file_path);
  }
  if (schedule_show) {
    return RunScheduleShow(&schedule_controller);
  }
  // 实时自检与计划自检一样：自己控制每一步（从空 store 开始、手动启用、
  // 手动等快照），所以不自动 start。
  if (realtime_show) {
    return RunRealtimeShow(&realtime_controller);
  }
  if (realtime_test) {
    return RunRealtimeTest(&realtime_controller, &operation_gate, &controller,
                           config_file_path);
  }
  if (preview_test_index >= 0) {
    return RunPreviewTest(&filter_rule_model,
                          arguments.at(preview_test_index + 1), arguments);
  }
  schedule_controller.start();
  // 实时备份：读同一份 realtime.json，enabled 时 attach + 合成 resync。
  // 与计划一样，自检模式不会走到这里。
  realtime_controller.start();

  if (self_test_index >= 0) {
    const int filter_status = ApplyFilterArguments(&controller, arguments);
    if (filter_status != 0) {
      return filter_status;
    }
    return RunSelfTest(&controller, arguments.at(self_test_index + 1),
                       arguments.at(self_test_index + 2),
                       arguments.at(self_test_index + 3));
  }

  if (repository_test_index >= 0) {
    return RunRepositoryTest(window, &controller,
                             arguments.at(repository_test_index + 1),
                             arguments.at(repository_test_index + 2),
                             arguments.at(repository_test_index + 3));
  }

  if (incremental_test_index >= 0) {
    if (incremental_test_index + 2 >= arguments.size()) {
      std::fprintf(stderr,
                   "--incremental-test needs <source> and <repository>\n");
      return 2;
    }
    return RunIncrementalTest(&controller,
                              arguments.at(incremental_test_index + 1),
                              arguments.at(incremental_test_index + 2));
  }

  if (backup_options_test) {
    return RunBackupOptionsTest(&controller, config_file_path);
  }

  if (official_acceptance_index >= 0) {
    if (official_acceptance_index + 2 >= arguments.size()) {
      std::fprintf(
          stderr,
          "--official-acceptance 需要 <用户名> 与 <输出 PNG> 两个参数\n");
      return 2;
    }
    return RunOfficialAcceptance(window, &remote_controller,
                                 arguments.at(official_acceptance_index + 1),
                                 arguments.at(official_acceptance_index + 2));
  }
  if (screenshot_remote_index >= 0) {
    if (screenshot_remote_index + 1 >= arguments.size()) {
      std::fprintf(stderr, "--screenshot-remote 需要一个输出目录参数\n");
      return 2;
    }
    return RunRemoteScreenshot(window, &remote_controller, &theme,
                               arguments.at(screenshot_remote_index + 1));
  }
  if (screenshot_index >= 0) {
    if (screenshot_index + 1 >= arguments.size()) {
      std::fprintf(stderr, "--screenshot 需要一个输出目录参数\n");
      return 2;
    }
    const int result =
        CaptureScreenshots(window, &theme, &controller, &remote_controller,
                           arguments.at(screenshot_index + 1));
    if (result != 0) {
      return result;
    }
    return g_qml_warnings == 0 ? 0 : 1;
  }

  if (path_test) {
    return RunPathTest(&controller);
  }

  if (close_guard_test) {
    // 三位 writer 都要交给它：只传手动那一位，测不出"实时/计划在写盘时关窗"。
    return RunCloseGuardTest(window, &controller, &schedule_controller,
                             &realtime_controller);
  }

  if (gui_contract_test) {
    return RunGuiContractTest(window, &controller);
  }

  if (combo_hover_test) {
    return RunComboHoverTest(window, &theme);
  }

  if (filter_ux_test) {
    return RunFilterUxTest(window, &filter_rule_model,
                           &schedule_filter_rule_model,
                           &realtime_filter_rule_model);
  }

  if (smoke_test) {
    // 六个页面都要真的被实例化并切换一次，两套主题也都要切到。
    // 只把 kPageCount 加一而不真正切页，等于根本没有验证新页面。
    for (int page = 0; page < kPageCount; ++page) {
      QTimer::singleShot(120 + page * 90, &app, [window, page]() {
        window->setProperty("currentPage", page);
      });
    }
    const int after_pages = 120 + kPageCount * 90;

    // 再把窗口缩到最小尺寸走一遍：窄窗口下的裁切、绑定循环、隐式高度为 0
    // 都会以 QML 运行期警告的形式暴露出来，而 --smoke-test 把警告算成失败。
    QTimer::singleShot(after_pages, &app, [window]() {
      window->setWidth(960);
      window->setHeight(620);
    });
    for (int page = 0; page < kPageCount; ++page) {
      QTimer::singleShot(after_pages + 80 + page * 70, &app, [window, page]() {
        window->setProperty("currentPage", page);
      });
    }
    const int after_narrow = after_pages + 80 + kPageCount * 70;
    QTimer::singleShot(after_narrow, &app, [window]() {
      window->setWidth(1180);
      window->setHeight(760);
    });
    QTimer::singleShot(after_narrow + 80, &app, [&theme]() { theme.toggle(); });
    QTimer::singleShot(after_narrow + 220, &app,
                       [&theme]() { theme.toggle(); });
    QTimer::singleShot(after_narrow + 360, &app, &QCoreApplication::quit);
  }

  const int exit_code = app.exec();
  if (exit_code != 0) {
    return exit_code;
  }
  // smoke / screenshot 之外的正常退出也报告 QML 警告数量，便于脚本判断。
  return g_qml_warnings == 0 ? 0 : 1;
}

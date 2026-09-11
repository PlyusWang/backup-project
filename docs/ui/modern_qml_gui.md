# 现代桌面 GUI（Qt Quick / QML）

> 状态：与 Qt 6 Widgets 版**并行存在**的第二套界面，随 `feature/modern-qml-gui` 分支引入。
> Widgets 版继续保留、继续可构建，两套之间没有替代关系。

## 1. 为什么再加一套，而不是把 Widgets 版改掉

- 两套界面解决的是不同问题：Widgets 版证明“核心能被 C++ 直接驱动”，
  这一版验证同一份核心换到 Qt Quick 之后是否同样成立，并给出更接近现代桌面工具的观感。
- 两套各自独立成目标：`make gui` 与 `make gui-modern` 互不依赖，
  任何一套坏掉都不影响另一套；`make` 仍然只构建 CLI。
- 界面层重写，核心一行没动：`src/`、`include/`、`app/` 全部保持原样。

## 2. 技术栈

Qt 6.4.2 + Qt Quick + QML + Qt Quick Controls 2（Basic 样式）+ Qt Concurrent，
界面逻辑用少量 JS（绑定与槽函数），C++ 侧只有两个 QObject 桥接类，
文件读写全部复用现有的 C++17 `BackupEngine` / `FileSystem`。

没有引入 Electron、WebView、Python/PySide、GTK，也没有使用任何 Qt 6.5 之后才有的 API：
目标机是 Ubuntu 24.04 自带的 Qt 6.4.2，用到 6.5 的写法当场就编不过。

## 3. 文件职责

| 文件 | 职责 |
| --- | --- |
| `ui/modern/main.cpp` | 入口；QML 引擎、上下文属性，以及 `--smoke-test` / `--screenshot` / `--self-test` / `--native-frame` 四个开关 |
| `ui/modern/app_theme.h/.cpp` | `AppTheme`（QObject）：两套调色板 + `QSettings` 记忆，用 Q_PROPERTY 暴露给 QML |
| `ui/modern/backup_controller.h/.cpp` | `BackupController`（QObject）：调用核心、跑线程、维护 busy 与状态文案 |
| `ui/modern/resources.qrc` | 把 QML 打进二进制（rcc），运行时不依赖源码目录 |
| `ui/modern/qml/Main.qml` | 窗口骨架：自绘标题栏、侧栏导航、`StackLayout` 三页、主题切换 |
| `ui/modern/qml/pages/*.qml` | 首页与操作页（备份 / 恢复共用一个 `OperationPage.qml`） |
| `ui/modern/qml/components/*.qml` | 按钮、卡片、图标、输入框、导航项、状态条六个基础件 |

## 4. QML 与 C++ 的接线方式

`main.cpp` 只把两个对象挂成**上下文属性**：

```cpp
engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
engine.rootContext()->setContextProperty(QStringLiteral("controller"), &controller);
```

QML 里直接写 `theme.accent` / `controller.busy`，不需要 import 任何自定义模块，
也就不需要维护 qmldir 与单例注册。这个选择是有代价的：qmllint 无法知道
`theme`、`controller` 是什么，会把每一处引用都报成 “Unqualified access”。
在 6.4.2 上，qmldir 单例方案一旦写错，症状是“类型不可用”，排查成本更高；
所以这里选了上下文属性，代价是 lint 噪音（处理方式见第 9 节）。

路径参数一律以字符串进出。核心通过 `std::string* error_message` 返回的错误原文
直接显示在界面上，界面不做二次包装、也不吞掉。

## 5. 线程模型：同一时刻只可能有一个任务

- 只有一个 `QFutureWatcher<OperationOutcome>` 成员，任务一律经 `QtConcurrent::run` 提交，
  结构上就不存在“两个任务同时在跑”的情况，不需要额外的锁或任务队列。
- 忙的时候：两个文本框、两个“浏览”按钮、主操作按钮共 **5 处**一起禁用，
  `scripts/modern_gui_check.sh` 会断言这个数字，防止以后改界面时漏掉某处。
- 空路径这类守卫在启动线程之前就返回，不会往线程池里塞一个注定失败的任务。

## 6. 进度：只有不确定动画

核心目前没有进度回调，所以运行中显示的是一条来回移动的指示条（`NumberAnimation on x`），
**不给百分比、不给预计剩余时间**。宁可少显示，也不显示编出来的数字。

## 7. 主题与视觉约束

- 两套调色板集中在 `app_theme.cpp`，QML 里不写死色值，一律引用 `theme.xxx`；
- 用户选择存进 `QSettings`（键 `appearance/modern_dark`），下次启动沿用；
- 图标是 `AppIcon.qml` 里用 Canvas 手绘的线条，不引入图标包、不放 SVG 文件、
  不用 emoji：颜色能跟着主题绑定走，也不增加外部资源；
- 不打包字体，用系统字体；没有 Logo、没有品牌元素；
- 用 `QQuickStyle::setStyle("Basic")` 固定控件样式，避免发行版把 GTK/GNOME 主题
  套到控件上，出现和 Widgets 版互相污染、或“看起来像系统设置面板”的效果；
- 卡片不做阴影：6.4.2 上没有稳定的轻量阴影实现，层次交给底色明度差 + 1px 边框 + 留白。

## 8. 窗口与 Wayland

窗口默认 1180×760，最小 960×620，自绘 40px 标题栏 + 228px 侧栏 + 内容区。
标题栏是无边框窗口，拖动请求交给合成器（`startSystemMove()`），
边缘缩放走 `startSystemResize()`，最小化 / 最大化 / 关闭三个按钮齐全，
双击标题栏切换最大化；最大化按钮的图标会随窗口状态切换。

开发机是 GNOME **Wayland** 会话（没有 XWayland 授权），实测可直接启动。
`--native-frame` 是兜底开关：万一某个合成器上无边框窗口表现不稳，
可以退回系统原生标题栏，界面其余部分不变。

需要人工确认的部分：拖拽移动与边缘缩放的**指针手势**没法用脚本验证，
必须真的动鼠标。程序化能验证的是“窗口能创建、能切页、能换主题、没有 QML 告警”，
这部分已经覆盖在 `scripts/modern_gui_check.sh` 里。

## 9. 已知限制

- 没有真实的进度百分比（核心没有回调），也没有取消功能；
- 界面只放现在真能用的东西：备份、恢复、主题切换、状态与错误显示；
  Archive / 压缩 / 加密 / 过滤 / 定时 / 实时 / 网络 / 历史记录都不在界面上出现，
  连占位控件也不放；
- qmllint 6.4.2 会把上下文属性引用报成 “Unqualified access”，
  把 `easing.type`、`Qt.AlignTop` 报成解析失败。这些是工具在 6.4 上的局限，
  所以 `scripts/modern_gui_check.sh` 只把**语法错误**和**未使用导入**当失败，
  其余只统计条数并写进日志；
- 自绘标题栏在极少数合成器上可能有拖拽 / 缩放不跟手的情况，用 `--native-frame` 规避。

## 10. 怎么构建、怎么验证

```bash
# 依赖（Ubuntu 24.04）
sudo apt-get install -y qt6-declarative-dev qt6-declarative-dev-tools \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-dialogs qml6-module-qtquick-window qml6-module-qtqml-workerscript

make gui-modern                    # 构建 build/backup-gui-modern
make gui-all                       # 两套 GUI 一起构建（共存，不互相覆盖）
./build/backup-gui-modern          # 正常启动

# 开发期开关
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software ./build/backup-gui-modern --smoke-test
./build/backup-gui-modern --screenshot tests/output/preview    # 3 页 × 2 主题共 6 张 PNG
./build/backup-gui-modern --self-test <源目录> <备份仓库> <恢复目录>
./build/backup-gui-modern --native-frame                      # 退回原生标题栏

./scripts/modern_gui_check.sh      # 构建 + 启动 + qmllint + 静态约束 + 端到端
./scripts/lint.sh                  # C++ 格式检查（含 ui/modern）
```

`--smoke-test` 与 `--screenshot` 会把 QML 运行期告警计入退出码：
只要出现一条 QML 告警就以非 0 退出，避免“界面看着正常、日志里其实在报错”。

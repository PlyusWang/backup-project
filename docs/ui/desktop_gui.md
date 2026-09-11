# 桌面 GUI（Qt 6 Widgets）

> 状态：第一版实现，随 `feature/ubuntu-desktop-gui` 分支引入。

## 1. 为什么第一版选 Qt 6 Widgets

项目核心本来就是 C++17，GUI 直接用 C++ 调 `BackupEngine` 最省事：

- 不用为界面再引入一套运行时（浏览器、Node、QML 引擎都不需要）；
- 启动快、内存占用可控，虚拟机和机房里都跑得动；
- Linux 支持成熟，系统自带 Qt 6 运行库，部署简单；
- Widgets 默认样式偏朴素，但布局和外观可以用 QSS 自己做，第一版够用。

因此没有选 Qt Quick / QML、GTK、Electron、WebView 这些方案。

## 2. GUI 与核心的调用关系

```text
backupctl ──┐
            ├── BackupEngine ── FileSystem
backup-gui ─┘
```

GUI 和 CLI 是两个并列入口，共用同一份核心：

- GUI 直接链接 `src/core/backup_engine.cpp` 与 `src/filesystem/file_system.cpp`；
- 界面里没有复制任何文件拷贝逻辑，也没有用 `QProcess` 去调 `backupctl`；
- 错误信息直接用 `BackupEngine` 通过 `std::string* error_message` 返回的原文。

## 3. 页面

左侧导航两个入口，右侧是内容区：

- **备份**：源目录 + 备份仓库 → 开始备份；
- **恢复**：备份仓库 + 恢复目录 → 开始恢复。

两个页面各有两条路径输入框。输入框始终可编辑，因为备份仓库允许是一个**尚不存在**的路径，
而系统的目录选择对话框只能方便地选已存在的目录；不能让对话框把核心本来就支持的能力限制住。

第一版界面上只有真正能用的东西，没有 Archive、压缩、加密、过滤、定时、实时、网络等占位控件。

## 4. 主题

- 提供 Light / Dark 两套完整主题，侧栏底部一键切换；
- 颜色、圆角、边框等集中定义在 `ui/desktop/theme.cpp`，页面里不写死色值；
- 选择通过 `QSettings` 保存（键 `appearance/theme`），下次启动沿用上次的主题；
- 不打包字体，使用系统字体。

## 5. 异步执行

文件复制可能持续很久，所以 GUI 不在主线程里调 `BackupEngine`：

- 用 `QtConcurrent::run` 把任务丢进线程池，主线程只等 `QFutureWatcher` 的 `finished` 信号；
- 任务运行期间禁用输入框和开始按钮，同一时刻只允许一个备份或恢复任务；
- 后台函数不接触任何 QWidget；回调以页面对象作为上下文，页面销毁后 Qt 会自动断开连接。

## 6. 进度显示

核心目前没有进度回调，所以运行中使用的是**不确定进度**（`QProgressBar` 的 `range` 设为 `0, 0`），
表示“正在执行，但无法知道真实百分比”。

第一版不做假的百分比。等核心提供真实进度接口之后，再改成精确进度。

## 7. 当前功能边界

这一版只暴露现在就真实可用的功能：备份、恢复、Light/Dark 主题、操作状态与错误显示。

还没有实现：Archive / 打包、压缩、加密、文件过滤、定时备份、实时备份、网络备份、历史记录。

## 8. 后续主题扩展点

`ThemeColors` 已经把颜色按用途拆开：窗口背景、侧栏背景、卡片背景、输入框背景、
主文字、次要文字、边框、强调色、强调色 hover、错误色、成功色。

后续要加 Accent Color 或自定义主题时，只需要：

1. 增加一个 preset 构造函数，或允许用户修改 `accent` 字段；
2. 把用户选择存进 `QSettings`（与 `appearance/theme` 同级）。

QSS 生成和页面代码都不用改。

## 附：构建与自检

```bash
make gui                                                     # 构建 build/backup-gui
./build/backup-gui                                           # 启动图形界面
QT_QPA_PLATFORM=offscreen ./build/backup-gui --smoke-test    # 无显示环境自检
./scripts/gui_smoke_test.sh                                  # 构建 + 自检一步完成
```

依赖：Qt 6 开发包（Ubuntu：`sudo apt-get install -y qt6-base-dev qt6-base-dev-tools`）。

`make` 仍然只构建 CLI；GUI 是独立的 `make gui` 目标。

# 桌面 GUI（Qt 6 Widgets，legacy）

> 状态：第一版 GUI，随 `feature/ubuntu-desktop-gui` 分支引入，现在只作为
> **回归 / 参考前端**保留，不是正式发布 GUI。正式前端是 Modern Qt 6 QML GUI
> （`backup-gui-modern`，见 `docs/ui/modern_qml_gui.md`），发行包里只有它。

## 1. 为什么第一版选 Qt 6 Widgets

项目核心本来就是 C++17，GUI 直接用 C++ 调 `BackupEngine` 最省事：

- 不用为界面再引入一套运行时（浏览器、Node、QML 引擎都不需要）；
- 启动快、内存占用可控，虚拟机和机房里都跑得动；
- Linux 支持成熟，系统自带 Qt 6 运行库，部署简单；
- Widgets 默认样式偏朴素，但布局和外观可以用 QSS 自己做，第一版够用。

因此第一版没有选 Qt Quick / QML、GTK、Electron、WebView 这些方案。
（正式 GUI 后来改用 Qt Quick / QML，见 `docs/ui/modern_qml_gui.md`；这一版
现在的定位见第 7 节。）

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

- **备份**：源目录 + 备份文件 → 开始备份（备份文件就是归档文件的完整路径，由用户自己填）；
- **恢复**：备份文件 + 恢复目录 → 开始恢复。

两个页面各有两条路径输入框。输入框始终可编辑：备份时"存成哪个归档文件"可以直接
手写一个尚不存在的路径，对话框只是帮你挑一个位置；路径合不合法仍然由核心判断，
GUI 不复制一套校验规则。

这一版界面上只有两个页面，外加备份页上一个纯文本的筛选规则列表；没有打包 /
压缩 / 加密选项、定时、实时、网络、历史记录等入口——这些能力现在由 Modern GUI
与 `backupctl` 提供，不在这里补。

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

## 7. 当前功能边界与技术差异

这一版只有**备份 / 恢复**两个页面、Light/Dark 主题、操作状态与错误显示，备份页上
另有一个纯文本的筛选规则列表（"加为 Include" / "加为 Exclude"，语法仍由核心
`Filter` 校验）。它没有打包 / 压缩 / 加密选项，没有定时 / 实时 / 网络备份，也没有
运行历史，更没有 Modern GUI 的可视化规则编辑器与筛选预览。

它和产品当前写法的差异是结构性的，所以**不要拿它演示**这些能力：

- 备份调的是 `BackupEngine::Backup(source, archive_file, filter, ...)` 这个不带
  `BackupOptions` 的重载，也就是 legacy v0.1 写入路径：产物是 `BKPARCH`，不是
  产品当前的 `BKPCNT2` v2 容器——压缩与加密在这里无从演示；
- v0.1 只保存 mode（0777 位）与 mtime，不含 uid / gid，所以元数据也不该用它演示；
- 归档路径由用户在界面上自己填，而产品模型是"归档写进配置好的仓库、文件名由程序
  生成"；"备份写到哪就是哪"这条路径在 Modern GUI 与 `backupctl` 里都不存在；
- 恢复走的是同一个不带密码参数的旧重载：未加密的 v2 容器按 magic 仍然认得出来，
  加密容器在这里会明确失败（而不是被当成坏文件）。

这一版现在有两个用途：一是历史参考，二是单实例锁的第三个前端回归——它和
Modern GUI、`backupctl` 抢同一把按 Unix UID 定位的应用锁，
`scripts/scheduled_backup_test.sh` 的 K.05..K.07 就断言这三个前端互斥。
演示、验收以及"GUI 与 CLI 功能一致"这条产品义务都属于 Modern GUI。

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

`make` 仍然只构建 CLI；这一版是独立的 `make gui` 目标。发行打包
（`make client`、`scripts/stage-client-release.sh`）只带 `backupctl` 与
`backup-gui-modern`，不会带上它。

# Modern GUI 截图

这 8 张是 Modern Qt 6 QML GUI 的正式截图，供 README、课程文档与答辩材料引用。

| 文件 | 界面 |
| --- | --- |
| `01-home.png` | 首页（三张导航卡） |
| `02-backup.png` | 手动备份页 |
| `03-backup-advanced.png` | 手动备份页 · 高级选项展开（策略 / 打包 / 压缩 / 加密） |
| `04-backup-management.png` | 备份管理页（归档列表与恢复入口） |
| `05-schedule.png` | 自动备份页 |
| `06-realtime.png` | 实时备份页 |
| `07-remote.png` | 远程备份页 |
| `11-settings.png` | 设置页 |

## 来源与再生成

截图由 GUI 自己的截图模式产出，不是手画的示意图，和用户看到的是同一条渲染路径：

```bash
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software \
  ./build/backup-gui-modern --screenshot <输出目录> --config-file <配置>
```

实际驱动它的是 `scripts/modern_gui_check.sh`：它会准备两份临时仓库（一份只含未加密 v2
记录、一份含加密记录），各跑一轮截图，并对产出的文件名做断言。完整产物包含七页 × 两套主题，
外加高级选项展开、三种服务器身份模式等状态，落在 `tests/output/screenshots/`——
该目录被 `.gitignore` 排除（属于测试产物），所以这里只挑正式引用的几张放进仓库。

再生成：

```bash
make gui-modern
bash scripts/modern_gui_check.sh      # 会重新生成 tests/output/screenshots/
cp tests/output/screenshots/plain/{home,backup,backup-expanded,management,schedule,\
realtime,remote,settings}-light.png docs/images/gui/
```

## 服务器身份三种模式

`--screenshot` 还会按服务器身份的三种模式各抓一张（`official-cloud-*.png`、
`advanced-ssh-mode-*.png`、`custom-server-profile-*.png`）。这三张留在测试产物目录里，
没有进本目录 —— 理由是这里的 8 张已经够文档用，不是因为它们有问题。

（2026-10-07 修）此前这三张会与 `remote-*.png` **逐字节相同**：模式切换是控制器上的
属性变更，页面只在构造时回填一次，运行时不再跟随；截图 harness 又是"设完模式立刻抓帧"，
所以抓到的全是切换前的画面，而断言只看文件存在与大小非空，一直没有拦住。
现在两侧都修好了：页面跟随 `connectionChanged` 更新模式显示，harness 改为等到界面上的
模式真的切过去（读 `remoteConnectionModeTabs` 的 `currentKey`）并用该模式独有的控件作证，
最后按主题比对三张图的 SHA-256，相同即判失败。同一次运行里浅色与深色主题下三张图
互不相同。

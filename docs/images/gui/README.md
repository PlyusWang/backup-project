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

## 已知限制（如实记录，未修）

`--screenshot` 会按"服务器身份三种模式"各抓一张（`official-cloud-*.png`、
`advanced-ssh-mode-*.png`、`custom-server-profile-*.png`）。在 2026-10-07 的这次运行里，
同主题下的这四张（含 `remote-*.png`）**逐字节相同**，也就是说切换连接模式之后画面并没有
真正变化就被抓帧了。测试脚本只断言这些文件存在与大小非空，不做像素比对，所以没有拦住它。

因此本目录只收录确定互不相同的 8 张，不把那几张同图放进文档冒充"不同模式"。
根因在截图 harness 的等待时机（`ui/modern/dev_harnesses.cpp` 的截图段），属于测试工具问题，
不是界面问题；产品代码已冻结，本轮未修改。

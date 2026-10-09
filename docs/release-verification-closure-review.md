# v0.1.1 发行验收收尾：审查与修复（release verification closure）

本文是对 **v0.1.1**（`3b90bb057022cbc2e8cfd758d2421c1baf17124a`）发行验收链条的
限定范围收尾：核查上一轮遗留的 4 项问题、补上缺陷检出型负向测试、做一次静态审查，
**不创建任何 GitHub Release / tag**（本轮只到 PR 为止）。

- 基线：`3b90bb057022cbc2e8cfd758d2421c1baf17124a`（v0.1.1 的 merge commit）
- 分支：`fix/release-ci-audit-closure`
- 审查范围：`packaging/` 全部脚本（21 个）+ `.github/workflows/release.yml` + 相关发行文档
- **不改产品核心**：C++ 源码、GUI 业务、加密协议、归档协议、用户数据格式一律未动
  （diff 只落在打包 / 验收脚本与文档）。

## 1. 四项遗留问题

### A（P2）许可检查在缺少核心 Qt 库时判断错误 —— 已修

旧代码：

```bash
libdir="$(dirname "$(find "$root" -name 'libQt6Core.so.6' -print -quit 2>/dev/null || true)")"
if [ ! -d "$libdir" ]; then ci_fail "..."; return; fi
```

`find` 无结果时 `dirname ""` 返回 `.`，而 `.` 永远存在 —— 失败分支形同
虚设（实测：`[ -d "." ]` = true）。更糟的是 `objectroot="$(dirname "$libdir")"`
会变成**当前工作目录的父目录**，后面的双向核对会去扫制品以外的目录。

修复（`packaging/ci-license-coverage.sh`）：先取 `find` 的原始结果 → 判空 →
判 `-f`（普通文件）与 `-L`（断链）分别给出不同原因 → `readlink -f` 解析真实路径 →
要求它落在**本次解包的根目录**之内 → 再取父目录，并要求父目录同样在根内
（注意 tar 情形下"对象根"就等于传入的根，边界要包含等号）。

### B（P2）两套私钥扫描规则不一致 —— 已修（并发现一条**从未生效**的规则）

现状核对：构建期 `ci-artifact-selfscan.sh` 只查 `*.key` / `secrets.env` +
内容规则；CI 的 `ci-secret-scan.sh` 还查 `*.bpcert` 与"32 字节裸密钥形状"，且
**内容规则在两处各写了一份**（`lib/common.sh` 与 `ci-secret-scan.sh` 内联）。

统一做法：规则集中到 `packaging/lib/common.sh` 的三个函数
（`secret_scan_content_files` / `secret_scan_key_files` /
`secret_scan_raw_key_files`），两个入口都调用同一份实现，只保留各自的日志与返回码
风格（CI 版仍是 `expect_eq` + `ci_finish`，构建期仍是 `log` + `die`）。

**顺带发现的 P1 级缺陷（本轮修复）**：内容规则的模式串以 `-----BEGIN` 开头，
`grep -rEl "$PATTERNS" "$root"` 会把它当成**命令行选项**（GNU grep 直接退出 2），
而这个错误被 `2>/dev/null` + `|| true` 吞掉 —— 于是"PEM / seed-hex / token 行"
这三条规则**在 v0.1.1 及更早版本里从未真正生效过**。实测复现：

```text
$ grep -rEl "$PAT" "$D"            # 不带 --
grep: 未识别的选项 "-----BEGIN [A-Z ]*PRIVATE KEY|..."
exit=2   （被 2>/dev/null 吞掉 -> 永远 0 命中）
$ grep -rIlE -- "$PAT" "$D"        # 带 -- 且只看文本
exit=0  命中 3 个文件
```

修复要点：
1. 加 `--`（必须，否则整条规则是死的）；
2. 同时加 `-I`：**只看文本文件**。这不是"放宽"，而是必要的判据 —— 随包的 Qt TLS
   后端（`lib/libQt6Network.so.6`、`plugins/tls/libqopensslbackend.so`）里含有
   `"-----BEGIN ... PRIVATE KEY"` 这样的**字符串常量**，按二进制匹配会在每个合法
   发行包上误报。合成数据把两个方向都钉住了（文本 PEM 必拦、真品必过）。
3. 32 字节形状规则：保留，并把排除目录（`/usr/share/ /docs/ /qml/ /lib/`）的
   工程依据写进注释 —— 明确写清它是一条**形状启发式**，不是"32 字节文件必然是密钥"
   的断言；排除目录内外的行为都有测试。

### C（P3）发行制品完整性缺少统一检查 —— 已修

旧状态：各检查脚本都是"遍历目录里现有的文件"，缺一件制品时循环直接跳过、整体照样
PASS（实测：把一个缺少 AppImage 的目录喂给旧 `ci-secret-scan.sh` → exit 0）。

新增 `packaging/ci-release-manifest.sh <目录> [--family all|client|server] [--expect-version V] [--expect-commit SHA]`：
从**应有清单**出发逐项核对

- 家族清单：client = AppImage + deb + tar，server = deb + tar，all = 五件；
- 命名 / 版本 / 平台：文件名里必须带期望版本与 `x86_64`/`amd64`；deb 的 `Version`
  与 `Architecture` 用 `dpkg-deb -f` 实际读取核对；
- 唯一性：同类文件必须**恰好 1 个**（重复制品 / 旧版本残留都判失败）；
- 未预期文件：不在正式发行包五件之列的任何 `*.deb|*.AppImage|*.tar.xz` 判失败；
- `SHA256SUMS`：必须列出本族全部制品（含 `RELEASE-INFO.txt`）、不得列入发行包之外的
  文件、并**逐个重新计算哈希**比对；
- `RELEASE-INFO.txt`：version 与期望一致、architecture 含 amd64、commit 是完整 40 位
  SHA（给了 `--expect-commit` 时还必须相等）。

**兼容性**：`--family` 只要求**本族**齐备，另一族的制品允许存在 —— 这样
`build-release.sh --only client|server` 的中间阶段不会被误伤；`--family all` 用于
最终 bundle 阶段（严格全检）。`build-release.sh` 末尾已经用 `--family "$ONLY"` 调它，
workflow 的 bundle 作业也用 `--family all` 带上 `github.sha` 调它 —— 不是"只加脚本
从不执行"。

顺带对齐了一处不一致：构建期生成的 `SHA256SUMS` 以前**排除** `RELEASE-INFO.txt`，
而 CI bundle 阶段重算时**包含**它。现在两处一致（都包含），检查器据此做严格集合比对。

### D（P3）AppImage BUILD-INFO 核对位置错误 —— 已修

实测五种制品的真实位置（**三种不同布局**）：

| 制品 | BUILD-INFO.txt | VERSION |
| --- | --- | --- |
| AppImage | `usr/share/backup-project/BUILD-INFO.txt` | `usr/share/backup-project/VERSION` |
| client .deb | `usr/share/doc/backup-project-client/BUILD-INFO.txt` | `usr/lib/backup-project-client/share/backup-project/VERSION` |
| client tar.xz | `BUILD-INFO.txt`（树根） | `share/backup-project/VERSION` |
| server .deb | `usr/share/doc/backup-project-server/BUILD-INFO.txt` | `usr/lib/backup-project-server/share/backup-project/VERSION` |
| server tar.xz | `BUILD-INFO.txt`（树根） | `share/backup-project/VERSION` |

上一轮的**仓库外验收命令**误用了 deb 的路径去找 AppImage，得到"找不到"却没有据此失败
—— 错误只存在于临时命令里，**产品打包逻辑本身没有问题**，因此不改打包层，只把正确
路径写进长期有效的自动化检查（`ci-artifact-selfscan.sh` 现在逐产品族核对
BUILD-INFO/VERSION 的存在与内容，并支持 `--expect-commit/--expect-version`）。本轮
**没有**修改任何旧证据文件；错误的核验记录以本文更正。

## 2. 发现的其他问题

| 编号 | 等级 | 位置 | 问题 | 处置 |
| --- | --- | --- | --- | --- |
| X1 | **P1** | `lib/common.sh` 内容规则 | 模式串以 `-` 开头被 grep 当选项 → PEM/seed/token 规则从未生效 | 已修（`--` + `-I`），负向+正向双向测试 |
| X2 | P3 | `build-release.sh` | `rm -rf "$tree/bin" "$tree/etc"`：`$tree` 为空时会变成 `rm -rf /bin /etc`（ShellCheck SC2115） | 已修（`tree:?` 守卫） |
| X3 | P3 | `build-release.sh` 生成的 `SHA256SUMS` | 与 CI bundle 阶段的集合定义不一致（是否含 `RELEASE-INFO.txt`） | 已修（统一为包含） |
| X4 | P3 | `build-release.sh --skip-appimage` | 该开发用开关会让 AppDir 没有 `usr/lib`，材料生成器因此失败（上一轮加"0 个对象即失败"守卫后暴露；更早版本同样会因"找不到随包库目录"失败） | **未修**：CI 与文档都不用这个开关；如要保留该路径需另立范围 |
| X6 | P3 | 本轮新增的 `ci-release-manifest.sh` | 用原始版本号拼 `.deb` 文件名；带短横线的预发布版本在 deb 里写成 `~`，于是自报假失败 | 已修（读 `RELEASE-INFO.txt` 的 `deb_version`），由本地 `--only server --version 0.1.1-local` 实测暴露 |
| X7 | P3 | 同上 | `comm` 在环境 locale 下运行、输入却是 `LC_ALL=C` 排的序，制品一多就报“文件没有被正确排序” | 已修（四处 `comm` 显式加 `LC_ALL=C`） |
| X8 | P3 | 同上 + `build-release.sh` | 单族目录生成 `SHA256SUMS` 时 `RELEASE-INFO.txt` 还没写出来，所以那份清单不含它；bundle 阶段重算时含它 | 已修（检查器要求“本族制品必须全部在清单里”，`RELEASE-INFO.txt` 两种布局都接受） |
| X5 | P3 | `build-release.sh:92/264/365/530` 等 | ShellCheck 的 SC1083（`HEAD^{tree}` 是合法 git 语法）、SC2094（清单生成已排除清单自身）、SC2015（`A && B \|\| true` 的既有写法） | **误报**，保留并在此说明 |

## 3. 负向用例与 RED/GREEN

`packaging/ci-packaging-quality-test.sh` 扩充为 **62 条断言**，全部用真实制品 + 隔离
副本构造，注入内容都是合成数据（假 PEM 文本、假十六进制 token、32 个 A 的文件），
不含任何真实密钥。旧版本（git `3b90bb0`）与新版本对同一夹具的对照：

| 夹具 | 旧构建期自查 | 旧 CI 扫描 | 新构建期自查 | 新 CI 扫描 |
| --- | --- | --- | --- | --- |
| 文本 PEM 私钥（合成） | **PASS（漏报）** | **PASS（漏报）** | FAIL | FAIL |
| `.bpcert`（合成） | **PASS（漏报）** | FAIL | FAIL | FAIL |
| 32 字节 raw 形状（排除目录外） | **PASS（漏报）** | FAIL | FAIL | FAIL |
| `.key`（合成） | FAIL | FAIL | FAIL | FAIL |
| token secret 行（合成） | FAIL | FAIL | FAIL | FAIL |
| 缺 BUILD-INFO 的 tar | **PASS（漏报）** | — | FAIL（点名 BUILD-INFO.txt） | — |
| 缺 `libQt6Core.so.6` 的 deb | — | 旧许可覆盖：失败原因指向"共享对象数/清单不一致"（走错分支，且会扫到制品之外） | 许可覆盖：点名"找不到随包核心库" | — |
| `libQt6Core.so.6` 为断链 | — | — | 许可覆盖：点名"断链" | — |
| 缺 AppImage 的发行目录 | — | **PASS（漏报）** | 完整清单：点名缺哪个文件 | — |

（"旧 CI 扫描"对 `.bpcert` / 32 字节两例本来就正确 —— 如实记录，不制造 RED。真正的
漏洞是构建期自查缺这两条规则，以及内容规则整体失效。）

## 4. 两套安全扫描的一致性证明

规则唯一实现（`lib/common.sh`），两个入口调用同一份函数；测试矩阵对**每一种注入**
同时跑两个入口并要求结论一致（PASS/FAIL 必须相同）。已验证：

- 真品：构建期自查 PASS、CI 扫描 PASS（8/0）；
- 5 种注入：两个入口一致 FAIL，且失败原因分别命中对应规则（`.key` / `bpcert` /
  私钥 / 口令 / 32 字节）；
- **排除目录边界**：把 32 字节文件放进 `usr/share/...` → 两个入口一致 PASS（这是写在
  注释里的已知假阴性边界，刻意用测试钉住，避免日后被改成"一律拦下"而在合法制品上误报）；
- 解包不完整时两个入口都**不输出扫描 PASS**（"解包不完整：拒绝报告扫描结果"）。

## 5. 完整清单与哈希验收（对真实 v0.1.1 制品）

`ci-release-manifest.sh <目录> --family all|client|server`：三种家族模式对真品全部
PASS；对隔离副本制造 9 类故障（缺 AppImage / 缺客户端 deb / 缺服务端 tar / 缺
RELEASE-INFO / 缺 SHA256SUMS / 哈希不符 / 版本不符 / 重复制品 / 只有客户端时按 all 检查）
全部按预期 FAIL，且**点名具体文件或字段**。

## 6. shell 静态检查

系统里没有 `shellcheck`，`sudo` 需要密码（无法 apt 安装）。按任务书允许的
"临时工具"方式：下载官方静态二进制到 `/tmp/shellcheck-v0.10.0/`（**未改动系统依赖、
未用 root**）后运行：

- 本轮改动/新增的 7 个脚本：**error = 0**，warning 仅剩 2 条 —— 都是
  `build-release.sh:92` 的 `TREE="$(git rev-parse HEAD^{tree})"`（SC1083，合法 git 语法）；
- 全部 `packaging/` 脚本：0 error，warning 均为既有的 SC2034/SC2115 一类（其中唯一会
  命中 `/bin`、`/etc` 的那一处**已按本轮审查修掉**）；
- 全部改动脚本 `bash -n` 通过。

## 7. 完整门禁结果

全部来自本轮实际运行（提交后的干净树；日志见证据包的 gates/ 与 tests/）：

| 项目 | 结果 |
| --- | --- |
| 改动脚本 `bash -n`（21 个） | 0 失败 |
| `git diff --check` | 通过 |
| ShellCheck 0.10.0（/tmp 临时静态二进制） | **error 0**；warning 2（均为 `build-release.sh:92` 的 SC1083 误报） |
| 干净构建 `make clean` + `all server cert-tool gui-all test-fixtures` | exit 0，**warning 0 / error 0** |
| `scripts/lint.sh` | PASS |
| `scripts/comment_ratio.py` | 21.40% |
| `scripts/source_style_check.py` | 0 / 0 / 0 |
| `scripts/test.sh` | **279 PASS / 0 FAIL** |
| `scripts/modern_gui_check.sh` | PASS |
| 打包质量套件（真品，62 条断言） | **62 / 0** |
| 许可覆盖（真品） | 71 / 0 |
| 私钥扫描（真品） | 8 / 0 |
| 完整清单（真品，all / client / server） | 19/0、17/0、16/0 |
| release staging（干净树） | 客户端 9 文件 / 服务端 13 文件，双 PASS |
| **仅服务端家族发行构建**（干净树） | **PASS**（产物：server deb + tar + RELEASE-INFO + SHA256SUMS；清单 14/0） |
| `scripts/final_gate.sh` | **49/49 套件，failed suites = 0**（2501 秒） |

两点如实说明：

- `final_gate.sh` 这一次是在**工作树有未提交改动**时跑的（它自己在日志里记了
  `worktree=9 个改动`）。该门禁只跑产品构建与 C++ 测试，与本次改动（纯打包 / 验收脚本）
  没有交集；而 staging 与单族构建**依赖干净树**，所以在提交之后单独复跑，结果见上表。
- 本轮有两次把长远程脚本放在前台调用，分别被工具超时（300 秒 / 5 分钟）打断；脚本在
  远端继续跑完，相关结论随后都用后台任务重新复算过（本条只为说明取证过程）。

## 8. 统计

| 项 | 值 |
| --- | --- |
| 审查文件数 | 22（`packaging/` 全部 21 个脚本 + `release.yml`），另核对 README 与 3 份发行文档 |
| 实际修改文件数 | 9（7 个脚本修改 + 1 个新脚本 `ci-release-manifest.sh` + 1 份新文档） |
| 新增 / 删除行数 | **+880 / −127**（9 个文件；6 个提交） |
| 发现的问题 | 11（P1×1、P2×2、P3×8 —— 含 X1...X8） |
| 已修复 | 10；未修 1（X4，已说明理由） |
| 新增测试数 | 1 个套件（`ci-packaging-quality-test.sh`，62 条断言，含 A/B/C/D 四组矩阵） |
| 测试套件与断言 | 打包质量套件 62/0；许可覆盖 71/0；私钥扫描 8/0；完整清单 3 种家族模式 PASS |
| 公共接口 / 协议 / 用户数据格式 | **无任何变更**（改动全部在打包与验收脚本层） |

## 9. 风险与未解决项

- **风险低**：新增的都是"更严"的检查（原来假通过的现在会失败），不改产品行为；唯一的
  行为变化是 `build-release.sh` 末尾多跑一次清单检查（家族感知，不影响单族构建 ——
  已用 `--only server` 的本地构建实测）。
- **未解决**：X4（`--skip-appimage` 开发开关，未被 CI/文档使用）；ShellCheck 的
  SC1083/SC2094/SC2015 误报（已在 §2 说明）；本轮**未**创建 tag/Release（任务书要求），
  因此 v0.1.1 的已发布制品保持不变。

## 10. 第3轮最终收尾：三处"检查存在但可以被绕过"（P2-01 / P2-02 / P3-01）

§1–§9 是 PR #33 前 7 个提交的历史记录，**原文保留不动**。本节记录 PR #33 的最后一次
收尾：只针对三个新发现的问题做最小修复，范围仍然限制在打包 / CI 校验层 —— 产品代码、
网络与加密协议、用户数据格式一行未动。

基线：`c922d5b5cd177a708a1c23a9f075142826e9cd83`（PR #33 的原 HEAD）。
本轮只更新功能分支 `fix/release-ci-audit-closure`：不新建 PR、不 merge、
不打 tag、不动 v0.1.0 / v0.1.1 的已有 Release。

### 10.1 P2-01 安全扫描吞掉执行错误 —— 已修

**根因**：`packaging/lib/common.sh` 的三个扫描函数（内容规则 / 敏感文件名 /
32 字节形状）都是"命令 + `2>/dev/null || true`"的写法。GNU grep 用退出码表达
三件事：`0` = 有命中、`1` = 正常完成但没有命中、`2` =
执行 / 读取 / 参数错误。`|| true` 把 2 也折叠成"没有发现"：

| 场景 | 旧行为 | 后果 |
| --- | --- | --- |
| 制品里有读不了的文件（权限 / 坏挂载） | grep 退出 2 → 被吞 → 报 0 命中 | **假通过**：读不了的密钥文件不会被发现 |
| 扫描根不存在 / 不是目录 | grep、find 报错 → 被吞 → 报 0 命中 | 什么都没扫，却报告"没有私钥" |
| grep 本身失败（被替换、`-P` 不支持） | 同上 | 同上 |
| `find … \| while … \| { grep -vE … \|\| true; }` | 管道吞掉 find 的错误状态 | 枚举失败 = "没有裸密钥文件" |

**改动**（`packaging/lib/common.sh` + 两个入口）：

- 新增 `SECRET_SCAN_ERROR_RC=2` 与 `secret_scan_require_root()`：
  扫描根必须存在、是目录、可进入（r+x），否则打印诊断并返回 2。
- 三个扫描函数统一 `0/2` 语义；`grep` 的 stderr **不再重定向**
  （诊断原样进日志，也避免与命中清单合流造成"警告文本被当成命中文件"）；退出码 > 1
  一律返回 2。
- `secret_scan_raw_key_files` 去掉管道：先取 `find` 清单（退出码非 0
  即失败），再在当前 shell 里逐个 `stat` / `grep -P`；`stat`
  失败、`grep -P` 退出 > 1 都返回 2；排除规则的 `grep` 出错时**不放行**。
- `packaging/ci-secret-scan.sh`：三个扫描任一返回 2 → 记一条 FAIL，并且
  **不给出"命中 = 0"的结论**（旧代码会给）。
- `packaging/ci-artifact-selfscan.sh`：同样统一判定，任一为 2 → `die`。
- 计数统一改用 `awk`：`grep -c . || true` 在"0 行"时退出码是 1，
  旧写法只能靠 `|| true` 压住 —— 与本条同源，一并收敛。

**新增测试**（`ci-packaging-quality-test.sh` 第 9 节）：

| 用例 | 期望 | 旧代码 |
| --- | --- | --- |
| 三个函数：扫描根不存在 | 返回 2 + 诊断含"扫描根目录不存在" | 返回 0 / 1，无诊断 |
| 三个函数：扫描根是普通文件 | 返回 2 | 返回 0 |
| 注入 `grep` 退出 2（PATH 前置假 grep，只对 `-rIlE` 那一次调用失败） | 两个入口都失败且原因含"扫描出错" | 两个入口都**成功** |
| 制品里有 0000 权限文件 | 两个入口都失败 | 两个入口都**成功** |
| 二进制里的 PEM 字符串常量 | 两个入口都放行（不误报） | 放行 |
| 发行目录不存在 | 两个入口都失败 | 失败 |

不可读文件那条只在非 root 下运行（root 会绕过文件权限，那种情况下打印 SKIP，而不是
伪造一个"通过"）；先断言夹具真的造出了不可读文件，再断言扫描失败。

### 10.2 P2-02 portable tar 的 BUILD-INFO / VERSION 路径不严格 —— 已修

**根因**：上一轮用 `resolve_tar_file()` 递归
`find "$dest" -name BUILD-INFO.txt`，只要求"恰好一个同名文件、且在解包目录
内"。这只保证**名字唯一**，不保证**位置正确**：把 BUILD-INFO.txt 挪到
`random-place/` 依然通过；顶层多出一个目录也依然通过。

**改动**（`packaging/ci-artifact-selfscan.sh`）：

- 新增 `tar_top_dir()`：解包目录的顶层必须**恰好一个条目**，而且必须是真实
  目录（不是符号链接）；0 个或多于 1 个都直接失败。
- 新增 `require_contract_file()`：契约文件必须是该目录下的**普通文件**，不许
  是符号链接，且 `readlink -f` 的真实路径必须落在给定根目录内。
- portable tar 一律按 `<top>/BUILD-INFO.txt`、
  `<top>/share/backup-project/VERSION` 取文件；deb / AppImage 用同一函数核对
  路径归属（`$dest/…`）。
- 顶层目录名**不比较大小写**（客户端 `Backup-Project-Client-…`、服务端
  `backup-project-server-…` 都兼容），正常发行包布局不变。

**新增测试**（第 10 节）：正常客户端 / 服务端 tar 正对照；BUILD-INFO 挪到
`random-place/`；VERSION 挪到 `random-place/`；VERSION 缺失；
顶层两个目录；BUILD-INFO 是指向树外的符号链接；`share` 是指向树外的符号
链接（真实路径逃逸）；VERSION 与期望不一致；commit 与期望不一致。每条失败用例都断言
**命中预期规则**，不是只看退出码。

### 10.3 P3-01 发行目录顶层只有黑名单 —— 已修

**根因**：多余文件检查只枚举 `*.deb / *.AppImage / *.tar.xz`，
`debug.log`、`.unexpected`、意外目录这类东西根本不进检查 ——
白名单写成了黑名单。

**改动**（`packaging/ci-release-manifest.sh`）：枚举顶层**全部**条目
（`find -mindepth 1 -maxdepth 1 -printf '%f\t%y\n'`，含隐藏文件、目录、
符号链接），逐个核对名字与类型：

- 只允许七件套：五件制品 + `SHA256SUMS` + `RELEASE-INFO.txt`；
  另外自检白名单条目数必须正好是 7（防止 `EXPECTED` 数组少一项而悄悄放宽）。
- 未预期条目**点名**报出；隐藏文件单独一类消息；类型不是普通文件（目录 / 符号链接）
  单独报出。
- 家族语义保留：`all` 要求本族齐备，`client` / `server`
  只要求本族齐备、容忍另一族的合法制品，但不允许任何意外条目。
- 顺带：读取 `RELEASE-INFO` 的 `grep|awk` 换成纯 `awk`
  （旧写法在字段缺失时会因 `pipefail` 直接中断脚本，看不到下面那条清楚的
  诊断）；deb 元数据读取失败从 `|| true` + 空值改成明确报错。

**新增测试**（第 11 节）：七件套正对照；多出 `debug.log`；多出隐藏文件
`.unexpected`；多出意外目录；多出未知格式文件 `weird.zip`；
符号链接冒充 `SHA256SUMS`；客户端目录里带另一族合法制品时
`family=client` 通过、`family=all` 仍失败。

### 10.4 解包阶段的路径安全：实测已有约束（不需要改）

任务书要求检查"tar 解包阶段是否已有路径安全约束"。本轮用 Python 直接构造恶意归档做了
三组实测（GNU tar 1.35）：

| 构造 | 结果 |
| --- | --- |
| 成员名 `../escaped.txt` | tar 退出 2：成员名称包含".."；树外没有文件被写出 |
| 先放符号链接 `top/link -> /tmp/…`，再放 `top/link/pwned.txt` | tar 退出 2：`无法 open: 不是目录`；`/tmp/…` 下没有被写入 |
| `share` 是指向树外的符号链接，且树外真有 `backup-project/VERSION` | 解包成功，但契约检查命中"真实路径逃出顶层目录" |

结论：解包阶段本身已有约束（拒绝 `..`、拒绝穿过符号链接写入），**不需要
在解包器上加代码**；本轮把这三条实测固化成回归用例（含"树外没有被写入"的断言），
并由 `require_contract_file()` 兜住"解包成功但路径归属不对"的情况。

### 10.5 RED/GREEN

把**新增测试**跑在**基线代码**上（RED），再跑在**本轮代码**上（GREEN）：

| 运行 | 结果 |
| --- | --- |
| RED：新测试 × `c922d5b5` 的 `packaging/` | 100 条用例，**20 FAIL / 80 PASS**，退出码 1 |
| GREEN：新测试 × 本轮 `packaging/` | 100 条用例，**0 FAIL / 100 PASS**，退出码 0 |

20 条 RED 覆盖了三类问题的"假通过"证据，例如
`扫描根不存在 -> 返回 2（期望 '2'，实际 '0'）`、
`注入 grep 退出 2 -> 构建期自查本应失败（rc=0）`、
`读不了制品里的文件 -> CI 扫描本应失败（rc=0）`、
`BUILD-INFO 被挪到 random-place/ -> 失败（本应失败却成功了）`、
`顶层目录不唯一 -> 失败（本应失败却成功了）`、
`多出 debug.log（本应失败却成功了）` 等。完整逐条对照见证据 ZIP 的
`tests/redgreen-table.txt`。

### 10.6 完整门禁（在干净的工作树上执行）

环境：Ubuntu 24.04 开发机（VM）。**这台机器没有外网**，`linuxdeploy` 等
AppImage 打包工具下载不动，所以客户端制品只能在 CI 的 Debian 12 基线容器里构建 ——
这一点如实记录，本轮**没有**用 v0.1.1 的旧二进制冒充新 HEAD 的产物。

#### (1) 本地门禁 A：代码提交 `86ed7e4`，`git status --porcelain` 为空

| 门禁 | 结果 |
| --- | --- |
| `bash -n`（全部 `packaging/*.sh`、`packaging/lib/*.sh`、`scripts/*.sh`） | 0 失败 |
| `git diff --check`（工作树 + `c922d5b..HEAD` 区间） | 0 / 0 |
| ShellCheck 0.10.0（`-x -S warning`，临时静态版本，未改系统） | 0 error / 2 warning（SC1083，`build-release.sh:92` 的 `HEAD^{tree}`，预先存在，误报） |
| `scripts/lint.sh` | exit 0 |
| `scripts/comment_ratio.py` | exit 0，`COMMENT_RATIO=21.40%` |
| `scripts/source_style_check.py` | exit 0 |
| `scripts/test.sh` | exit 0，`PASS=279 FAIL=0` |
| `scripts/modern_gui_check.sh` | exit 0 |
| 打包质量套件（对真实 v0.1.1 制品做正 / 负对照） | exit 0，100 条断言 0 失败 |
| 本地从当前提交构建发行制品（`--only server`，版本 `0.1.1-local3`） | exit 0 |
| ↳ 该新制品的自查 / 清单(server) / 私钥扫描 / 许可覆盖 | exit 0；14:0 / 5:0 / 8:0 |
| 真品目录的三种家族清单 | all 19:0、client 17:0、server 16:0 |
| `scripts/final_gate.sh` | **49 套件，`failed suites = 0`**，3196 秒 |

#### (2) CI 门禁：run [37915246454](https://github.com/PlyusWang/backup-project/actions/runs/37915246454)，同一提交 `86ed7e4`

用 `workflow_dispatch` 触发（不是 tag）：该 workflow 是 `permissions: contents: read`，
**不创建 tag、不发布 GitHub Release**。6 个作业全部 success：

| 作业 | 结果 |
| --- | --- |
| version | success |
| build server (ubuntu:20.04, glibc 2.31) | success（含同一 commit 两次 clean build 的**可复现性**比对） |
| build client (debian:12, Qt 6.4 / glibc 2.36) | success |
| client compatibility (Ubuntu 22.04) | success |
| systemd acceptance (ubuntu:22.04 真实 VM) | success |
| bundle + clean-container install tests | success |

bundle 作业里与本轮直接相关的步骤逐个 success：`assemble release directory`、
`AppImage launch tests`、`client deb install (clean debian:12)`、
`server deb install (clean debian:12, no systemd)`、
`third-party license coverage`、**`release manifest（完整清单 + 哈希）`**、
**`artifact self-scan（解包 + 安全检查 + BUILD-INFO 契约）`**、
**`packaging quality tests（检查自身的缺陷检出）`**、
`secret scan on final artifacts`。

#### (3) 对 CI 新制品的本地复验

把 CI 产出的 `release-bundle`（版本 `0.1.1-r3ci`，七件套，由
`86ed7e4` 在 debian:12 / ubuntu:20.04 容器里构建）取回本地，用本轮的新检查
再跑一遍：

| 检查 | 结果 |
| --- | --- |
| `SHA256SUMS` 自校验 | 通过 |
| 构建期自查（严格 tar 布局 + 安全检查，`--expect-commit/-version`） | exit 0 |
| 完整清单 all / client / server | 19:0 / 17:0 / 16:0 |
| CI 私钥扫描 | exit 0 |
| 许可覆盖 | exit 0 |
| 打包质量套件（100 条断言，对 CI 新制品） | **passed=100 failed=0** |

因此"新 HEAD 的发行验收"用的是**本次提交真实构建出来的制品**（本地服务端 +
CI 两个产品族），v0.1.1 的真品只用于测试逻辑的正 / 负对照。

### 10.7 统计与交付

| 项 | 值 |
| --- | --- |
| BASE_HEAD | `3b90bb057022cbc2e8cfd758d2421c1baf17124a`（main） |
| START_HEAD（本轮起点） | `c922d5b5cd177a708a1c23a9f075142826e9cd83` |
| 代码提交 | `86ed7e4` fix(packaging): close the round-3 release-check false passes |
| 交付 HEAD | 本文档所在提交（`docs(release): ...`），SHA 见证据 ZIP 的 `git/final-head.txt` |
| 分支 | `fix/release-ci-audit-closure`（只更新 PR #33，未新建 PR） |
| 本轮代码改动 | 5 个文件，`+480 / −77`（本文档另计） |
| 测试规模 | 打包质量套件 62 → **100** 条断言 |
| 新增负向用例 | 30 条（P2-01 8 条、P2-02 14 条、P3-01 8 条） |
| 发现的问题 | 3（P2×2、P3×1）；同类"吞掉错误状态"写法 4 处顺带收敛 |
| 已修复 | 3 / 3 |
| 产品代码 / 协议 / 用户数据格式 | **无变更** |

未解决 / 明确不做：

- **客户端制品的本地构建**：开发机无外网，`fetch-tools.sh` 下不动
  `linuxdeploy`；这是环境限制而不是代码问题，客户端已由 CI 覆盖（见 10.6）。
- ShellCheck 的 2 条 SC1083 误报（`HEAD^{tree}`，预先存在，本轮未改）。
- X4（`--skip-appimage` 开发开关）仍按上一轮的决定不修。
- 本轮**不**创建 tag / Release；v0.1.0 / v0.1.1 的已发布制品保持不变。

## 11. 两项遗留问题最终核查（第4轮）

上一轮独立验收提出两个疑点：证据里同时出现 **91 / 97 / 100** 三个测试数字，以及
ShellCheck 的 **SC1083 + exit=1** 被上一轮报告称为"既有误报"。本轮把两个都查到底：
**两处都是真实问题**（一个是我上一轮汇总脚本的口径错误，一个是确实不规范的引用写法），
各做最小修复。§1–§10 的历史记录原文保留，本节是核查与更正的记录。

### 11.1 断言计数 91 / 97 / 100：三个数字的来源与口径

先给结论：**套件从来没有漏跑或重复计数**；91 是**我上一轮证据汇总脚本的统计口径**造成
的（按"去掉括号后缀的唯一标签"聚合），97 是 **CI 的 root 环境跳过 3 条依赖文件权限的
断言**，100 是套件定义的断言总数。三个数字的原始出处：

| 数字 | 原始出处（不可改写的证据） | 运行主体与环境 | 命令 | 口径 |
| --- | --- | --- | --- | --- |
| **100** | 证据 ZIP（第3轮）`tests/green-new-tests-on-new-code.log`，末行 `[packaging-quality] passed=100 failed=0` | 本轮代码 + 本轮测试；VM 普通用户（非 root），基线 c922d5b5 的 `packaging/` 作为 RED 侧 | `bash packaging/ci-packaging-quality-test.sh /tmp/v011-verify/dl/0.1.1` | 套件自带计数器：每执行一条断言 +1；该日志逐条 `  PASS` 行正好 100 条 |
| **80 / 20（RED）** | 证据 ZIP（第3轮）`tests/red-new-tests-on-old-code.log`，末行 `[packaging-quality] passed=80 failed=20` | 同一份测试文件；`packaging/` 换成基线 c922d5b5 的版本 | 同上 | 同上（100 条断言，20 条失败） |
| **91** | 证据 ZIP（第3轮）`tests/redgreen-summary.log` 里的 `用例总数：RED=91 GREEN=91` | **我上一轮的对照脚本** `/tmp/r3/redgreen.sh`（不在仓库里） | `bash /tmp/r3/redgreen.sh` | 脚本内联 Python 用 `d[去掉括号后缀的标签] = PASS/FAIL` 聚合，`len(d)` = **唯一标签数**，不是断言数 |
| **97** | CI run [37915246454](https://github.com/PlyusWang/backup-project/actions/runs/37915246454) 的 bundle 作业日志（job 113771564326）里 `[packaging-quality] passed=97 failed=0` | CI 的 root 容器（`id -u = 0`） | workflow 步骤 `bash packaging/ci-packaging-quality-test.sh "dist/release/0.1.1-r3ci"` | 套件计数器：root 会绕过文件权限，3 条依赖权限的断言整段跳过（日志里 1 行 `  SKIP`），97 + 3 = 100 |

**9 条差额到底是什么**：不是丢失的测试，而是 **9 个标签被两条断言复用** —— 每个注入
夹具都同时跑"构建期自查"和"CI 扫描"两个入口，标签只差括号里的后缀：

``
P1-01 截断的 AppImage（构建期自查）/（CI 扫描）
P1-01 空文件冒充 AppImage（构建期自查）/（CI 扫描）
P1-01 不认识的发行文件（构建期自查）/（CI 扫描）
B 含测试用 .key（构建期自查）/（CI 扫描）
B 含测试用 .bpcert（构建期自查）/（CI 扫描）
B 含模拟 PEM 私钥（构建期自查）/（CI 扫描）
B 含模拟 token secret（构建期自查）/（CI 扫描）
B 含 32 字节原始密钥形状（排除目录之外）（构建期自查）/（CI 扫描）
P2-01 二进制里的 PEM 字符串常量不误报（构建期自查）/（CI 扫描）
``

9 个标签 × 2 条 = 18 行；100 − 18 + 9 = **91**。RED 与 GREEN 两份日志都是同样的
100 行 / 91 标签，差额完全一致 —— 也就是说这个 91 与"旧代码还是新代码"无关，
纯粹是汇总口径。

**逐项回答核查清单**：

- *91 代表哪个提交、哪个时间点、哪个测试集合*：不代表任何提交的测试集合，它是我在
  2026-10-09 生成第3轮证据时，对 RED/GREEN 两份日志用"唯一标签"口径重算出来的数字。
- *100 代表哪一个*：本轮代码 + 本轮测试（100 条断言）在**非 root** 环境下的结果，
  同时也是套件定义的全部断言数。
- *97 是不是因为 root 跳过 3 条*：是。CI 日志里那 3 条落在
  `if [ "$(id -u)" -eq 0 ]` 分支（夹具有效性 + 两个入口的失败断言）；
  本轮已把它们改成显式 `ci_skip` 并在收尾行报出口径。
- *是否存在统计遗漏 / 重复计数 / 历史日志混入*：没有。RED/GREEN 两份日志逐条核对，
  100 条断言一一对应；两份日志来自同一次对照运行（同一台机器、同一份测试文件、
  同一输入目录），没有混入旧日志。
- *RED/GREEN 是否可比*：可比。对照脚本把**当前工作树的测试文件**复制进基线树
  （`cp "$REPO/packaging/ci-packaging-quality-test.sh" "$OLD/packaging/"`），
  两侧测试定义逐字节相同；输入都是同一份 v0.1.1 真品目录；两侧用例数相同（100/100），
  逐条按顺序配对。
- *是否有"代码改了但汇总没同步"*：有，就是这次的 91 —— 汇总脚本的口径与套件不一致。
  本轮修掉了汇总口径，并让套件自己输出可核对的口径（见下）。
- *最终 HEAD 是否覆盖全部要求的负向场景*：是，100 条断言的分组为
  P1-01 解包 fail-closed（6）、P1-02 声明不被简化（1）、P1-03 插件/QML 双向登记（2）、
  A 核心库定位（5）、B 两套扫描一致性矩阵（24）、C 完整清单与哈希（12）、
  D BUILD-INFO 契约（4）、P2-01 扫描错误传播（15）、P2-02 tar 严格路径（16）、
  P3-01 顶层白名单（9）、正对照与家族模式（6）。

**修复**（把口径固化进套件，避免以后再靠人对日志）：

- `packaging/ci-lib.sh`：新增 `ci_skip()`（SKIP 单独计数，
  **永远不计入 PASS**）；`ci_finish()` 追加一行
  `[套件名] skipped=N assertions=M（passed + failed + skipped）`，
  并支持传入期望断言总数，对不上就记一条 FAIL。历史格式的
  `passed=… failed=…` 一行**保持不变**（仓内已有脚本用 `grep -oE` 提取它）。
- `packaging/ci-packaging-quality-test.sh`：root 分支改用 3 条
  `ci_skip`（如实记录而不是伪造通过），收尾声明总数 100。

**修复后的实测**：

| 场景 | 结果 |
| --- | --- |
| 非 root（本机，v0.1.1 真品） | `passed=100 failed=0` + `skipped=0 assertions=100`，exit 0 |
| root 路径（把判据改成恒真的**副本**模拟；本机拿不到 root：sudo 需要密码、`unshare -r` 被内核限制） | `passed=97 failed=0` + `skipped=3 assertions=100`，exit 0 |
| CI（真实 root 容器，run 37937121722） | 见 §11.4 |
| 守卫自测：1 条断言 vs 期望 2 | `FAIL  断言总数 1 != 预期的 2`，exit 1（守卫真的会响） |
| 守卫自测：1 通过 + 1 SKIP vs 期望 2 | exit 0（SKIP 计入总数、不计入通过） |

### 11.2 ShellCheck SC1083：引用不规范，不是误报（更正式上一轮的说法）

**原始异常**：ShellCheck 0.10.0 对 `packaging/build-release.sh:92` 报 2 条
`SC1083 (warning)`，进程退出码 1；上一轮报告称其为"既有误报"，但没有给出依据。

**原始输出（改动前，逐字）**：

``
In packaging/build-release.sh line 92:
TREE="$(git rev-parse HEAD^{tree})"
                           ^-- SC1083 (warning): This { is literal. Check expression (missing ;/
?) or quote it.
                                ^-- SC1083 (warning): This } is literal. Check expression (missing ;/
?) or quote it.
build-release.sh exit=1
``

**SC1083 的含义**：它在提醒"这个 `{` 会被当作字面量"，建议检查是不是漏写了
`$` 或者**加引号**。

**bash 实际怎么解析**（实测，GNU bash 5.2）：

| 写法 | 传给命令的参数 | 说明 |
| --- | --- | --- |
| `HEAD^{tree}` | 1 个参数，内容 `HEAD^{tree}` | 大括号里没有逗号/序列 → 不触发 brace expansion |
| `HEAD^{tree,blob}` | **2 个参数**：`HEAD^tree`、`HEAD^blob` | 有逗号 → brace expansion 生效 |

所以原文案"能用"依赖的是一个巧合（大括号里恰好没有逗号），**ShellCheck 的说法是准确的**：
这是字面量大括号，而且是**脆弱的引用方式** —— 一旦有人写出 `HEAD^{tree,blob}`
之类的写法，bash 就会悄悄拆参数。**结论：不是工具误报，是引用不规范**；上一轮的
"既有误报"结论依据不足，本节更正。

**两条诊断的原因**：`{` 与 `}` 各报一条，所以是 2 条 warning（不是 2 个问题）。

**退出码与严重度**：两条都是 `warning`（不是 `error`），但 ShellCheck
只要报告了 warning，进程退出码就是 1 —— 这就是"0 error / 2 warning / exit=1"的来源。

**对制品有没有影响**：没有，且已实测。

| 检查 | 结果 |
| --- | --- |
| 四种写法（不加引号 / 单引号 / 双引号 / 反斜杠转义）取值 | 全部相同：`516ccb2fa3db8218e6cfe732bf8ff2d71fec56d1`（= 改动前的 HEAD tree） |
| 与 `git cat-file -p HEAD` 的 tree 字段对照 | 一致 |
| 从仓库根 / `packaging/` / `scripts/` 子目录执行 | 一致 |
| 制品里是否包含 `build-release.sh` | 不包含（tar 里只有 `packaging/portable`、`packaging/client`、`packaging/server` 的安装脚本），所以改这个文件不改变任何制品字节 |
| 修复后从新 HEAD（`e5d4049`）实际构建服务端制品 | `BUILD-INFO` / `RELEASE-INFO` 的 `tree` = `ecb2740c82b5c748bfbd0272b3e7e159528f07fb`、`commit` = `e5d4049b…`，与独立计算的 `git rev-parse HEAD^{tree}` / `git rev-parse HEAD` **完全一致** |

**修法**：`TREE="$(git rev-parse 'HEAD^{tree}')"` —— 用单引号把 revision 钉成
字面量，语义与取值都不变，只把意图写明。

**修复后**：本 PR 改动的脚本（`lib/common.sh`、`ci-secret-scan.sh`、
`ci-artifact-selfscan.sh`、`ci-release-manifest.sh`、
`ci-packaging-quality-test.sh`、`ci-lib.sh`、`build-release.sh`）
ShellCheck **0 error / 0 warning / exit=0**。

**没有做的事**：没有用 `# shellcheck disable=SC1083`、没有屏蔽整个文件、没有
缩小检查范围、没有 `|| true`。

**顺带记录（不改）**：把范围放宽到 `packaging/*.sh` 全部 21 个脚本时，
还剩 3 条**预先存在**的 `SC2034`（未使用变量）：
`ci-server-install-test.sh:144`（`PIN2`）、
`ci-server-install-test.sh:147` 与 `ci-upgrade-rollback-test.sh:39`
（循环变量 `i`）。它们不在本 PR 的改动范围里，本轮按"不扩大范围"处理，
如实记录在此（其中 `PIN2` 看起来是一处**被赋值但从未断言**的取值，
可能是那两个安装测试的遗留，建议后续单独核查）。

### 11.3 本轮的修改

| 文件 | 变化 | 原因 |
| --- | --- | --- |
| `packaging/build-release.sh` | +6 / −1 | SC1083：把 `HEAD^{tree}` 用单引号钉成字面量 |
| `packaging/ci-lib.sh` | +20 / −2 | 新增 `CI_SKIPPED` / `ci_skip()`；`ci_finish()` 输出 skipped/assertions 并支持期望总数校验 |
| `packaging/ci-packaging-quality-test.sh` | +15 / −3 | root 分支改用 `ci_skip`；收尾声明断言总数 100 |

未改动：产品代码、协议、用户数据格式、发行包布局、命令行接口、
`ci_finish` 的历史输出格式（`passed=… failed=…` 一行保持原样）。

### 11.4 复核与门禁

#### (1) CI：真实 root 容器

run [37937121722](https://github.com/PlyusWang/backup-project/actions/runs/37937121722)
（`workflow_dispatch`，只读权限，提交 `e5d4049b`）：**6 / 6 作业 success**。
bundle 作业里每个套件的收尾行（新格式）：

| 步骤 | PASS | FAIL | SKIP | 收尾行 |
| --- | --- | --- | --- | --- |
| `ci-packaging-quality-test.sh` | 97 | 0 | **3** | `passed=97 failed=0` / `skipped=3 assertions=100` |
| `ci-release-manifest.sh` | 19 | 0 | 0 | `skipped=0 assertions=19` |
| `ci-secret-scan.sh` | 8 | 0 | 0 | `skipped=0 assertions=8` |
| `ci-license-coverage.sh` | 71 | 0 | 0 | `skipped=0 assertions=71` |
| `ci-client-install-test.sh` | 33 | 0 | 0 | `skipped=0 assertions=33` |
| `ci-server-install-test.sh` | 75 | 0 | 0 | `skipped=0 assertions=75` |
| `ci-appimage-test.sh` | 14 | 0 | 0 | `skipped=0 assertions=14` |

CI 日志里那 3 条 SKIP 的标签与本地非 root 环境下真正执行的 3 条断言**完全一致**，
因此"97 + 3 = 100"在 CI 日志里是自解释的 —— 这正是本轮要修的东西。

#### (2) 本地复核

| 场景 | 结果 |
| --- | --- |
| 打包质量套件（非 root，v0.1.1 真品） | `passed=100 failed=0` + `skipped=0 assertions=100`，exit 0 |
| root 路径（判据恒真的副本模拟） | `passed=97 failed=0` + `skipped=3 assertions=100`，exit 0 |
| ShellCheck（本轮改动的 7 个脚本） | **0 error / 0 warning / exit=0** |
| ShellCheck（`packaging/*.sh` 全量 21 个脚本） | 3 条预先存在的 `SC2034`（见 §11.2，未修） |
| 从新 HEAD（`e5d4049`）构建服务端制品 | exit 0；`BUILD-INFO`/`RELEASE-INFO` 的 tree = `ecb2740c…`、commit = `e5d4049b…`，与 git 独立计算一致 |
| 新制品上的检查器 | 自查 exit 0；清单(server) 14/0；私钥扫描 5/0；许可覆盖 8/0（均带 `skipped=0 assertions=N`） |
| `ci_finish` 守卫自测 | 1 条 vs 期望 2 → `FAIL  断言总数 1 != 预期的 2` + exit 1；1 通过 + 1 SKIP vs 期望 2 → exit 0 |

本节引用的 CI 与本地数字都对应**代码提交 `e5d4049`**；文档提交之后的交付 HEAD
上重跑了完整门禁（`scripts/final_gate.sh`），日志在证据 ZIP 的 `gates/` 下，
两者的 SHA 分别标注在日志首行。
## 12. 第5轮收尾：报告格式与 portable 身份认证测试

本轮只做两件事：修掉第 11.4 节的 Markdown 占位符（外加一处被撑坏的表格行），以及补上
portable 安装早就该有的**真实身份握手**测试。产品代码、协议、密钥算法一律未动。

### 12.1 报告格式：占位符与一处表格

**原始异常**：第 11.4 节有 **68 处未替换的模板标记**（第 4 轮生成脚本遗留），GitHub 上直接渲染成
字面量 —— 本该是行内代码的 `workflow_dispatch`、`passed=97 failed=0` 等片段前面都多出一串标记。

**根因**：第 11.4 节的生成脚本忘了做"标记 → 反引号"这一步替换（§11.1–§11.3 的脚本做了，所以
只有 11.4 中招）。这是**真实的交付质量问题**，但不影响任何技术结论。

**修法**：

- 34 对标记（成对出现）换成行内代码 —— 用**成对**替换：把两个标记之间的内容取出来加反引号，
  不做全局盲替换，因此不可能破坏代码块或嵌套格式。
- 不变式校验：把标记与反引号都去掉之后，修复前后的文本**逐字节一致** —— 证明没有任何
  正文被顺手改动（反引号计数 776 → 844，正好 +2×34）。
- 第 2 节 X5 行的表格单元格里有一个**未转义的 `||`**，把 6 列撑成 8 列、破坏整张表
  的渲染；按 GFM 规范写成 `A && B \|\| true`（渲染结果不变）。

**复查**（可复现的检查器 `mdstruct2.py`，随证据包提供）：

| 检查项 | 结果 |
| --- | --- |
| 模板标记残留 | **0** |
| 代码围栏 | 4 行 = 2 块，成对 |
| 标题层级 | 32 个标题，无跳级 |
| 表格 | 20 张，未转义竖线数与表头一致 |
| 行内代码 | 全部闭合 |

另外记录一处**既有、不在本轮范围**的问题：仓库根 `README.md` 第 237 行
`#### 界面预览` 从 h1 直接跳到 h4（渲染层级不连续），本轮未改。

### 12.2 portable 服务端身份认证测试

**原始缺口**：第 9 节（portable tar.xz）里 `PIN2` 只被赋值、**从未使用**，之后只断言
`127.0.0.1:18999` 在监听。也就是说：测试证明了服务端能起来，但**没有**证明 portable 安装
自己的身份密钥能用于真实的安全传输握手 —— 而同一文件的 `.deb` 那一节早就在用
`backupctl remote ping` 做真实握手了。

**修法**（`packaging/ci-server-install-test.sh` 第 9 节，新增 8 条断言）：

1. 指纹提取后立刻自检：非空；
2. 形状必须匹配 `keygen --show` 实际打印的 `sha256:<64 位小写十六进制>`（格式取自
   `server/keygen_main.cpp:62`，不是凭空规定）；
3. 启动前断言 `127.0.0.1:18999` **空闲** —— 否则后面的监听成功可能来自上一个残留
   进程（端口断言的假阳性）；
4. 进入监听后做**真实回环握手**：
   `timeout 30 backupctl remote ping --host 127.0.0.1 --port 18999 --server-key $PIN2`；
5. 负向：把指纹最后一位十六进制改掉，错误身份必须被**明确拒绝**；
6. 客户端**超时（退出码 124）不算拒绝**，单独报 FAIL；被接受则报安全缺陷；
7. 拿不到指纹时**不跳过**：两条握手断言直接判 FAIL（断言总数恒为 21）；
8. 收尾：先 `wait` 回收再断言进程消失（`kill -0` 对僵尸进程仍为真），并断言端口
   已释放；失败时保留服务端日志尾部，全程不打印任何密钥材料。

**缺陷检出验证（注入实验）**：

| 注入 | 期望 | 实测 |
| --- | --- | --- |
| 真实运行（不注入） | 全过 | 21 PASS / 0 FAIL，含真实握手与错误指纹被拒绝（退出码 1） |
| `backupctl` 永远成功 | 负向用例 FAIL | `FAIL 错误身份指纹被接受了（安全缺陷）` |
| `backupctl` 永远失败 | 正向握手 FAIL | `FAIL portable 回环 ping …：退出码 1` |
| 指纹强制为空 | 4 条 FAIL（不是跳过） | 指纹存在性 / 形状 / 正向握手 / 负向用例全 FAIL，总数仍 21 |

失败传播：断言走 `ci_fail` → `CI_FAILED` → `ci_finish` 非零退出，CI 里
表现为该步骤失败。

### 12.3 ShellCheck SC2034：三条未使用变量

§11.2 记录的三条 `SC2034` 本轮全部处理完：

| 位置 | 原来 | 现在 |
| --- | --- | --- |
| `ci-server-install-test.sh` 的 `.deb` 段等待循环 | `for i in $(seq 1 40)`（i 未使用） | `for _ in $(seq 1 40)` |
| `ci-server-install-test.sh` 的 portable 段等待循环 | 同上 | 同上 |
| `ci-upgrade-rollback-test.sh` 的 `wait_active()` | `local i` + `for i …` | 去掉 `local i`，改 `for _ …` |

先做了形式实验（`_` 与 C 风格 `for ((i=0;i<40;i++))` 都是 ShellCheck-clean），并对
照了语义：提前退出时都是 3 次迭代、不退出时都是 40 次。**等待上限、间隔、提前退出与
超时失败的语义完全不变**；没有使用任何 `shellcheck disable`。

结果：这两个脚本 `0 error / 0 warning / exit=0`；`packaging/*.sh` 全量 21 个脚本
同样 exit=0（第 4 轮还剩 3 条 warning，现在清零）。

### 12.4 测试与门禁

定向验证：

- `bash -n`：两个改动脚本 + 全量 packaging 脚本，0 失败；
- ShellCheck 0.10.0：见 §12.3；
- `git diff --check`：0；
- portable 身份认证：把**提交文件里第 9/10 节的原文**取出来在本地真实执行（非 root，
  portable 不需要 root）：21/21 PASS；三组注入全部按预期 FAIL；
- portable 卸载：数据与私钥按契约保留（两节末尾的既有断言）；
- `.deb` 安装测试与升级/回滚测试需要 root 与 systemd：由 CI 的 release 流水线覆盖
  （bundle 作业跑 `ci-server-install-test.sh`，systemd 验收作业跑
  `ci-upgrade-rollback-test.sh`）。

本轮的门禁与 CI 记录（提交号标注在各自日志首行）随证据包
`release-verification-closure-evidence-round5.zip` 提供：CI 走 `workflow_dispatch`（只读权限，
不创建 tag、不发布 Release），本地在交付 HEAD 上执行 `scripts/final_gate.sh`。

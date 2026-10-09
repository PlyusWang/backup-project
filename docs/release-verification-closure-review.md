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
| X5 | P3 | `build-release.sh:92/264/365/530` 等 | ShellCheck 的 SC1083（`HEAD^{tree}` 是合法 git 语法）、SC2094（清单生成已排除清单自身）、SC2015（`A && B || true` 的既有写法） | **误报**，保留并在此说明 |

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

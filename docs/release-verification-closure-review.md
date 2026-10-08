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

见证据包 test-logs/ 与 summary.txt（本轮实际运行）。

## 8. 统计

| 项 | 值 |
| --- | --- |
| 审查文件数 | 22（`packaging/` 全部 21 个脚本 + `release.yml`），另核对 README 与 3 份发行文档 |
| 实际修改文件数 | 8（7 个修改 + 1 个新增 `ci-release-manifest.sh`） |
| 新增 / 删除行数 | **+501 / −127** |
| 发现的问题 | 9（P1×1、P2×2、P3×6 —— 含 X1...X5） |
| 已修复 | 8；未修 1（X4，已说明理由） |
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

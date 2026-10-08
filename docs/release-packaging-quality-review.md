# 发布工程质量审查与修复（v0.1.1）

本文是一次**限定范围的发布工程收口**记录：审查 `packaging/`、
`.github/workflows/release.yml`、发行相关脚本与发行文档，修掉能让发行检查
**假通过**或让交付材料**不一致**的问题，并为每一处补上能检出缺陷的回归测试。

- 基准 SHA：`cee9cb5a752a76a9261997f919b4934999a425f6`（v0.1.0 的 merge commit）
- 审查文件数：46（`git ls-files packaging/ .github/workflows/release.yml`）
- 本轮**不动**备份核心、R-01、GUI 业务、归档格式、安全协议与网络逻辑；不改公共接口，
  不改用户数据结构。
- 正式发行物（五个二进制 + `SHA256SUMS` + `RELEASE-INFO.txt`）不在本文里，见 GitHub Releases。

## 1. 问题清单与结论

| 编号 | 等级 | 位置 | 问题 | 结论 |
| --- | --- | --- | --- | --- |
| P1-01 | P1 | `packaging/ci-secret-scan.sh`、`packaging/build-release.sh` | 解包失败被 `|| true` 吞掉，且用"预先 `mkdir` 出来的目录存在"当解包成功证据 | **已修**（fail-closed） |
| P1-02 | P1 | `packaging/build-release.sh`（`pack_client_deb`） | 客户端 `.deb` 里已生成的完整第三方声明被一份 6 行简介覆盖 | **已修** |
| P1-03 | P1 | `packaging/licenses/build-materials.sh`、`packaging/ci-license-coverage.sh` | 许可清单只数 `lib/` 顶层的 `*.so*`，`plugins/` 与 `qml/` 里的 33 个共享对象从未登记 | **已修**（完整清点 + 双向核对） |
| P1-04 | P1 | `packaging/licenses/build-materials.sh` | 提供包查询用 `find ... -maxdepth 3`，Qt 插件 / QML 模块的深路径对象根本查不到（会退化成"不来自发行版包"） | **已修**（本轮的验证过程中发现） |
| P2-01 | P2 | `packaging/licenses/build-materials.sh` | 双引号里的未转义反引号触发命令替换：构建日志出现 `.so: command not found`，生成的声明里那句话还被替换掉了 | **已修** |
| P2-02 | P2 | Release Notes | v0.1.0 说明把 BPSEC1 的 X25519 pin 与 BPSEC2 的 Ed25519 证书混在一句里 | **已修**（v0.1.1 说明按源码改写） |
| P3-01 | P3 | README、`docs/04_release_and_demo.md`、`docs/release-packaging.md` | 把 `6d1e2ba` 当"当前 main"、发行目录写成不存在的路径、04 还标着 Draft | **已修**（历史与当前分开表述） |
| P3-02 | P3 | `packaging/build-release.sh`:345 | `gzip -9n -c ... > /dev/null 2>&1 || true` 是一句没有效果的调用 | **未修**（无功能影响，见 §5） |

## 2. 逐项根因、影响与修法

### P1-01 解包失败被当成"扫描通过"

**旧代码**（`packaging/ci-secret-scan.sh`）：

```bash
*.AppImage)
  mkdir -p "$WORK/$base"
  ( cd "$WORK/$base" && "$artifact" --appimage-extract >/dev/null 2>&1 ) || true ;;
esac
expect_file "解包 $base" "$WORK/$base"     # <- 只检查"目录在不在"
```

`mkdir -p` 先把目录建出来，解包失败又被 `|| true` 吞掉，于是
`expect_file` 必然成功：后面整段扫描会去扫一个**空目录**，照样报
"私钥/口令结构规则命中 = 0"。`build-release.sh` 构建期的自查是同一写法。

**实测**（同一个截断到 4 KiB 的 AppImage，见 §4）：

| 扫描器 | 结果 | 退出码 |
| --- | --- | --- |
| 旧（git `cee9cb5`） | `PASS` 4 项、0 失败 | **0（假通过）** |
| 新 | `FAIL` 解包失败 + "解包不完整：拒绝报告扫描结果" | 非 0 |

**修法**：新增 `ci_unpack`（`packaging/ci-lib.sh`）作为唯一的解包入口：
目标目录由它创建、解包命令非零即失败、并逐个核对**解出来的真实内容**
（AppImage 必须有 `squashfs-root/usr/bin/backupctl` 等；客户端 deb 必须有
`usr/bin/backupctl`；服务端 deb 必须有 `usr/bin/backup-project-server`；
tar 按 `glob:*/bin/...` 核对）。不认识的发行文件一律判失败，不允许"跳过扫描"。
解包不完整时**不进入扫描阶段**，直接判失败。AppImage 用临时副本解包，不改动发行目录。
`build-release.sh` 的构建期自查同样改成 `die` 硬失败 + 内容核对。

### P1-02 完整声明被简化版覆盖

`pack_client_deb()` 先调 `build-materials.sh` 生成完整声明，随后又写了一份
6 行简介并 `install` 到**同一个路径**：

```bash
install -m 0644 "$WORK/third-party-notices-client.txt" \
  "$tree/usr/share/doc/backup-project-client/THIRD-PARTY-NOTICES.txt"
```

**实测（v0.1.0 真实制品）**：`.deb` 里的 `THIRD-PARTY-NOTICES.txt` 是
**533 字节 / 6 行**，而同族的 `tar.xz` 里是 **3307 字节 / 98 行** ——
同一份产品两个包，一个带完整声明、一个只剩简介。旧 CI 只检查文件存在与清单映射，
识别不出这种覆盖。

**修法**：删掉那次覆盖；把那段"构建期工具 + Qt 可替换"的说明**并入生成器**统一输出
（`build-materials.sh` 的声明末尾新增"构建期工具（不随包分发）"小节），
三个客户端制品由此拿到**同一份**完整声明。检查侧新增内容断言：声明必须含
"随包组件清单"与"构建期工具"两节，且**逐个列出全部组件**（不只是文件存在）。

### P1-03 / P1-04 许可覆盖的盲区

旧清单与旧检查都只数一个目录的顶层：

```bash
mapfile -t SONAMES < <(find "$LIB_DIR" -maxdepth 1 -name '*.so*' -printf '%f\n' ...)
expect_eq "..." "$(find "$libdir" -maxdepth 1 -name '*.so*' | wc -l)" "$total"
```

而客户端实际随包 **98 个共享对象**：`lib/` 65 个、`plugins/` 12 个、
`qml/` 21 个（用 v0.1.0 的 `.deb` 实测）。也就是 **33 个随包第三方二进制
从未进过清单**。另外 AppDir 的 offscreen / minimal 平台插件是在
"材料生成之后"才补齐的，顺序上也不可能被旧调用点覆盖。P1-04 是修完范围后暴露的
第二层问题：提供包查询 `find /usr/lib /lib -maxdepth 3` 看不到
`/usr/lib/<triplet>/qt6/qml/...` 这类深路径，插件对象会被判成
"不来自发行版包"。

**修法**：
1. 生成器改为接受**对象根**（AppImage `<appdir>/usr`、tar 树根、
   `usr/lib/backup-project-client`），递归清点全部 `*.so*`；
2. 逐路径分类 `file` / `symlink`，软链接解析到树内同一真实文件时只算
   **一个组件**，但每个路径都写进新文件 `licenses/shipped-objects.tsv`；
3. `coverage.tsv` 保持"一个组件一行"，新增路径级清单，两者互相约束；
4. 提供包查询放宽到深路径，并**无条件**再问一次 `dpkg -S "*/$soname"`；
5. 材料生成点移到"AppDir 内容全部就位之后"（`pack_client_appimage` 开头，
   `appimagetool` 之前），这样补齐的平台插件也在清单里；
6. 检查侧改成双向核对（制品里的每个对象 ↔ 清单里的一行；清单里的组件 ↔
   coverage 的一行），并新增：查不到提供包的组件必须在
   `packaging/licenses/UNMAPPED-COMPONENTS.txt` 里显式登记（空表 = 一个都不许）；
7. 生成器在"节点数为 0"时直接失败，避免再出现"材料生成得太早 → 空材料"。

实测：对 v0.1.0 的真实 `.deb` 对象根跑新生成器 → **98 个路径 / 98 个组件**，
本地环境（Ubuntu 24.04）下 5 个组件因缺少 Debian 12 的 ICU 72 / libjpeg62-turbo /
Qt6 QML 模块而查不到提供包（v0.1.0 自己在 Debian 12 容器里生成的清单证明这四个
库在那里是可解析的：`libicu72 72.1-3+deb12u1`、`libjpeg62-turbo 1:2.1.5-2`）；
在 CI 的 Debian 12 基线里由发行构建自行验证。

### P2-01 反引号命令替换

`build-materials.sh` 的声明块里有一句：

```bash
echo "随包组件清单（由构建容器自动生成；每个 `.so` 一行）"
```

同文件里另外三处反引号是转义的（`\````...``），这一处漏了。于是
`.so` 被当成命令执行：构建日志出现 `.so: command not found`，**并且生成的
声明里那句话变成"每个  一行"**（v0.1.0 的 `tar.xz` 里可以逐字核对到）。
修法：按同文件既有写法转义，并在生成器测试里断言 stderr 无 `command not found`、
声明里保留字面量 ``.so``。

### P2-02 Release Notes 的身份认证表述

v0.1.0 说明写成"BPSEC1 安全传输（Ed25519 身份、X25519 + HKDF…）"。按源码
（`include/secure_transport.h`、`include/bpcert.h`、
`docs/bpsec2-design.md`）实际是两件事：

- **BPSEC1**：服务端长期 **X25519** 身份密钥；客户端必须**预先配置**公钥或指纹
  （pin），没有 pin 直接拒绝，不做 TOFU；
- **BPSEC2 / BPCERT1**：**离线 Ed25519 根**签发的服务器身份证书，证书认证的正是
  握手使用的 X25519 公钥；校验失败 fail-closed，不回退 pin、不回退明文。

v0.1.1 的说明按这个区分重写；协议本身没有任何改动。

### P3-01 发行文档的时效性

- README 把 `6d1e2ba` 标成"当前 main 的真实结果" → 改成"基线
  `6d1e2ba` 上的实测结果（历史记录）"，并指向 `docs/release-packaging.md`
  与 GitHub Releases；
- `docs/04_release_and_demo.md` 的状态行与两处"本轮 `6d1e2ba`"改成
  "写作时"的表述（**不改数字、不改历史结论**）；
- `docs/release-packaging.md` 引用的 `backup_project_artifacts/releases/<版本>/`
  改成真实存在的 `dist/release/<版本>/`，并补一句当前正式发行版。

## 3. 受控的额外审查（`packaging/` + `release.yml`）

- **剩余 `|| true` 逐条分类**：`touch` 的 mtime 归一化、工具路径探测、
  `dpkg -S` 查询回退、以及各种"计数用 `grep -c`"——都不参与"发行是否通过"
  的判定（计数结果本身仍然被断言）；没有发现第二处"失败后继续报成功"的解包 / 复制 /
  安装路径。`ci-appimage-test.sh` 的解包本来就没有 `|| true`，并且随后逐项
  `expect_file` 核对内容。
- **P3-02（未修）**：`build-release.sh:345` 的
  `gzip -9n -c ... > /dev/null 2>&1 || true` 既不做校验也不产出文件，是一句
  没有效果的调用。删改它属于与本轮缺陷无关的清理，故**如实记录、不动**。
- **P3-03（随 P1-03 一并解决）**：旧的 `ci-license-coverage.sh` 解包前对**发行
  目录里的制品本身** `chmod 0755`；新的 `ci_unpack` 改用临时副本，发行目录
  保持只读。
- 未发现：错误码被覆盖、临时文件/版本信息残留、可复现性被破坏（发行 CI 内仍有
  "同一 commit 两次 clean build"比对）。

## 4. 测试（缺陷检出型）

新增 `packaging/ci-packaging-quality-test.sh`，用**真实制品**当正对照、用它的
隔离副本做负对照；只读正式制品，所有"做坏"都在 `mktemp` 目录里。

| 场景 | 预期 | 实测（本地，Ubuntu 24.04） |
| --- | --- | --- |
| 真实 v0.1.0 制品：私钥扫描 | PASS | PASS（8/0，三类制品都解包并核对内容） |
| 截断 4 KiB 的 AppImage | FAIL | 旧扫描器 **PASS(0)** / 新扫描器 FAIL(1) |
| 空文件冒充 AppImage | FAIL | 旧 **PASS(0)** / 新 FAIL(1) |
| 不认识的发行文件（`weird.zip`） | FAIL | 新 FAIL(1) |
| 修复后的 `.deb`：许可覆盖 | PASS | PASS（21/0，98 对象 / 98 组件） |
| 修复后的 `tar.xz`：许可覆盖 | PASS | PASS（21/0） |
| `.deb` 声明被 6 行简介覆盖 | FAIL | FAIL（"声明含随包组件清单/构建期工具"两条都报缺） |
| 清单漏登记一个 QML 对象 | FAIL | FAIL（路径级 98≠97、组件级 97≠98） |
| 清单多登记一个不存在的对象 | FAIL | FAIL（路径级 98≠99、组件级 99≠98） |
| `.so` 反引号回归 | PASS | 生成器 stderr 无 `command not found`；声明含字面量 ``.so``；含"构建期工具"节 |
| 生成器在 0 个对象时 | FAIL | 明确失败并提示"材料生成得太早" |

（上表里"修复后的制品"是把 v0.1.0 的真实 `.deb` / `tar.xz` 用**新生成器**
重新生成材料后重打包得到的本地端到端验证；AppImage 的对应路径由发行 CI 在
Debian 12 容器里覆盖，因为本地没有 appimagetool / linuxdeploy。）

产物与制品相关的完整回归（`test.sh` / `modern_gui_check.sh` /
`final_gate.sh` / R-01 定点）与发行 CI 结果见 v0.1.1 的证据包。

## 5. 未修复问题

| 项 | 原因 |
| --- | --- |
| P3-02 `gzip ... || true` 空调用 | 与本轮"检查假通过 / 材料不一致"无关，删改它超出范围 |
| `shellcheck` 未执行 | 环境里没有安装（`command -v shellcheck` 为空）；改用 `bash -n` 对全部改动脚本做语法检查，并逐条人工审查 `|| true` 与解包/复制路径 |
| 本地无法验证 AppImage 的"重建后材料" | 本地没有 appimagetool / linuxdeploy（与 v0.1.0 相同：正式制品只在基线容器里构建）；由发行 CI 覆盖 |
| R-01 的两条纵深防御分支无动态用例 | 与 v0.1.0 相同（前置校验后单进程不可达），不属于本轮范围 |

## 6. 统计

| 项 | 值 |
| --- | --- |
| 审查文件数 | 46（`packaging/` 全部脚本 + `release.yml`），另核对 README 与 3 份发行文档 |
| 发现问题 | 9（P1×4、P2×2、P3×3） |
| 已修复 | 8（P1×4、P2×2、P3×1）；未修 1（P3-02，已说明） |
| 改动文件 | 9 个修改（+344/−112）+ 2 个新增（144 行）＝ 11 个文件、+488/−112 |
| 新增测试 | 1 个新套件（`ci-packaging-quality-test.sh`，~20 条断言）+ 覆盖检查新增 6 类断言 + 生成器 1 条守卫 |
| 接口 / 数据格式影响 | **无**：没有改产品源码、公共头文件、归档格式、链格式、协议或用户数据结构；改动全部在打包 / 验收 / 文档层 |
| 产品编译警告 | 未受影响（本轮没有改 C++）；仍以发行 CI 与 `final_gate.sh` 的 0 warning 为准 |

# 最终验证报告（2026-10-07）

> 状态：**当前有效**。基线是产品代码冻结后的 main（commit
> `6d1e2ba71a48862927f23442604d6bd4b430e62d`，即 PR #27 合并后的 main）。
> 本文记录的是**一次真实运行**的结果，不是目标值，也不是从别处抄来的数字。

早先那份 Sprint 1+2 的验证记录见 `docs/testing/sprint12_validation_20260917.md`，
它锁在 `5e1e54e` 上，只覆盖当时的范围，不要拿它代表现在。

---

## 1. 环境

| 项 | 值 |
| --- | --- |
| 发行版 | Ubuntu 24.04.4 LTS |
| 编译器 | g++ 14 |
| clang-format | 18.1.3 (Ubuntu 1ubuntu1) |
| Qt | 6.4.2 |
| CPU | 8 核 |
| 构建 | GNU Make，`make -j8` |

## 2. 从零构建

```bash
make clean
make -j8 all test-fixtures server cert-tool gui-all
```

| 项 | 结果 |
| --- | --- |
| `make clean` | exit 0 |
| 构建 | exit 0 |
| 编译器警告 | **0** |
| 编译错误 | **0** |

第一次构建特意从 `make clean` 开始。这台机器上出现过时钟偏移导致 `make` 认为目标
文件比源文件新的情况（改了源文件却没重编），增量构建的结果不能作为验收证据。

## 3. 静态门禁

| 检查 | 命令 | 结果 |
| --- | --- | --- |
| 代码格式 | `./scripts/lint.sh` | exit 0（clang-format `--dry-run --Werror`，覆盖 app / src / include / server / tools / ui/desktop / ui/modern） |
| 注释率 | `python3 scripts/comment_ratio.py` | 21.40%（183 个文件 / 91438 物理行 / 19566 注释行） |
| 行宽 | `python3 scripts/source_style_check.py` | 可处理超 80 列 = **0**（注释 0、代码 0），放行例外 0 |

## 4. 功能与质量套件

| 套件 | 命令 | 结果 | 耗时 |
| --- | --- | --- | --- |
| 功能回归 | `bash scripts/test.sh` | **PASS=279 FAIL=0** | 21 s |
| Modern GUI | `bash scripts/modern_gui_check.sh` | **通过 692 项、失败 0** | 245 s |
| 完整门禁 | `bash scripts/final_gate.sh` | **49/49 PASS，failed suites = 0** | 2852 s |

`final_gate.sh` 的 49 行 `GATE` = 3 行构建记录（build / sanitize-build / gui-rebuild）
+ 46 个套件。这一次 49 行全部 exit=0，没有任何一个套件失败。

46 个套件里比较慢的几个（同一次运行的耗时）：archive_pipeline 385 s、
quality 373 s、network-sanitize 264 s、network 254 s、modern_gui 195 s、
secure-transport-sanitize 179 s、server-profile 12 s。

## 5. 发布打包冒烟

两条 staging 都在**干净工作树**上运行 —— 脚本会拒绝从有未提交改动的工作树打包，
这是刻意的（避免把"和 Git 历史对不上"的包发出去）。

| 脚本 | 结果 | 产物 |
| --- | --- | --- |
| `scripts/stage-client-release.sh` | exit 0 | `dist/client`，**9 个文件**；冒烟：`ldd` 无 not found、`backupctl --help`、GUI offscreen 启动正常 |
| `scripts/stage-server-release.sh` | exit 0 | `dist/server`，**13 个文件**；冒烟：`ldd` 无 not found、`backup-server --help`、`backup-cert-tool --help`、包内文件清单符合预期、包内没有任何根密钥内容 |

## 6. 失败项

| 项 | 数量 |
| --- | --- |
| 编译警告 / 错误 | 0 / 0 |
| `test.sh` 失败 | 0 |
| `modern_gui_check.sh` 失败 | 0 |
| `final_gate.sh` 失败套件 | 0 |
| 静态门禁失败 | 0 |
| release staging 失败 | 0 |

## 7. 复现步骤

```bash
cd <repo>
git switch docs/course-closeout        # 或合并后的 main

make clean
make -j8 all test-fixtures server cert-tool gui-all

./scripts/lint.sh
python3 scripts/comment_ratio.py
python3 scripts/source_style_check.py

bash scripts/test.sh
bash scripts/modern_gui_check.sh
bash scripts/final_gate.sh             # 约 45 分钟

bash scripts/stage-client-release.sh   # 需要干净工作树
bash scripts/stage-server-release.sh
```

## 8. 说明与边界

- 本文的数字来自 2026-10-07 的这一次运行。后续如果改了产品代码，这些数字就过期了，
  需要重跑；只改文档不影响它们（本次收口分支就只改文档）。
- **性能与压力没有专项报告**：`tests/performance/` 只有一个占位文件，性能数字
  散在几个套件内部，没有基线也没有阈值。这是已知空白，不是漏跑。
- **需要真实 ECS 的用例本机不跑**：`scripts/aliyun_*.sh`、`scripts/pr22_ecs_e2e.sh`、
  `scripts/pr23_ecs_phase3..9*.sh`。网络相关的验收在本机走的是回环脚本
  （`network_test.sh` / `remote_incremental_test.sh` / `bpsec2_loopback_e2e.sh`），
  它们是真进程 + 真 TCP + 真 SQLite，只是不经过公网。
- **GUI 不做像素比对、不模拟鼠标**：`modern_gui_check.sh` 用 offscreen 真实启动、
  按 objectName 驱动、并对源码做静态契约断言；渲染结果的一致性靠截图人工看。
- **qmllint 是登记放行而不是零告警**：脚本维护一份已知告警清单，出现未登记告警才失败。
- 这一轮同时重新生成了 GUI 截图（在 `tests/output/screenshots/`，该目录 gitignore）；
  正式引用的 8 张已挑选进入 `docs/images/gui/`。

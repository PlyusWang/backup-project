# PRV-41 closure：超长 child path 的合同复核

这份文档是 PR #23 closure round 的 PRV-41 复核记录。它回答一件事：
`scripts/test.sh` 里那条一直红的 PRV-41，到底是**产品缺陷**、**测试契约写错**，
还是**端到端构造本身不可成立**。

结论先写在前面：

> **原始用例想验证的合同是成立的，而且产品行为是对的。**
> 红的原因是**夹具把源目录放在了一条过长的路径前缀下**，
> 于是内核的 `ENAMETOOLONG` 先于项目自己的长度守卫触发。
> 既不是产品 bug，也不是「这条端到端构造不可能」——
> 此前的注释把这两件事都写错了，本轮一并纠正。

| 项 | 结论 |
|---|---|
| PRV-41 原始期望是否成立 | **成立**（可达，见 §6） |
| 产品 bug | **无** |
| 测试契约 bug | **有**（夹具路径前缀，外加一段错误注释） |
| `kMaxArchivePathLength` 守卫是否有 unit 覆盖 | **有，且一直是绿的**（见 §7） |
| 是否需要改产品代码 | **不需要** |
| 是否需要 fd-relative / openat 遍历 | 需要才谈得上支持超长绝对路径；**本轮不做**，只留 TODO（§11） |

---

## 1. PRV-41 想验证什么

一句话：**归档内相对路径的长度上限，是「遍历阶段」的硬边界。**

具体三条：

1. 某条目的 **archive 相对路径** 超过 `kMaxArchivePathLength`（`include/archive_path.h:25`，值 4096）时，
   整次 backup / preview 失败，报项目自己的原文 `Archive path too long: <archive 相对路径>`；
2. 这条判断发生在 **lstat 之前、Filter 之前**（历史语义，`include/source_tree_walker.h:45-61` 的决策顺序，
   实现见 `src/core/source_tree_walker.cpp:268-275`）——所以**即使规则本来会把该条目排除，也照样失败**；
3. preview 与 backup 走**同一份遍历**（`src/core/backup_preview.cpp:129` 与 `src/core/tree_scanner.cpp:210`
   都调用 `WalkSourceTree`），所以两边必须报出**同一句**原文，退出码都是 1，且不留半个归档。

第 1 条是「守卫本身」，第 2 条是「early 语义」，第 3 条是「两侧一致」。三条都是真合同。

---

## 2. 旧夹具是怎么构造 `archive 相对路径 > 4096` 的

`scripts/test.sh`（旧版 1966-2011 行）：

```
PLONG="$PG/too-long/src"                 # $PG = $TEST_ROOT/preview/grammar
rm -rf "$PG/too-long"; mkdir -p "$PLONG"
python3 - "$PLONG" <<'PYEOF'
import os, sys
src = sys.argv[1]
os.chdir(src)
component = 'd' * 190
relative = 0
while relative < 3900:
    os.makedirs(component, exist_ok=True)
    os.chdir(component)
    relative += 1 + len(component)
open('v' * 200, 'w').write('x')
PYEOF
run_preview_cli "$PLONG" --exclude 'name:vvv*'
"$BACKUPCTL" ... backup "$PLONG" --exclude 'name:vvv*' ...
# 断言：两边 exit=1，且两边 stderr 都含 "Archive path too long"
```

形状可以精确算出来（实测输出一致）：

* 21 层 `d`×190 的目录链：第 k 层的 archive 相对长度是 `190 + (k-1)×191`；
  最深一层（k=21）为 **4010**；
* 最深处一个 `v`×200 的文件：archive 相对长度为 **4211**，确实 > 4096；
* 构造全部用 `chdir` + 相对名字，所以**创建过程**中没有任何一次 syscall 超过 `PATH_MAX`——
  这一点旧夹具做对了。

问题不在**创建**，而在**遍历**：产品把「源目录路径 + 相对路径」拼成一个完整字符串交给 syscall。

---

## 3. 谁会先触发：守卫，还是 `ENAMETOOLONG`

### 3.1 推导

设：

* `P = strlen(source_directory)`（交给 CLI 的源目录路径长度，夹具里是绝对路径）；
* `A = strlen(entry 的 archive 相对路径)`；
* walker 交给 syscall 的磁盘路径长度 = `P + 1 + A`（`FileSystem::JoinPath`，
  `src/filesystem/file_system.cpp:148-154`，父路径非空且不以 `/` 结尾时拼一个 `/`）。

`PATH_MAX = 4096` 且**包含结尾 NUL**，所以 `lstat` 在 `P + 1 + A > 4095` 时失败并置 `errno = ENAMETOOLONG`。

walker 的顺序是「先查长度守卫（`A > 4096`），再 `lstat`」。因此守卫能在第 k 层触发，当且仅当：

```
对所有祖先层 j < k：  P + 1 + A_j <= 4095      （祖先必须真的 stat 得到）
第 k 层：             A_k > 4096               （守卫的条件）
```

⇒ **守卫可达的充要条件是 `P <= 4094 - A_最深祖先`。**

对旧夹具（`A_最深 = 4010`）：**`P <= 84`**。

而旧夹具实际的前缀是：

```
<repo>/testdata/preview/grammar/too-long/src
= 72 + len("/testdata/preview/grammar/too-long/src") = 72 + 38 = 110
```

`110 > 84` —— 所以 `lstat` 先炸。**这就是红的全部原因。**

### 3.2 实测（不是推理）

同一形状的夹具，只改源目录路径前缀长度，跑 `backupctl preview`：

| 源路径前缀 `P` | 退出码 | 首行错误 | 判定 |
|---|---|---|---|
| 14 | 1 | `Error: Archive path too long: dddd…` | 项目守卫 |
| 60 | 1 | `Error: Archive path too long: dddd…` | 项目守卫 |
| 100 | 1 | `Error: Failed to inspect path: /tmp/ppp…: File name too long` | 内核 `ENAMETOOLONG` |
| 110（旧夹具真实值） | 1 | 同上 | 内核 `ENAMETOOLONG` |
| 130 / 190 / 240 | 1 | 同上 | 内核 `ENAMETOOLONG` |

实测分界落在 60 与 100 之间，与推导的 84 一致。

**结论：`ENAMETOOLONG` 不是「任何情况下都先触发」，只在 `P` 太大的时候先触发。**
旧夹具恰好落在错的一侧。

---

## 4. 产品有没有承诺「超长绝对路径 + openat 逐级下钻」这种能力

**没有。** 三处证据：

1. `include/archive_path.h:24-25` 的原文是「归档内路径的长度上限，**与 Linux PATH_MAX 同量级**」——
   是「量级相当」，不是「不受 PATH_MAX 约束」；
2. `include/source_tree_walker.h:23-39` 列举这份遍历负责的事情，只有
   「archive 相对路径构造（'/' 分隔，root 是 '.'）与路径长度校验」，没有任何 fd 相对遍历的承诺；
3. 代码里根本没有这条路径：`src/`、`include/`、`server/` 全文搜索 `openat` / `fchdir` 无命中
   （`include/backup_catalog.h:38` 反而明确写着 dirfd + openat 方案「当前版本没有这么做」）。

所以「绝对路径超出单个 syscall 能表示的范围、但还能遍历」**不是当前产品契约的一部分**。
它是一条**已知边界**，不是回归。

---

## 5. walker 到底把什么交给了 syscall

全部是拼接出来的**完整路径字符串**，没有任何 dirfd：

| 位置 | 调用 | 参数 |
|---|---|---|
| `src/core/source_tree_walker.cpp:101` | `::lstat` | `source_directory`（源根，用户给的绝对路径） |
| `:192` | `::lstat` | `disk_directory`（逐层拼出来的绝对路径） |
| `:220` | `::opendir` | `disk_directory` |
| `:264` | `FileSystem::JoinPath` | `disk_directory` + `name` → `child_disk` |
| `:283` | `::lstat` | `child_disk`（绝对路径） |

递归本身是**字符串递归**（`WalkDirectory(child_disk, child_archive)`），
所以每往下一层，交给内核的路径就长 `1 + len(name)`。这就是 §3 那个不等式的来源。

---

## 6. 不重写 walker，原始端到端期望可达吗

**可达。** 上一轮写在 `scripts/test.sh` 里的注释说「无论如何，那个夹具的绝对路径都至少是
`<前缀> + 4097 + 溢出 + 文件名`……对任何仓库位置都必然超过 PATH_MAX」——
**这段推理是错的**，它把「最深那一层」当成了「守卫触发的那一层」。

守卫在**第一个** `A > 4096` 的层触发，而不是最深一层。只要源目录前缀 `P <= 4094 - A_最深祖先`，
从根到那一层的每一次 `lstat` 都还在 `PATH_MAX` 以内，守卫就正常触发。

实测：`P = 14` 与 `P = 60` 时，preview 与 backup **都**报出项目自己的 `Archive path too long`。

所以本轮不需要动 walker，也不需要动任何产品代码。

---

## 7. `kMaxArchivePathLength` 守卫已有的 unit-level 覆盖

**有，而且一直是绿的。** `tests/unit/backup_preview_test.cpp:1191` 起的
`PREV 11. 超长 child path：历史 early 语义保留` 就是它：

* 用 `chdir` + 相对名字建 21 层 `d`×190 的链，再放一个 `v`×200 的文件（与 CLI 夹具同形）；
* 断言 `deepest.size() - 1 > bp::kMaxArchivePathLength`（`:1237-1240`）——
  确认构造出来的 archive 相对路径**真的**超过上限；
* 断言 `PreviewBackupSelection` 与 `ScanSourceTree` 两边都失败且**同一句**原文（`:1251-1254`）；
* 断言原文里含 `Archive path too long`（`:1255-1257`）；
* 用的是 `--exclude name:vvv*` 同款规则，验证「被排除也照样失败」。

它之所以一直是绿的、CLI 那条一直是红的，差别只在**夹具根目录**：

```
unit : TempRoot() = /tmp/bp-sprint-<pid>          (tests/unit/test_support.h:74)
       source      = <上面>/backup-preview-pathlen/src   ->  P ≈ 45   <= 84  ✅
cli  : TEST_ROOT   = <repo>/testdata               (scripts/test.sh:38)
       source      = <上面>/preview/grammar/too-long/src ->  P = 110   >  84  ❌
```

这正是「同一份合同、两种夹具位置、两种结果」的完整解释。

另外 `tests/unit/backup_preview_test.cpp:886-897` 在 grammar 层单独覆盖了
`IsValidArchivePath(..., kMaxArchivePathLength, ...)` 对超长路径的拒绝。

实测：`backup_preview_test` 182/182 通过，其中 `PREV PATH-LEN` 4/4。

---

## 8. 最终合同（拆成两半，各自显式构造）

把一件事拆成两件事，各自用**受控的前缀长度**去构造，不再靠「仓库在哪」碰运气。

两半用的是**同一个树形**，只有源目录前缀不同：

```
20 层 d×190           -> archive 3819
+ 1 层 e×64            -> archive 3884   （最深目录）
+ 一个 v×250 的文件     -> archive 4135   （> 4096，溢出存在）

守卫要 lstat 得到最深那层： prefix <= 4094 - 3884 = 210
内核先炸：                prefix >= 211
```

两半正好互补：

| | 源目录前缀 | 期望原文 | 期望退出码 |
|---|---|---|---|
| **A. 守卫（early 语义）** | `/tmp/bp41-short/src` = **19** | `Archive path too long: ddd…` | 1（preview 与 backup 同一句） |
| **B. 文件系统路径上限** | `/tmp/LLLL…(240)/src` = **249** | `Failed to inspect path: <path>: File name too long` | 1（preview 与 backup 同一句） |

两半都断言：

* 退出码**恰好是 1**（不是 124 超时、不是 >=128 被信号打死）；
* preview 与 backup 的**首行完全相同**（同一份遍历、同一句原文）；
* 失败后**仓库目录是空的**（没有半个归档、没有 partial success）；
* 前缀长度这一「自变量」被**显式断言**：A 必须 `<= 210`，B 必须 `>= 211`，
  否则用例以「夹具前提不成立」失败，而不是悄悄换成另一种失败。

B 这一半是**新增覆盖**：旧用例从来没有把「内核路径上限」当成一条独立合同来断言，
它只是碰巧撞上；现在它被显式命名、显式构造、显式断言。

---

## 9. 回归证据

| 要求 | 覆盖 |
|---|---|
| archive 路径 > 项目上限的守卫仍有明确 PASS 覆盖 | `PREV PATH-LEN`（unit，4 条）+ PRV-41 A（CLI E2E） |
| 真实文件系统路径过长返回受控错误 | PRV-41 B（preview + backup 两侧） |
| preview 与 backup 一致 | A、B 两半都断言首行相同；两者共用 `WalkSourceTree` |
| 正常深目录不受影响 | `ARC-13 directory depth 24`（`scripts/test.sh:698-706`） |
| 普通短路径不受影响 | 全套 `scripts/test.sh` 其余用例 |
| unsupported / special-file 不受影响 | `PRV-39` / `PRV-40`（socket 优先级）与 `H. UNSUP` 分区 |

---

## 10. 为什么不需要动产品代码

两条行为都是**正确且已文档化**的：

* 归档内路径超过 4096 → 项目自己的错误（fail closed，early 语义）；
* 源目录的**磁盘**路径超过内核 `PATH_MAX` → 把内核的 errno 原样报出来，
  同样 fail closed、不崩不挂、不产出半个归档。

后一条的根因是「没有 fd 相对遍历」，那是一次**架构变更**，不是一次修复。
把它塞进本 PR 会让这个 PR 从「签名身份 + 公网直连」变成「遍历重写」，
且会同时影响 restore、增量、catalog 三处路径语义。**因此本轮不做。**

---

## 11. FUTURE（本轮不实现，只登记）

```
FUTURE: fd-relative traversal / openat-based deep-path support
```

要做的事：把 `WalkDirectory` 从「字符串递归」改成「dirfd 递归」
（`openat(dirfd, name, O_RDONLY|O_NOFOLLOW|O_DIRECTORY)` + `fstatat` + `fdopendir`），
让交给内核的始终是**单个 component**，从而真正解耦「磁盘路径长度」与「归档路径长度」。
涉及的调用点：`src/core/source_tree_walker.cpp:192/220/283`；
同类问题在 `include/backup_catalog.h:38` 与 `:220` 已经被记为已知边界。

前置条件（都在本 PR 之外）：

* restore 侧的 `ResolveUnderRootNoSymlinkAncestors`（`include/archive_path.h:89`）同样需要 fd 版本；
* 增量 delta 的 tombstone 落盘路径也是字符串拼接；
* 需要一套「深到超过 PATH_MAX」的端到端夹具，且要能在 `rm -rf` 也失效的前提下清理干净。

---

## 12. 一句话总结

> PRV-41 的红不是产品错了，是**夹具站错了位置**：
> 它把源目录放在了 110 字符的前缀下，而这条夹具形状要求前缀 <= 84，
> 于是内核对路径长度的检查抢在项目自己的守卫前面。
> 该用例想验证的每一条合同都是真的，也都可以端到端跑绿；
> 本轮把它拆成「项目守卫」与「内核上限」两半，各自显式构造前缀并显式断言，
> 产品代码一行未改。

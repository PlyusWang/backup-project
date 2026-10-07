# 基础 CLI 使用说明

> 描述对象：当前 `main` 上的产品 CLI `backupctl`（构建产物 `build/backupctl`）。
> 归档格式见 `docs/format/`，筛选规则见 `docs/filter_usage.md`，定时备份见
> `docs/scheduled_backup_usage.md`，网络备份见 `docs/network_backup_usage.md`。

## 1. 实现范围

- 普通文件的递归备份与恢复（字节级一致，含空文件与二进制文件）；
- 普通目录（含空目录、多级嵌套目录）的备份与恢复；
- 符号链接按链接本身保存（不跟随）；硬链接只存一份 payload，恢复时重新指向
  归档里的第一个副本；
- 元数据：mode（07777，含 setuid / setgid / sticky）、uid / gid、
  mtime（秒 + 纳秒）、设备号；
- FIFO、字符设备、块设备按条目类型保存并在恢复时重建（设备节点需要
  CAP_MKNOD，建不出来就明确失败并说清原因，不会静默降级成普通文件）；
- 文件过滤：11 个字段 name / path / stem / ext / type / size / mtime /
  uid / gid / user / group，语法见 `docs/filter_usage.md`；
- 打包 mypack / ustar / fast-ustar、压缩 none / huffman / lzss-huffman、
  加密 none / aes-256-ctr-hmac-sha256 / des-cbc-hmac-sha256；
- 定时备份、实时备份（inotify）、网络备份（backup-server + BPNET1）；
- 文件名支持空格与 UTF-8（中文）；
- 错误路径明确报错并返回非 0：源不存在、源不是目录、仓库不存在、
  仓库缺少 data/、目标已存在且非空、不支持的文件类型、权限不足、
  无法创建目录等。

## 2. 边界

- 产品 CLI 与 GUI 都不提供"把归档写到任意路径"的入口：备份写进配置好的仓库，
  文件名由程序生成，见第 4 节；
- socket 不进入归档：只要它没有被 Filter 明确排除，整次备份就明确失败并返回
  非 0（既不静默跳过，也不跟随它）；
- Archive v0.1（`BKPARCH`）只保留**读**兼容：产品当前一律写出 v2 容器
  （`BKPCNT2`），旧归档只要在仓库里就仍然恢复得回来；
- 加密口令只从 `/dev/tty` 交互读取，不接受参数、环境变量或管道输入。

## 3. 构建

```bash
./scripts/build.sh
# 或
make
# 清理后重建
make clean && make
```

产物：`build/backupctl`（产品 CLI）。

测试夹具 `build/archive-cli` **不在默认构建目标里**，需要时显式构建：

```bash
make test-fixtures        # 产出 build/archive-cli
```

## 4. CLI 用法

```bash
./build/backupctl config repository set <仓库目录>   # 先配置仓库
./build/backupctl preview <source_directory> [--include R]... [--exclude R]...
./build/backupctl backup <source_directory> [--pack ...] [--compression ...] [--encryption ...] [--include R]... [--exclude R]...
./build/backupctl repository list                     # 列出仓库里的归档
./build/backupctl repository delete <file_name>
./build/backupctl restore <file_name> <destination_directory>
./build/backupctl schedule show | set | enable | disable | run | history | watch
./build/backupctl realtime show | set | enable | disable | watch | history
./build/backupctl remote ping | register | login | list | upload | download | backup | restore | delete | delete-account
./build/backupctl --help
```

每个子命令的完整参数见 `backupctl --help`（用法错误时也会把整份 usage 再打一
次）。定时备份的详细说明见 `docs/scheduled_backup_usage.md`，网络备份见
`docs/network_backup_usage.md` 与 `docs/remote_incremental.md`。

产品 CLI 与 Modern GUI 使用**同一套业务模型**：

* `backup` 把归档写进**配置好的备份仓库**，文件名由程序生成
  （`<source-base>_YYYYMMDD_HHMMSS.bak`，冲突时追加 `_001`…）。
  调用方不能指定归档路径——那是 GUI 也没有的能力，因为"备份写到哪就是哪"
  不是这个产品的业务概念。
* 产物始终是 v2 容器（`BKPCNT2`）：pack / compression / encryption 只是
  选择怎么打包、压缩、加密，详见 `docs/format/archive_v2_container.md`。
* `restore` 的 `<file_name>` 必须是仓库里的**单组件**名字
  （`repository list` 给出的那个），拒绝 `/`、`\\`、`..`、
  绝对路径与符号链接。
* 历史遗留的 `BKPARCH`（Archive Format v0.1）归档仍然**读得回来**：
  只要它作为合法记录放在仓库里，`restore` 就能恢复它。
* `preview` 是**只读**的：用同一组筛选规则列出"哪些条目会进归档"，不创建
  归档、不需要仓库、不改 config / schedule / history。它与 Modern GUI 的
  Manual Backup 预览调用同一个核心（`backupproject::PreviewBackupSelection`），
  所以两边给出的条目集合、顺序、失败语义与截断行为完全一致。
  预览会**完整走完与真实备份完全相同的那次遍历**（同一份实现：同样的
  `lstat` / `opendir` / `readdir`、同样的 lexical DFS 顺序、同样的失败语义，
  被规则剪枝的子树同样不递归），但最多**列出** 300 个 preview entries；超出时
  会在输出里分开写出三个数字：完整遍历的匹配总数、窗口大小（preview entries）
  与窗口里实际列出的匹配项数。所以"第 301 个条目是 socket"这类情况不会被
  窗口掩盖：预览会直接报 `Backup would fail unless this entry is excluded.`
  并以 1 退出。

### 归档格式的测试夹具

`build/archive-cli` 是**测试专用**的可执行文件，不是产品命令、不出现在
`backupctl --help` 里、不在默认构建目标里（要 `make test-fixtures`）、
也不参与 GUI/CLI parity：

```bash
./build/archive-cli backup <source_directory> <backup_file> [filter...] [pipeline...]
./build/archive-cli restore <backup_file> <destination_directory>
```

它让"把归档写到指定路径 / 从不存在的任意路径恢复"这类**格式回归**继续测得到
真实的读写器与引擎（不给 pipeline 选项时产 legacy v0.1，给了就走 v2）。

## 5. 完整示例

```bash
./build/backupctl config repository set ~/backups
./build/backupctl preview testdata/source --include 'ext:txt'
./build/backupctl backup testdata/source
./build/backupctl repository list                    # 拿到程序生成的名字
./build/backupctl restore <上面列出的 file_name> restored
diff -r testdata/source restored   # 应无差异
```

## 6. 目标路径已存在时的策略

- 归档文件已存在 → 备份失败，拒绝覆盖、拒绝截断；
- 归档文件所在目录不存在 → 会自动补建（mkdir -p 语义）；
- restore 的 destination 已存在且非空 → 恢复失败，拒绝覆盖；
- 已存在的**空目录** → 允许直接使用。

## 7. 退出码

| 退出码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 操作失败（路径、文件类型、I/O 等） |
| 2 | 命令行用法错误 |
| 3 | 已经有另一个 backup-project 实例在跑（GUI 或 CLI） |

**整个产品同一时刻只允许一个进程**：Modern GUI、`backupctl` 与历史遗留的
Qt Widgets GUI 共用同一把按 Unix UID 定位的应用锁。已经有实例在跑时，任何
业务命令（包括只读的 `preview`）都以 3 退出，并且在碰配置 / 仓库 / 计划之前
就退出。`--help` 是唯一的例外：看用法不需要抢锁。

`--config-file` / `--schedule-file` 可以覆盖配置与计划存储的位置，但**换不掉
那把锁**——它只取决于 UID。

## 8. 测试与质量检查

```bash
make test       # 端到端 round-trip + 错误路径测试
./scripts/lint.sh
valgrind --leak-check=full --show-leak-kinds=all ./build/backupctl preview <src>
valgrind --leak-check=full --show-leak-kinds=all ./build/backupctl backup <src>
valgrind --leak-check=full --show-leak-kinds=all ./build/backupctl restore <file_name> <dest>
make sanitize   # ASan + UBSan 构建，产物 build-sanitize/backupctl
```

## 9. 主要源文件及职责

| 文件 | 职责 |
| --- | --- |
| app/backupctl.cpp | CLI 参数解析、结果输出、退出码 |
| include/backup_engine.h / src/core/backup_engine.cpp | Backup / Restore 高层流程（校验 + 编排） |
| include/archive.h / src/archive/archive.cpp | Archive Format v0.1 的打包与解包 |
| include/file_system.h / src/filesystem/file_system.cpp | 路径检查、目录创建、目录树复制（CopyTree，GUI 与归档不再直接使用，保留备用） |
| scripts/test.sh | 自动测试脚本 |

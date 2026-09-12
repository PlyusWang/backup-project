# 文件筛选使用说明（PR #9）

> 状态：PR #9 已实现，行为由 scripts/test.sh 的 FIL-01 ~ FIL-37 固定。  
> PR #9 完成并通过测试后，第一部分改为“已实现”；只有真实可用且有测试覆盖的能力才能放入“已实现”。  
> 本项目参考 Everything 的筛选思想，但不追求兼容 Everything 的完整查询语言。

## 一、PR #9 基础功能（已实现）

PR #9 只解决备份中最常见的问题：哪些文件进入 `.bak`，哪些文件不进入。

### 1. Include / Exclude 规则

每条规则只有两种动作：

- `include`：允许匹配项进入备份；
- `exclude`：阻止匹配项进入备份。

规则优先级固定为：

1. `exclude` 优先；
2. 如果存在至少一条 `include`，普通文件必须命中至少一条 `include` 才进入备份；
3. 如果没有任何 `include`，默认包含所有未被 `exclude` 的普通文件；
4. 没有任何规则时，行为必须与 PR #8 完全一致。

示例：

```text
exclude path:**/build/**
exclude path:**/.git/**
exclude ext:tmp;log
include ext:cpp;h;hpp
```

### 2. 按名称、路径、主文件名、扩展名筛选

支持：

```text
name:
path:
stem:
ext:
```

含义：

| 字段 | 含义 |
| --- | --- |
| `name:` | 最后一段文件名或目录名 |
| `path:` | 相对于备份源根目录的完整相对路径 |
| `stem:` | 去掉扩展名后的主文件名 |
| `ext:` | 扩展名 |

示例：

```text
exclude name:Thumbs.db
exclude path:**/.git/**
include stem:report*
include ext:cpp;cc;cxx;h;hpp
```

路径统一使用归档内部的 `/` 分隔符，不依赖绝对磁盘路径。

### 3. 文件 / 目录类型

支持区分：

```text
type:file
type:folder
```

目录排除规则可以剪掉整个子树，例如：

```text
exclude type:folder path:**/build
exclude type:folder path:**/.git
```

PR #9 中 `include` 主要决定普通文件是否进入归档；目录仍作为恢复目录结构所需的结构项保留，除非被明确排除。这样不会因为只包含 `*.cpp` 就丢失其父目录结构。

源目录根条目 `.` 始终保留。

### 4. 通配符

支持：

| 语法 | 含义 |
| --- | --- |
| `*` | 匹配 0 个或多个字符，但不跨 `/` |
| `?` | 匹配恰好 1 个字符，但不跨 `/` |
| `**` | 匹配 0 个或多个字符，可以跨 `/` |

示例：

```text
*.log
test?.txt
build/*
**/build/**
src/**/*.cpp
```

### 5. 扩展名列表

`ext:` 支持用分号表示多个扩展名：

```text
ext:cpp;cc;cxx;h;hpp
ext:jpg;png;webp
```

分号列表表示“任意一个扩展名匹配”。

### 6. 文件大小

支持基础大小比较和范围：

```text
size:<1MB
size:<=100MB
size:>4KB
size:>=1GB
size:1MB..10MB
```

PR #9 必须固定：

- 支持哪些单位；
- 单位是否按 1024 进制；
- 边界是否包含；
- 非法值如何报错。

这些语义必须写入文档并由测试固定。

### 7. 修改时间

支持基础 `mtime:`：

```text
mtime:today
mtime:yesterday
mtime:7days
mtime:2026-09-01
mtime:2026-09-01..2026-09-12
```

PR #9 只支持修改时间，不扩展到创建时间、访问时间等平台差异较大的字段。

测试中应尽量使用固定绝对时间，避免依赖执行时刻造成 flaky。

### 8. CLI

CLI 必须支持重复添加规则，不要求用户把所有规则写成一个复杂表达式。

建议形式：

```bash
backupctl backup <source_directory> <backup_file> \
  --include 'ext:cpp;h;hpp' \
  --exclude 'path:**/build/**' \
  --exclude 'ext:tmp;log'
```

`--include` 与 `--exclude` 都允许重复出现。

非法规则必须：

- 明确报错；
- 返回非 0；
- 不留下半成品 `.bak`。

### 9. Classic GUI / Modern GUI

两套 GUI 都增加基础筛选入口，但不做 Everything 那样的完整高级搜索界面。

最低要求：

- 可以新增 Include 规则；
- 可以新增 Exclude 规则；
- 可以删除规则；
- 能看到当前规则列表；
- 备份时把规则传给同一个 C++ Filter 核心；
- 无规则时保持 PR #8 行为。

Classic GUI 与 Modern GUI 不允许各自写一套匹配逻辑。

### 10. 核心架构原则

Filter 应是 Archive 之前的一层：

```text
源目录
  ↓
Filter
  ↓
ArchiveWriter
  ↓
.bak
```

Filter 只负责“是否进入备份”，不负责：

- 压缩；
- 加密；
- 增量；
- 网络；
- 完整性校验。

### 11. 实现细节与语义固定

以下是 PR #9 实际实现并已被测试固定的细节，避免口头理解与代码行为不一致。

**规则内部是"子句 AND，规则之间 OR"。** 一条规则可以写多个子句，用空白分隔，
只有空白后面紧跟已知字段名（`name:` `path:` `stem:` `ext:` `type:` `size:` `mtime:`）
才切分，所以 `name:my file.txt` 里的空格不会被误切，而
`type:folder path:**/build` 会正确切成两个子句。不做 NOT、括号与优先级。

**目录剪枝的精确语义。**

- 目录命中 `exclude` 时才剪枝；剪掉之后整棵子树不再扫描，
  子树里的 symlink / FIFO / socket / 设备也因此不再触发"不支持的类型"失败。
- `path:` 规则额外按"目录前缀"匹配一次（相当于把目录路径末尾补一个 `/` 再比），
  所以 `exclude path:**/build/**` 会直接剪掉 `build` 目录本身。
- `include` 不剪枝：目录始终作为结构项保留，即使里面没有任何文件被 include。
  因此"只 include `ext:cpp`"不会丢掉父目录结构。
- 源目录根条目 `.` 永远保留；所有文件都被过滤掉时，归档仍然合法，
  恢复出来是一棵只有目录骨架的空树。

**size 语义。** 支持 `<` `<=` `>` `>=` 与 `a..b` 闭区间；
单位 `B` / `KB` / `MB` / `GB` 按 1024 进制；数值溢出会在解析阶段直接报错。
`size:` 只对普通文件生效，目录一律不命中。

**mtime 语义。** 只读 `st_mtim`。`today` / `yesterday` 取**本地时区**的整天区间；
`Ndays` 表示"最近 N × 24 小时"；`YYYY-MM-DD` 表示那一整天；
`A..B` 表示从 A 当天 00:00 到 B 当天 23:59:59（含首尾）。
测试里用 `TZ=UTC` + 固定时间戳，避免跨时区与跨零点造成 flaky。

**特殊文件。** 没有被 exclude 的 symlink / FIFO / socket / 设备仍然让整次备份失败，
不会因为"存在 include 规则"就被静默跳过——那会让"备份成功"变成假话。

**退出码。** 非法规则属于命令行错误：CLI 返回 2，明确报错，且不留下半成品 `.bak`。

**实现位置。** `include/filter.h` + `src/filter/filter.cpp` 是唯一的匹配实现；
CLI、Classic GUI、Modern GUI 都调用它，界面不实现任何 glob。

## 二、未实现 / 后续计划

以下能力明确不属于 PR #9。它们需要记录，但不能为了“更像 Everything”而扩大本轮范围。

> 这些条目的**技术债、设计取舍、前置依赖**统一记在
> [docs/backlog/filter_future.md](backlog/filter_future.md)，本节只保留能力清单。

### 1. 完整布尔表达式语言

PR #9 不实现完整的：

```text
AND
OR
NOT
<分组>
```

也不实现复杂运算符优先级解析。

本轮用“多条 Include / Exclude 规则”表达常见需求。

### 2. Regex

暂不支持：

```text
regex:name:
regex:path:
regex:content:
```

PR #9 先把 Glob 规则做稳定。Regex 留到后续。

### 3. Everything 式高级匹配修饰符

暂不支持：

```text
case:
diacritics:
ignore-punctuation:
ignore-whitespace:
prefix:
suffix:
whole:
whole-words:
start-with:
end-with:
no-...
```

### 4. 内容搜索

暂不支持：

```text
content:
binary:
hex:
```

原因：必须读取文件内容，会使筛选阶段变成明显的 I/O 密集型内容检索。

### 5. 高级文件系统属性

暂不支持：

- owner / uid / gid；
- ACL；
- xattr；
- hard-link count；
- Windows attributes；
- size-on-disk / allocation-size；
- reparse point 等平台专属属性。

### 6. 文档、图片、音频、视频元数据

暂不支持：

- 图片宽高、EXIF、ISO、曝光；
- 音频 artist / album / bitrate；
- 视频 frame rate / subtitle count；
- 文档 title / author / page count / word count；
- 可执行文件 company / version / digital signature。

### 7. Hash / 文件签名筛选

暂不支持：

```text
crc:
md5:
sha1:
sha256:
file-signature:
```

这类能力代价较高，也容易与未来完整性校验职责混淆。

### 8. child / descendant 目录结构查询

暂不支持类似 Everything 的：

```text
child:
child-file:
child-folder:
descendant:
descendant-file:
descendant-folder:
child-count:
descendant-count:
```

PR #9 只根据“当前对象自己的名称、路径、类型、大小、mtime”进行判断，不为了判断一个目录而预先统计其后代。

### 9. parent / ancestor / depth 高级查询

暂不支持：

```text
parent:
ancestor:
depth:
parent-count:
```

大多数路径范围需求先通过：

```text
path:
*
?
**
```

解决。

### 10. 保存的 Filter / Macro / 预设

暂不支持：

- 给规则集命名并长期保存；
- 自定义 macro；
- `audio:` / `image:` / `video:` 等内置宏；
- 多规则集组合；
- GUI 高级预设管理。

### 11. Everything 风格字符实体

暂不支持：

```text
&sp:
&vert:
&excl:
&lt:
&gt:
&quot:
&#123:
&#x7B:
```

CLI 使用正常 shell 引号和参数边界；GUI 使用普通文本输入。

## 三、行为约束

1. 无筛选规则时必须与 PR #8 完全兼容。
2. Filter 不能修改源文件。
3. 被排除的目录应尽早停止递归扫描。
4. 被排除的普通文件不能出现在 `.bak` 中。
5. 恢复端不需要重新执行 Filter；它只恢复归档中实际存在的条目。
6. 错误规则不能被静默忽略。
7. CLI、Classic GUI、Modern GUI 必须共享同一套 C++ Filter 实现。
8. 只有真实实现且测试通过的能力才能写入“已实现”部分。

## 四、与 Everything 的关系

本项目只参考 Everything 的筛选思想与部分语法设计。Everything 1.5 还提供完整布尔查询、Regex、内容检索、目录后代查询、属性/媒体元数据、Hash、宏和保存的 Filter 等大量能力；这些不属于 PR #9 的基础筛选范围，统一记录为后续计划。

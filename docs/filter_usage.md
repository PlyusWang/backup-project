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
`size:` 只对**普通文件**生效：目录与特殊条目（软链接 / FIFO / 字符设备 / 块设备 /
socket）一律不命中。硬链接在文件系统层就是普通文件，所以照常命中。

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

---

## 11. GUI 规则编辑器（Modern GUI）

可视化规则编辑器是**对现有规则语法的可视化封装**：界面负责收集字段、条件与值，
最终仍然生成同一份 DSL，交给同一个 C++ Filter 核心匹配。GUI 不实现第二套
glob、后缀、大小或日期判断，也没有自己的规则语法。

数据流（单向、唯一语义源）：

`@
QML 表单
  -> FilterRuleModel（Qt 桥：表单 -> 草稿，只做转发与状态同步）
    -> FilterRuleBuilder（序列化 / 摘要 / 表单校验）
      -> Filter::AddRule（最终语法裁决，CLI 用的是同一个）
        -> BackupController -> BackupEngine（真正备份）
`@

### 11.1 表单字段 -> DSL 映射

| 表单控件 | 用户填什么 | 生成的 DSL |
| --- | --- | --- |
| ext + 扩展名输入（分号分隔） | txt;md | ext:txt;md |
| name / path / stem + 通配符 | *.txt、**/build/** | name:*.txt、path:**/build/** |
| type 下拉 | file / folder | type:file、type:folder |
| size：运算符 + 数值 + 单位 | >= 1 KB | size:>=1KB |
| size：区间 | 1 MB .. 10 MB | size:1MB..10MB |
| 动作 Include / Exclude | —— | 决定该条进 --include 还是 --exclude |

单位按 1024 进制；字节不带后缀（size:>=512）。第一版每条规则一个条件，
底层与 builder 已经支持多子条件 AND（如 type:folder path:**/cache），界面留到下一版。

### 11.2 界面行为

- 左侧文件预览：**逐条调用真实 Filter 判定**（ShouldPruneDirectory /
  ShouldIncludeFile / ShouldSkipSpecialEntry），标注“进入归档 / 被规则排除 /
  目录被排除（整棵剪掉）/ 特殊文件（会导致备份失败）”。
  预览在后台线程扫描（QtConcurrent），最多 300 项，超过会明说“仅预览前 300 项”，
  不会因为目录过大卡死界面。
- 右侧规则列表：每条规则显示动作、人类可读摘要与 DSL，可上移 / 下移 / 删除。
- 编辑器底部显示人类可读摘要、只读 DSL 预览与“CLI 等价参数”。
- 非法输入（例如扩展名列表为空）在表单里即时标红并给出具体原因
  （ext 至少需要一个扩展名），此时“添加规则”按钮不可用；即使绕过界面，
  提交时后端仍会用 Filter::AddRule 再校验一次。
- 预览只是展示：真正备份时会重新经过完整 Filter 流程，预览结果不作为业务输入。

### 11.3 语义与 CLI 完全一致

同一场景用「界面点出来的规则」与「手写 CLI 规则」各跑一次备份恢复，结果用
diff -r 与全树 sha256sum 比对必须逐字节相同 —— 这条断言由
scripts/filter_rule_builder_int_test.sh 真实执行。

### 11.4 预览刷新与规则编辑的当前边界

- 预览采用 **latest request wins**：扫描期间如果又改了规则或换了源目录，当前这次扫描的
  结果会被丢弃，并立刻用最新的「源目录 + 规则」重扫；界面最终显示的结果必定对应当前输入，
  不会出现"改了规则却看到旧结果"。预览说明里会写出这份结果对应的源目录，与当前源目录
  不一致时提示"源目录已改，请刷新"——只是提示过期，不会把旧结果当作当前结果。
- 表单校验是即时的：size 的运算符、上下界、单位，以及字段、类型、扩展名、通配符改动后
  都会立刻重新校验，非法时"添加规则"按钮不可用；后端 Filter::AddRule 仍会再校验一次。
- **已有规则暂不支持编辑**：规则卡片上只有上移 / 下移 / 删除；需要修改规则时请删除后重新添加。
  字段回填式的编辑入口留到后续版本。

---

## 12. 元数据字段：uid / gid / user / group 与完整 type（已实现）

本节只追加新字段的说明，前面各节的语法、优先级与剪枝语义一个字都没有改。
第二节「高级文件系统属性」里把 owner / uid / gid 列为暂不支持，指的 ACL、xattr、
hard-link count 等仍然不支持；**只有 uid / gid / user / group 这四个字段已经实现**，
以下为它们的确切语义。

### 12.1 uid / gid

支持等于、四种比较和闭区间：

```text
uid:1000
uid:<1000
uid:<=1000
uid:>1000
uid:>=1000
uid:1000..2000
gid:100
gid:>=1000
gid:1000..2000
```

- 裸数字表示**等于**（这一点与 `size:` 不同：`size:` 必须写运算符，
  `uid:1000` 合法而 `size:1000` 非法）。
- `a..b` 是**闭区间**，两端都算命中；`a` 大于 `b` 会明确报错。
- 取值必须是 0..4294967295 的十进制整数：`uid:4294967295` 合法，
  `uid:4294967296` 与 `uid:99999999999` 都会被 `AddRule` 拒绝并给出
  `uid value out of range (0..4294967295)`，不会被截断成一个"看起来能跑"的数。
- `0` 是合法值（root / root 组），不能当成"没填"。
- uid / gid 对目录同样生效：目录命中 exclude 时照常整棵剪枝。

### 12.2 user / group

```text
user:alice
group:staff
```

- **精确匹配、大小写敏感**：`user:Alice` 不匹配 `alice`，
  `user:ali` 也不匹配（不是前缀匹配，也不做 glob）。
- 名字里的空格不会被切开：子句切分只在"空白 + 已知字段名 + 冒号"处发生，
  所以 `user:my user` 是一个完整的 user 子句。
- 扫描层用 `getpwuid_r` / `getgrgid_r` 解析名字，**解析失败时名字为空**。
  空名字遇到 `user:` / `group:` 一律视为**不匹配**：不报错、不崩，
  更不会把"读不出名字"当成"匹配所有用户"。

### 12.3 type 的 7 个取值

```text
type:file
type:folder
type:symlink
type:fifo
type:char
type:block
type:socket
```

| 取值 | 含义 |
| --- | --- |
| `type:file` | **普通文件**（`!is_directory` 且 `type == kRegularFile`）；软链接 / FIFO / 设备 / socket 不再命中，请用下面的专用取值 |
| `type:folder` | 目录（同样只看 `is_directory`） |
| `type:symlink` | 符号链接 |
| `type:fifo` | 命名管道（FIFO） |
| `type:char` | 字符设备 |
| `type:block` | 块设备 |
| `type:socket` | 套接字 |

- `file` / `folder` 继续按 `is_directory` 判断，不按类型枚举判断：
  只填 `is_directory` 的旧调用方行为完全不变。
- 其余 5 个取值读条目的 `EntryType`。旧调用方不填这个字段时，它们默认是
  `kRegularFile`，因此这些规则对旧调用方**不会命中**（不会误伤），
  要让它们生效，调用方必须填写条目类型。
- 未知取值（如 `type:hardlink`、`type:regular`）会被明确拒绝。
- 特殊文件（symlink / FIFO / socket / 设备）的既有约定不变：没有被 exclude 就仍然让
  整次备份失败，只有明确写了 exclude 才跳过；现在可以用 `type:symlink` 这类规则
  精确地只排除某一类。

### 12.4 优先级与组合（未变）

- 子句之间 AND，规则之间 OR，`exclude` 优先于 `include`；
- `size:` 只对普通文件（含硬链接）生效，目录与特殊条目一律不命中；
  uid / gid / user / group / type 对所有类型都有效；
- 目录剪枝条件不变：目录命中任意 exclude 就整棵剪掉，剪掉之后子树里的路径
  不会再被询问（子路径上的规则也不会再被求值）。

示例：

```text
include type:file uid:1000 size:>=1KB
exclude user:alice
exclude type:socket
include uid:1000..2000 mtime:2026-09-01..2026-09-12
```

### 12.5 调用方契约

`FilterEntry` 新增字段都有默认值（`uid` / `gid` 为 0，`type` 为
`kRegularFile`，名字为空串）。因此：

- 只填 `is_directory` / `size` / `mtime_sec` 的老调用方语义不变，
  但 `uid:0` / `gid:0` 会命中它们——要用 uid / gid 规则，调用方必须
  真的把 uid / gid 填进去。
- 名字字段留空时 `user:` / `group:` 不匹配，这是有意的：
  宁可漏匹配，也不能把"解析失败"变成"匹配所有"。

### 12.6 GUI 规则编辑器映射

| 表单控件 | 用户填什么 | 生成的 DSL |
| --- | --- | --- |
| uid + 运算符 + 数值 | = 1000 | `uid:1000` |
| uid + 区间 | 1000 .. 2000 | `uid:1000..2000` |
| gid 同上 | >= 100 | `gid:>=100` |
| user 文本框 | alice | `user:alice` |
| group 文本框 | staff | `group:staff` |
| type 下拉 | symlink / fifo / char / block / socket | `type:symlink` 等 |

- 表单校验只查结构：uid / gid 区间反了、user / group 为空都会拦下并给出中文原因；
  最终语法仍然由 `Filter::AddRule` 裁决（builder 不复制匹配逻辑）。
- size 没有"等于"运算符：builder 的等于会序列化成 `size:1024..1024`，
  生成的 DSL 一样被核心接受。
- 人类可读摘要示例：`属主 uid = 1000`、`属组 gid 在 100 到 200 之间`、
  `用户 user = alice`、`用户组 group = staff`、`类型 = 符号链接`。

### 12.7 测试入口

```bash
bash scripts/filter_metadata_test.sh
FILTER_METADATA_SANITIZE=1 bash scripts/filter_metadata_test.sh   # ASan + UBSan
```

覆盖：uid / gid 的全部运算符与区间、边界值（0、4294967295、超 uint32 被拒绝）、
user / group 精确匹配与空名字、type 的 7 个取值、exclude 优先、目录剪枝后子路径不再
被询问、子句 AND、与 mtime 的组合，以及 builder 的 ToDsl / ValidateRule / Summarize。

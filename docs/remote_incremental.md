# 远端增量备份与恢复

本轮（PR #21）在 PR #20 的远端存储之上补上了增量链。核心原则只有一条：

> **网络增量不重新实现任何增量算法。** 变化的判断、delta 的生成、链的验证、
> 恢复的应用，全部是既有的本地增量核心（PR #18 收口的那一套）。

## 1. 三层结构

    客户端                                            服务端
    -----                                              -----
    源目录
      |  BackupEngine / 增量引擎（既有实现）
      v
    本地"已验证缓存"目录  <--- 三件套 --->  材料包（BPSNAP1）<---> blob 存储
      |                                        + 元数据（parent / generation / lineage）
      v
    恢复引擎（既有实现）-> 目标目录

服务端**只做三件事**：存字节、存链关系、enforce 归属与依赖。它不扫源目录、
不算 diff、不解释文件级 tombstone、不重新实现 BackupEngine。

## 2. 为什么是"材料包"，而不是只传 .bak

增量引擎眼中的一份快照是三件套：

    <name>.bak        归档本体（完整快照或 BKPINC1 delta）
    <name>.manifest   强 manifest（每个文件的内容摘要）
    <name>.identity   BPIDENT2 身份记录

生成下一份 delta 需要**父的三件套**，恢复链上的每一跳同样需要。只上传 .bak
的话，远端链在"副文件验证"这一步必然失败（LoadVerifiedSnapshotIdentity 会报
sidecars_verified=false）——这正是 PR #18 踩过的坑。

所以客户端把三件套打成一个 BPSNAP1 容器（见 include/snapshot_bundle.h）后上传；
服务端把它当成一段不透明字节存起来。容器里每个成员都带长度与 SHA-256。

## 3. 元数据（SQLite schema 2）

snapshots 表在 PR #20 的七列之外新增四列：

    snapshot_kind INTEGER NOT NULL DEFAULT 0   -- 0 = full, 1 = incremental
    parent_id     TEXT    NOT NULL DEFAULT ''  -- 父快照 id；full 恒为空串
    generation    INTEGER NOT NULL DEFAULT 0   -- 距离链根的代数
    lineage       TEXT    NOT NULL DEFAULT ''  -- 链归属摘要（64 位十六进制）

迁移（PRAGMA user_version 1 -> 2）只做 ALTER TABLE ADD COLUMN 与
CREATE INDEX IF NOT EXISTS：**不清库、不 DROP、不 UPDATE**。旧的 PR #20 行
靠 DEFAULT 子句自动成为"legacy standalone full"（kind 0 / parent 空 / generation 0 /
lineage 空），所以迁移没有"改了一半"的中间态。整个迁移在一个事务里，任何一步
失败都 ROLLBACK；按**列名**（PRAGMA table_info）判断缺哪列，因此重复执行是幂等的。

只读的管理工具走 VerifyExistingSchema：版本不是 2 就直接拒绝工作，**绝不**顺手
升级 schema（那会是一次写操作，而它可能正在服务端运行时执行）。

## 4. 服务端校验（客户端说什么都要过一遍）

UPLOAD_BEGIN 里客户端声明 kind / parent_id / lineage；服务端：

| 声明 | 校验 |
|---|---|
| full | parent_id 必须为空；generation 由服务端定为 0 |
| incremental | parent_id 必须存在、属于**同一个用户**、lineage 相同；generation = parent.generation + 1 |
| 未知 kind | 拒绝（kInvalidRequest） |
| lineage 不是 64 位十六进制 | 拒绝 |

客户端**根本没有 generation 字段可填**（服务端按父推导），所以"代数跳跃"这条
攻击面在协议上就不存在。parent 只能指向已经存在的不可变快照，因此也不可能出现
指向未来的环（self parent 同理：自己的 id 是校验之后才生成的）。

## 5. 依赖感知删除

    A(full) -> B(delta) -> C(delta)

* 删除 A 或 B：**拒绝**（还有活着的后代，删掉会让 C 永远无法恢复）；
* 删除 C：允许；
* 注销账户：整棵树一起删（同一个事务）。

这条规则在 RemoteMetadataStore::DeleteSnapshot（最后一道闸门）与
RemoteMaintenance::DeleteSnapshot（动磁盘之前）各查一次；管理工具
（backup-server-admin）走的是同一个 RemoteMaintenance，所以管理员也绕不过它。

## 6. 客户端流程

### remote backup <源目录> [--strategy incremental|full]

1. 连上并按 pin 完成 BPSEC1 握手，登录；
2. 算这条链的 lineage（= 引擎的 source identity：源路径 + 逻辑仓库身份）；
3. LIST 找到这条 lineage 的 head（没有被任何快照引用为父的那一个；多个候选取
   最新的）；
4. **把整条链的材料下载并逐字节验证**进本地缓存（缓存命中就不重复下载）；
5. 调既有的 RunIncrementalBackup，基线就是 head 的本地归档名；
6. 引擎给出 kDelta / kFullBaseline / kNoChanges 三选一：
   * kDelta -> 打包新 delta 上传，声明 parent = head；
   * kFullBaseline -> 打包完整基线上传，声明 full（引擎判断无法续链时的如实行为）；
   * kNoChanges -> 不上传，打印"源目录没有变化"；
7. 校验服务端回填的 generation 与本地预期一致。

--strategy full 会强制新建一份完整基线（给引擎一个不受管的基线名，按
include/incremental_backup.h 的公开契约清空基线），它与增量路径共用同一条
lineage，所以之后仍然可以继续增量。

### remote restore <快照ID> <目标目录>

1. LIST -> 从目标沿 parent_id 走回链根，并校验 kind/generation 自洽；
2. 对链上每一份：缓存命中验证即用，否则下载材料包 -> 校验每个成员的 SHA-256 ->
   解包 -> 再用 LoadVerifiedSnapshotIdentity 确认"这份材料真的就是那个快照"；
3. 把目标归档名交给既有的 RestoreSnapshotChain：它会**再验一遍**整条链的
   字节、副文件绑定与父子关系，然后原子发布到目标目录。

用户只需要指定"要恢复到哪个快照"，不需要手工挑父、也不需要一个个下载 delta。

## 7. 服务端元数据不替代归档自验证

这是 PR #18 已经踩过的坑，本轮的结构性防线是：

1. 下载路径先验**整个材料包**的长度与 SHA-256（服务端声明的值必须等于实际字节）；
2. 解包时逐成员验长度与 SHA-256；
3. 交给引擎时，LoadVerifiedSnapshotIdentity 验归档 payload 的 SHA-256 与
   两个副文件的绑定；
4. 恢复时 RestoreSnapshotChain 验每一跳的 parent 名字、parent 身份与
   manifest 摘要。

任何一层不过，材料都不会被当成可信状态；身份对不上的缓存条目会被**撤掉**，
而不是留着下次再用。

## 8. 本地缓存

    <应用配置目录>/remote-cache/<服务端指纹前 16 位>/<用户名>/

* 里面只有三件套 + 一个 .remote-index.tsv（服务端快照 id -> 本地归档名）；
* **没有任何凭据**：没有口令、没有 token、没有服务端私钥；
* 索引是缓存层而不是信任来源：它指向的每份材料在使用前都要过完整的验证；
  索引丢了最多让下一次备份重新下载一遍；
* 逻辑仓库身份是 remote:<指纹>:<用户名>（跨机器稳定），而不是本机路径——
  否则从另一台机器继续增量会被判成另一条链，白白重建完整基线；
* 新机器 bootstrap：缓存不存在时，restore 与 backup 都会自动把需要的链拉下来
  （见第 6 节）。源路径在不同机器上不同的话，引擎会如实重建一份完整基线
  （这是 source identity 的设计，不是 bug）。

## 9. 已知限制

* 远端增量只支持 MyPack + 不压缩/不加密的组合（沿用引擎的限制：外层信封不受
  内层保护，加密的 delta 等于接受未认证的路径指令）；
* 链深度上限 64 个 delta（引擎的上限），到顶后引擎会自动重建完整基线；
* 缓存目录不会自动清理（删掉它是安全的，只是下一次要重新下载）；
* 远端没有"批量删除整条链"的命令：按后代优先逐个删（叶子先删），
  账户注销时才整棵树一起删。

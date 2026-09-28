// backup_catalog.h
//
// 备份仓库（repository）的核心层：列目录、命名、寻址、删除。
//
// 这里说的 repository 就是一个普通的本地目录，里面平铺着用户创建的归档文件
// （约定扩展名 .bak）。Catalog 自己不创建备份、也不恢复备份——那两件事是
// BackupEngine 的职责；它只回答三个问题："仓库里现在有哪几个备份""下一个备份
// 该叫什么名字""这个名字对应的到底是哪个文件"。
//
// 与配置层的边界：所有接口都把 repository 作为显式参数传进来，不从配置文件、
// 环境变量或 QSettings 读取任何东西。这一层因此可以独立测试，也与并行的配置
// 模块完全解耦。
//
// 与归档格式的边界：Catalog 只读归档头，不解析 entry、不读 payload，也从不要求
// 密码。识别格式用 IdentifyArchiveFile：按 magic 区分 legacy v0.1 与 v2
// container，并从 v2 的 160 字节外层 header 读出 pack / compression /
// encryption 三个算法 id；legacy 归档的全局 header 仍然由
// ArchiveReader::InspectHeader 读。归档文件的大小与 mtime 是文件系统属性，
// 不是格式字段，所以 archive v0.1 不需要为列表做任何升级。
//
// 一条贯穿全文件的规则：本类不跟随软链接。仓库根自己、以及仓库里的每一项，
// 都用 lstat 判断，软链接既不会出现在列表里，也不会被解析、被删除。
//
// ---- 安全边界（别读过头）----
//
// 当前实现做的是三件事：拒绝 repository 本身是符号链接；拒绝最终目标文件是
// 符号链接；把 file_name 限制成 repository 的单个直接子项名称（不含分隔符、
// 不是 "." / ".."、不含内嵌 NUL）。这覆盖的是普通使用场景下的路径越界。
//
// 它不保证：
//   * repository 的祖先路径组件中没有符号链接——lstat 只能证明最后一个组件
//     不是链接，无法证明 /a/b/repo 里的 /a、/a/b 也没被替换；
//   * 检查与随后的 open / unlink 之间没有竞态（check/use TOCTOU）：校验通过
//     之后、真正动文件之前，文件系统仍然可能被并发修改；
//   * 在"恶意并发修改文件系统"这一威胁模型下的完全隔离。
//
// 若将来需要更强的本地对抗安全边界，可以把校验与使用绑到同一个目录句柄上
// （dirfd + openat / openat2 / unlinkat）；当前版本没有这么做。

#ifndef BACKUP_PROJECT_INCLUDE_BACKUP_CATALOG_H_
#define BACKUP_PROJECT_INCLUDE_BACKUP_CATALOG_H_

#include <cstdint>
#include <string>
#include <vector>

#include "container_format.h"
#include "pack_stream.h"

namespace backupproject {

// 归档文件名的规范扩展名，以及"这是不是一个受管理的备份文件名"。
//
// 这条规则只属于 Catalog：BuildArchivePath 生成它，Resolve / Delete 校验它。
// 增量 delta 的 parent_file_name 也来自不可信归档、也必须满足同一条边界，
// 所以这里公开出来给那条校验复用——同一套规则不可能在两处走散。
inline constexpr const char kBackupFileExtension[] = ".bak";

// 非空、单组件（不含 '/' 或 '\\'、不是 "." / ".."、不含 NUL）、以 .bak 结尾。
// 只看字符串，不访问文件系统：真正的"必须是仓库的直接子项、普通文件、非软
// 链接"由 Resolve 负责。
bool IsManagedBackupFileName(const std::string& file_name);

// 仓库里的一个备份候选。
//
// 一个 record 必然对应仓库的直接子项、普通文件、文件名以 .bak 结尾；但它不
// 一定是个能用的归档。recognized_archive 为 false 时 diagnostic 里是
// InspectHeader 给出的具体原因，这种记录照样出现在列表里，也照样能删。
struct BackupRecord {
  // 仓库内的文件名（不含目录）。
  std::string file_name;
  // 拼好的完整路径。
  std::string archive_path;

  // 归档文件自身的字节数。
  std::uint64_t archive_size = 0;
  // 归档文件自身的 mtime（秒）。
  std::int64_t modified_time_sec = 0;

  // header（legacy v0.1 的全局 header，或 v2 的 160 字节外层 container
  // header）是否被当前实现认识。注意这只说明 header 可读，既不保证归档内容
  // 完整，也不保证恢复得出来；完整校验始终在恢复路径里。
  bool recognized_archive = false;
  std::uint16_t format_version = 0;
  std::uint64_t entry_count = 0;

  // 下面四个字段描述归档"声明"的流水线。它们来自 v2 container 的 160 字节外层
  // header，读它不需要密码，也从来不是完整校验：header 完好只说明写入那一刻用
  // 了这些算法，归档是否完整、能不能恢复仍然由恢复路径判断。
  //
  // has_pipeline_methods 为 false 表示这不是 v2 container；legacy v0.1 没有
  // 流水线概念，此时另外三个字段保持默认值，没有任何含义。
  bool has_pipeline_methods = false;
  // 打包方式（MyPack / USTAR / FastUSTAR）。
  PackMethod pack_method = PackMethod::kMyPack;
  // 压缩方式。
  CompressionMethod compression_method = CompressionMethod::kNone;
  // 加密方式；不是 kNone 就意味着恢复需要密码。
  EncryptionMethod encryption_method = EncryptionMethod::kNone;
  // 恢复是否需要密码：等价于"v2 container 且 encryption_method != kNone"。
  // 单独留一个字段，界面层就不必自己拼这条判断。
  bool password_required = false;

  // recognized_archive 为 false 时的原因；为 true 时为空。
  std::string diagnostic;

  // ---- PR #18：快照种类与依赖链 ----
  //
  // 一份 delta 不是"坏归档"，它是另一种快照：有独立 magic，自己带着父身份。
  // catalog 必须先按 magic 分类，否则一份完好的 delta 会被报成"认不出来"。
  bool incremental_delta = false;
  // delta 的父快照文件名（完整归档时为空）。
  std::string parent_file_name;
  // 这条依赖链现在能不能恢复。
  //
  // 这里刻意只做**廉价**判断（父文件存不存在）：列表可能要看上千条记录，
  // 逐条把整条链读一遍代价太大。真正的身份校验在恢复路径里 —— 那里才是
  // 必须正确、也真的会拒绝的地方。这一位只回答"看起来能不能恢复"。
  bool chain_restorable = false;
  // 不能恢复时的原因（parent 缺失等）。
  std::string chain_diagnostic;
};

// 仓库的稳定 identity：用来回答"这两个仓库路径是不是同一个仓库"。
//
// 为什么不能直接比较用户写下的字符串："/repo"、"/repo/"、以及经过软链接的
// "/mnt/data/repo" 可能指向同一个目录，也可能不是——字符串比较给出的答案
// 与文件系统给的答案不一致，会让"同一份 manifest 属于哪个仓库"这件事失去意义。
//
// 取值规则（确定、可复现）：
//   * 空路径 -> 空串；
//   * 先去掉尾部 '/'（"/" 本身除外）；
//   * 路径存在时返回 realpath() 的规范化结果；
//   * 路径不存在时返回去掉尾部 '/' 的原字符串——**不做任何猜测**，
//     也绝不因此把 identity 变成空串（那会让"仓库暂时不可用"看起来像
//     "仓库换了"）。
//
// 刻意不读 inode / 设备号：仓库目录可以被删掉再建出来，那时 inode 变了但
// 它仍然是用户心里的"同一个仓库"；反过来 device 号在容器与 bind mount 下
// 也不稳定。路径 identity 是这里能给出的最诚实的答案。
std::string RepositoryIdentity(const std::string& repository_path);

class BackupCatalog {
 public:
  BackupCatalog() = default;

  // 确保 repository 存在。不存在就按 mkdir -p 连父目录一起建出来；已存在且
  // 是目录则成功；已存在但不是目录（含软链接）则失败。
  // 绝不删除、绝不覆盖已有的东西；建目录失败时错误信息里带 errno 说明。
  bool EnsureRepository(const std::string& repository,
                        std::string* error_message) const;

  // 列出仓库里的备份：按 modified_time_sec 从新到旧，时间相同再按 file_name
  // 升序。只扫直接子项，不递归；只有普通文件参与，软链接与目录一律不算。
  //
  // 单个 .bak 坏掉不会让整个 List 失败：它照样出现在结果里，只是
  // recognized_archive = false 且 diagnostic 非空。List 只在仓库打不开、
  // 不是目录、或目录枚举本身出错时才返回 false。
  //
  // repository 必须已经存在且是真实目录；软链接根会被拒绝，因为 POSIX 会跟随
  // 它，后面所有 opendir / lstat 都会作用在链接指向的那个目录上。
  //
  // 进入函数时先把 *records 清空，所以失败时它一定为空，不会是上一次的残留。
  bool List(const std::string& repository, std::vector<BackupRecord>* records,
            std::string* error_message) const;

  // 生成一个当前不存在的归档路径：
  //
  //   <repository>/<source-base>_YYYYMMDD_HHMMSS.bak
  //
  // 时间用 localtime_r 展开 now_sec，所以文件名对应用户本地时间（单元测试把
  // TZ 固定成 UTC 才谈得上确定结果）。同名已存在时依次尝试 _001 … _999，
  // 全部占用则明确失败。
  //
  // repository 必须已经存在且是真实目录：本方法不创建任何东西，创建仓库只属于
  // EnsureRepository。source_directory 里若有内嵌 NUL 字节，直接失败。
  //
  // source-base 里的 '\\' 会被替换成 '_'。因为 ValidateFileName 禁止
  // '\\'，原样保留就会产生"自己生成、自己却 Resolve / Delete 不了"的名字；
  // 换字符而不是放宽校验，安全规则一条都不松。UTF-8、空格、'#'、'%' 等一律
  // 原样保留。
  //
  // 本方法只找候选路径，不创建任何文件。真正的竞态安全由 ArchiveWriter 的
  // O_EXCL 兜底：两个进程同时算到同一个名字时，只有一个能建成功。
  bool BuildArchivePath(const std::string& repository,
                        const std::string& source_directory,
                        std::int64_t now_sec, std::string* archive_path,
                        std::string* error_message) const;

  // 把 file_name 解析成仓库内的一个真实文件。
  //
  // 两个前置条件都要满足：repository 已经存在、是真实目录、且自身不是软链接；
  // file_name 是单个文件名组件——拒绝空串、"."、".."、含 '/' 或 '\\'、
  // 以及内嵌 NUL 字节，并要求以 .bak 结尾。
  //
  // 两道检查缺一不可：文件名的规则只管路径的最后一段，而 POSIX 会跟随路径里的
  // 中间组件。少了前一条，(repo -> /outside) 这种布局会让后一条形同虚设。
  //
  // 解析结果必须是仓库的直接子项、普通文件、且不是软链接，否则失败。
  //
  // 返回的路径是 repository（去掉尾部斜杠）与 file_name 的直接拼接：不做
  // symlink 解析、不做 canonicalize、不解析 cwd。调用方传进来的 repository
  // 是什么命名空间，返回的路径就在什么命名空间里。
  bool Resolve(const std::string& repository, const std::string& file_name,
               std::string* archive_path, std::string* error_message) const;

  // 删除仓库里的一个备份。安全校验与 Resolve 逐字一致：只有直接子项、普通
  // 文件、非软链接、以 .bak 结尾的名字才能删。
  //
  // 刻意不要求 InspectHeader 成功：坏掉的备份同样是普通 .bak 文件，必须删得
  // 掉，否则损坏的存档会永远留在列表里删不掉。
  //
  // repository 的前置条件与 Resolve 相同（存在、真实目录、自身不是软链接）。
  //
  // 不递归、不删目录、不删软链接；普通路径下不会去动仓库之外的文件。
  // 校验与 unlink 之间仍有时间窗，确切边界见本文件顶部的"安全边界"。
  //
  // 路径安全边界（拒绝软链接仓库根、拒绝软链接目标、file_name 必须是直接
  // 子项名）覆盖普通使用场景；它不构成对祖先路径符号链接替换或 check/use
  // 竞态的完整防护。更强的本地对抗边界可用 dirfd + openat/openat2/unlinkat
  // 实现，本版本未采用。
  //
  // PR #18 起，删除还是**依赖感知**的：任何还有可达后代（还活着、读得出来的
  // 子快照）的快照都会被拒绝——删掉一个祖先等于让那些后代永远不可恢复，
  // 那不是"少留一份"，是数据丢失。要一次删掉一整条已经计划好的链，用
  // DeleteSnapshots。
  //
  // 删除成功之后，这份快照拥有的副文件（<name>.manifest / <name>.identity）
  // 一起清理；清理失败会进 diagnostics（不静默）。
  bool Delete(const std::string& repository, const std::string& file_name,
              std::string* error_message) const;
  bool Delete(const std::string& repository, const std::string& file_name,
              std::vector<std::string>* diagnostics,
              std::string* error_message) const;

  // 删除一个**已经过依赖检查的集合**（retention 计划用这一条）。
  //
  // 规则：集合里任何一个名字的所有可达后代必须**也在集合里**，否则整次调用
  // 一个文件都不删并返回失败。这条规则正好覆盖两种调用方：
  //   * 手工删除（集合只有一个名字）= "只允许删叶子"；
  //   * retention（集合是整份淘汰计划）= "同一条链一起删"。
  // 先整体校验再逐个 unlink，所以不会出现"删到一半发现不该删"的中间状态。
  // deleted_file_names 可以为空：非空时按实际删除成功的顺序填入文件名。
  // 单个 unlink 失败会立刻停止（后面的文件保持原样），所以调用方必须靠它——
  // 而不是靠"返回值为真"——来决定哪些记录可以从状态里去掉。
  //
  // 真正 unlink 的顺序是 **descendants-first**（叶子在前），由集合内部的依赖图
  // 决定，不看 created_time：先删祖先、删到一半崩掉会留下"指向不存在父节点"的
  // 后代，那是自己制造 broken chain。集合内部成环则整批拒绝。
  bool DeleteSnapshots(const std::string& repository,
                       const std::vector<std::string>& file_names,
                       std::vector<std::string>* deleted_file_names,
                       std::vector<std::string>* diagnostics,
                       std::string* error_message) const;
};

// ---- 测试接缝 ----
//
// 让某一条 unlink 在"即将执行"时被人为判成失败：用来验证"删到一半停住"之后
// 剩下的链仍然自洽（还存在的子节点，其父亲也还存在）。默认 nullptr，产品的
// 任何路径都不会设置它。
void SetBackupCatalogUnlinkFailureHookForTesting(
    bool (*hook)(const char* archive_path));

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_CATALOG_H_

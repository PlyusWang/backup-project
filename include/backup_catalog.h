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
// 与归档格式的边界：Catalog 只调用 ArchiveReader::InspectHeader 读全局 header，
// 不解析 entry、不读 payload，也不要求归档格式为列表做任何升级。因此列表里能
// 显示的额外信息只有归档文件自己的大小与 mtime——那是文件系统属性，不是格式
// 字段，所以不需要动 archive v0.1。
//
// 一条贯穿全文件的安全规则：本类不跟随软链接。仓库根自己、以及仓库里的每一项，
// 都用 lstat 判断，软链接既不会出现在列表里，也不会被解析、被删除。

#ifndef BACKUP_PROJECT_INCLUDE_BACKUP_CATALOG_H_
#define BACKUP_PROJECT_INCLUDE_BACKUP_CATALOG_H_

#include <cstdint>
#include <string>
#include <vector>

namespace backupproject {

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

  // 全局 header 是否被当前实现认识。注意这只说明 header 可读，
  // 既不保证归档内容完整，也不保证恢复得出来；完整校验始终在 Extract 里。
  bool recognized_archive = false;
  std::uint16_t format_version = 0;
  std::uint64_t entry_count = 0;

  // recognized_archive 为 false 时的原因；为 true 时为空。
  std::string diagnostic;
};

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
  // repository 的前置条件与 Resolve 相同（存在、真实目录、自身不是软链接）：
  // 校验与 unlink 之间的时间窗之所以逃不出仓库，靠的正是"根不是软链接"与
  // "最后一段是普通文件"这两条同时成立。
  //
  // 不递归、不删目录、不删软链接、不碰仓库外的任何文件。
  bool Delete(const std::string& repository, const std::string& file_name,
              std::string* error_message) const;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_CATALOG_H_

// backup_catalog.cpp
//
// BackupCatalog 的实现。三条贯穿全文件的规则：
//
//   1. 只认仓库的直接子项。所有对外部路径的读写都要先过 ValidateFileName，
//      路径拼接永远发生在校验之后；
//   2. 只认普通文件。一律用 lstat 而不是 stat，路径最后一段的软链接被当成
//      链接本身看，不会被顺着走到别处去；
//   3. 坏归档不阻断列表。IdentifyArchiveFile / InspectHeader 失败只是给这条
//      记录写一句 diagnostic，不改变 List 的成功与否——列表的价值之一就是让
//      用户看见"这个文件还在，但已经不是能被恢复的归档了"。
//
// 这三条只覆盖路径的安全解析，不覆盖并发修改：祖先路径组件的符号链接替换与
// check/use 竞态都不在防护范围内，确切边界见 backup_catalog.h 的"安全边界"。

#include "backup_catalog.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "archive.h"
#include "archive_pipeline.h"
#include "container_format.h"
#include "file_system.h"

namespace backupproject {

namespace {

// 仓库里被视为"用户备份"的扩展名。大小写按 Linux 原义：.BAK 不算。
constexpr const char kBackupExtension[] = ".bak";
constexpr std::size_t kBackupExtensionLength = 4;

// 同名冲突时的序号上限：_001 … _999，固定三位十进制。
// 一秒钟内在同一个仓库里创建 1000 个同名备份不是正常使用场景；与其悄悄换成
// 更长的后缀，不如明确失败。
constexpr int kMaxCollisionSuffix = 999;

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 统一错误文案：动作 + 路径 + errno 说明，三个要素都带上。
std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

bool HasBackupExtension(const std::string& name) {
  return name.size() >= kBackupExtensionLength &&
         name.compare(name.size() - kBackupExtensionLength,
                      kBackupExtensionLength, kBackupExtension) == 0;
}

// 去掉尾部的 '/'，但保留根目录 "/" 本身。
// 只处理分隔符：不解析 "." / ".."、不展开软链接——那是 canonicalize 的活，
// 而 canonicalize 恰恰可能把路径带到仓库外面去，这里不做。
std::string StripTrailingSlashes(const std::string& path) {
  if (path.empty()) {
    return path;
  }
  std::size_t end = path.size();
  while (end > 1 && path[end - 1] == '/') {
    --end;
  }
  return path.substr(0, end);
}

// repository 根本身必须是一个真实存在的目录。
//
// 为什么 Resolve / Delete 也要查这个——文件名的规则只管路径的"最后一段"，
// 而 POSIX 路径解析会跟随路径里的"中间组件"。假设
//
//     /tmp/repo  ->  /outside/real_repo        （软链接）
//
// 那么 unlink("/tmp/repo/a.bak") 真正删掉的是 /outside/real_repo/a.bak。
// 最后一段 "a.bak" 确实通过了所有普通文件检查，越界却发生在它前面。
// 所以"仓库根不是软链接"必须和"文件名不含分隔符"一样，是每个入口的前置条件，
// 而不能只由 List 顺手查一下。
//
// 成功时把去掉尾斜杠的路径写进 *normalized，调用方直接拿它拼接，避免各处
// 再各自 StripTrailingSlashes 一遍而出现分叉。
//
// 本函数不创建任何东西：创建仓库只属于 EnsureRepository。
bool ValidateRepositoryDirectory(const std::string& repository,
                                 std::string* normalized,
                                 std::string* error_message) {
  if (repository.empty()) {
    SetError(error_message, "Repository path is empty");
    return false;
  }
  // 内嵌 NUL 会让"校验看到的字符串"和"内核看到的路径"不是同一个东西：
  // 下面的 POSIX 调用都拿 c_str()，所以先拦掉。
  if (repository.find('\0') != std::string::npos) {
    SetError(error_message, "Repository path contains a NUL byte");
    return false;
  }

  const std::string stripped = StripTrailingSlashes(repository);

  // lstat 而不是 stat：仓库根自己是不是软链接，只有 lstat 才看得见。
  struct stat info;
  if (lstat(stripped.c_str(), &info) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to inspect repository", stripped));
    return false;
  }
  if (S_ISLNK(info.st_mode)) {
    SetError(error_message,
             "Repository path must not be a symbolic link: " + stripped);
    return false;
  }
  if (!S_ISDIR(info.st_mode)) {
    SetError(error_message, "Repository path is not a directory: " + stripped);
    return false;
  }

  *normalized = stripped;
  return true;
}

// file_name 必须能安全地当成"仓库里的一个名字"用。
//
// 这一层是整个 Catalog 安全性的入口：只要名字不含分隔符、不是 "." / ".."、
// 不含内嵌 NUL，后面的 repository + "/" + file_name 就必然还落在仓库里。
// 之所以还要单独查 NUL，是因为下面的 POSIX 调用都拿 c_str()，一个内嵌 NUL
// 会让"校验看到的字符串"和"内核看到的路径"不是同一个东西。
bool ValidateFileName(const std::string& file_name,
                      std::string* error_message) {
  if (file_name.empty()) {
    SetError(error_message, "Backup file name is empty");
    return false;
  }
  if (file_name == "." || file_name == "..") {
    SetError(error_message, "Invalid backup file name: " + file_name);
    return false;
  }
  if (file_name.find('/') != std::string::npos ||
      file_name.find('\\') != std::string::npos) {
    SetError(
        error_message,
        "Backup file name must not contain a path separator: " + file_name);
    return false;
  }
  if (file_name.find('\0') != std::string::npos) {
    SetError(error_message, "Backup file name contains a NUL byte");
    return false;
  }
  if (!HasBackupExtension(file_name)) {
    SetError(error_message, "Backup file name must end with " +
                                std::string(kBackupExtension) + ": " +
                                file_name);
    return false;
  }
  return true;
}

// 校验 + 拼接 + lstat + "必须是普通文件"，Resolve 与 Delete 共用。
//
// 共用是刻意的：删除的安全边界必须与解析逐字一致，不能出现"解析拦住了、
// 删除却放过去"这种分叉。
bool LocateDirectChildFile(const std::string& repository,
                           const std::string& file_name, const char* action,
                           std::string* archive_path,
                           std::string* error_message) {
  // 仓库根先验一遍，再验文件名：前者管中间组件，后者管最后一段，
  // 两件事缺一不可。
  std::string normalized;
  if (!ValidateRepositoryDirectory(repository, &normalized, error_message)) {
    return false;
  }
  if (!ValidateFileName(file_name, error_message)) {
    return false;
  }

  const std::string candidate = FileSystem::JoinPath(normalized, file_name);

  // 用 lstat 而不是 stat：路径最后一段是软链接时，这里就已经被判成
  // "不是普通文件"，不会被顺着走到别处去。
  struct stat info;
  if (lstat(candidate.c_str(), &info) != 0) {
    SetError(error_message, Describe(errno, action, candidate));
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    SetError(error_message, "Backup path is not a regular file: " + candidate);
    return false;
  }

  *archive_path = candidate;
  return true;
}

// DIR* 的 RAII：任何提前 return 都会自动 closedir。
class ScopedDir {
 public:
  explicit ScopedDir(DIR* dir) : dir_(dir) {}
  ~ScopedDir() {
    if (dir_ != nullptr) {
      ::closedir(dir_);
    }
  }

  ScopedDir(const ScopedDir&) = delete;
  ScopedDir& operator=(const ScopedDir&) = delete;

  DIR* get() const { return dir_; }

 private:
  DIR* dir_ = nullptr;
};

// 从源目录路径里取"源目录名"，用作归档文件名的前缀。
//
// 纯文本处理，一共两步：去掉尾部斜杠后取最后一段，再把 '\\' 换成 '_'。
// 取不到可读名字时退回 "backup"，不使用 hash / UUID / 随机数——名字要能让人
// 一眼看懂。
//
// 为什么要换 '\\'：Linux 上 '\\' 是合法的文件名字符，但 ValidateFileName
// 为了路径安全一律禁止它。这里若原样保留，BuildArchivePath 就会生成一个
// Resolve / Delete 都不认的文件名——系统自己产出了自己管不了的东西。
// 换掉而不是放宽 ValidateFileName，安全规则一条都不松。
//
// 其它字符（UTF-8、空格、'#'、'%' …）一律原样保留：文件名应当和源目录名对得上。
std::string SourceBaseName(const std::string& source_directory) {
  const std::string trimmed = StripTrailingSlashes(source_directory);
  const std::size_t slash = trimmed.rfind('/');
  std::string base =
      (slash == std::string::npos) ? trimmed : trimmed.substr(slash + 1);
  for (char& ch : base) {
    if (ch == '\\') {
      ch = '_';
    }
  }
  if (base.empty() || base == "." || base == "..") {
    return "backup";
  }
  return base;
}

}  // namespace

bool BackupCatalog::EnsureRepository(const std::string& repository,
                                     std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (repository.empty()) {
    SetError(error_message, "Repository path is empty");
    return false;
  }
  if (repository.find('\0') != std::string::npos) {
    SetError(error_message, "Repository path contains a NUL byte");
    return false;
  }

  const std::string normalized = StripTrailingSlashes(repository);

  // 这里刻意不复用 ValidateRepositoryDirectory：那个函数的语义是"必须已经
  // 存在"，而 EnsureRepository 的职责恰恰是"不存在就建出来"。两者只共享
  // "仓库根不能是软链接"这一条判断。
  struct stat info;
  if (lstat(normalized.c_str(), &info) == 0) {
    if (S_ISLNK(info.st_mode)) {
      SetError(error_message,
               "Repository path must not be a symbolic link: " + normalized);
      return false;
    }
    if (!S_ISDIR(info.st_mode)) {
      SetError(error_message,
               "Repository path exists and is not a directory: " + normalized);
      return false;
    }
    return true;
  }
  if (errno != ENOENT) {
    SetError(error_message,
             Describe(errno, "Failed to inspect repository", normalized));
    return false;
  }

  // 复用 FileSystem 的 mkdir -p：它的错误文案、EEXIST 处理和"全部建完后再
  // 确认一次路径本身是目录"的逻辑都已经过测试，没必要在这里重写一遍。
  // 它对路径本身同样用 lstat，所以软链接仓库在这里也是被拒绝的，与本类
  // "不跟随软链接"的规则一致。
  FileSystem file_system;
  return file_system.MakeDirectories(normalized, error_message);
}

bool BackupCatalog::List(const std::string& repository,
                         std::vector<BackupRecord>* records,
                         std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (records == nullptr) {
    SetError(error_message, "List: records must not be null");
    return false;
  }
  records->clear();

  // 仓库根必须是真实目录：软链接根会让后面所有的 opendir / lstat 都作用在
  // 链接指向的那个目录上。
  std::string normalized;
  if (!ValidateRepositoryDirectory(repository, &normalized, error_message)) {
    return false;
  }

  DIR* dir = ::opendir(normalized.c_str());
  if (dir == nullptr) {
    SetError(
        error_message,
        Describe(errno, "Failed to open repository directory", normalized));
    return false;
  }
  ScopedDir scoped_dir(dir);

  const ArchiveReader reader;
  std::vector<BackupRecord> found;

  while (true) {
    // readdir 用返回值 nullptr 同时表示"读完"和"出错"，只能靠 errno 区分，
    // 所以每次调用前先把它清零。
    errno = 0;
    struct dirent* entry = ::readdir(scoped_dir.get());
    if (entry == nullptr) {
      if (errno != 0) {
        SetError(
            error_message,
            Describe(errno, "Failed to read repository directory", normalized));
        return false;
      }
      break;
    }

    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (!HasBackupExtension(name)) {
      continue;
    }

    const std::string candidate = FileSystem::JoinPath(normalized, name);

    // lstat：软链接报的是链接自己（S_ISLNK），所以它天然不满足"普通文件"
    // 这一条，不会被当成备份列出来。目录同理。
    //
    // 这里 stat 失败只说明这一项在本趟扫描里不可用（多半是刚好被删掉了），
    // 跳过它而不是让整张列表失败。
    struct stat info;
    if (lstat(candidate.c_str(), &info) != 0) {
      continue;
    }
    if (!S_ISREG(info.st_mode)) {
      continue;
    }

    BackupRecord record;
    record.file_name = name;
    record.archive_path = candidate;
    record.archive_size = static_cast<std::uint64_t>(info.st_size);
    record.modified_time_sec = static_cast<std::int64_t>(info.st_mtime);

    // 坏掉的 .bak 照样进列表，只是带上诊断信息。
    // 注意 diagnostic 用的是局部变量而不是 error_message：一个坏文件不能
    // 把调用方的错误信息写成"失败"。
    //
    // 先认格式：IdentifyArchiveFile 只看 magic，就能把 legacy v0.1 与 v2
    // container 分开，而且不需要密码就能读出 v2 的三个算法 id。
    ArchiveFileInfo archive_info;
    std::string identify_error;
    if (!IdentifyArchiveFile(candidate, &archive_info, &identify_error)) {
      // 既不是 legacy v0.1，也不是 header 可读的 v2 container。
      //
      // 诊断优先用 InspectHeader 的说法：列表里的 diagnostic 是要直接显示给
      // 用户的文案，不能因为多认了一种格式就变样。IdentifyArchiveFile 的原因
      // 只在它也说不出话时兜底——diagnostic 必须非空，否则用户只看到一个
      // "认不出来"，什么线索都没有。
      ArchiveSummary summary;
      std::string legacy_diagnostic;
      // magic 不是 BKPARCH 时 InspectHeader 必然失败，这里只取它的原因；
      // 返回值不参与判断，识别结论已经由 IdentifyArchiveFile 给出。
      reader.InspectHeader(candidate, &summary, &legacy_diagnostic);
      record.recognized_archive = false;
      record.has_pipeline_methods = false;
      record.password_required = false;
      if (!legacy_diagnostic.empty()) {
        record.diagnostic = legacy_diagnostic;
      } else if (!identify_error.empty()) {
        record.diagnostic = identify_error;
      } else {
        record.diagnostic = "Unrecognized archive file: " + candidate;
      }
    } else if (archive_info.kind == ArchiveFileInfo::Kind::kLegacyV01) {
      // legacy v0.1：IdentifyArchiveFile 认得 magic 就算成功，哪怕全局 header
      // 是坏的（它此时静默地留下 entry_count = 0）。所以"认得"这条结论仍然由
      // InspectHeader 决定，与本次改动之前逐字一致。
      ArchiveSummary summary;
      std::string diagnostic;
      if (reader.InspectHeader(candidate, &summary, &diagnostic)) {
        record.recognized_archive = true;
        record.format_version = summary.format_version;
        record.entry_count = summary.entry_count;
      } else {
        record.recognized_archive = false;
        record.diagnostic = diagnostic.empty()
                                ? "Unreadable archive header: " + candidate
                                : diagnostic;
      }
      // v0.1 没有流水线，也从不加密：全部保持默认值。
      record.has_pipeline_methods = false;
      record.password_required = false;
    } else {
      // v2 container：160 字节外层 header 把三种算法写得很清楚，读它不需要
      // 密码。这里记录的只是 header 的"声明"，不代表归档完整或恢复得出来。
      record.recognized_archive = true;
      record.format_version = archive_info.format_version;
      record.entry_count = archive_info.entry_count;
      record.has_pipeline_methods = true;
      record.pack_method = archive_info.pack_method;
      record.compression_method = archive_info.compression_method;
      record.encryption_method = archive_info.encryption_method;
      record.password_required =
          (archive_info.encryption_method != EncryptionMethod::kNone);
    }

    found.push_back(std::move(record));
  }

  // 固定排序：新的在前；时间相同按文件名升序。
  // 顺序固定下来，GUI 列表和测试结果才是稳定的。
  std::sort(found.begin(), found.end(),
            [](const BackupRecord& left, const BackupRecord& right) {
              if (left.modified_time_sec != right.modified_time_sec) {
                return left.modified_time_sec > right.modified_time_sec;
              }
              return left.file_name < right.file_name;
            });

  *records = std::move(found);
  return true;
}

bool BackupCatalog::BuildArchivePath(const std::string& repository,
                                     const std::string& source_directory,
                                     std::int64_t now_sec,
                                     std::string* archive_path,
                                     std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (archive_path == nullptr) {
    SetError(error_message, "BuildArchivePath: archive_path must not be null");
    return false;
  }
  archive_path->clear();

  // repository 必须已经存在且是真实目录。这里不做任何创建：创建仓库只属于
  // EnsureRepository，命名函数不该有副作用。
  std::string normalized;
  if (!ValidateRepositoryDirectory(repository, &normalized, error_message)) {
    return false;
  }
  if (source_directory.empty()) {
    SetError(error_message, "Source directory path is empty");
    return false;
  }
  // 源路径要参与文件名生成并最终交给 POSIX API，内嵌 NUL 同样先拦掉。
  if (source_directory.find('\0') != std::string::npos) {
    SetError(error_message, "Source directory path contains a NUL byte");
    return false;
  }

  // 文件名对应用户看到的时间，所以用 localtime_r 而不是 gmtime_r。
  // 单元测试把 TZ 固定成 UTC，结果才是确定的。
  const std::time_t stamp_time = static_cast<std::time_t>(now_sec);
  struct tm local = {};
  if (localtime_r(&stamp_time, &local) == nullptr) {
    SetError(error_message, "Failed to convert timestamp to local time: " +
                                std::to_string(now_sec));
    return false;
  }
  char stamp[32] = {};
  if (std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local) == 0) {
    SetError(error_message,
             "Failed to format timestamp: " + std::to_string(now_sec));
    return false;
  }

  const std::string prefix =
      SourceBaseName(source_directory) + "_" + std::string(stamp);

  // 先试不带序号的名字，再依次试 _001 … _999。
  // lstat 的三种结果要分开处理：成功 = 名字被占，换下一个；ENOENT = 可用；
  // 其它 errno（路径太长、中间某层不是目录等）= 真的出错，直接报。
  for (int suffix = 0; suffix <= kMaxCollisionSuffix; ++suffix) {
    std::string candidate = normalized;
    candidate += '/';
    candidate += prefix;
    if (suffix > 0) {
      char number[8] = {};
      std::snprintf(number, sizeof(number), "_%03d", suffix);
      candidate += number;
    }
    candidate += kBackupExtension;

    struct stat info;
    if (lstat(candidate.c_str(), &info) != 0) {
      if (errno == ENOENT) {
        // 这里只给出候选路径，不创建文件。真正的竞态安全由 ArchiveWriter
        // 的 O_EXCL 兜底：两个进程同时算到同一个名字时，只有一个能建成功。
        *archive_path = candidate;
        return true;
      }
      SetError(error_message,
               Describe(errno, "Failed to inspect candidate archive path",
                        candidate));
      return false;
    }
  }

  SetError(error_message,
           "Failed to find a free archive name in repository: " + normalized +
               " (" + prefix + std::string(kBackupExtension) + " and all " +
               std::to_string(kMaxCollisionSuffix) +
               " collision suffixes are taken)");
  return false;
}

bool BackupCatalog::Resolve(const std::string& repository,
                            const std::string& file_name,
                            std::string* archive_path,
                            std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (archive_path == nullptr) {
    SetError(error_message, "Resolve: archive_path must not be null");
    return false;
  }
  archive_path->clear();

  return LocateDirectChildFile(repository, file_name,
                               "Failed to inspect backup file", archive_path,
                               error_message);
}

bool BackupCatalog::Delete(const std::string& repository,
                           const std::string& file_name,
                           std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }

  std::string archive_path;
  if (!LocateDirectChildFile(repository, file_name,
                             "Failed to inspect backup file", &archive_path,
                             error_message)) {
    return false;
  }

  // 不要求 InspectHeader 成功：坏掉的备份仍然是普通 .bak 文件，必须删得掉，
  // 否则损坏的存档会永远留在列表里。
  //
  // 校验与 unlink 之间确实存在时间窗。LocateDirectChildFile 里那两条前提
  // （仓库根不是软链接、最后一段是普通文件）只在"校验那一刻"成立：
  //   * 仓库根若是软链接，POSIX 会跟随这个中间组件，真正被 unlink 的是链接
  //     指向的那个目录里的文件。这一条只靠"最后一段不是软链接"挡不住，所以
  //     必须单独验，本文件早期版本的注释漏了它；
  //   * unlink 本身不跟随软链接（删的是链接本身，不是它指向的目标），也不会
  //     删目录（会以 EISDIR 失败）。
  // 因此在这个时间窗里把最后一段换成别的东西，最坏结果是仓库内少了一个软链接。
  // 但校验之后文件系统若被并发改写（包括把某个祖先目录换成软链接），上面的
  // 结论就不再成立——那属于 backup_catalog.h 顶部列出的、当前不提供防护的范围。
  if (::unlink(archive_path.c_str()) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to delete backup file", archive_path));
    return false;
  }
  return true;
}

}  // namespace backupproject

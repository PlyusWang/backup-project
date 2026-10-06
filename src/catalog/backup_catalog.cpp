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
#include <limits.h>
#include <stdlib.h>
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
#include "incremental_backup.h"
#include "incremental_delta.h"

namespace backupproject {

namespace {

// 仓库里被视为"用户备份"的扩展名。大小写按 Linux 原义：.BAK 不算。
// 常量本体已经公开（backup_catalog.h 的 kBackupFileExtension），这里只是
// 本文件内部的一个短别名，避免把每一处都写长。
constexpr const char* kBackupExtension = kBackupFileExtension;
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

// 语法层面的"这是不是一个由本系统管理的归档文件名"：不写错误信息、不碰磁盘。
// 拒绝规则与 ValidateFileName 一致（空、"."、".."、含 '/' 或 '\'、含内嵌
// NUL），另加"扩展名必须是 .bak"。
// 区别在用途：调用方（realtime 的 marker 路径、增量 delta 的父快照名）先用它
// 把名字钉死，再拼接成真实路径——JoinPath 不做净化，这里就是那条路径上唯一
// 的名字边界，所以判断只能收紧，不能放松。
bool IsManagedBackupFileName(const std::string& file_name) {
  if (file_name.empty() || file_name == "." || file_name == "..") return false;
  if (file_name.find('/') != std::string::npos) return false;
  if (file_name.find('\\') != std::string::npos) return false;
  if (file_name.find('\0') != std::string::npos) return false;
  return HasBackupExtension(file_name);
}

// 仓库的持久化身份：仓库路径的规范化形式，会写进 schedule / realtime 的存储，
// 用来判断"这次要备份的仓库是不是当初绑定的那一个"。
// 优先 realpath（展开全部软链接、消掉 "." 与 ".."），失败时退回去掉尾斜杠的
// 原样文本：仓库可能还不存在（EnsureRepository 创建之前就会被问到），这时
// realpath 必然失败，退回至少保证同一串输入得到同一串输出。
// 因为会落盘，这个值必须稳定，不能掺入时间戳或临时路径；它只用于相等比较与
// 展示，拼路径一律走校验之后的形式，不拿它当安全判断。
std::string RepositoryIdentity(const std::string& repository_path) {
  if (repository_path.empty()) return std::string();
  const std::string normalized = StripTrailingSlashes(repository_path);
  char resolved[PATH_MAX];
  if (::realpath(normalized.c_str(), resolved) != nullptr) {
    return std::string(resolved);
  }
  return normalized;
}

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
    // 先按 magic 分出增量 delta。
    //
    // 顺序很重要：delta 既不是 legacy v0.1、也不是 v2 container，
    // 直接走 IdentifyArchiveFile 会被归到"认不出来"，而它其实是一份**完好**的
    // 快照。分类只读 magic，不做任何解密、不读 payload。
    if (ClassifySnapshotFile(candidate, nullptr) == SnapshotFileKind::kDelta) {
      record.incremental_delta = true;
      record.has_pipeline_methods = false;
      record.password_required = false;
      DeltaEnvelope delta_envelope;
      std::string envelope_error;
      if (!ReadDeltaEnvelope(candidate, &delta_envelope, &envelope_error)) {
        record.recognized_archive = false;
        record.diagnostic = envelope_error.empty()
                                ? "Unreadable incremental delta: " + candidate
                                : envelope_error;
      } else {
        record.recognized_archive = true;
        record.format_version =
            static_cast<std::uint16_t>(delta_envelope.format_version);
        record.parent_file_name = delta_envelope.parent_file_name;
        // 依赖链能不能恢复：这里只确认父文件还在（廉价判断）。
        // 身份是否真的对得上由恢复路径校验。
        const std::string parent_path =
            repository + "/" + delta_envelope.parent_file_name;
        struct stat parent_info;
        if (delta_envelope.parent_file_name.empty()) {
          record.chain_restorable = false;
          record.chain_diagnostic = "the delta names no parent snapshot";
        } else if (::lstat(parent_path.c_str(), &parent_info) != 0 ||
                   !S_ISREG(parent_info.st_mode)) {
          record.chain_restorable = false;
          record.chain_diagnostic =
              "parent snapshot is missing: " + delta_envelope.parent_file_name;
        } else {
          record.chain_restorable = true;
        }
        // 变化计数放进 entry_count：列表本来就有这一列，用它显示"这份 delta
        // 装了多少条变化"，不必再加一个只对这一种记录有意义的字段。
        record.entry_count = delta_envelope.added + delta_envelope.modified +
                             delta_envelope.metadata_changed;
      }
      // 注意：这个循环累加的是局部变量 found，最后由它整体赋给 *records。
      // 直接往 *records 里塞会被最后那句 move 覆盖掉。
      found.push_back(std::move(record));
      continue;
    }

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

namespace {

// 测试接缝（默认 nullptr）：见 backup_catalog.h 的声明。
bool (*g_unlink_failure_hook)(const char*) = nullptr;

// 把"这一批要删的名字"排成 descendants-first（叶子在前）。
//
// 只依据依赖图：对每个名字读一次它声明的父（信封自证，所以"边"这个形状本身
// 是可信的），而且只在**这批名字内部**连边。于是：
//   * 在这批里没有子节点的先出队；
//   * 任何一个父亲一定排在它所有后代之后；
//   * 集合内部出现环 -> 拒绝整批（环本来就没有合法顺序，更不能靠猜）。
//
// 只排顺序，不做信任判断：一份坏掉的 .bak 仍然必须删得掉（它没有可解析的父，
// 因此不连任何边）。
bool DeletionOrderByDependencies(const std::string& repository,
                                 const std::vector<std::string>& file_names,
                                 std::vector<std::size_t>* order,
                                 std::string* error_message) {
  if (order == nullptr) {
    SetError(error_message, "Deletion order output must not be null");
    return false;
  }
  order->clear();
  const std::size_t count = file_names.size();
  std::vector<std::vector<std::size_t>> children(count);
  for (std::size_t index = 0; index < count; ++index) {
    std::string parent;
    std::string read_error;
    if (!SnapshotParentOf(repository, file_names[index], &parent,
                          &read_error)) {
      continue;  // 读不出来就不连边：它不是任何人的父亲
    }
    if (parent.empty()) continue;
    for (std::size_t other = 0; other < count; ++other) {
      if (file_names[other] == parent) {
        children[other].push_back(index);
        break;
      }
    }
  }

  std::vector<bool> emitted(count, false);
  std::size_t remaining = count;
  bool progressed = true;
  while (remaining > 0 && progressed) {
    progressed = false;
    for (std::size_t index = 0; index < count; ++index) {
      if (emitted[index]) continue;
      bool has_live_child = false;
      for (const std::size_t child : children[index]) {
        if (!emitted[child]) {
          has_live_child = true;
          break;
        }
      }
      if (has_live_child) continue;
      order->push_back(index);
      emitted[index] = true;
      --remaining;
      progressed = true;
    }
  }
  if (remaining != 0) {
    SetError(error_message,
             "Cannot order this deletion set: the dependency graph inside it "
             "contains a cycle");
    return false;
  }
  return true;
}

}  // namespace

void SetBackupCatalogUnlinkFailureHookForTesting(
    bool (*hook)(const char* path)) {
  g_unlink_failure_hook = hook;
}

bool BackupCatalog::Delete(const std::string& repository,
                           const std::string& file_name,
                           std::string* error_message) const {
  return Delete(repository, file_name, nullptr, error_message);
}

bool BackupCatalog::Delete(const std::string& repository,
                           const std::string& file_name,
                           std::vector<std::string>* diagnostics,
                           std::string* error_message) const {
  return DeleteSnapshots(repository, {file_name}, nullptr, diagnostics,
                         error_message);
}

bool BackupCatalog::DeleteSnapshots(
    const std::string& repository, const std::vector<std::string>& file_names,
    std::vector<std::string>* deleted_file_names,
    std::vector<std::string>* diagnostics, std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (deleted_file_names != nullptr) deleted_file_names->clear();
  if (file_names.empty()) {
    SetError(error_message, "No snapshot was named for deletion");
    return false;
  }

  // ---- 第一遍：整体校验。任何一条不成立就一个文件都不动。----
  std::vector<std::string> archive_paths;
  archive_paths.reserve(file_names.size());
  for (const std::string& file_name : file_names) {
    std::string archive_path;
    if (!LocateDirectChildFile(repository, file_name,
                               "Failed to inspect backup file", &archive_path,
                               error_message)) {
      return false;
    }
    if (std::find(archive_paths.begin(), archive_paths.end(), archive_path) !=
        archive_paths.end()) {
      SetError(error_message,
               "Backup file was named twice for deletion: " + file_name);
      return false;
    }
    archive_paths.push_back(archive_path);

    // 依赖检查：还有后代活着、而且它不在这次要删的集合里 -> 拒绝。
    std::vector<std::string> descendants;
    if (!FindReachableDescendants(repository, file_name, &descendants,
                                  error_message)) {
      return false;
    }
    for (const std::string& child : descendants) {
      if (std::find(file_names.begin(), file_names.end(), child) !=
          file_names.end()) {
        continue;
      }
      SetError(error_message,
               "Cannot delete '" + file_name + "': the snapshot '" + child +
                   "' is built on top of it and would become unrestorable. "
                   "Delete the dependent snapshot(s) first.");
      return false;
    }
  }

  // ---- 第二遍：按依赖顺序（descendants-first）真的删 ----
  //
  // 顺序不是风格问题：如果先删祖先、删到一半崩掉，剩下的后代就指向一个不存在
  // 的父快照——那是 retention 自己制造 broken chain。叶子先删则任何中断点上
  // "还存在的子节点，其父亲也还存在"。
  //
  // 排序只依据依赖图（child -> parent 的边），**不靠 created_time 猜**：
  // 时间戳只能说明"谁先写出来"，说明不了谁依赖谁。
  std::vector<std::size_t> order;
  if (!DeletionOrderByDependencies(repository, file_names, &order,
                                   error_message)) {
    return false;
  }

  for (const std::size_t index : order) {
    const std::string& file_name = file_names[index];
    const std::string& archive_path = archive_paths[index];

    // 测试接缝：让某一次 unlink 被人为判成失败，用来验证"删到一半停住"之后
    // 剩下的链仍然自洽。默认没有钩子，产品路径不受影响。
    if (g_unlink_failure_hook != nullptr &&
        g_unlink_failure_hook(archive_path.c_str())) {
      SetError(error_message,
               "Injected unlink failure for backup file: " + archive_path);
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
    // 因此在这个时间窗里把最后一段换成别的东西，
    // 最坏结果是仓库内少了一个软链接。
    // 但校验之后文件系统若被并发改写（包括把某个祖先目录换成软链接），上面的
    // 结论就不再成立——那属于 backup_catalog.h
    // 顶部列出的、当前不提供防护的范围。
    if (::unlink(archive_path.c_str()) != 0) {
      SetError(error_message,
               Describe(errno, "Failed to delete backup file", archive_path));
      return false;
    }
    if (deleted_file_names != nullptr) deleted_file_names->push_back(file_name);

    // 副文件跟随快照一起走：只删 .bak 会让仓库里攒下一堆孤儿副文件，
    // 而且下一次同名快照出现时它们会看起来"属于"新快照。失败不静默。
    std::string normalized_repository;
    std::string normalize_error;
    if (!ValidateRepositoryDirectory(repository, &normalized_repository,
                                     &normalize_error)) {
      if (diagnostics != nullptr) {
        diagnostics->push_back("Cannot clean up the sidecars of " + file_name +
                               ": " + normalize_error);
      }
      continue;
    }
    for (const std::string& sidecar : SnapshotSidecarFileNames(file_name)) {
      const std::string sidecar_path =
          FileSystem::JoinPath(normalized_repository, sidecar);
      if (::unlink(sidecar_path.c_str()) != 0 && errno != ENOENT) {
        if (diagnostics != nullptr) {
          diagnostics->push_back("Cannot remove the sidecar " + sidecar_path +
                                 ": " + std::strerror(errno));
        }
      }
    }
  }
  return true;
}

}  // namespace backupproject

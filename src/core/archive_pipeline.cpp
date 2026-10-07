// archive_pipeline.cpp
//
// 可组合归档流水线的编排。三层算法各自不知道对方存在：
//
//   backup:  ScanSourceTree → PackEntries → compress → encrypt → container
//   restore: container header → HMAC 认证 → decrypt → decompress → unpack →
//   metadata
//
// 两条硬性约定写在这里，而不是散在三个模块里：
//   * 顺序只能是 PACK → COMPRESS → ENCRYPT（反过来先加密再压缩没有意义）；
//   * 恢复先认证再解密（Encrypt-then-MAC 的必然要求），wrong password 必须
//     在 HMAC 这一关就失败，而不是靠 PKCS#7 padding 校验失败才发现。
//
// 内存边界：三个阶段全程 bounded memory，与归档大小无关。
//   * pack       —— 逐条目流式写，固定 I/O 缓冲；
//   * compress   —— Huffman 两遍扫输入（第一遍只攒 256 个频次，第二遍边读边
//                   写比特）；LZSS 把 token
//                   流落到**私有工作目录里的临时文件**， 内存只留 32 KiB 窗口 +
//                   261 B 前瞻；
//   * encrypt    —— 固定缓冲流式加解密。
// 实测：256 MiB 语料在 RLIMIT_AS = 160 MiB 下跑完 Huffman 与 LZSS 的
// backup + restore，峰值 RSS 约 6 MiB（见 tests/unit/stream_rlimit_test.cpp）。

// 本文件是 backup/restore 的编排层：自己不做打包、压缩、加密，只负责按固定
// 顺序驱动三层算法，并在层与层之间搬运文件、传递长度、收紧失败边界。pack 格式
// 在 src/archive，压缩在 src/compression，加密在 src/crypto，container header
// 在 container_format.cpp —— 这里只消费它们暴露的接口。
//
// 对外入口：写路径 RunBackupPipeline / RunBackupPipelineFromEntries，读路径
// RunRestorePipeline / RunRestorePackedStream，另有只读的 IdentifyArchiveFile
// 与 VerifyContainerPayloadBytes（DetectKind 是本文件内部的格式探测）。
//
// 统一失败契约：返回 bool，false 时把面向用户的文本写进 error_message（允许
// 传 nullptr），errno 细节由 Describe() 拼成 "动作: 路径: strerror" 三段式。
//
// 发布语义"全有或全无"：中间产物只写在 0700 私有工作目录里，最终结果靠一次
// rename/link 出现；任何一步失败都会连带清掉工作目录，绝不留下半成品。
//
// 线程模型：本文件没有全局可变状态；同一目标路径的并发备份/恢复之间没有
// 互斥，靠内核的 no-replace 语义与"目标必须为空"的检查兜住。
#include "archive_pipeline.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "archive.h"
#include "archive_path.h"
#include "byte_order.h"
#include "compression.h"
#include "crypto.h"
#include "file_io.h"
#include "file_system.h"
#include "tree_scanner.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

// 流式搬运的块大小：所有 ReadAt / Write / Process 循环都用它。它不是性能
// 旋钮，而是内存上界的一部分——256 KiB 的栈外缓冲乘上同时存在的循环数，
// 仍然远小于测试里 160 MiB 的 RLIMIT_AS，且与归档总大小无关。
constexpr std::size_t kStreamBufferSize = 256 * 1024;

// 返回空串表示"没有父目录需要创建"：相对路径的父目录就是当前目录，调用方据此
// 跳过 MakeDirectories，而不是去 mkdir("")。
std::string ParentDirectoryOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return std::string();
  }
  if (slash == 0) {
    return std::string("/");
  }
  return path.substr(0, slash);
}

// 路径最后一段。"out/dest" -> "dest"，"dest" -> "dest"。
std::string BaseNameOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

// 去掉尾部的 '/'（保留根目录 "/"）。staging 目录名是在 destination 后面接后缀，
// 带尾斜杠会让暂存目录跑到 destination 里面去。
std::string StripTrailingSlashes(const std::string& path) {
  std::string trimmed = path;
  while (trimmed.size() > 1 && trimmed.back() == '/') {
    trimmed.pop_back();
  }
  return trimmed;
}

// 恢复前用它确认目标目录"空"。打不开目录时返回 false，也就是当作"不空"，
// 这是 fail-closed：无法确认目标为空时宁可拒绝恢复，也不在可能有内容的目录上
// 继续写。只扫一层目录项、不递归，代价与目录条目数成正比而不是子树大小。
bool IsEmptyDirectory(const std::string& path) {
  DIR* raw_dir = ::opendir(path.c_str());
  if (raw_dir == nullptr) {
    return false;
  }
  bool empty = true;
  while (struct dirent* item = ::readdir(raw_dir)) {
    const std::string name = item->d_name;
    if (name != "." && name != "..") {
      empty = false;
      break;
    }
  }
  ::closedir(raw_dir);
  return empty;
}

// PKCS#7 的补齐长度与上界判断都只有一份实现，在 container_format.cpp：
// writer 用的是和 reader 完全相同的算术与常量。

bool FileSizeOf(const std::string& path, std::uint64_t* size,
                std::string* error_message) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    SetError(error_message, Describe(errno, "Failed to inspect file", path));
    return false;
  }
  *size = static_cast<std::uint64_t>(info.st_size);
  return true;
}

// ---- 压缩阶段 --------------------------------------------------------------
//
// 输入输出都是文件，全程流式：峰值内存与归档大小无关。旧实现把整条输入读成
// std::string、再把整条输出攒成另一个 std::string，一个 1 GiB 的归档就能让
// 峰值 RSS 到 2 GiB 以上——流式实现正是为了避免这个问题。
//
// 中间产物（LZSS 的 token 流）落在调用方给的 0700 私有工作目录里，0600，
// 函数返回前由工作目录的守卫清掉。

// 输出是"要么完整、要么没有"：先写 output_file，只有 Close()（flush + fsync +
// close）成功才算成功，任何一步失败都 Abandon()，不给调用方留半成品。
// kNone 不产生新文件，只把输入文件的大小当作输出大小返回。
bool CompressFile(const std::string& input_file, const std::string& output_file,
                  CompressionMethod method,
                  const std::string& workspace_directory,
                  std::uint64_t* output_size, std::string* error_message) {
  if (method == CompressionMethod::kNone) {
    return FileSizeOf(input_file, output_size, error_message);
  }
  FileSink sink;
  if (!sink.Open(output_file, error_message)) {
    return false;
  }
  std::uint64_t produced = 0;
  const bool ok =
      method == CompressionMethod::kHuffman
          ? compression::HuffmanCompressStream(input_file, &sink, nullptr,
                                               &produced, error_message)
          : compression::LzssHuffmanCompressStream(input_file, &sink,
                                                   workspace_directory, nullptr,
                                                   &produced, error_message);
  if (!ok) {
    sink.Abandon();
    return false;
  }
  if (!sink.Close(error_message)) {
    sink.Abandon();
    return false;
  }
  *output_size = produced;
  return true;
}

bool DecompressFile(const std::string& input_file,
                    const std::string& output_file, CompressionMethod method,
                    const std::string& workspace_directory,
                    std::uint64_t expected_size, std::string* error_message) {
  if (method == CompressionMethod::kNone) {
    // 没压缩：只要长度对上，就把字节流式搬过去。
    std::uint64_t actual = 0;
    if (!FileSizeOf(input_file, &actual, error_message)) {
      return false;
    }
    if (actual != expected_size) {
      SetError(error_message,
               "Packed stream size does not match the container header");
      return false;
    }
    FileSource source;
    if (!source.Open(input_file, error_message)) {
      return false;
    }
    FileSink sink;
    if (!sink.Open(output_file, error_message)) {
      return false;
    }
    if (!source.CopyRangeTo(0, source.size(), &sink, error_message) ||
        !sink.Close(error_message)) {
      sink.Abandon();
      return false;
    }
    return true;
  }
  FileSink sink;
  if (!sink.Open(output_file, error_message)) {
    return false;
  }
  std::uint64_t written = 0;
  // 两个 Stream 解码器都会在写任何输出之前先把头部读完、把 original_size 与
  // expected_size 对上，所以"解完几 GB 才发现长度不对"这条路径不存在。
  const bool ok =
      method == CompressionMethod::kHuffman
          ? compression::HuffmanDecompressStream(
                input_file, 0, &sink, expected_size, &written, error_message)
          : compression::LzssHuffmanDecompressStream(
                input_file, &sink, workspace_directory, expected_size, &written,
                error_message);
  if (!ok) {
    sink.Abandon();
    return false;
  }
  if (!sink.Close(error_message)) {
    sink.Abandon();
    return false;
  }
  if (written != expected_size) {
    SetError(error_message,
             "Decompressed size does not match the container header");
    return false;
  }
  return true;
}

// ---- 恢复：一条条目落盘 -----------------------------------------------------

// 三步顺序有硬依赖，不能重排：lchown 在 Linux 上会清掉 setuid/setgid 位，所以
// 必须先 chown 再 chmod，否则归档里的 setgid 可执行文件会静默丢位；utimensat
// 放最后，避免前面的调用把它刚设定的时间戳再覆盖掉。
//
// 全用 l* 系列（lchown / AT_SYMLINK_NOFOLLOW）：路径本身可能就是软链接，跟随
// 链接等于去改链接目标——那是别人的文件，属于越权。
bool ApplyMetadata(const std::string& path, const ArchiveEntry& entry,
                   bool is_symlink, RestoreReport* report,
                   std::string* error_message) {
  // 1) ownership。归档精确保存 uid/gid，但非 root 进程不允许把文件改成任意
  //    属主。这里的策略是"尽力而为 + 如实记录"：权限不够就记一条诊断，
  //    绝不让整次普通恢复因此不可用，也绝不假装已经完全恢复。
  // 只有"权限不足"这一类才降级成诊断：EPERM/EACCES 是预期的非 root 情形，
  // EINVAL 是非 root 试图设置不认识的 id。其它 errno（ENOENT/EIO）说明暂存目录
  // 结构已经坏了，必须硬失败，不能把真实故障说成"权限问题"糊过去。
  if (::lchown(path.c_str(), static_cast<uid_t>(entry.uid),
               static_cast<gid_t>(entry.gid)) != 0) {
    if (errno == EPERM || errno == EACCES || errno == EINVAL) {
      if (report != nullptr) {
        report->skipped_ownership += 1;
        report->notes.push_back(
            "ownership not restored (insufficient privilege): " + path);
      }
    } else {
      SetError(error_message,
               Describe(errno, "Failed to restore ownership", path));
      return false;
    }
  }
  // 2) mode。软链接不 chmod：chmod 会跟随链接，等于去改别人的目标文件。
  if (!is_symlink &&
      ::chmod(path.c_str(), static_cast<mode_t>(entry.mode)) != 0) {
    SetError(error_message, Describe(errno, "Failed to restore mode", path));
    return false;
  }
  // 3) mtime。atime 用 UTIME_OMIT：恢复不该顺手改掉"上次访问时间"。
  struct timespec times[2];
  times[0].tv_sec = 0;
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = static_cast<time_t>(entry.mtime_sec);
  times[1].tv_nsec = static_cast<long>(entry.mtime_nsec);
  if (::utimensat(AT_FDCWD, path.c_str(), times,
                  is_symlink ? AT_SYMLINK_NOFOLLOW : 0) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to restore modification time", path));
    return false;
  }
  return true;
}

// 把一条条目恢复到暂存目录里。硬链接目标在后时先挂起，主循环结束后再补。
// 归档内的父路径："a/b/c" -> "a/b"，"a" -> "."。
std::string ArchiveParentOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Phase A + B：从全部条目路径推导出需要的目录集合（**含隐式父目录**），
// 按深度升序把骨架建出来。
//
// 为什么必须处理隐式父目录：标准 tar 不保证 "a/" 一定先于 "a/b.txt" 出现，
// 合法归档里完全可以只有后者。逐条往下建就会在 "staging/a 不存在" 上失败。
//
// "某个父路径被显式声明为非目录"这种归档在 Scan 阶段就已经被
// ArchivePathRegistry 拒绝，所以这里只可能遇到正常结构。
bool BuildDirectorySkeleton(const std::vector<PackedEntry>& entries,
                            const std::string& staging_root,
                            std::string* error_message) {
  std::vector<std::string> directories;
  for (const PackedEntry& record : entries) {
    const std::string& path = record.entry.archive_path;
    if (path == ".") {
      continue;
    }
    if (record.entry.type == EntryType::kDirectory) {
      directories.push_back(path);
    }
    std::string parent = ArchiveParentOf(path);
    while (parent != "." && !parent.empty()) {
      directories.push_back(parent);
      parent = ArchiveParentOf(parent);
    }
  }
  // 深度升序保证父目录一定先于子目录被 mkdir；同深度再按字典序，只是为了让
  // 创建顺序可复现（同一个归档在两台机器上产生同样的 syscall 序列，便于测试
  // 断言与排障），深度相同的两个目录之间本来没有依赖关系。
  std::sort(directories.begin(), directories.end(),
            [](const std::string& left, const std::string& right) {
              const std::size_t depth_left = ArchivePathDepth(left);
              const std::size_t depth_right = ArchivePathDepth(right);
              if (depth_left != depth_right) {
                return depth_left < depth_right;
              }
              return left < right;
            });
  directories.erase(std::unique(directories.begin(), directories.end()),
                    directories.end());

  for (const std::string& path : directories) {
    const std::string target = JoinArchivePath(staging_root, path);
    // 目录先一律 0700：归档里可能是 0555，先设成最终权限会让子文件写不进去。
    // 已经是目录（显式条目 + 隐式父目录重合）不是错误。
    if (::mkdir(target.c_str(), 0700) != 0 && errno != EEXIST) {
      SetError(error_message,
               Describe(errno, "Failed to create directory", target));
      return false;
    }
  }
  return true;
}

// Phase C：把一条"叶子条目"恢复到暂存目录里——普通文件 / 软链接 / FIFO /
// 字符设备 / 块设备。目录在 Phase B 建好了，硬链接在 Phase D 统一解决。
// 该函数是"类型分发点"：目录条目与硬链接条目走到这里，说明 packed 流与恢复流程
// 的约定不一致，属于内部错误，直接报错而不是猜一个合理行为。
//
// 特殊文件一律先以 0600 创建，最终 mode 交给 ApplyMetadata 统一设置：创建窗口
// 内不会短暂出现一个按归档 mode（可能是 0666）对外可见的文件。
bool CreateLeafEntry(const PackedStreamReader& reader, std::size_t index,
                     const std::string& staging_root, RestoreReport* report,
                     std::string* error_message) {
  const ArchiveEntry& entry = reader.entries()[index].entry;
  const std::string target_path =
      JoinArchivePath(staging_root, entry.archive_path);

  switch (entry.type) {
    case EntryType::kRegularFile: {
      if (!reader.ExtractPayload(index, target_path, error_message)) {
        return false;
      }
      return ApplyMetadata(target_path, entry, /*is_symlink=*/false, report,
                           error_message);
    }
    case EntryType::kSymlink: {
      if (::symlink(entry.link_target.c_str(), target_path.c_str()) != 0) {
        SetError(
            error_message,
            Describe(errno, "Failed to create symbolic link", target_path));
        return false;
      }
      return ApplyMetadata(target_path, entry, /*is_symlink=*/true, report,
                           error_message);
    }
    case EntryType::kFifo: {
      if (::mkfifo(target_path.c_str(), 0600) != 0) {
        SetError(error_message,
                 Describe(errno, "Failed to create FIFO", target_path));
        return false;
      }
      return ApplyMetadata(target_path, entry, /*is_symlink=*/false, report,
                           error_message);
    }
    case EntryType::kCharDevice:
    case EntryType::kBlockDevice: {
      const mode_t type_bits =
          entry.type == EntryType::kCharDevice ? S_IFCHR : S_IFBLK;
      const dev_t device =
          static_cast<dev_t>(MakeDevice(entry.dev_major, entry.dev_minor));
      if (::mknod(target_path.c_str(), type_bits | 0600, device) != 0) {
        // 非 root 一定拿不到 CAP_MKNOD。明确失败并说清原因：既不静默降级成
        // 普通文件，也不假装恢复了设备节点。
        SetError(
            error_message,
            Describe(errno, "Failed to create device node (requires CAP_MKNOD)",
                     target_path));
        return false;
      }
      return ApplyMetadata(target_path, entry, /*is_symlink=*/false, report,
                           error_message);
    }
    case EntryType::kDirectory:
    case EntryType::kHardLink:
      SetError(error_message,
               "Internal error: 该条目类型不由 CreateLeafEntry 处理");
      return false;
    case EntryType::kSocket:
      SetError(error_message,
               "Unsupported special type: socket: " + entry.archive_path);
      return false;
  }
  SetError(error_message, "Internal error: unknown entry type");
  return false;
}

void SortDirectoriesDeepestFirst(const PackedStreamReader& reader,
                                 std::vector<std::size_t>* indices) {
  std::sort(indices->begin(), indices->end(),
            [&reader](std::size_t left, std::size_t right) {
              const std::string& a = reader.entries()[left].entry.archive_path;
              const std::string& b = reader.entries()[right].entry.archive_path;
              const std::size_t depth_a = ArchivePathDepth(a);
              const std::size_t depth_b = ArchivePathDepth(b);
              if (depth_a != depth_b) {
                return depth_a > depth_b;
              }
              return a > b;
            });
}

}  // namespace

// ---- 备份 ------------------------------------------------------------------

// 备份入口：先做纯检查（参数非空、源目录存在且是目录、目标不在源目录内部），
// 再扫描出条目表，然后交给 RunBackupPipelineFromEntries。检查全部前置，是因为
// 扫描很贵（要 stat 整棵树），不能等扫完才发现目标写在自己家里。
//
// filter 的生命周期只覆盖 ScanSourceTree：条目表一旦生成，过滤结果就已经固化，
// 后续流程不再看 filter。
bool RunBackupPipeline(const std::string& source_directory,
                       const std::string& archive_file, const Filter& filter,
                       const BackupOptions& options,
                       std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (source_directory.empty() || archive_file.empty()) {
    SetError(error_message,
             "Source directory and archive file must both be provided.");
    return false;
  }

  struct stat source_info;
  if (::lstat(source_directory.c_str(), &source_info) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to inspect source directory",
                      source_directory));
    return false;
  }
  if (!S_ISDIR(source_info.st_mode)) {
    SetError(error_message, "Source is not a directory: " + source_directory);
    return false;
  }

  FileSystem file_system;
  if (!file_system.IsDestinationOutsideSource(source_directory, archive_file,
                                              error_message)) {
    return false;
  }

  // 扫描：三种 pack 后端消费同一份条目表。
  std::vector<ArchiveEntry> entries;
  if (!ScanSourceTree(source_directory, &filter, &entries, error_message)) {
    return false;
  }
  return RunBackupPipelineFromEntries(entries, archive_file, options,
                                      error_message);
}

// 调用方契约：entries 必须是 ScanSourceTree 的产物（或测试里人工构造的
// 等价物），且首元素必须代表源根目录（"."、kDirectory），所有
// archive_path 都相对它解释 —— 三条下游流程都建立在这条不变量上。
// 从一份已经准备好的条目表走完整条流水线：扫描是唯一被跳过的步骤，
// 三种 pack 后端、压缩、加密、container 写入全部照常执行。
//
// 存在的意义是"设备节点"这类东西：非 root 环境造不出真的字符/块设备，
// 但格式层与 metadata 层必须被完整覆盖，所以测试需要能直接构造条目表。
bool RunBackupPipelineFromEntries(const std::vector<ArchiveEntry>& entries,
                                  const std::string& archive_file,
                                  const BackupOptions& options,
                                  std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (archive_file.empty()) {
    SetError(error_message, "Archive file must be provided.");
    return false;
  }
  if (options.encryption_method != EncryptionMethod::kNone &&
      options.password.empty()) {
    // 空密码不是"没有密码"，而是"任何人都能解开的密码"。直接拒绝。
    SetError(error_message,
             "A non-empty password is required when encryption is enabled.");
    return false;
  }
  // 首元素必须是源根目录：它定义了归档里所有路径的解释基准，packed 流、恢复端
  // preflight 与 legacy reader 都依赖这条约定。
  if (entries.empty() || entries.front().archive_path != "." ||
      entries.front().type != EntryType::kDirectory) {
    SetError(error_message,
             "Internal error: the entry list must start with the source root");
    return false;
  }
  // 目标必须尚不存在。这里用 lstat + errno == ENOENT 而不是 access()：判断
  // 与真正发布之间的竞态由最后 PublishNoReplace() 的内核语义兜住，此处只是
  // 尽早给出友好报错，不承担安全职责。
  struct stat target_info;
  if (::lstat(archive_file.c_str(), &target_info) == 0) {
    SetError(error_message, "Archive file already exists: " + archive_file);
    return false;
  }
  if (errno != ENOENT) {
    SetError(error_message,
             Describe(errno, "Failed to inspect archive file", archive_file));
    return false;
  }

  FileSystem file_system;
  const std::string archive_parent = ParentDirectoryOf(archive_file);
  if (!archive_parent.empty() &&
      !file_system.MakeDirectories(archive_parent, error_message)) {
    return false;
  }
  // 私有工作目录：一次备份的所有中间产物（packed / token / compressed /
  // 未发布的 container）都住在里面。0700 的目录 + 0600 的文件意味着
  // "加密之前先把明文落到 archive parent" 这件事不再可能被别人读到，
  // 而 RAII 守卫保证成功失败都不留残余。
  TempDirectoryGuard workspace;
  if (!workspace.Create(archive_parent, ".bp-work-", error_message)) {
    return false;
  }
  const std::string packed_file = workspace.Child("packed.tmp");

  if (!PackEntries(options.pack_method, entries, packed_file, error_message)) {
    return false;
  }
  std::uint64_t packed_size = 0;
  if (!FileSizeOf(packed_file, &packed_size, error_message)) {
    return false;
  }
  if (packed_size == 0) {
    SetError(error_message, "Internal error: empty packed stream");
    return false;
  }
  // 磁盘预算 sanity check：packed + compressed + 最终 container 都可能同时
  // 存在。packed_size 是刚刚自己写出来的，不是不可信输入。
  if (!CheckFreeSpace(workspace.path(), packed_size * 3 + (1u << 20),
                      error_message)) {
    return false;
  }

  std::string compressed_file = packed_file;
  std::uint64_t compressed_size = packed_size;
  if (options.compression_method != CompressionMethod::kNone) {
    compressed_file = workspace.Child("compressed.tmp");
    if (!CompressFile(packed_file, compressed_file, options.compression_method,
                      workspace.path(), &compressed_size, error_message)) {
      return false;
    }
  }

  // ---- container header ----
  ContainerHeader header;
  header.pack_method = static_cast<std::uint8_t>(options.pack_method);
  header.compression_method =
      static_cast<std::uint8_t>(options.compression_method);
  header.encryption_method =
      static_cast<std::uint8_t>(options.encryption_method);
  header.flags = 0;
  header.entry_count = entries.size();
  header.packed_size = packed_size;
  header.compressed_size = compressed_size;

  std::string cipher_key;
  std::string mac_key;
  // 长度语义（三个 size 字段各有明确含义，读侧会逐个核对）：
  //   packed_size     —— 打包流字节数，恢复端解压后必须精确等于它；
  //   compressed_size —— 压缩流字节数，解密后必须精确等于它；
  //   payload_size    —— 磁盘上密文的字节数。AES-256-CTR 是无填充流密码，所以
  //                      与 compressed_size 相等；DES-CBC 要补 PKCS#7，所以是
  //                      DesPaddedSize() 算出的上取整长度（含 1..8 字节填充）。
  // iv_len 8/16 由算法决定（CBC 一个分组 / CTR 一个计数块），tag_len 32 则是
  // SHA-256 的完整长度——不是截断标签，随后按整长度做定长比较。
  switch (options.encryption_method) {
    case EncryptionMethod::kNone:
      // 无加密是"显式选定的一种算法"而不是缺省：所有加密参数域一律清零，读侧
      // 会校验字段与算法的组合是否自洽，残留上一次的值会让 header 自我矛盾。
      header.kdf_iterations = 0;
      header.salt_len = 0;
      header.iv_len = 0;
      header.tag_len = 0;
      header.payload_size = compressed_size;
      break;
    case EncryptionMethod::kDesCbcHmacSha256: {
      // 迭代轮数写进 header，读侧按"必须等于本版本约定的常量"校验（见
      // container_format.cpp），它的作用是让参数与算法的组合可被验证。
      header.kdf_iterations = container_v2::kProductionIterations;
      header.salt_len = 16;
      header.iv_len = 8;
      header.tag_len = 32;
      header.payload_size = DesPaddedSize(compressed_size);
      if (!crypto::RandomBytes(header.salt_len, &header.salt, error_message) ||
          !crypto::RandomBytes(header.iv_len, &header.iv, error_message)) {
        return false;
      }
      break;
    }
    case EncryptionMethod::kAes256CtrHmacSha256: {
      header.kdf_iterations = container_v2::kProductionIterations;
      header.salt_len = 16;
      header.iv_len = 16;
      header.tag_len = 32;
      header.payload_size = compressed_size;
      if (!crypto::RandomBytes(header.salt_len, &header.salt, error_message) ||
          !crypto::RandomBytes(header.iv_len, &header.iv, error_message)) {
        return false;
      }
      break;
    }
  }
  // MAC 与 SHA-256 都是"写完 payload 才知道"的值，先用全 0 占位。
  // 归一化 header（auth_tag 全 0）就是 HMAC 的输入，所以这份占位 header 的
  // 字节可以直接拿来做 MAC 的前缀。
  header.auth_tag.assign(header.tag_len, '\0');
  header.payload_sha256.assign(container_v2::kSha256Size, '\0');

  // header 先编码成字节串再写：EncodeContainerHeader 是唯一的序列化实现，
  // 后面算 HMAC 时还要再编码一次（归一化 header），两处字节因此必然一致。
  std::string header_bytes;
  if (!EncodeContainerHeader(header, &header_bytes, error_message)) {
    return false;
  }

  if (options.encryption_method != EncryptionMethod::kNone &&
      !DeriveKeys(options.password, header, &cipher_key, &mac_key,
                  error_message)) {
    return false;
  }

  // 正式 .bak 不是边生成边发布的：先在私有目录里写完整的 container。
  const std::string container_file = workspace.Child("container.tmp");
  FileSink sink;
  if (!sink.Open(container_file, error_message)) {
    return false;
  }
  bool ok = sink.Write(header_bytes.data(), header_bytes.size(), error_message);

  // SHA-256 覆盖"写出去的密文"而不是明文：恢复端在不解密的前提下就能校验它，
  // 把"归档损坏"与"密码错误"两种失败彻底分开。
  crypto::Sha256 sha;

  // 两个加密器无条件构造（kNone 时密钥为空、valid() 为 false），只有真正用到的
  // 那个才检查 valid()。构造失败意味着 DeriveKeys 给出的密钥长度不对，属于内部
  // 错误：必须停下，不能拿一把无效密钥继续写文件。
  crypto::DesCbcEncryptor des_encryptor(cipher_key, header.iv);
  crypto::Aes256Ctr aes_ctr(cipher_key, header.iv);
  if (ok && options.encryption_method == EncryptionMethod::kDesCbcHmacSha256 &&
      !des_encryptor.valid()) {
    SetError(error_message, "Internal error: invalid DES key");
    ok = false;
  }
  if (ok &&
      options.encryption_method == EncryptionMethod::kAes256CtrHmacSha256 &&
      !aes_ctr.valid()) {
    SetError(error_message, "Internal error: invalid AES key");
    ok = false;
  }

  std::uint64_t payload_bytes = 0;
  if (ok) {
    FileSource plain;
    if (!plain.Open(compressed_file, error_message)) {
      ok = false;
    } else {
      std::vector<unsigned char> buffer(kStreamBufferSize);
      std::uint64_t offset = 0;
      const std::uint64_t total = plain.size();
      while (ok && offset < total) {
        const std::uint64_t remaining = total - offset;
        const std::size_t want = static_cast<std::size_t>(
            remaining < kStreamBufferSize ? remaining : kStreamBufferSize);
        if (!plain.ReadAt(offset, buffer.data(), want, error_message)) {
          ok = false;
          break;
        }
        // 每块独立 Process：DES-CBC 把链式状态留在 encryptor 里，最后 Finish
        // 才吐带 padding 的尾块；AES-CTR 只推进计数器。两者都不需要把整条
        // payload 放进内存，这正是"峰值 RSS 与归档大小无关"的来源。
        std::string chunk;
        switch (options.encryption_method) {
          case EncryptionMethod::kNone:
            chunk.assign(reinterpret_cast<const char*>(buffer.data()), want);
            break;
          case EncryptionMethod::kDesCbcHmacSha256:
            des_encryptor.Process(buffer.data(), want, &chunk);
            break;
          case EncryptionMethod::kAes256CtrHmacSha256:
            aes_ctr.Process(buffer.data(), want, &chunk);
            break;
        }
        if (!chunk.empty()) {
          if (!sink.Write(chunk.data(), chunk.size(), error_message)) {
            ok = false;
            break;
          }
          sha.Update(chunk.data(), chunk.size());
          payload_bytes += chunk.size();
        }
        offset += want;
      }
      if (ok &&
          options.encryption_method == EncryptionMethod::kDesCbcHmacSha256) {
        std::string tail;
        if (!des_encryptor.Finish(&tail, error_message)) {
          ok = false;
        } else if (!tail.empty()) {
          if (!sink.Write(tail.data(), tail.size(), error_message)) {
            ok = false;
          } else {
            sha.Update(tail.data(), tail.size());
            payload_bytes += tail.size();
          }
        }
      }
      plain.Close();
    }
  }

  // 自检而不是输入校验：payload_bytes 是刚刚自己写出去的字节数，对不上说明
  // header 与写入逻辑已经不同步（例如 DesPaddedSize 的算法改过），是编程错误。
  if (ok && payload_bytes != header.payload_size) {
    SetError(
        error_message,
        "Internal error: payload size does not match the container header");
    ok = false;
  }
  if (ok) {
    unsigned char digest[crypto::kSha256DigestSize];
    sha.Final(digest);
    header.payload_sha256.assign(reinterpret_cast<const char*>(digest),
                                 crypto::kSha256DigestSize);
    // Patch 会先 Flush，所以这一步之后文件里的 header 已经是最终版本
    // （auth_tag 仍然是全 0），可以直接当 HMAC 的归一化 header 用。
    if (!sink.Patch(container_v2::kPayloadSha256Offset,
                    header.payload_sha256.data(), header.payload_sha256.size(),
                    error_message)) {
      ok = false;
    }
  }
  if (ok && options.encryption_method != EncryptionMethod::kNone) {
    // HMAC 的输入是"归一化 header + 密文"，而 payload_sha256 属于参与 MAC 的
    // 字段之一（只有 auth_tag 视为全 0）。所以它必须先定下来，MAC 只能在这一
    // 步之后算——这也是本函数要再读一遍密文的原因。
    //
    // 归一化 header 就是刚才写进文件的那 160 字节：tag 全 0，其余都是最终值。
    std::string normalized;
    if (!EncodeContainerHeader(header, &normalized, error_message)) {
      ok = false;
    } else {
      crypto::HmacSha256 mac(mac_key);
      mac.Update(normalized.data(), normalized.size());
      // 再顺序读一遍刚写完的密文来喂 HMAC：写的过程中只保留了 SHA-256 状态，
      // 没有第二份密文副本，所以这里用一次磁盘读换取 O(1) 的内存占用。
      // sink 之前已经 Patch 过（Patch 内部先 Flush），读到的一定是完整密文。
      FileSource ciphertext;
      if (!ciphertext.Open(sink.path(), error_message)) {
        ok = false;
      } else {
        std::vector<unsigned char> buffer(kStreamBufferSize);
        std::uint64_t offset = 0;
        const std::uint64_t total = header.payload_size;
        while (ok && offset < total) {
          const std::uint64_t remaining = total - offset;
          const std::size_t want = static_cast<std::size_t>(
              remaining < kStreamBufferSize ? remaining : kStreamBufferSize);
          if (!ciphertext.ReadAt(container_v2::kHeaderSize + offset,
                                 buffer.data(), want, error_message)) {
            ok = false;
            break;
          }
          mac.Update(buffer.data(), want);
          offset += want;
        }
        ciphertext.Close();
      }
      if (ok) {
        unsigned char tag[crypto::kSha256DigestSize];
        mac.Final(tag);
        // MAC 成功后 tag 才写回 header 缓冲并 Patch 进文件；中途失败时磁盘上的
        // tag 仍是全 0，而那个半成品根本不会被发布（发布只在 ok 时发生）。
        header.auth_tag.assign(reinterpret_cast<const char*>(tag),
                               crypto::kSha256DigestSize);
        if (!sink.Patch(container_v2::kAuthTagOffset, header.auth_tag.data(),
                        header.auth_tag.size(), error_message)) {
          ok = false;
        }
      }
    }
  }
  if (ok) {
    ok = sink.Close(error_message);
  }
  if (ok) {
    // 完整、已 fsync 的 container 才发布成 archive_file，而且绝不覆盖：
    // link() 的 EEXIST 由内核保证，不存在 "unlink 之后再 rename" 那段窗口。
    // container 必须与 archive_file 在同一文件系统：link() 不跨设备（EXDEV），
    // 这正是工作目录建在 archive 父目录下、而不是 /tmp 下的原因。
    ok = PublishNoReplace(container_file, archive_file, error_message);
  }
  if (!ok) {
    sink.Abandon();
    return false;
  }
  return true;
}

// ---- 恢复 ------------------------------------------------------------------

namespace {

// 只读地辨认 .bak 的类型：不看扩展名，只看 magic。
// v0.1 的 mypack 归档以 "BKPARCH" + NUL 开头，v2 container 以 "BKPCNT2" +
// NUL 开头，两者都只认 magic、不认扩展名（用户改名或去掉后缀是常态）。认不
// 出来时返回 kUnknown 并填 error，调用方据此拒绝而不是按默认格式硬解。
ArchiveFileInfo::Kind DetectKind(const std::string& archive_file,
                                 std::string* error_message) {
  FileSource source;
  if (!source.Open(archive_file, error_message)) {
    return ArchiveFileInfo::Kind::kUnknown;
  }
  unsigned char magic[container_v2::kMagicSize];
  if (source.size() < sizeof(magic) ||
      !source.ReadAt(0, magic, sizeof(magic), error_message)) {
    SetError(error_message,
             "File is too small to be an archive: " + archive_file);
    return ArchiveFileInfo::Kind::kUnknown;
  }
  if (LooksLikeContainer(magic, sizeof(magic))) {
    return ArchiveFileInfo::Kind::kContainerV2;
  }
  constexpr unsigned char kMyPackMagic[8] = {'B', 'K', 'P', 'A',
                                             'R', 'C', 'H', '\0'};
  if (std::memcmp(magic, kMyPackMagic, sizeof(kMyPackMagic)) == 0) {
    return ArchiveFileInfo::Kind::kLegacyV01;
  }
  SetError(error_message, "Unrecognized archive format: " + archive_file);
  return ArchiveFileInfo::Kind::kUnknown;
}

// 认证第一遍：把整条 payload 读一遍，算 HMAC 与 SHA-256，和 header 里的值比对。
// 这一遍不产生任何写入，所以 wrong password / 被篡改的归档在这里就被拦下。
// 前置条件：header 已成功解码，source 已打开，且 payload_size 已与文件长度核对
// 一致。调用方的顺序保证是：本函数在解密之前、在任何写盘之前给出结论。
//
// 先 SHA-256 后 HMAC：SHA-256 不需要密码，能把"文件损坏"与"密码不对/被篡改"
// 分成两条可区分的报错；反过来会让损坏的归档被报成密码问题。
bool AuthenticatePayload(const FileSource& source,
                         const ContainerHeader& header,
                         const std::string& mac_key,
                         std::string* error_message) {
  EncryptionMethod method = EncryptionMethod::kNone;
  if (!ParseEncryptionMethodId(header.encryption_method, &method)) {
    SetError(error_message, "Unknown encryption method id");
    return false;
  }
  // kNone 归档也要算 SHA-256：没有 HMAC 时它是唯一的完整性防线。它能发现随机
  // 损坏，但不能防篡改——摘要与 payload 在同一个文件里，攻击者可以一起改。
  // 无密码归档的安全性边界就在这里，不要把它当成认证。
  crypto::HmacSha256 mac(mac_key);
  if (method != EncryptionMethod::kNone) {
    const std::string normalized = NormalizedHeaderForMac(header);
    if (normalized.size() != container_v2::kHeaderSize) {
      SetError(error_message,
               "Internal error: bad normalized container header");
      return false;
    }
    mac.Update(normalized.data(), normalized.size());
  }
  crypto::Sha256 sha;
  std::vector<unsigned char> buffer(kStreamBufferSize);
  std::uint64_t offset = 0;
  const std::uint64_t total = header.payload_size;
  while (offset < total) {
    const std::uint64_t remaining = total - offset;
    const std::size_t want = static_cast<std::size_t>(
        remaining < kStreamBufferSize ? remaining : kStreamBufferSize);
    if (!source.ReadAt(container_v2::kHeaderSize + offset, buffer.data(), want,
                       error_message)) {
      return false;
    }
    sha.Update(buffer.data(), want);
    if (method != EncryptionMethod::kNone) {
      mac.Update(buffer.data(), want);
    }
    offset += want;
  }

  unsigned char digest[crypto::kSha256DigestSize];
  sha.Final(digest);
  const std::string computed_sha(reinterpret_cast<const char*>(digest),
                                 crypto::kSha256DigestSize);
  if (!crypto::ConstantTimeEquals(computed_sha, header.payload_sha256)) {
    SetError(error_message,
             "Payload checksum mismatch (the archive is corrupted)");
    return false;
  }
  if (method != EncryptionMethod::kNone) {
    unsigned char tag[crypto::kSha256DigestSize];
    mac.Final(tag);
    const std::string computed_tag(reinterpret_cast<const char*>(tag),
                                   crypto::kSha256DigestSize);
    // 先比 HMAC 再解密：wrong password 在这一关就失败，而不是等 PKCS#7 padding
    // 校验失败才发现。tag 比较必须是 constant-time 的。
    if (!crypto::ConstantTimeEquals(computed_tag, header.auth_tag)) {
      SetError(error_message,
               "Authentication failed: wrong password or tampered archive");
      return false;
    }
  }
  return true;
}

// 解密第二遍：认证已经通过，这一步才把 payload 变成 compressed 流。
// 输出只有两种结局：完整写出的 compressed 流，或者 false 加上一个半成品文件
// （由调用方的工作目录整体清理）。写完立刻与 header.compressed_size 对账，用来
// 拦住去填充或密文截断引起的静默偏差。
bool DecryptPayload(const FileSource& source, const ContainerHeader& header,
                    const std::string& cipher_key,
                    const std::string& output_file,
                    std::string* error_message) {
  FileSink sink;
  if (!sink.Open(output_file, error_message)) {
    return false;
  }
  std::vector<unsigned char> buffer(kStreamBufferSize);
  std::uint64_t offset = 0;
  const std::uint64_t total = header.payload_size;
  bool ok = true;
  crypto::DesCbcDecryptor des_decryptor(cipher_key, header.iv);
  crypto::Aes256Ctr aes_ctr(cipher_key, header.iv);
  while (ok && offset < total) {
    const std::uint64_t remaining = total - offset;
    const std::size_t want = static_cast<std::size_t>(
        remaining < kStreamBufferSize ? remaining : kStreamBufferSize);
    if (!source.ReadAt(container_v2::kHeaderSize + offset, buffer.data(), want,
                       error_message)) {
      ok = false;
      break;
    }
    std::string chunk;
    switch (header.encryption_method) {
      // 这里用数值 id 而不是枚举：header.encryption_method 是不可信的原始字节，
      // 上面的 ParseEncryptionMethodId 已经把未知 id 归一化成 kNone；default
      // 分支仍然保留，作为"解析与使用不一致"的最后一道自检。
      case 0:
        chunk.assign(reinterpret_cast<const char*>(buffer.data()), want);
        break;
      case 1:
        des_decryptor.Process(buffer.data(), want, &chunk);
        break;
      case 2:
        aes_ctr.Process(buffer.data(), want, &chunk);
        break;
      default:
        SetError(error_message, "Unknown encryption method id");
        ok = false;
        break;
    }
    if (!ok) {
      break;
    }
    if (!chunk.empty() &&
        !sink.Write(chunk.data(), chunk.size(), error_message)) {
      ok = false;
      break;
    }
    offset += want;
  }
  if (ok && header.encryption_method == 1) {
    std::string tail;
    if (!des_decryptor.Finish(&tail, error_message)) {
      ok = false;
    } else if (!tail.empty() &&
               !sink.Write(tail.data(), tail.size(), error_message)) {
      ok = false;
    }
  }
  if (ok && sink.bytes_written() != header.compressed_size) {
    SetError(error_message,
             "Decrypted size does not match the container header");
    ok = false;
  }
  if (ok) {
    ok = sink.Close(error_message);
  }
  if (!ok) {
    sink.Abandon();
    return false;
  }
  return true;
}

}  // namespace

// 恢复入口的分层顺序（每层只依赖上一层已经验证过的事实）：
//   magic → 目标目录 → header 解码 → 长度自洽 → 派生密钥 → 只读认证 →
//   解密 → 压缩流头部自洽 → 解压 → RunRestorePackedStream。
// 真正的写入发生在最后那次 staging rename，前面任何一步失败都不会碰
// destination，所以恢复路径不需要"失败后回滚"这一层逻辑。
bool RunRestorePipeline(const std::string& archive_file,
                        const std::string& destination_directory,
                        const RestoreOptions& options, RestoreReport* report,
                        std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (report != nullptr) {
    *report = RestoreReport();
  }
  if (archive_file.empty() || destination_directory.empty()) {
    SetError(error_message,
             "Archive file and destination directory must both be provided.");
    return false;
  }
  const std::string destination = StripTrailingSlashes(destination_directory);
  // 拒绝恢复到 "/"：归档里可以有任意路径，写进根目录等于让归档内容变成系统的
  // 一部分。空路径同样拒绝（StripTrailingSlashes 已经去掉多余的尾斜杠）。
  if (destination.empty() || destination == "/") {
    SetError(error_message,
             "Invalid destination directory: " + destination_directory);
    return false;
  }

  const ArchiveFileInfo::Kind kind = DetectKind(archive_file, error_message);
  if (kind == ArchiveFileInfo::Kind::kUnknown) {
    return false;
  }
  if (kind == ArchiveFileInfo::Kind::kLegacyV01) {
    // v0.1 的归档没有密码、没有压缩、没有加密：原样交给 legacy reader，
    // 两者用的是同一份 preflight 与同一份路径规则。
    ArchiveReader reader;
    return reader.Extract(archive_file, destination, error_message);
  }

  // destination 的检查放在最前面：失败时连暂存目录都不会建出来。
  struct stat destination_info;
  if (::lstat(destination.c_str(), &destination_info) == 0) {
    if (!S_ISDIR(destination_info.st_mode)) {
      SetError(error_message,
               "Destination exists and is not a directory: " + destination);
      return false;
    }
    // 目标必须是空目录：本工具不做 merge，也不覆盖已有内容。这条 fail-closed
    // 策略让"恢复到错误位置"最多是个空操作，而不是一次静默的数据破坏。
    if (!IsEmptyDirectory(destination)) {
      SetError(error_message,
               "Destination directory is not empty: " + destination);
      return false;
    }
  } else if (errno != ENOENT) {
    SetError(error_message,
             Describe(errno, "Failed to inspect destination", destination));
    return false;
  }

  FileSource source;
  if (!source.Open(archive_file, error_message)) {
    return false;
  }
  if (source.size() < container_v2::kHeaderSize) {
    SetError(error_message, "Truncated container header: " + archive_file);
    return false;
  }
  unsigned char header_block[container_v2::kHeaderSize];
  if (!source.ReadAt(0, header_block, sizeof(header_block), error_message)) {
    return false;
  }
  ContainerHeader header;
  if (!DecodeContainerHeader(header_block, sizeof(header_block), &header,
                             error_message)) {
    return false;
  }
  // 容器后面不允许有多余字节：payload_size 必须精确对上文件剩下的长度。
  if (header.payload_size != source.size() - container_v2::kHeaderSize) {
    SetError(
        error_message,
        "Container payload size does not match the file size: " + archive_file);
    return false;
  }

  EncryptionMethod encryption = EncryptionMethod::kNone;
  ParseEncryptionMethodId(header.encryption_method, &encryption);
  std::string cipher_key;
  std::string mac_key;
  if (encryption != EncryptionMethod::kNone &&
      !DeriveKeys(options.password, header, &cipher_key, &mac_key,
                  error_message)) {
    return false;
  }
  // 第一遍：只读认证。HMAC 通过之前不写一个字节。
  if (!AuthenticatePayload(source, header, mac_key, error_message)) {
    return false;
  }

  // 工作目录建在 destination 的父目录下：同文件系统才能让最后一步是纯 rename
  // （跨设备会 EXDEV），而且 0700 的临时目录落在 destination 外面，不会污染
  // 即将整体发布的恢复结果。
  const std::string destination_parent = ParentDirectoryOf(destination);
  FileSystem file_system;
  if (!destination_parent.empty() &&
      !file_system.MakeDirectories(destination_parent, error_message)) {
    return false;
  }
  // 私有工作目录：解密结果与解压结果都落在里面，0600，用完即清。
  TempDirectoryGuard workspace;
  if (!workspace.Create(destination_parent, ".bp-work-", error_message)) {
    return false;
  }
  // 磁盘预算：解密后的 payload 与解压后的 packed 流会同时存在。
  // payload_size / packed_size 都是不可信的归档元数据，这里只当作
  // "需要多少空间"的估计，绝不当作数组大小。
  if (!CheckFreeSpace(workspace.path(),
                      header.payload_size + header.packed_size,
                      error_message)) {
    return false;
  }
  const std::string compressed_file = workspace.Child("compressed.tmp");
  // 第二遍：认证已过，这时才解密。
  if (!DecryptPayload(source, header, cipher_key, compressed_file,
                      error_message)) {
    return false;
  }
  source.Close();

  CompressionMethod compression = CompressionMethod::kNone;
  ParseCompressionMethodId(header.compression_method, &compression);
  // 解压之前先把两层头部与三处长度对上。这一步不做任何大块输出：
  // 坏归档在这里就被拒绝，而不是解完几个 GB 才发现长度不对。
  std::uint64_t token_stream_size = 0;
  if (compression == CompressionMethod::kHuffman) {
    compression::HuffmanStreamInfo info;
    if (!compression::HuffmanReadStreamInfo(compressed_file, 0, &info,
                                            error_message)) {
      return false;
    }
    if (info.original_size != header.packed_size) {
      SetError(error_message,
               "Compressed stream original_size does not match the container "
               "header");
      return false;
    }
  } else if (compression == CompressionMethod::kLzssHuffman) {
    // LZSS 是两层：外层 original_size 必须对上 header.packed_size，内层
    // original_size 必须对上 token_stream_size。token 流是解压期间落在工作
    // 目录里的中间文件，它的长度只有在这个头部里才能提前知道。
    compression::LzssHuffmanStreamInfo info;
    if (!compression::LzssHuffmanReadStreamInfo(compressed_file, &info,
                                                error_message)) {
      return false;
    }
    if (info.original_size != header.packed_size ||
        info.inner.original_size != info.token_stream_size) {
      SetError(error_message,
               "Compressed stream sizes do not match the container header");
      return false;
    }
    token_stream_size = info.token_stream_size;
    // 再加上 LZSS 的 token 临时文件：它和解压输出会同时存在。
    if (!CheckFreeSpace(
            workspace.path(),
            header.payload_size + header.packed_size + token_stream_size,
            error_message)) {
      return false;
    }
  }
  const std::string packed_file = workspace.Child("packed.tmp");
  if (!DecompressFile(compressed_file, packed_file, compression,
                      workspace.path(), header.packed_size, error_message)) {
    return false;
  }

  PackMethod pack_method = PackMethod::kMyPack;
  ParsePackMethodId(header.pack_method, &pack_method);
  // preflight、暂存目录、metadata 收尾、rename —— 这些步骤与"直接从一条
  // packed 流恢复"完全一样，所以只有一份实现。
  return RunRestorePackedStream(packed_file, pack_method, header.entry_count,
                                destination, report, error_message);
}

// 整条恢复路径上唯一真正写 destination 的地方，采用两阶段提交：
//   Scan(preflight) → staging → Phase A..E → rename(staging, destination)。
// preflight 把整条 packed 流校验完（含硬链接环检测）才动磁盘；一旦开始写，所有
// 内容都落在 staging 里，失败时整棵树丢弃，destination 保持原样。
bool RunRestorePackedStream(const std::string& packed_file,
                            PackMethod pack_method,
                            std::uint64_t expected_entry_count,
                            const std::string& destination_directory,
                            RestoreReport* report, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (report != nullptr) {
    *report = RestoreReport();
  }
  if (packed_file.empty() || destination_directory.empty()) {
    SetError(error_message,
             "Packed stream and destination directory must both be provided.");
    return false;
  }
  const std::string destination = StripTrailingSlashes(destination_directory);
  if (destination.empty() || destination == "/") {
    SetError(error_message,
             "Invalid destination directory: " + destination_directory);
    return false;
  }
  // destination 的检查放在最前面：失败时连暂存目录都不会建出来。
  struct stat destination_info;
  if (::lstat(destination.c_str(), &destination_info) == 0) {
    if (!S_ISDIR(destination_info.st_mode)) {
      SetError(error_message,
               "Destination exists and is not a directory: " + destination);
      return false;
    }
    if (!IsEmptyDirectory(destination)) {
      SetError(error_message,
               "Destination directory is not empty: " + destination);
      return false;
    }
  } else if (errno != ENOENT) {
    SetError(error_message,
             Describe(errno, "Failed to inspect destination", destination));
    return false;
  }

  PackedStreamReader reader;
  if (!reader.Open(packed_file, error_message)) {
    return false;
  }
  // preflight：整条 packed 流校验完才动磁盘。
  if (!reader.Scan(pack_method, error_message)) {
    return false;
  }
  // 条目数来自 container header（压缩/加密层），entries() 来自 packed 流（格式
  // 层）。两者相等是"header 与内容同源"的第二份证据，第一份是上层核对过的
  // packed_size。
  if (reader.entries().size() != expected_entry_count) {
    SetError(error_message, "Entry count does not match the expected count");
    return false;
  }

  // 暂存目录用 mkdtemp：同样一个进程连续恢复两次也不会撞名（以前是用 pid
  // 拼出来的，pid 写了两遍，等于没有唯一性）。0700，与 destination 同一个
  // 文件系统，所以最后一步是纯 rename。
  const std::string staging_parent = ParentDirectoryOf(destination);
  FileSystem file_system;
  if (!staging_parent.empty() &&
      !file_system.MakeDirectories(staging_parent, error_message)) {
    return false;
  }
  TempDirectoryGuard staging_guard;
  if (!staging_guard.Create(staging_parent, BaseNameOf(destination) + ".bptmp-",
                            error_message)) {
    return false;
  }
  const std::string staging = staging_guard.path();

  bool ok = true;
  // Phase A + B：目录骨架（显式目录 + 隐式父目录），深度升序，一律 0700。
  ok = BuildDirectorySkeleton(reader.entries(), staging, error_message);

  // Phase C：普通文件 / 软链接 / FIFO / 设备。硬链接留到 Phase D。
  // 顺序遍历而不是递归：packed 流是平的，目录层级由 Phase B 的骨架加路径拼接
  // 体现，所以这里既不需要建树，也不需要显式栈。
  std::vector<std::size_t> pending_hardlinks;
  for (std::size_t index = 0; ok && index < reader.entries().size(); ++index) {
    const EntryType type = reader.entries()[index].entry.type;
    if (type == EntryType::kDirectory) {
      continue;
    }
    if (type == EntryType::kHardLink) {
      pending_hardlinks.push_back(index);
      continue;
    }
    ok = CreateLeafEntry(reader, index, staging, report, error_message);
  }

  // Phase D：硬链接。目标已经出现就直接建，没出现就挂起；反复重试到没有进展
  // 再判失败。环与自指已经在 preflight 被拒绝（ustar::Scan 的三色标记 /
  // MyPack 的目标必须是普通文件那一条），所以这里不会死循环。
  while (ok && !pending_hardlinks.empty()) {
    std::vector<std::size_t> still_pending;
    bool progress = false;
    for (const std::size_t index : pending_hardlinks) {
      const ArchiveEntry& entry = reader.entries()[index].entry;
      const std::string link_source =
          JoinArchivePath(staging, entry.link_target);
      struct stat link_info;
      if (::lstat(link_source.c_str(), &link_info) != 0) {
        still_pending.push_back(index);
        continue;
      }
      const std::string target_path =
          JoinArchivePath(staging, entry.archive_path);
      if (::link(link_source.c_str(), target_path.c_str()) != 0) {
        SetError(error_message,
                 Describe(errno, "Failed to create hard link", target_path));
        ok = false;
        break;
      }
      // 硬链接与目标共享 inode，metadata 由第一次出现的普通文件负责：
      // 在这里再 chmod / utimensat 一次等于改同一个 inode，纯属重复。
      progress = true;
    }
    if (!ok) {
      break;
    }
    if (!progress) {
      SetError(error_message, "Hard link target was never restored");
      ok = false;
      break;
    }
    pending_hardlinks = still_pending;
  }

  // Phase E：显式目录的 metadata 最后设，从深到浅（创建子项会改父目录的
  // mtime）。归档里没有显式 "." 时，暂存根目录保持 0700 这个安全默认值——
  // 不伪造一个它并没有声明过的 uid/gid/mtime。
  if (ok) {
    std::vector<std::size_t> directory_indices;
    for (std::size_t index = 0; index < reader.entries().size(); ++index) {
      if (reader.entries()[index].entry.type == EntryType::kDirectory) {
        directory_indices.push_back(index);
      }
    }
    SortDirectoriesDeepestFirst(reader, &directory_indices);
    for (const std::size_t index : directory_indices) {
      const ArchiveEntry& entry = reader.entries()[index].entry;
      const std::string path = JoinArchivePath(staging, entry.archive_path);
      if (!ApplyMetadata(path, entry, /*is_symlink=*/false, report,
                         error_message)) {
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    // 失败时立刻递归删除 staging：析构函数稍后也会做同一件事，但显式删除让
    // "恢复失败不占磁盘"这一点不依赖对象存活到作用域结束。
    staging_guard.Remove();
    return false;
  }
  // 提交点：staging 与 destination 同在父目录下（同一文件系统），rename 是原子
  // 的。从这一刻起恢复结果才对外可见；失败则丢弃整棵 staging 树。
  if (::rename(staging.c_str(), destination.c_str()) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to finalize destination", destination));
    staging_guard.Remove();
    return false;
  }
  staging_guard.Release();
  if (report != nullptr) {
    report->restored_entries = reader.entries().size();
  }
  return true;
}

// 实际的 payload 字节是不是它自己声明的那些：流式把 payload 区读一遍算
// SHA-256。 与恢复路径的 AuthenticatePayload 用同一套"payload 从 160
// 字节之后开始、长度 由 header.payload_size 决定"的布局规则，只是不做需要密码的
// HMAC 那一半。
// 只用于诊断与自检：不改文件、不产生输出，只返回结论。它与 AuthenticatePayload
// 共享同一套 payload 布局规则，两边的结论可以直接对照——但这里没有 HMAC，
// 通过只代表"字节没坏"，不代表"没有被改"。
bool VerifyContainerPayloadBytes(const std::string& container_file,
                                 ContainerHeader* header,
                                 std::string* error_message) {
  ContainerHeader local;
  if (!InspectContainerFile(container_file, &local, error_message)) {
    return false;
  }
  // 没有摘要就拒绝：老版本或被人为清零的 header 不能因为"没得比"而通过校验。
  if (local.payload_sha256.size() != container_v2::kSha256Size) {
    SetError(error_message,
             "Container header carries no payload digest: " + container_file);
    return false;
  }
  FileSource source;
  if (!source.Open(container_file, error_message)) {
    return false;
  }
  const std::uint64_t expected_size =
      static_cast<std::uint64_t>(container_v2::kHeaderSize) +
      local.payload_size;
  if (source.size() != expected_size) {
    SetError(error_message,
             "Container length does not match its header (declared " +
                 std::to_string(expected_size) + " bytes, file has " +
                 std::to_string(source.size()) + "): " + container_file);
    return false;
  }
  crypto::Sha256 sha;
  std::vector<unsigned char> buffer(kStreamBufferSize);
  std::uint64_t offset = 0;
  while (offset < local.payload_size) {
    const std::uint64_t remaining = local.payload_size - offset;
    const std::size_t want = static_cast<std::size_t>(
        remaining < kStreamBufferSize ? remaining : kStreamBufferSize);
    if (!source.ReadAt(container_v2::kHeaderSize + offset, buffer.data(), want,
                       error_message)) {
      return false;
    }
    sha.Update(buffer.data(), want);
    offset += want;
  }
  unsigned char digest[crypto::kSha256DigestSize];
  sha.Final(digest);
  const std::string computed(reinterpret_cast<const char*>(digest),
                             crypto::kSha256DigestSize);
  if (!crypto::ConstantTimeEquals(computed, local.payload_sha256)) {
    SetError(error_message,
             "Payload checksum mismatch: the actual archive bytes do not match "
             "the digest declared in the container header (" +
                 container_file + ")");
    return false;
  }
  if (header != nullptr) *header = local;
  return true;
}

// 只读元数据：读 magic 与 header，不解密、不读 payload，因此不需要密码，可以在
// GUI/CLI 里安全地用来"先看看这是什么"。输出只含展示所需信息，password_hint
// 只说明"需要密码"，不泄露 KDF 参数。
bool IdentifyArchiveFile(const std::string& archive_file, ArchiveFileInfo* info,
                         std::string* error_message) {
  if (info == nullptr) {
    SetError(error_message, "Internal error: null archive info");
    return false;
  }
  *info = ArchiveFileInfo();
  const ArchiveFileInfo::Kind kind = DetectKind(archive_file, error_message);
  if (kind == ArchiveFileInfo::Kind::kUnknown) {
    return false;
  }
  if (kind == ArchiveFileInfo::Kind::kLegacyV01) {
    info->kind = kind;
    info->format_version = 1;
    ArchiveReader reader;
    ArchiveSummary summary;
    // v0.1 的摘要读失败不算识别失败：格式与版本已经确定，条目数只是尽力而为的
    // 补充信息，所以这里刻意传 nullptr，不覆盖调用方拿到的 error_message。
    if (reader.InspectHeader(archive_file, &summary, nullptr)) {
      info->entry_count = summary.entry_count;
    }
    return true;
  }
  ContainerHeader header;
  if (!InspectContainerFile(archive_file, &header, error_message)) {
    return false;
  }
  info->kind = kind;
  info->format_version = container_v2::kVersion;
  info->entry_count = header.entry_count;
  ParsePackMethodId(header.pack_method, &info->pack_method);
  ParseCompressionMethodId(header.compression_method,
                           &info->compression_method);
  ParseEncryptionMethodId(header.encryption_method, &info->encryption_method);
  if (header.encryption_method != 0) {
    info->password_hint = "password required";
  }
  return true;
}

}  // namespace backupproject

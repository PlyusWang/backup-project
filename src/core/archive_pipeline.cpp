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
// 内存边界：pack 与 encrypt 阶段全程流式（256 KiB 缓冲）。压缩阶段例外——
// HUF1 / LZH1 是"整条流一个 header + 一条 bitstream"的格式，频次表必须看过
// 全部输入才能确定，所以这一层需要 O(n) 内存（上限见
// kMaxCompressionInputSize）。

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

constexpr std::size_t kStreamBufferSize = 256 * 1024;

// 压缩层是唯一需要整条流在内存里的阶段，这里给它一个明确的上限：
// 与其在 2 GiB 的归档上 OOM，不如给一句能看懂的失败信息。
constexpr std::uint64_t kMaxCompressionInputSize = 1ull << 30;

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

// 去掉尾部的 '/'（保留根目录 "/"）。staging 目录名是在 destination 后面接后缀，
// 带尾斜杠会让暂存目录跑到 destination 里面去。
std::string StripTrailingSlashes(const std::string& path) {
  std::string trimmed = path;
  while (trimmed.size() > 1 && trimmed.back() == '/') {
    trimmed.pop_back();
  }
  return trimmed;
}

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

// 尽力而为地删掉一棵树，用于失败时清理暂存目录。不 follow 软链接。
void RemoveTreeQuietly(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return;
  }
  if (!S_ISDIR(info.st_mode)) {
    ::unlink(path.c_str());
    return;
  }
  DIR* raw_dir = ::opendir(path.c_str());
  if (raw_dir != nullptr) {
    while (struct dirent* item = ::readdir(raw_dir)) {
      const std::string name = item->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      RemoveTreeQuietly(path + "/" + name);
    }
    ::closedir(raw_dir);
  }
  ::rmdir(path.c_str());
}

// PKCS#7 一定补 1..8 个字节：DES-CBC 之后的长度是 8 的倍数且严格更大。
// 这个公式必须和 container_format.cpp 里读侧用的那条完全一致，
// 否则写出来的 header 会被自己的读侧拒绝。
std::uint64_t DesPaddedSize(std::uint64_t plain_size) {
  return (plain_size / 8 + 1) * 8;
}

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
// 输入输出都是文件：packed 流与 compressed 流都不会只因为"要压缩"就被拆开，
// 但这个模块内部按整条流处理（见文件头的内存边界说明）。

bool CompressFile(const std::string& input_file, const std::string& output_file,
                  CompressionMethod method, std::uint64_t* output_size,
                  std::string* error_message) {
  FileSource source;
  if (!source.Open(input_file, error_message)) {
    return false;
  }
  if (method == CompressionMethod::kNone) {
    *output_size = source.size();
    return true;
  }
  if (source.size() > kMaxCompressionInputSize) {
    SetError(error_message,
             "Compression stage is limited to 1 GiB per stream in this "
             "version: " +
                 input_file);
    return false;
  }
  std::string input(static_cast<std::size_t>(source.size()), '\0');
  if (!input.empty() &&
      !source.ReadAt(0, &input[0], input.size(), error_message)) {
    return false;
  }
  source.Close();

  std::string output;
  const bool ok =
      method == CompressionMethod::kHuffman
          ? compression::HuffmanCompress(input, &output, error_message)
          : compression::LzssHuffmanCompress(input, &output, error_message);
  if (!ok) {
    return false;
  }
  FileSink sink;
  if (!sink.Open(output_file, error_message)) {
    return false;
  }
  if (!sink.Write(output.data(), output.size(), error_message)) {
    sink.Abandon();
    return false;
  }
  if (!sink.Close(error_message)) {
    sink.Abandon();
    return false;
  }
  *output_size = output.size();
  return true;
}

bool DecompressFile(const std::string& input_file,
                    const std::string& output_file, CompressionMethod method,
                    std::uint64_t expected_size, std::string* error_message) {
  if (method == CompressionMethod::kNone) {
    FileSource source;
    if (!source.Open(input_file, error_message)) {
      return false;
    }
    if (source.size() != expected_size) {
      SetError(error_message, "Archive payload size mismatch: " + input_file);
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

  FileSource source;
  if (!source.Open(input_file, error_message)) {
    return false;
  }
  if (source.size() > kMaxCompressionInputSize) {
    SetError(error_message,
             "Compression stage is limited to 1 GiB per stream in this "
             "version: " +
                 input_file);
    return false;
  }
  std::string input(static_cast<std::size_t>(source.size()), '\0');
  if (!input.empty() &&
      !source.ReadAt(0, &input[0], input.size(), error_message)) {
    return false;
  }
  source.Close();

  std::string output;
  const bool ok =
      method == CompressionMethod::kHuffman
          ? compression::HuffmanDecompress(input, &output, error_message)
          : compression::LzssHuffmanDecompress(input, &output, error_message);
  if (!ok) {
    return false;
  }
  if (output.size() != expected_size) {
    SetError(error_message,
             "Decompressed size does not match the container header");
    return false;
  }
  FileSink sink;
  if (!sink.Open(output_file, error_message)) {
    return false;
  }
  if (!sink.Write(output.data(), output.size(), error_message) ||
      !sink.Close(error_message)) {
    sink.Abandon();
    return false;
  }
  return true;
}

// ---- 恢复：一条条目落盘 -----------------------------------------------------

bool ApplyMetadata(const std::string& path, const ArchiveEntry& entry,
                   bool is_symlink, RestoreReport* report,
                   std::string* error_message) {
  // 1) ownership。归档精确保存 uid/gid，但非 root 进程不允许把文件改成任意
  //    属主。这里的策略是"尽力而为 + 如实记录"：权限不够就记一条诊断，
  //    绝不让整次普通恢复因此不可用，也绝不假装已经完全恢复。
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
bool CreateEntry(const PackedStreamReader& reader, std::size_t index,
                 const std::string& staging_root,
                 std::vector<std::size_t>* directory_indices,
                 std::vector<std::size_t>* pending_hardlinks,
                 RestoreReport* report, std::string* error_message) {
  const ArchiveEntry& entry = reader.entries()[index].entry;
  const std::string target_path =
      JoinArchivePath(staging_root, entry.archive_path);

  switch (entry.type) {
    case EntryType::kDirectory: {
      // 先用 0700 建出来：归档里可能是 0555 的目录，先设成最终权限会让后面的
      // 子文件写不进去。目录自己的 metadata 全部留到最后统一收尾。
      //
      // "." 是暂存目录自己，调用方已经建好了，不能再 mkdir 一次。
      if (entry.archive_path != "." &&
          ::mkdir(target_path.c_str(), 0700) != 0) {
        SetError(error_message,
                 Describe(errno, "Failed to create directory", target_path));
        return false;
      }
      directory_indices->push_back(index);
      return true;
    }
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
    case EntryType::kHardLink: {
      const std::string link_source =
          JoinArchivePath(staging_root, entry.link_target);
      struct stat link_info;
      if (::lstat(link_source.c_str(), &link_info) != 0) {
        // 目标还没恢复出来（归档顺序反常）。挂起，主循环结束后重试。
        pending_hardlinks->push_back(index);
        return true;
      }
      if (::link(link_source.c_str(), target_path.c_str()) != 0) {
        SetError(error_message,
                 Describe(errno, "Failed to create hard link", target_path));
        return false;
      }
      // 硬链接与目标共享 inode，metadata 由第一次出现的普通文件负责：
      // 在这里再 chmod / utimensat 一次等于改同一个 inode，纯属重复。
      return true;
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
        // 非 root 一定拿不到 CAP_MKNOD。这里明确失败并说清楚原因：
        // 既不能静默降级成普通文件，也不能假装恢复了设备节点。
        SetError(
            error_message,
            Describe(errno, "Failed to create device node (requires CAP_MKNOD)",
                     target_path));
        return false;
      }
      return ApplyMetadata(target_path, entry, /*is_symlink=*/false, report,
                           error_message);
    }
    case EntryType::kSocket: {
      SetError(error_message,
               "Unsupported special type: socket: " + entry.archive_path);
      return false;
    }
  }
  SetError(error_message, "Internal error: unknown entry type");
  return false;
}

// 目录 metadata 必须等孩子全部恢复完再设：创建子项会改父目录的 mtime。
// 从深到浅处理，同一深度按路径逆序，保证结果与顺序无关地确定。
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
  if (entries.empty() || entries.front().archive_path != "." ||
      entries.front().type != EntryType::kDirectory) {
    SetError(error_message,
             "Internal error: the entry list must start with the source root");
    return false;
  }
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
  const std::string temp_directory =
      archive_parent.empty() ? std::string(".") : archive_parent;
  const std::string unique = std::to_string(static_cast<long>(::getpid()));

  // 临时产物：packed 流与 compressed 流。两个都走 RAII 守卫，
  // 成功或失败都会在函数返回时被清掉。
  std::string packed_file;
  if (!ReserveTempPath(temp_directory, ".bp-packed-" + unique + "-",
                       &packed_file, error_message)) {
    return false;
  }
  TempFileGuard packed_guard(packed_file);

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

  std::string compressed_file = packed_file;
  std::unique_ptr<TempFileGuard> compressed_guard;
  std::uint64_t compressed_size = packed_size;
  if (options.compression_method != CompressionMethod::kNone) {
    if (!ReserveTempPath(temp_directory, ".bp-packed-" + unique + "-",
                         &compressed_file, error_message)) {
      return false;
    }
    compressed_guard = std::make_unique<TempFileGuard>(compressed_file);
    if (!CompressFile(packed_file, compressed_file, options.compression_method,
                      &compressed_size, error_message)) {
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
  switch (options.encryption_method) {
    case EncryptionMethod::kNone:
      header.kdf_iterations = 0;
      header.salt_len = 0;
      header.iv_len = 0;
      header.tag_len = 0;
      header.payload_size = compressed_size;
      break;
    case EncryptionMethod::kDesCbcHmacSha256: {
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

  std::string header_bytes;
  if (!EncodeContainerHeader(header, &header_bytes, error_message)) {
    return false;
  }

  if (options.encryption_method != EncryptionMethod::kNone &&
      !DeriveKeys(options.password, header, &cipher_key, &mac_key,
                  error_message)) {
    return false;
  }

  FileSink sink;
  if (!sink.Open(archive_file, error_message)) {
    return false;
  }
  bool ok = sink.Write(header_bytes.data(), header_bytes.size(), error_message);

  crypto::Sha256 sha;

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
  if (!ok) {
    sink.Abandon();
    return false;
  }
  return true;
}

// ---- 恢复 ------------------------------------------------------------------

namespace {

// 只读地辨认 .bak 的类型：不看扩展名，只看 magic。
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
bool AuthenticatePayload(const FileSource& source,
                         const ContainerHeader& header,
                         const std::string& mac_key,
                         std::string* error_message) {
  EncryptionMethod method = EncryptionMethod::kNone;
  if (!ParseEncryptionMethodId(header.encryption_method, &method)) {
    SetError(error_message, "Unknown encryption method id");
    return false;
  }
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

  const std::string destination_parent = ParentDirectoryOf(destination);
  FileSystem file_system;
  if (!destination_parent.empty() &&
      !file_system.MakeDirectories(destination_parent, error_message)) {
    return false;
  }
  const std::string temp_directory =
      destination_parent.empty() ? std::string(".") : destination_parent;
  const std::string unique = std::to_string(static_cast<long>(::getpid()));

  std::string compressed_file;
  if (!ReserveTempPath(temp_directory, ".bp-unpack-" + unique + "-",
                       &compressed_file, error_message)) {
    return false;
  }
  TempFileGuard compressed_guard(compressed_file);
  // 第二遍：认证已过，这时才解密。
  if (!DecryptPayload(source, header, cipher_key, compressed_file,
                      error_message)) {
    return false;
  }
  source.Close();

  CompressionMethod compression = CompressionMethod::kNone;
  ParseCompressionMethodId(header.compression_method, &compression);
  std::string packed_file = compressed_file;
  std::unique_ptr<TempFileGuard> packed_guard;
  if (compression != CompressionMethod::kNone) {
    if (!ReserveTempPath(temp_directory, ".bp-unpack-" + unique + "-",
                         &packed_file, error_message)) {
      return false;
    }
    packed_guard = std::make_unique<TempFileGuard>(packed_file);
    if (!DecompressFile(compressed_file, packed_file, compression,
                        header.packed_size, error_message)) {
      return false;
    }
  } else {
    std::uint64_t packed_size = 0;
    if (!FileSizeOf(packed_file, &packed_size, error_message)) {
      return false;
    }
    if (packed_size != header.packed_size) {
      SetError(error_message,
               "Packed stream size does not match the container header");
      return false;
    }
  }

  PackMethod pack_method = PackMethod::kMyPack;
  ParsePackMethodId(header.pack_method, &pack_method);
  // preflight、暂存目录、metadata 收尾、rename —— 这些步骤与"直接从一条
  // packed 流恢复"完全一样，所以只有一份实现。
  return RunRestorePackedStream(packed_file, pack_method, header.entry_count,
                                destination, report, error_message);
}

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

  const std::string unique = std::to_string(static_cast<long>(::getpid()));
  PackedStreamReader reader;
  if (!reader.Open(packed_file, error_message)) {
    return false;
  }
  // preflight：整条 packed 流校验完才动磁盘。
  if (!reader.Scan(pack_method, error_message)) {
    return false;
  }
  if (reader.entries().size() != expected_entry_count) {
    SetError(error_message, "Entry count does not match the expected count");
    return false;
  }

  // 暂存目录：destination 的兄弟目录，同一个文件系统，所以最后一步是纯 rename。
  const std::string staging =
      destination + ".bptmp-" + unique + "-" + std::to_string(::getpid());
  if (::mkdir(staging.c_str(), 0700) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to create staging directory", staging));
    return false;
  }
  bool ok = true;
  std::vector<std::size_t> directory_indices;
  std::vector<std::size_t> pending_hardlinks;
  for (std::size_t index = 0; ok && index < reader.entries().size(); ++index) {
    ok = CreateEntry(reader, index, staging, &directory_indices,
                     &pending_hardlinks, report, error_message);
  }
  // 硬链接目标在后时在这里补：反复重试直到没有进展，再判失败。
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
  // 目录 metadata 最后设，从深到浅：创建子项会改父目录的 mtime。
  if (ok) {
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
    RemoveTreeQuietly(staging);
    return false;
  }
  if (::rename(staging.c_str(), destination.c_str()) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to finalize destination", destination));
    RemoveTreeQuietly(staging);
    return false;
  }
  if (report != nullptr) {
    report->restored_entries = reader.entries().size();
  }
  return true;
}

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

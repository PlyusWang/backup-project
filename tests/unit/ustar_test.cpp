// tests/unit/ustar_test.cpp
//
// ustar 编解码、两个写入器与读取器的单元测试。
//
// 独立可执行，不依赖 GoogleTest：main() 自己跑完全部检查，逐项打印 PASS/FAIL，
// 有任何一项失败就返回非零。另外给 scripts/ustar_test.sh 提供三个子命令：
//
//   --pack <源目录> <归档文件> baseline|fast   扫描真实目录树并写归档
//   --list <归档文件>                          打印 Scan 的结果
//   --restore <归档文件> <目标目录>            用 Scan + ExtractData 还原目录树
//
// 后两个子命令是 GNU tar interoperability 测试的脚手架：脚本既用系统 tar 解
// 我们写的归档，也用我们的读取器解 tar 写的归档，两边都落成真实目录再 diff。
// "扫描目录树"属于测试脚手架，不进产品代码。

#include "ustar.h"

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "archive_entry.h"

namespace {

using backupproject::ArchiveEntry;
using backupproject::EntryType;
using backupproject::EntryTypeName;
using backupproject::MakeDevice;
using backupproject::ustar::EncodeHeader;
using backupproject::ustar::ExtractData;
using backupproject::ustar::ExtractDataToString;
using backupproject::ustar::Header;
using backupproject::ustar::kBlockSize;
using backupproject::ustar::kMaxMembers;
using backupproject::ustar::Member;
using backupproject::ustar::Scan;
using backupproject::ustar::SplitPath;
using backupproject::ustar::TypeFlagFor;
using backupproject::ustar::TypeFromFlag;
using backupproject::ustar::WriteBaseline;
using backupproject::ustar::WriteFast;

int g_passed = 0;
int g_failed = 0;
std::string g_root;

void Check(bool ok, const std::string& name, const std::string& detail = "") {
  if (ok) {
    ++g_passed;
    std::printf("PASS: %s\n", name.c_str());
  } else {
    ++g_failed;
    std::printf("FAIL: %s%s%s\n", name.c_str(), detail.empty() ? "" : " -- ",
                detail.c_str());
    std::fflush(stdout);
  }
}

// 期望操作失败，并且错误信息里含有关键词。只确认"返回了 false"是不够的：
// 因为另一个 bug 而失败同样算没覆盖到。
void CheckFailsWith(bool ok, const std::string& message,
                    const std::string& keyword, const std::string& name) {
  if (ok) {
    Check(false, name, "操作意外成功（期望失败）");
    return;
  }
  if (message.find(keyword) == std::string::npos) {
    Check(false, name, "错误信息里没有 \"" + keyword + "\"：" + message);
    return;
  }
  Check(true, name);
}

void CheckEqU64(std::uint64_t actual, std::uint64_t expected,
                const std::string& name) {
  Check(
      actual == expected, name,
      "期望 " + std::to_string(expected) + "，实际 " + std::to_string(actual));
}

void CheckEqStr(const std::string& actual, const std::string& expected,
                const std::string& name) {
  Check(actual == expected, name,
        "期望 \"" + expected + "\"，实际 \"" + actual + "\"");
}

// ---- 临时目录与文件 ----

std::string PathJoin(const std::string& base, const std::string& name) {
  if (name.empty() || name == ".") {
    return base;
  }
  // base 是归档根 "."（或当前目录）时，子项路径就是 name 本身：
  // 归档内部路径不允许出现 "./x" 这种带 '.' component 的形式。
  if (base.empty() || base == "." || base.back() == '/') {
    return base == "." ? name : base + name;
  }
  return base + "/" + name;
}

bool WriteFile(const std::string& path, const std::string& content) {
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return false;
  }
  std::size_t done = 0;
  bool ok = true;
  while (done < content.size()) {
    const ssize_t written =
        ::write(fd, content.data() + done, content.size() - done);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      break;
    }
    done += static_cast<std::size_t>(written);
  }
  if (::close(fd) != 0) {
    ok = false;
  }
  return ok;
}

// 原始 fd 上写满一段。条目数上限测试要写 512 MB，走这个函数。
bool WriteAllRaw(int fd, const char* data, std::size_t length,
                 std::string* error_message) {
  std::size_t done = 0;
  while (done < length) {
    const ssize_t written = ::write(fd, data + done, length - done);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      *error_message = std::string("写入失败: ") + std::strerror(errno);
      return false;
    }
    done += static_cast<std::size_t>(written);
  }
  return true;
}

bool ReadFile(const std::string& path, std::string* content) {
  content->clear();
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  char buffer[4096];
  while (true) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      (void)::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    content->append(buffer, static_cast<std::size_t>(got));
  }
  (void)::close(fd);
  return true;
}

std::uint64_t FileSize(const std::string& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

bool FileExists(const std::string& path) {
  return ::access(path.c_str(), F_OK) == 0;
}

// 手工算 checksum 并写回：用于"只改一个字段、其余保持合法"的坏样本。
// 这里刻意不复用产品的 ComputeChecksum，第二份实现才能验证第一份。
void FixChecksum(std::string* block) {
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    sum +=
        (i >= 148 && i < 156) ? 32u : static_cast<unsigned char>((*block)[i]);
  }
  for (std::size_t i = 0; i < 6; ++i) {
    (*block)[148 + i] = static_cast<char>('0' + ((sum >> ((5 - i) * 3)) & 7u));
  }
  (*block)[154] = '\0';
  (*block)[155] = ' ';
}

std::uint64_t ChecksumOf(const std::string& block) {
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    sum += (i >= 148 && i < 156) ? 32u : static_cast<unsigned char>(block[i]);
  }
  return sum;
}

std::string Octal6(std::uint64_t value) {
  std::string text(6, '0');
  for (std::size_t i = 0; i < 6; ++i) {
    text[5 - i] = static_cast<char>('0' + ((value >> (i * 3)) & 7u));
  }
  return text;
}

// 覆盖一个数字字段（不改 checksum 的那种，调用方自己决定要不要 FixChecksum）。
void PatchField(std::string* block, std::size_t offset,
                const std::string& text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    (*block)[offset + i] = text[i];
  }
}

// ---- 手工构造归档 ----

struct RawEntry {
  Header header;
  std::string payload;
};

Header MakeRawHeader(const std::string& path, char typeflag,
                     std::uint64_t size = 0) {
  Header header;
  std::string error;
  if (!SplitPath(path, &header.name, &header.prefix, &error)) {
    std::fprintf(stderr, "构造测试 header 失败（%s）: %s\n", path.c_str(),
                 error.c_str());
    std::exit(2);
  }
  header.typeflag = typeflag;
  header.mode = 0644;
  header.size = size;
  header.mtime = 1000000000;
  return header;
}

std::string BuildRawArchive(const std::vector<RawEntry>& entries,
                            bool terminator = true,
                            std::size_t trailing_zeros = 0) {
  std::string out;
  for (const RawEntry& entry : entries) {
    std::string block;
    std::string error;
    if (!EncodeHeader(entry.header, &block, &error)) {
      std::fprintf(stderr, "构造测试归档失败: %s\n", error.c_str());
      std::exit(2);
    }
    out += block;
    if (entry.header.size != 0) {
      out += entry.payload;
      const std::uint64_t padded =
          ((entry.header.size + kBlockSize - 1) / kBlockSize) * kBlockSize;
      out.append(static_cast<std::size_t>(padded - entry.header.size), '\0');
    }
  }
  if (terminator) {
    out.append(2 * kBlockSize, '\0');
  }
  out.append(trailing_zeros, '\0');
  return out;
}

// 把字节写进临时目录里的文件，然后 Scan。
bool WriteAndScan(const std::string& name, const std::string& bytes,
                  std::vector<Member>* members, std::string* error_message) {
  const std::string path = PathJoin(g_root, name);
  if (!WriteFile(path, bytes)) {
    *error_message = "写测试归档失败: " + path;
    return false;
  }
  return Scan(path, members, error_message);
}

// ---- 1. 路径拆分 ----

void TestSplitPath() {
  std::string name;
  std::string prefix;
  std::string error;

  Check(SplitPath("a", &name, &prefix, &error) && name == "a" && prefix.empty(),
        "SplitPath: 短路径整体进 name");
  Check(SplitPath(".", &name, &prefix, &error) && name == "." && prefix.empty(),
        "SplitPath: 归档根 '.'");
  Check(SplitPath("a/b/c.txt", &name, &prefix, &error) && name == "a/b/c.txt" &&
            prefix.empty(),
        "SplitPath: 100 字节以内的多级路径不拆");

  const std::string exact100(100, 'x');
  Check(SplitPath(exact100, &name, &prefix, &error) && name == exact100 &&
            prefix.empty(),
        "SplitPath: 恰好 100 字节进 name");

  const std::string single101(101, 'x');
  CheckFailsWith(SplitPath(single101, &name, &prefix, &error), error, "prefix",
                 "SplitPath: 101 字节单层名字失败（必须失败，不能截断）");

  const std::string single120(120, 'y');
  CheckFailsWith(SplitPath(single120, &name, &prefix, &error), error, "name",
                 "SplitPath: 120 字节单层名字失败");

  // 200 字节：100 个 'a' + '/' + 99 个 'b'，切点在 index 100。
  const std::string split_path =
      std::string(100, 'a') + "/" + std::string(99, 'b');
  const bool split_ok = SplitPath(split_path, &name, &prefix, &error);
  Check(split_ok && prefix.size() == 100 && name.size() == 99 &&
            prefix + "/" + name == split_path,
        "SplitPath: 200 字节路径按 '/' 切成 prefix+name");

  // 另一个 200 字节样本：prefix 正好 155（上限）。
  const std::string max_prefix =
      "deep/" + std::string(150, 'z') + "/" + std::string(44, 'f');
  const bool max_ok = SplitPath(max_prefix, &name, &prefix, &error);
  Check(max_ok && prefix.size() == 155 && name.size() == 44 &&
            prefix + "/" + name == max_prefix,
        "SplitPath: prefix 恰好 155 字节（字段上限）");

  // 255 = 154 prefix + '/' + 100 name，正好是 POSIX 允许的最长路径。
  const std::string longest =
      std::string(154, 'p') + "/" + std::string(100, 'n');
  const bool longest_ok = SplitPath(longest, &name, &prefix, &error);
  Check(longest_ok && prefix.size() == 154 && name.size() == 100 &&
            prefix + "/" + name == longest,
        "SplitPath: 255 字节路径（上限）可以表示");

  const std::string too_long =
      std::string(155, 'p') + "/" + std::string(100, 'n');
  CheckFailsWith(SplitPath(too_long, &name, &prefix, &error), error, "255",
                 "SplitPath: 256 字节路径必须失败");

  const std::string single255(255, 'q');
  CheckFailsWith(SplitPath(single255, &name, &prefix, &error), error, "prefix",
                 "SplitPath: 255 字节但只有一个 component 时失败");

  CheckFailsWith(SplitPath("/etc/passwd", &name, &prefix, &error), error,
                 "绝对路径", "SplitPath: 绝对路径失败");
  CheckFailsWith(SplitPath("../escape", &name, &prefix, &error), error, "..",
                 "SplitPath: '..' 穿越失败");
  CheckFailsWith(SplitPath("a/../b", &name, &prefix, &error), error, "..",
                 "SplitPath: 中间 '..' 失败");
  CheckFailsWith(SplitPath("a//b", &name, &prefix, &error), error,
                 "空 component", "SplitPath: 空 component 失败");
  CheckFailsWith(SplitPath("a/./b", &name, &prefix, &error), error,
                 "'.' component", "SplitPath: '.' component 失败");
  CheckFailsWith(SplitPath("a/", &name, &prefix, &error), error, "结尾",
                 "SplitPath: 结尾 '/' 失败");
  CheckFailsWith(SplitPath("", &name, &prefix, &error), error, "空",
                 "SplitPath: 空路径失败");
  const std::string with_nul("a\0b", 3);
  CheckFailsWith(SplitPath(with_nul, &name, &prefix, &error), error, "NUL",
                 "SplitPath: 含 NUL 字节失败");
  Check(SplitPath("...", &name, &prefix, &error) && name == "...",
        "SplitPath: '...' 是普通名字，不当作 '..'");

  // UTF-8 按字节处理：多字节名字整体进 name，字节原样保存。
  const std::string unicode_name = "目录/ünïcode 名字.txt";
  Check(SplitPath(unicode_name, &name, &prefix, &error) &&
            name == unicode_name && prefix.empty(),
        "SplitPath: Unicode 名字按字节保存");

  std::string long_unicode;
  for (int i = 0; i < 20; ++i) {
    long_unicode += "目录";
  }
  long_unicode += "/f";
  const bool unicode_ok = SplitPath(long_unicode, &name, &prefix, &error);
  Check(unicode_ok && prefix + "/" + name == long_unicode &&
            prefix.size() <= 155 && name.size() <= 100,
        "SplitPath: 长 Unicode 路径切分后能原样拼回");

  // 切点可以落在多字节字符中间：ustar 的 name/prefix 就是字节字段，
  // 我们不做转码、也不做字符边界对齐。
  std::string mid_char(99, 'a');
  mid_char += "\xe4\xb8";  // "中" 的前两个字节
  mid_char += "/";
  mid_char += std::string(98, 'b');
  const bool mid_ok = SplitPath(mid_char, &name, &prefix, &error);
  Check(mid_ok && prefix + "/" + name == mid_char,
        "SplitPath: 按字节切分（切点可以落在多字节字符中间）");
}

// ---- 2. typeflag 映射 ----

void TestTypeFlags() {
  Check(TypeFlagFor(EntryType::kRegularFile) == '0',
        "TypeFlagFor: regular -> '0'");
  Check(TypeFlagFor(EntryType::kHardLink) == '1',
        "TypeFlagFor: hardlink -> '1'");
  Check(TypeFlagFor(EntryType::kSymlink) == '2', "TypeFlagFor: symlink -> '2'");
  Check(TypeFlagFor(EntryType::kCharDevice) == '3',
        "TypeFlagFor: chardev -> '3'");
  Check(TypeFlagFor(EntryType::kBlockDevice) == '4',
        "TypeFlagFor: blockdev -> '4'");
  Check(TypeFlagFor(EntryType::kDirectory) == '5',
        "TypeFlagFor: directory -> '5'");
  Check(TypeFlagFor(EntryType::kFifo) == '6', "TypeFlagFor: fifo -> '6'");
  Check(TypeFlagFor(EntryType::kSocket) == 's',
        "TypeFlagFor: socket 用 GNU 的 's'（写侧拒绝，读侧报 socket）");

  EntryType type = EntryType::kDirectory;
  std::string error;
  Check(TypeFromFlag('0', &type, &error) && type == EntryType::kRegularFile,
        "TypeFromFlag: '0' -> regular");
  Check(TypeFromFlag('\0', &type, &error) && type == EntryType::kRegularFile,
        "TypeFromFlag: NUL -> regular（老 tar 的写法）");
  Check(TypeFromFlag('1', &type, &error) && type == EntryType::kHardLink,
        "TypeFromFlag: '1' -> hardlink");
  Check(TypeFromFlag('2', &type, &error) && type == EntryType::kSymlink,
        "TypeFromFlag: '2' -> symlink");
  Check(TypeFromFlag('3', &type, &error) && type == EntryType::kCharDevice,
        "TypeFromFlag: '3' -> chardev");
  Check(TypeFromFlag('4', &type, &error) && type == EntryType::kBlockDevice,
        "TypeFromFlag: '4' -> blockdev");
  Check(TypeFromFlag('5', &type, &error) && type == EntryType::kDirectory,
        "TypeFromFlag: '5' -> directory");
  Check(TypeFromFlag('6', &type, &error) && type == EntryType::kFifo,
        "TypeFromFlag: '6' -> fifo");

  CheckFailsWith(TypeFromFlag('s', &type, &error), error, "socket",
                 "TypeFromFlag: 's' 报 unsupported special type: socket");
  CheckFailsWith(TypeFromFlag('7', &type, &error), error, "contiguous",
                 "TypeFromFlag: '7'（contiguous）明确拒绝");
  CheckFailsWith(TypeFromFlag('x', &type, &error), error, "extension",
                 "TypeFromFlag: pax 扩展头 'x' 拒绝");
  CheckFailsWith(TypeFromFlag('L', &type, &error), error, "extension",
                 "TypeFromFlag: GNU 长名字 'L' 拒绝");
  CheckFailsWith(TypeFromFlag('S', &type, &error), error, "extension",
                 "TypeFromFlag: GNU sparse 'S' 拒绝");
  CheckFailsWith(TypeFromFlag('Z', &type, &error), error, "unknown typeflag",
                 "TypeFromFlag: 未知 typeflag 拒绝");
  CheckFailsWith(TypeFromFlag(static_cast<char>(0x01), &type, &error), error,
                 "unknown typeflag", "TypeFromFlag: 0x01 未知 typeflag 拒绝");
}

// ---- 3. header 编解码往返 ----

std::string OctalNul(std::uint64_t value, std::size_t digits) {
  std::string text(digits, '0');
  for (std::size_t i = 0; i < digits; ++i) {
    text[digits - 1 - i] = static_cast<char>('0' + ((value >> (i * 3)) & 7u));
  }
  text.push_back('\0');
  return text;
}

void CheckHeaderRoundTrip(const Header& header, const std::string& label) {
  std::string block;
  std::string error;
  if (!EncodeHeader(header, &block, &error)) {
    Check(false, label + ": EncodeHeader 成功", error);
    return;
  }
  Header decoded;
  if (!DecodeHeader(block.data(), &decoded, &error)) {
    Check(false, label + ": DecodeHeader 成功", error);
    return;
  }
  Check(decoded.typeflag == header.typeflag, label + ": typeflag 往返");
  CheckEqStr(decoded.name, header.name, label + ": name 往返");
  CheckEqStr(decoded.prefix, header.prefix, label + ": prefix 往返");
  CheckEqStr(decoded.linkname, header.linkname, label + ": linkname 往返");
  CheckEqU64(decoded.mode, header.mode, label + ": mode 往返");
  CheckEqU64(decoded.uid, header.uid, label + ": uid 往返");
  CheckEqU64(decoded.gid, header.gid, label + ": gid 往返");
  CheckEqU64(decoded.size, header.size, label + ": size 往返");
  CheckEqU64(static_cast<std::uint64_t>(decoded.mtime),
             static_cast<std::uint64_t>(header.mtime), label + ": mtime 往返");
  CheckEqU64(decoded.devmajor, header.devmajor, label + ": devmajor 往返");
  CheckEqU64(decoded.devminor, header.devminor, label + ": devminor 往返");
}

void TestHeaderLayout() {
  std::string block;
  std::string error;
  Header header;
  header.typeflag = '0';
  header.name = "file.txt";
  header.mode = 0644;
  header.uid = 1000;
  header.gid = 1000;
  header.size = 14;
  header.mtime = 1234567890;
  header.uname = "pw-is-123";
  header.gname = "pw-is-123";
  Check(EncodeHeader(header, &block, &error), "EncodeHeader: 普通文件编码成功",
        error);
  CheckEqU64(block.size(), kBlockSize, "EncodeHeader: 输出恰好 512 字节");

  // 逐字段核对偏移与写法：这是"我们写的确实是 ustar"的第一手证据。
  CheckEqStr(block.substr(0, 8), "file.txt", "header 布局: name 在 offset 0");
  CheckEqStr(block.substr(100, 8), OctalNul(0644, 7),
             "header 布局: mode = 7 位八进制 + NUL");
  CheckEqStr(block.substr(108, 8), OctalNul(1000, 7),
             "header 布局: uid = 7 位八进制 + NUL");
  CheckEqStr(block.substr(116, 8), OctalNul(1000, 7),
             "header 布局: gid = 7 位八进制 + NUL");
  CheckEqStr(block.substr(124, 12), OctalNul(14, 11),
             "header 布局: size = 11 位八进制 + NUL");
  CheckEqStr(block.substr(136, 12), OctalNul(1234567890, 11),
             "header 布局: mtime = 11 位八进制 + NUL");
  CheckEqStr(block.substr(148, 8),
             Octal6(ChecksumOf(block)) + std::string(1, '\0') + " ",
             "header 布局: chksum = 6 位八进制 + NUL + 空格");
  Check(block[156] == '0', "header 布局: typeflag 在 offset 156");
  CheckEqStr(block.substr(257, 6), std::string("ustar\0", 6),
             "header 布局: magic = \"ustar\" + NUL");
  CheckEqStr(block.substr(263, 2), std::string("00"),
             "header 布局: version = \"00\"");
  CheckEqStr(block.substr(265, 9), "pw-is-123",
             "header 布局: uname 在 offset 265");
  CheckEqStr(block.substr(297, 9), "pw-is-123",
             "header 布局: gname 在 offset 297");
  CheckEqStr(block.substr(329, 8), OctalNul(0, 7),
             "header 布局: devmajor 在 offset 329");
  CheckEqStr(block.substr(337, 8), OctalNul(0, 7),
             "header 布局: devminor 在 offset 337");

  // 数字字段必须留终止符：写满整个字段的实现会让读侧不知道字段到哪结束。
  bool terminated = block[107] == '\0' && block[115] == '\0' &&
                    block[123] == '\0' && block[135] == '\0' &&
                    block[147] == '\0' && block[336] == '\0' &&
                    block[344] == '\0';
  Check(terminated, "header 布局: 每个数字字段的最后 1 字节都是 NUL");

  // prefix 之后的 12 字节 pad 必须是 0。
  bool pad_zero = true;
  for (std::size_t i = 500; i < 512; ++i) {
    pad_zero = pad_zero && block[i] == '\0';
  }
  Check(pad_zero, "header 布局: prefix 之后的 12 字节 pad 全为 0");

  // checksum 的独立复核：把字段里写的值解出来，与"把 chksum 当 8 个空格
  // 重算一遍"的结果比较。两个方向都独立于产品实现。
  std::uint64_t parsed_checksum = 0;
  for (std::size_t i = 0; i < 6; ++i) {
    parsed_checksum =
        parsed_checksum * 8 + static_cast<std::uint64_t>(block[148 + i] - '0');
  }
  CheckEqU64(parsed_checksum, ChecksumOf(block),
             "checksum: 字段里的值与独立重算一致");
}

void TestHeaderRoundTrip() {
  std::string block;
  std::string error;

  Header regular;
  regular.name = "file.txt";
  regular.mode = 0644;
  regular.uid = 1000;
  regular.gid = 1000;
  regular.size = 14;
  regular.mtime = 1234567890;
  regular.uname = "pw-is-123";
  regular.gname = "pw-is-123";
  CheckHeaderRoundTrip(regular, "往返: 普通文件");

  Header directory;
  directory.typeflag = '5';
  directory.name = "sub";
  directory.mode = 0755;
  directory.mtime = 1000000000;
  CheckHeaderRoundTrip(directory, "往返: 目录");

  Header symlink;
  symlink.typeflag = '2';
  symlink.name = "link.txt";
  symlink.linkname = "../outside/target with space";
  symlink.mode = 0777;
  symlink.mtime = 1000000000;
  CheckHeaderRoundTrip(symlink, "往返: 软链接（linkname 含空格与 '..'）");

  Header hardlink;
  hardlink.typeflag = '1';
  hardlink.name = "hard.txt";
  hardlink.linkname = "file.txt";
  hardlink.mode = 0644;
  hardlink.mtime = 1000000000;
  CheckHeaderRoundTrip(hardlink, "往返: 硬链接");

  Header fifo;
  fifo.typeflag = '6';
  fifo.name = "pipe.fifo";
  fifo.mode = 0644;
  fifo.mtime = 1000000000;
  CheckHeaderRoundTrip(fifo, "往返: FIFO");

  Header chardev;
  chardev.typeflag = '3';
  chardev.name = "null";
  chardev.mode = 0666;
  chardev.devmajor = 1;
  chardev.devminor = 3;
  chardev.mtime = 1000000000;
  CheckHeaderRoundTrip(chardev, "往返: 字符设备 1,3");

  Header blockdev;
  blockdev.typeflag = '4';
  blockdev.name = "sda";
  blockdev.mode = 0660;
  blockdev.devmajor = 8;
  blockdev.devminor = 0;
  blockdev.mtime = 1000000000;
  CheckHeaderRoundTrip(blockdev, "往返: 块设备 8,0");

  Header typeflag_nul;
  typeflag_nul.typeflag = '\0';
  typeflag_nul.name = "old-school.txt";
  typeflag_nul.mode = 0644;
  typeflag_nul.mtime = 1;
  CheckHeaderRoundTrip(typeflag_nul, "往返: typeflag NUL（老 tar 的普通文件）");

  // mode 07777：setuid + setgid + sticky 全开。
  Header all_mode;
  all_mode.name = "all-mode";
  all_mode.mode = 07777;
  all_mode.mtime = 0;
  CheckHeaderRoundTrip(all_mode, "往返: mode 07777 全位");

  // size 的最大值：11 位八进制的上限 077777777777（值 8589934591）。
  Header max_size;
  max_size.name = "big.bin";
  max_size.size = 077777777777ULL;
  max_size.mtime = 1000000000;
  CheckHeaderRoundTrip(max_size, "往返: size 最大值 077777777777");
  max_size.size = 077777777777ULL + 1;
  CheckFailsWith(EncodeHeader(max_size, &block, &error), error, "size",
                 "EncodeHeader: size 超过 077777777777 明确失败");

  // mtime：0 与上限可以表示，负值（1970 之前）在 11 位八进制里没有表示。
  Header epoch;
  epoch.name = "epoch";
  epoch.mtime = 0;
  CheckHeaderRoundTrip(epoch, "往返: mtime 0（1970-01-01）");
  Header late;
  late.name = "late";
  late.mtime = 077777777777LL;
  CheckHeaderRoundTrip(late, "往返: mtime 上限 077777777777");
  Header negative;
  negative.name = "negative";
  negative.mtime = -1;
  CheckFailsWith(EncodeHeader(negative, &block, &error), error, "1970",
                 "EncodeHeader: mtime -1（1970 之前）明确失败，不写成别的值");
  negative.mtime = -86400;
  CheckFailsWith(EncodeHeader(negative, &block, &error), error, "1970",
                 "EncodeHeader: mtime -86400 明确失败");

  // uid / gid：65534 与 7 位八进制上限可以表示，4294967294 超出字段。
  Header nobody;
  nobody.name = "nobody";
  nobody.uid = 65534;
  nobody.gid = 65534;
  nobody.mtime = 1;
  CheckHeaderRoundTrip(nobody, "往返: uid/gid 65534");
  Header max_id;
  max_id.name = "max-id";
  max_id.uid = 07777777;
  max_id.gid = 07777777;
  max_id.mtime = 1;
  CheckHeaderRoundTrip(max_id, "往返: uid/gid 上限 07777777");
  Header over_id;
  over_id.name = "over-id";
  over_id.uid = 4294967294u;
  over_id.mtime = 1;
  CheckFailsWith(
      EncodeHeader(over_id, &block, &error), error, "uid",
      "EncodeHeader: uid 4294967294 超出 7 位八进制，明确失败（绝不截断）");
  over_id.uid = 07777777 + 1;
  CheckFailsWith(EncodeHeader(over_id, &block, &error), error, "uid",
                 "EncodeHeader: uid 超出上限 1 就失败");
  over_id.uid = 0;
  over_id.gid = 4294967294u;
  CheckFailsWith(EncodeHeader(over_id, &block, &error), error, "gid",
                 "EncodeHeader: gid 4294967294 超出 7 位八进制，明确失败");
  Header over_dev;
  over_dev.name = "over-dev";
  over_dev.typeflag = '3';
  over_dev.devmajor = 07777777 + 1;
  over_dev.mtime = 1;
  CheckFailsWith(EncodeHeader(over_dev, &block, &error), error, "devmajor",
                 "EncodeHeader: devmajor 超出 7 位八进制，明确失败");

  // 字段长度边界。
  Header long_name;
  long_name.name = std::string(100, 'n');
  long_name.mtime = 1;
  CheckHeaderRoundTrip(long_name, "往返: name 恰好 100 字节");
  long_name.name = std::string(101, 'n');
  CheckFailsWith(EncodeHeader(long_name, &block, &error), error, "100",
                 "EncodeHeader: name 101 字节失败");
  Header long_prefix;
  long_prefix.name = "tail";
  long_prefix.prefix = std::string(155, 'p');
  long_prefix.mtime = 1;
  CheckHeaderRoundTrip(long_prefix, "往返: prefix 恰好 155 字节");
  long_prefix.prefix = std::string(156, 'p');
  CheckFailsWith(EncodeHeader(long_prefix, &block, &error), error, "155",
                 "EncodeHeader: prefix 156 字节失败");
  Header long_link;
  long_link.typeflag = '2';
  long_link.name = "l";
  long_link.linkname = std::string(100, 't');
  long_link.mtime = 1;
  CheckHeaderRoundTrip(long_link, "往返: linkname 恰好 100 字节");
  long_link.linkname = std::string(101, 't');
  CheckFailsWith(
      EncodeHeader(long_link, &block, &error), error, "100",
      "EncodeHeader: linkname 101 字节失败（软链接超长写侧必须失败）");

  // uname / gname：31 字节 + NUL，超过按 tar 惯例截断。
  Header owner;
  owner.name = "owner";
  owner.mtime = 1;
  owner.uname = "0123456789";
  owner.gname = "abcdefghij";
  CheckHeaderRoundTrip(owner, "往返: uname/gname");
  owner.uname = std::string(40, 'u');
  owner.gname = std::string(31, 'g');
  std::string owner_block;
  if (EncodeHeader(owner, &owner_block, &error)) {
    Header decoded;
    if (DecodeHeader(owner_block.data(), &decoded, &error)) {
      CheckEqU64(decoded.uname.size(), 31,
                 "EncodeHeader: uname 40 字节截断到 31");
      CheckEqU64(decoded.gname.size(), 31, "EncodeHeader: gname 31 字节不截断");
    } else {
      Check(false, "EncodeHeader: uname 截断后能解回来", error);
    }
  } else {
    Check(false, "EncodeHeader: uname 40 字节允许（截断）", error);
  }

  // 其它编码侧拒绝。
  Header no_name;
  no_name.mtime = 1;
  CheckFailsWith(EncodeHeader(no_name, &block, &error), error, "name",
                 "EncodeHeader: name 为空失败");
  Header nul_name;
  nul_name.name = std::string("a\0b", 3);
  nul_name.mtime = 1;
  CheckFailsWith(EncodeHeader(nul_name, &block, &error), error, "NUL",
                 "EncodeHeader: name 含 NUL 失败");
  Header dir_with_size;
  dir_with_size.typeflag = '5';
  dir_with_size.name = "d";
  dir_with_size.size = 4096;
  dir_with_size.mtime = 1;
  CheckFailsWith(EncodeHeader(dir_with_size, &block, &error), error, "payload",
                 "EncodeHeader: 目录带 size 失败（非普通条目没有 payload）");
  Header over_mode;
  over_mode.name = "m";
  over_mode.mode = 0100644;
  over_mode.mtime = 1;
  CheckFailsWith(EncodeHeader(over_mode, &block, &error), error, "07777",
                 "EncodeHeader: mode 0100644（带类型位）失败");
  Header socket_header;
  socket_header.typeflag = 's';
  socket_header.name = "sock";
  socket_header.mtime = 1;
  CheckFailsWith(EncodeHeader(socket_header, &block, &error), error, "socket",
                 "EncodeHeader: socket 不能写进归档");
  Header empty_link;
  empty_link.typeflag = '2';
  empty_link.name = "l";
  empty_link.mtime = 1;
  CheckFailsWith(EncodeHeader(empty_link, &block, &error), error, "linkname",
                 "EncodeHeader: 软链接 linkname 为空失败");
}

void TestDecodeRejects() {
  std::string error;
  Header base;
  base.name = "file.txt";
  base.mode = 0644;
  base.uid = 1000;
  base.gid = 1000;
  base.mtime = 1000000000;
  std::string good;
  if (!EncodeHeader(base, &good, &error)) {
    Check(false, "构造基准 header", error);
    return;
  }
  Header decoded;

  {
    std::string block = good;
    block[0] = 'F';  // 改了内容不改 checksum
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "checksum", "DecodeHeader: checksum 不匹配被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 148, std::string("0012ab\0 ", 8));
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "chksum", "DecodeHeader: chksum 字段非法八进制被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 148, std::string("01234567", 8));
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "终止符", "DecodeHeader: chksum 字段缺终止符被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 148, std::string(8, '\0'));
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error, "空的",
                   "DecodeHeader: chksum 字段为空被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 100, std::string("77777777", 8));
    FixChecksum(&block);  // 只留字段本身的问题，checksum 保持正确
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "终止符",
                   "DecodeHeader: mode 字段被数字填满（缺终止符）被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 100, std::string("00007x5\0", 8));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "非法八进制",
                   "DecodeHeader: mode 字段含非法八进制字符被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 100, std::string("0177777\0", 8));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error, "07777",
                   "DecodeHeader: mode 字段数值超出 07777 被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 124, std::string("077777777777", 12));
    FixChecksum(&block);
    CheckFailsWith(
        DecodeHeader(block.data(), &decoded, &error), error, "终止符",
        "DecodeHeader: size 字段 12 位数字填满（077777777777 无终止符）被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 124, std::string(12, ' '));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error, "空的",
                   "DecodeHeader: size 字段全空格（空字段）被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 136, std::string("-0000000001\0", 12));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error, "mtime",
                   "DecodeHeader: mtime 带负号（非法八进制）被拒绝");
  }
  {
    // GNU 的老变体："ustar  \0"（magic 带空格、version 是 " \0"）。
    std::string block = good;
    PatchField(&block, 257, std::string("ustar ", 6));
    PatchField(&block, 263, std::string(" \0", 2));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error, "magic",
                   "DecodeHeader: GNU 的 \"ustar  \\0\" magic 变体被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 263, std::string("01", 2));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "version", "DecodeHeader: version \"01\" 被拒绝");
  }
  {
    // name 字段里塞两个名字：第一个 NUL 之后还有非 NUL 字节。
    std::string block = good;
    PatchField(&block, 0, std::string("a.txt\0evil\0", 11));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "NUL 注入", "DecodeHeader: name 字段 NUL 注入被拒绝");
  }
  {
    std::string block = good;
    std::memset(&block[0], 0, 100);
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "name 是空的", "DecodeHeader: name 全 NUL 被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 156, std::string("5", 1));
    PatchField(&block, 124, OctalNul(12, 11));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "payload", "DecodeHeader: 目录条目声明 size 被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 156, std::string("2", 1));
    PatchField(&block, 157, std::string(100, '\0'));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "linkname 是空的",
                   "DecodeHeader: 软链接 linkname 为空被拒绝");
  }
  {
    std::string block = good;
    PatchField(&block, 157, std::string("a\0b\0", 4));
    FixChecksum(&block);
    CheckFailsWith(DecodeHeader(block.data(), &decoded, &error), error,
                   "NUL 注入", "DecodeHeader: linkname 字段 NUL 注入被拒绝");
  }
  {
    // GNU tar 对非设备条目把 devmajor / devminor 写成 8 个 NUL，必须接受。
    std::string block = good;
    std::memset(&block[329], 0, 8);
    std::memset(&block[337], 0, 8);
    FixChecksum(&block);
    Header gnu_style;
    const bool ok = DecodeHeader(block.data(), &gnu_style, &error);
    Check(ok && gnu_style.devmajor == 0 && gnu_style.devminor == 0,
          "DecodeHeader: devmajor/devminor 全 NUL 被接受（GNU tar 的写法）",
          error);
  }
  {
    // 有点怪的但合法的写法：数字字段带前导空格、末尾空格终止。
    std::string block = good;
    PatchField(&block, 100, std::string("  644 \0\0", 8));
    FixChecksum(&block);
    Header spaced;
    const bool ok = DecodeHeader(block.data(), &spaced, &error);
    Check(ok && spaced.mode == 0644, "DecodeHeader: 容忍前导空格与空格终止符",
          error);
  }
}

// ---- 4. Scan：正常读取 ----

RawEntry MakeEntry(const std::string& path, char typeflag,
                   const std::string& payload) {
  RawEntry entry;
  entry.header = MakeRawHeader(path, typeflag, payload.size());
  entry.payload = payload;
  return entry;
}

void TestScanBasics() {
  std::string error;
  std::vector<Member> members;

  std::vector<RawEntry> raw;
  RawEntry root = MakeEntry(".", '5', "");
  root.header.mode = 0755;
  root.header.mtime = 111;
  RawEntry file = MakeEntry("file.txt", '0', "hello");
  file.header.mtime = 222;
  RawEntry zero = MakeEntry("zero.bin", '0', "");
  RawEntry sub = MakeEntry("sub", '5', "");
  RawEntry inner = MakeEntry("sub/inner.txt", '0', "abc");
  RawEntry link = MakeEntry("link.txt", '2', "");
  link.header.linkname = "file.txt";
  RawEntry hard = MakeEntry("hard.txt", '1', "");
  hard.header.linkname = "file.txt";
  RawEntry fifo = MakeEntry("pipe.fifo", '6', "");
  RawEntry device = MakeEntry("null", '3', "");
  device.header.devmajor = 1;
  device.header.devminor = 3;
  raw.push_back(root);
  raw.push_back(file);
  raw.push_back(zero);
  raw.push_back(sub);
  raw.push_back(inner);
  raw.push_back(link);
  raw.push_back(hard);
  raw.push_back(fifo);
  raw.push_back(device);

  const std::string bytes = BuildRawArchive(raw);
  const bool scanned = WriteAndScan("basics.tar", bytes, &members, &error);
  Check(scanned, "Scan: 读取混合类型归档成功", error);
  if (!scanned) {
    return;
  }
  CheckEqU64(members.size(), raw.size(), "Scan: 条目数");
  if (members.size() != raw.size()) {
    return;
  }
  const char* kExpectedPaths[] = {".",        "file.txt",      "zero.bin",
                                  "sub",      "sub/inner.txt", "link.txt",
                                  "hard.txt", "pipe.fifo",     "null"};
  const EntryType kExpectedTypes[] = {
      EntryType::kDirectory, EntryType::kRegularFile, EntryType::kRegularFile,
      EntryType::kDirectory, EntryType::kRegularFile, EntryType::kSymlink,
      EntryType::kHardLink,  EntryType::kFifo,        EntryType::kCharDevice};
  bool paths_ok = true;
  bool types_ok = true;
  for (std::size_t i = 0; i < members.size(); ++i) {
    paths_ok = paths_ok && members[i].entry.archive_path == kExpectedPaths[i];
    types_ok = types_ok && members[i].entry.type == kExpectedTypes[i];
  }
  Check(paths_ok, "Scan: 路径顺序与内容（DFS 先序）");
  Check(types_ok, "Scan: 条目类型");

  CheckEqU64(members[0].data_size, 0, "Scan: 目录 data_size 为 0");
  CheckEqU64(members[1].data_size, 5, "Scan: 普通文件 data_size");
  CheckEqU64(members[2].data_size, 0, "Scan: 空文件 data_size 为 0");
  CheckEqU64(members[5].data_size, 0, "Scan: 软链接 data_size 为 0");
  CheckEqU64(members[6].data_size, 0, "Scan: 硬链接 data_size 为 0");
  CheckEqU64(members[8].data_size, 0, "Scan: 字符设备 data_size 为 0");
  CheckEqStr(members[5].entry.link_target, "file.txt",
             "Scan: 软链接 link_target");
  CheckEqStr(members[6].entry.link_target, "file.txt",
             "Scan: 硬链接 link_target");
  CheckEqU64(members[8].entry.dev_major, 1, "Scan: 字符设备 major");
  CheckEqU64(members[8].entry.dev_minor, 3, "Scan: 字符设备 minor");
  CheckEqU64(members[0].entry.mode, 0755, "Scan: 目录 mode");
  CheckEqU64(members[1].entry.mtime_sec, 222, "Scan: 普通文件 mtime");
  Check(members[1].entry.source_path.empty(),
        "Scan: source_path 留空（Scan 不碰文件系统）");

  // data_offset 必须 512 对齐，且等于前一条的 payload+padding 之后的位置。
  std::uint64_t expected_offset = 0;
  bool offsets_ok = true;
  for (const Member& member : members) {
    offsets_ok =
        offsets_ok && member.data_offset == expected_offset + kBlockSize;
    const std::uint64_t padded =
        ((member.data_size + kBlockSize - 1) / kBlockSize) * kBlockSize;
    expected_offset = member.data_offset + padded;
  }
  Check(offsets_ok, "Scan: data_offset 逐条 512 对齐");
  CheckEqU64(expected_offset + 2 * kBlockSize, bytes.size(),
             "Scan: 条目之后正好是两个全零 block");

  std::string payload;
  Check(ExtractDataToString(PathJoin(g_root, "basics.tar"), members[1],
                            &payload, &error),
        "ExtractDataToString: 取普通文件内容", error);
  CheckEqStr(payload, "hello", "ExtractDataToString: 内容正确");
  Check(ExtractDataToString(PathJoin(g_root, "basics.tar"), members[2],
                            &payload, &error) &&
            payload.empty(),
        "ExtractDataToString: 0 字节文件得到空串", error);

  // 空归档（只有两个全零 block）是合法的：0 个条目。
  std::vector<Member> empty_members;
  Check(WriteAndScan("empty-ok.tar", BuildRawArchive({}), &empty_members,
                     &error) &&
            empty_members.empty(),
        "Scan: 只有结尾标记的空归档合法（0 条目）", error);
}

// ---- 5. Scan：preflight 必须拒绝的输入 ----

void TestScanRejects() {
  std::string error;
  std::vector<Member> members;

  CheckFailsWith(WriteAndScan("empty.tar", "", &members, &error), error,
                 "结尾标记", "Scan 拒绝: 空文件（第一个 header 之前就结束）");
  CheckFailsWith(
      WriteAndScan("short.tar", std::string(200, 'x'), &members, &error), error,
      "512", "Scan 拒绝: 文件不满 512 字节");
  CheckFailsWith(WriteAndScan("onezero.tar", std::string(kBlockSize, '\0'),
                              &members, &error),
                 error, "1 个全零 block", "Scan 拒绝: 结尾只有一个全零 block");
  {
    std::string bytes = BuildRawArchive({});
    bytes += "X";
    CheckFailsWith(WriteAndScan("trailing.tar", bytes, &members, &error), error,
                   "非零字节", "Scan 拒绝: 两个全零 block 之后出现非零字节");
  }
  {
    // GNU tar 会把归档补到 10240 字节：结尾标记之后的 0 是填充，必须容忍。
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("a", '0', "x"));
    Check(WriteAndScan("gnu-pad.tar", BuildRawArchive(raw, true, 8192),
                       &members, &error) &&
              members.size() == 1,
          "Scan 容忍: 结尾标记之后的零填充（GNU tar 的 10240 字节对齐）",
          error);
  }
  {
    // header 中间被截断：第一个 header 完整，第二个只剩 300 字节。
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("a", '0', "x"));
    std::string bytes = BuildRawArchive(raw);
    bytes.resize(kBlockSize + 300);
    CheckFailsWith(WriteAndScan("cut-header.tar", bytes, &members, &error),
                   error, "512", "Scan 拒绝: 第二个 header 读不满 512 字节");
  }
  {
    // payload 中间被截断：声明 1024 字节，文件里只剩 700。
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("a", '0', std::string(1024, 'p')));
    std::string bytes = BuildRawArchive(raw);
    bytes.resize(kBlockSize + 700);
    CheckFailsWith(WriteAndScan("cut-payload.tar", bytes, &members, &error),
                   error, "payload 越界",
                   "Scan 拒绝: payload 越界（size 超出文件剩余长度）");
  }
  {
    // payload 恰好读完，但补齐到 512 的 padding 越界。
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("a", '0', std::string(400, 'p')));
    std::string bytes = BuildRawArchive(raw);
    bytes.resize(kBlockSize + 400);
    CheckFailsWith(WriteAndScan("cut-padding.tar", bytes, &members, &error),
                   error, "padding 越界", "Scan 拒绝: padding 越界");
  }
  {
    Header absolute;
    absolute.name = "/etc/passwd";
    absolute.mode = 0644;
    absolute.mtime = 1;
    CheckFailsWith(
        WriteAndScan("absolute.tar", BuildRawArchive({{absolute, ""}}),
                     &members, &error),
        error, "绝对路径", "Scan 拒绝: 绝对路径");
  }
  {
    Header traversal;
    traversal.name = "../escape";
    traversal.mode = 0644;
    traversal.mtime = 1;
    CheckFailsWith(
        WriteAndScan("traversal.tar", BuildRawArchive({{traversal, ""}}),
                     &members, &error),
        error, "..", "Scan 拒绝: '..' 路径穿越");
  }
  {
    Header empty_component;
    empty_component.name = "a//b";
    empty_component.mode = 0644;
    empty_component.mtime = 1;
    CheckFailsWith(WriteAndScan("empty-component.tar",
                                BuildRawArchive({{empty_component, ""}}),
                                &members, &error),
                   error, "空 component", "Scan 拒绝: 空 component（a//b）");
  }
  {
    Header dot_component;
    dot_component.name = "foo/./bar";
    dot_component.mode = 0644;
    dot_component.mtime = 1;
    CheckFailsWith(
        WriteAndScan("dot-component.tar",
                     BuildRawArchive({{dot_component, ""}}), &members, &error),
        error, "'.' component", "Scan 拒绝: '.' component（foo/./bar）");
  }
  {
    // 非目录条目带结尾 '/'。
    Header with_slash;
    with_slash.name = "a/";
    with_slash.typeflag = '0';
    with_slash.mode = 0644;
    with_slash.mtime = 1;
    CheckFailsWith(
        WriteAndScan("trailing-slash.tar", BuildRawArchive({{with_slash, ""}}),
                     &members, &error),
        error, "只有目录", "Scan 拒绝: 普通文件名字带结尾 '/'");
  }
  {
    // name 字段里塞 NUL：字节层面构造，checksum 重新算正确。
    std::string bytes = BuildRawArchive({MakeEntry("ab", '0', "")});
    PatchField(&bytes, 0, std::string("a\0b\0", 4));
    FixChecksum(&bytes);
    CheckFailsWith(WriteAndScan("nul-name.tar", bytes, &members, &error), error,
                   "NUL 注入", "Scan 拒绝: name 里有 NUL 字节");
  }
  {
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("dup", '0', ""));
    raw.push_back(MakeEntry("dup", '0', ""));
    CheckFailsWith(
        WriteAndScan("duplicate.tar", BuildRawArchive(raw), &members, &error),
        error, "重复路径", "Scan 拒绝: 重复路径");
  }
  {
    // 父子冲突：a 已经是普通文件，后面又出现 a/b。
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("a", '0', ""));
    raw.push_back(MakeEntry("a/b", '0', ""));
    CheckFailsWith(
        WriteAndScan("parent-file.tar", BuildRawArchive(raw), &members, &error),
        error, "父子冲突", "Scan 拒绝: 普通文件下面又出现子路径");
  }
  {
    // 反向父子冲突：先有 a/b，后来 a 是普通文件。
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("a/b", '0', ""));
    raw.push_back(MakeEntry("a", '0', ""));
    CheckFailsWith(
        WriteAndScan("child-first.tar", BuildRawArchive(raw), &members, &error),
        error, "父子冲突", "Scan 拒绝: 路径已经是别人的父目录却又当成文件");
  }
  {
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("a", '0', ""));
    std::string bytes = BuildRawArchive(raw);
    PatchField(&bytes, 156, std::string("Z", 1));
    FixChecksum(&bytes);
    CheckFailsWith(WriteAndScan("unknown-flag.tar", bytes, &members, &error),
                   error, "unknown typeflag", "Scan 拒绝: 未知 typeflag");
  }
  {
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("sock", '0', ""));
    std::string bytes = BuildRawArchive(raw);
    PatchField(&bytes, 156, std::string("s", 1));
    FixChecksum(&bytes);
    CheckFailsWith(
        WriteAndScan("socket.tar", bytes, &members, &error), error, "socket",
        "Scan 拒绝: socket 类型（unsupported special type: socket）");
  }
  {
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("long", '0', ""));
    std::string bytes = BuildRawArchive(raw);
    PatchField(&bytes, 156, std::string("L", 1));
    FixChecksum(&bytes);
    CheckFailsWith(WriteAndScan("gnu-long.tar", bytes, &members, &error), error,
                   "extension", "Scan 拒绝: GNU 长名字头 'L'");
  }
  {
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("a", '0', ""));
    std::string bytes = BuildRawArchive(raw);
    PatchField(&bytes, 124, OctalNul(12, 11));
    PatchField(&bytes, 156, std::string("5", 1));
    FixChecksum(&bytes);
    CheckFailsWith(WriteAndScan("dir-size.tar", bytes, &members, &error), error,
                   "payload", "Scan 拒绝: 目录条目声明 size");
  }
  {
    std::vector<RawEntry> raw;
    RawEntry hard = MakeEntry("hard", '1', "");
    hard.header.linkname = "missing";
    raw.push_back(hard);
    CheckFailsWith(WriteAndScan("hard-missing.tar", BuildRawArchive(raw),
                                &members, &error),
                   error, "硬链接目标不在归档中",
                   "Scan 拒绝: 硬链接目标不存在于归档");
  }
  {
    std::vector<RawEntry> raw;
    RawEntry hard = MakeEntry("hard", '1', "");
    hard.header.linkname = "later";
    raw.push_back(hard);
    raw.push_back(MakeEntry("later", '0', ""));
    CheckFailsWith(WriteAndScan("hard-forward.tar", BuildRawArchive(raw),
                                &members, &error),
                   error, "硬链接目标出现在硬链接之后",
                   "Scan 拒绝: 硬链接目标出现在后面");
  }
  {
    std::vector<RawEntry> raw;
    raw.push_back(MakeEntry("dir", '5', ""));
    RawEntry hard = MakeEntry("hard", '1', "");
    hard.header.linkname = "dir";
    raw.push_back(hard);
    CheckFailsWith(
        WriteAndScan("hard-dir.tar", BuildRawArchive(raw), &members, &error),
        error, "硬链接目标不是普通文件", "Scan 拒绝: 硬链接目标不是普通文件");
  }
  {
    // GNU tar 用 "./" 前缀写路径、目录名带结尾 '/'，两种写法都要归一化。
    std::vector<RawEntry> raw;
    Header root;
    root.typeflag = '5';
    root.name = "./";
    root.mode = 0755;
    root.mtime = 1;
    Header a;
    a.typeflag = '0';
    a.name = "./a";
    a.size = 1;
    a.mode = 0644;
    a.mtime = 1;
    Header sub;
    sub.typeflag = '5';
    sub.name = "./sub/";
    sub.mode = 0755;
    sub.mtime = 1;
    Header b;
    b.typeflag = '0';
    b.name = "./sub/b";
    b.size = 1;
    b.mode = 0644;
    b.mtime = 1;
    Header hard;
    hard.typeflag = '1';
    hard.name = "./hard";
    hard.linkname = "./a";
    hard.mode = 0644;
    hard.mtime = 1;
    raw.push_back({root, ""});
    raw.push_back({a, "x"});
    raw.push_back({sub, ""});
    raw.push_back({b, "y"});
    raw.push_back({hard, ""});
    std::vector<Member> gnu_members;
    const bool ok = WriteAndScan("gnu-style.tar", BuildRawArchive(raw),
                                 &gnu_members, &error);
    Check(ok, "Scan: GNU 风格（./ 前缀 + 目录结尾 /）归档能读", error);
    if (ok) {
      const char* kPaths[] = {".", "a", "sub", "sub/b", "hard"};
      bool paths_ok = gnu_members.size() == 5;
      for (std::size_t i = 0; i < gnu_members.size() && i < 5; ++i) {
        paths_ok = paths_ok && gnu_members[i].entry.archive_path == kPaths[i];
      }
      Check(paths_ok, "Scan: './' 前缀与结尾 '/' 被归一化");
      Check(gnu_members.size() == 5 && gnu_members[4].entry.link_target == "a",
            "Scan: 硬链接目标 './a' 归一化成 'a'");
    }
  }
}

// ---- 6. 写入器 ----

bool MakeParentDirectories(const std::string& path) {
  for (std::size_t i = 1; i < path.size(); ++i) {
    if (path[i] != '/') {
      continue;
    }
    const std::string prefix = path.substr(0, i);
    if (::mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST) {
      return false;
    }
  }
  return true;
}

// 测试用的固定树。regular file 的正文来自真实文件（source_path），目录 /
// FIFO / 设备 / 链接这些没有 payload，元数据由条目自己带。
void MakeFixtureFiles() {
  std::filesystem::create_directories(PathJoin(g_root, "sub"));
  std::filesystem::create_directories(PathJoin(g_root, "emptydir"));
  if (!WriteFile(PathJoin(g_root, "file.txt"), "hello world\n") ||
      !WriteFile(PathJoin(g_root, "owner.txt"), "hello world\n") ||
      !WriteFile(PathJoin(g_root, "zero.bin"), "") ||
      !WriteFile(PathJoin(g_root, "空 格 ünïcode.txt"), "unicode\n") ||
      !WriteFile(PathJoin(g_root, "sub/inner.txt"), "abc")) {
    std::fprintf(stderr, "准备测试 fixture 失败\n");
    std::exit(2);
  }
  std::string big;
  big.reserve(1543);
  for (std::size_t i = 0; i < 1543; ++i) {
    big.push_back(static_cast<char>('a' + (i % 26)));
  }
  if (!WriteFile(PathJoin(g_root, "big.bin"), big)) {
    std::fprintf(stderr, "准备 big.bin 失败\n");
    std::exit(2);
  }
}

ArchiveEntry MakeEntryMeta(const std::string& archive_path, EntryType type,
                           std::uint32_t mode, std::uint64_t size,
                           std::int64_t mtime) {
  ArchiveEntry entry;
  entry.archive_path = archive_path;
  entry.type = type;
  entry.mode = mode;
  entry.uid = 65534;
  entry.gid = 65534;
  entry.mtime_sec = mtime;
  entry.size = size;
  if (type == EntryType::kRegularFile) {
    entry.source_path = PathJoin(g_root, archive_path);
  }
  return entry;
}

std::vector<ArchiveEntry> MakeFixtureEntries() {
  std::vector<ArchiveEntry> entries;
  entries.push_back(
      MakeEntryMeta(".", EntryType::kDirectory, 0755, 0, 1000000000));
  entries.push_back(
      MakeEntryMeta("file.txt", EntryType::kRegularFile, 0644, 12, 1000000001));
  entries.push_back(
      MakeEntryMeta("zero.bin", EntryType::kRegularFile, 0600, 0, 0));
  entries.push_back(MakeEntryMeta("big.bin", EntryType::kRegularFile, 07777,
                                  1543, 2000000000));
  entries.push_back(MakeEntryMeta("空 格 ünïcode.txt", EntryType::kRegularFile,
                                  0644, 8, 1000000002));
  entries.push_back(
      MakeEntryMeta("sub", EntryType::kDirectory, 0700, 0, 1000000003));
  entries.push_back(MakeEntryMeta("sub/inner.txt", EntryType::kRegularFile,
                                  0644, 3, 1000000004));
  entries.push_back(
      MakeEntryMeta("emptydir", EntryType::kDirectory, 0755, 0, 1000000005));
  ArchiveEntry symlink =
      MakeEntryMeta("link.txt", EntryType::kSymlink, 0777, 0, 1000000006);
  symlink.link_target = "file.txt";
  entries.push_back(symlink);
  ArchiveEntry hardlink =
      MakeEntryMeta("hard.txt", EntryType::kHardLink, 0644, 0, 1000000007);
  hardlink.link_target = "file.txt";
  entries.push_back(hardlink);
  entries.push_back(
      MakeEntryMeta("pipe.fifo", EntryType::kFifo, 0644, 0, 1000000008));
  ArchiveEntry chardev =
      MakeEntryMeta("null", EntryType::kCharDevice, 0666, 0, 1000000009);
  chardev.dev_major = 1;
  chardev.dev_minor = 3;
  entries.push_back(chardev);
  ArchiveEntry blockdev =
      MakeEntryMeta("sda", EntryType::kBlockDevice, 0660, 0, 1000000010);
  blockdev.dev_major = 8;
  blockdev.dev_minor = 0;
  entries.push_back(blockdev);
  entries.push_back(MakeEntryMeta("owner.txt", EntryType::kRegularFile, 0644,
                                  12, 1000000011));
  entries.back().user_name = "pw-is-123";
  entries.back().group_name = "pw-is-123";
  return entries;
}

std::uint64_t ExpectedArchiveBytes(const std::vector<ArchiveEntry>& entries) {
  std::uint64_t total = 0;
  for (const ArchiveEntry& entry : entries) {
    total += kBlockSize;
    if (entry.type == EntryType::kRegularFile) {
      total += ((entry.size + kBlockSize - 1) / kBlockSize) * kBlockSize;
    }
  }
  return total + 2 * kBlockSize;
}

void CheckMemberEquals(const Member& member, const ArchiveEntry& expected,
                       const std::string& label) {
  Check(member.entry.archive_path == expected.archive_path &&
            member.entry.type == expected.type &&
            member.entry.mode == expected.mode &&
            member.entry.uid == expected.uid &&
            member.entry.gid == expected.gid &&
            member.entry.mtime_sec == expected.mtime_sec &&
            member.entry.size == expected.size,
        label + ": 元数据（路径/类型/mode/uid/gid/mtime/size）",
        "路径 " + member.entry.archive_path + "，类型 " +
            EntryTypeName(member.entry.type) + "，mode " +
            std::to_string(member.entry.mode) + "，size " +
            std::to_string(member.entry.size));
  CheckEqStr(member.entry.link_target, expected.link_target,
             label + ": link_target");
  CheckEqU64(member.entry.dev_major, expected.dev_major, label + ": dev_major");
  CheckEqU64(member.entry.dev_minor, expected.dev_minor, label + ": dev_minor");
  Check(member.entry.source_path.empty() &&
            member.data_size == member.entry.size &&
            member.data_offset % kBlockSize == 0,
        label + ": source_path 为空、data_size 一致、偏移 512 对齐");
}

void TestWriterRoundTrip(const std::vector<ArchiveEntry>& entries,
                         const std::string& writer_name, bool fast,
                         const std::string& archive_name) {
  std::string error;
  const std::string archive = PathJoin(g_root, archive_name);
  const bool written = fast ? WriteFast(entries, archive, &error)
                            : WriteBaseline(entries, archive, &error);
  Check(written, "写入器 " + writer_name + ": 写出归档", error);
  if (!written) {
    return;
  }
  CheckEqU64(
      FileSize(archive), ExpectedArchiveBytes(entries),
      "写入器 " + writer_name + ": 归档字节数（含 padding 与两个结尾 block）");

  std::string bytes;
  if (!ReadFile(archive, &bytes)) {
    Check(false, "写入器 " + writer_name + ": 读回归档", "读取失败");
    return;
  }
  bool tail_zero = bytes.size() >= 2 * kBlockSize;
  for (std::size_t i =
           bytes.size() >= 2 * kBlockSize ? bytes.size() - 2 * kBlockSize : 0;
       i < bytes.size(); ++i) {
    tail_zero = tail_zero && bytes[i] == '\0';
  }
  Check(tail_zero, "写入器 " + writer_name + ": 结尾恰好是两个全零 block");

  std::vector<Member> members;
  const bool scanned = Scan(archive, &members, &error);
  Check(scanned, "写入器 " + writer_name + ": Scan 读回", error);
  if (!scanned) {
    return;
  }
  CheckEqU64(members.size(), entries.size(),
             "写入器 " + writer_name + ": 条目数");
  if (members.size() != entries.size()) {
    return;
  }
  for (std::size_t i = 0; i < members.size(); ++i) {
    CheckMemberEquals(
        members[i], entries[i],
        "写入器 " + writer_name + " [" + entries[i].archive_path + "]");
  }

  bool payload_ok = true;
  std::string detail;
  for (const ArchiveEntry& entry : entries) {
    if (entry.type != EntryType::kRegularFile) {
      continue;
    }
    std::string source_content;
    if (!ReadFile(entry.source_path, &source_content)) {
      payload_ok = false;
      detail = "读不到源文件 " + entry.source_path;
      break;
    }
    std::string extracted;
    for (const Member& member : members) {
      if (member.entry.archive_path == entry.archive_path) {
        if (!ExtractDataToString(archive, member, &extracted, &error)) {
          payload_ok = false;
          detail = error;
        }
        break;
      }
    }
    if (!payload_ok) {
      break;
    }
    if (extracted != source_content) {
      payload_ok = false;
      detail = "payload 与源文件不一致: " + entry.archive_path;
      break;
    }
  }
  Check(payload_ok,
        "写入器 " + writer_name + ": 每个普通文件的 payload 与源文件逐字节一致",
        detail);
}

ArchiveEntry SingleRegularEntry(const std::string& archive_path,
                                const std::string& source_path,
                                std::uint64_t size) {
  ArchiveEntry entry;
  entry.archive_path = archive_path;
  entry.type = EntryType::kRegularFile;
  entry.mode = 0644;
  entry.mtime_sec = 100;
  entry.size = size;
  entry.source_path = source_path;
  return entry;
}

// 预校验失败的样本：两个写入器都必须失败，而且**不能**在磁盘上留下归档文件。
void CheckWriterRejects(const std::vector<ArchiveEntry>& entries,
                        const std::string& archive_name,
                        const std::string& keyword, const std::string& label) {
  std::string error;
  const std::string archive = PathJoin(g_root, archive_name);
  CheckFailsWith(WriteBaseline(entries, archive, &error), error, keyword,
                 "baseline: " + label);
  Check(!FileExists(archive), "baseline: " + label + " —— 没有留下半成品");
  error.clear();
  CheckFailsWith(WriteFast(entries, archive, &error), error, keyword,
                 "Fast: " + label);
  Check(!FileExists(archive), "Fast: " + label + " —— 没有留下半成品");
}

void TestWriterFailures(const std::vector<ArchiveEntry>& entries) {
  std::string error;

  {
    const std::string archive = PathJoin(g_root, "existing.tar");
    WriteFile(archive, "PRECIOUS");
    CheckFailsWith(WriteBaseline(entries, archive, &error), error,
                   "不覆盖已有文件",
                   "baseline: 已存在的归档文件被拒绝（O_EXCL）");
    std::string content;
    ReadFile(archive, &content);
    CheckEqStr(content, "PRECIOUS", "baseline: 已存在文件的内容没有被改动");
    error.clear();
    CheckFailsWith(WriteFast(entries, archive, &error), error, "不覆盖已有文件",
                   "Fast: 已存在的归档文件被拒绝（O_EXCL）");
    ReadFile(archive, &content);
    CheckEqStr(content, "PRECIOUS", "Fast: 已存在文件的内容没有被改动");
    (void)::unlink(archive.c_str());
  }

  {
    std::vector<ArchiveEntry> bad;
    bad.push_back(SingleRegularEntry(std::string(256, 'x'),
                                     PathJoin(g_root, "file.txt"), 12));
    CheckWriterRejects(bad, "too-long-path.tar", "255", "路径 256 字节");
  }
  {
    std::vector<ArchiveEntry> bad;
    bad.push_back(SingleRegularEntry(std::string(120, 'y'),
                                     PathJoin(g_root, "file.txt"), 12));
    CheckWriterRejects(bad, "unsplittable-path.tar", "prefix",
                       "120 字节单层路径存不下");
  }
  {
    std::vector<ArchiveEntry> bad;
    ArchiveEntry socket_entry =
        MakeEntryMeta("sock", EntryType::kSocket, 0644, 0, 1);
    bad.push_back(socket_entry);
    CheckWriterRejects(bad, "socket-entry.tar", "socket", "socket 条目");
  }
  {
    std::vector<ArchiveEntry> bad;
    ArchiveEntry entry =
        SingleRegularEntry("neg-mtime", PathJoin(g_root, "file.txt"), 12);
    entry.mtime_sec = -3600;
    bad.push_back(entry);
    CheckWriterRejects(bad, "neg-mtime.tar", "1970", "负 mtime（1970 之前）");
  }
  {
    std::vector<ArchiveEntry> bad;
    ArchiveEntry entry =
        SingleRegularEntry("big-uid", PathJoin(g_root, "file.txt"), 12);
    entry.uid = 4294967294u;
    bad.push_back(entry);
    CheckWriterRejects(bad, "big-uid.tar", "uid", "uid 4294967294 超出字段");
  }
  {
    std::vector<ArchiveEntry> bad;
    ArchiveEntry entry =
        SingleRegularEntry("mode-bits", PathJoin(g_root, "file.txt"), 12);
    entry.mode = 0100644;
    bad.push_back(entry);
    CheckWriterRejects(bad, "mode-bits.tar", "07777", "mode 带类型位");
  }
  {
    std::vector<ArchiveEntry> bad;
    ArchiveEntry link =
        MakeEntryMeta("long-link", EntryType::kSymlink, 0777, 0, 1);
    link.link_target = std::string(101, 't');
    bad.push_back(link);
    CheckWriterRejects(bad, "long-link.tar", "100", "软链接 linkname 101 字节");
  }

  // 源文件读不到 / 比声明的短：归档文件已经创建，失败时必须删掉半成品。
  {
    const std::string archive = PathJoin(g_root, "missing-source.tar");
    std::vector<ArchiveEntry> bad;
    bad.push_back(SingleRegularEntry(
        "missing.txt", PathJoin(g_root, "no-such-file.bin"), 100));
    CheckFailsWith(WriteBaseline(bad, archive, &error), error, "打开源文件失败",
                   "baseline: 源文件不存在时失败");
    Check(!FileExists(archive), "baseline: 源文件不存在时删掉了半成品归档");
    error.clear();
    CheckFailsWith(WriteFast(bad, archive, &error), error, "打开源文件失败",
                   "Fast: 源文件不存在时失败");
    Check(!FileExists(archive), "Fast: 源文件不存在时删掉了半成品归档");
  }
  {
    // 声明 100 字节，实际只有 5 字节：读不满就必须失败，不能补 0 冒充。
    const std::string short_source = PathJoin(g_root, "short-source.bin");
    WriteFile(short_source, "12345");
    std::vector<ArchiveEntry> bad;
    bad.push_back(SingleRegularEntry("short.bin", short_source, 100));
    const std::string archive = PathJoin(g_root, "short-source.tar");
    CheckFailsWith(WriteBaseline(bad, archive, &error), error,
                   "比声明的 size 短", "baseline: 源文件短于声明 size 时失败");
    Check(!FileExists(archive), "baseline: 源文件读不满时删掉了半成品归档");
    error.clear();
    CheckFailsWith(WriteFast(bad, archive, &error), error, "比声明的 size 短",
                   "Fast: 源文件短于声明 size 时失败");
    Check(!FileExists(archive), "Fast: 源文件读不满时删掉了半成品归档");
  }
}

// ---- 7. ExtractData ----

void TestExtract(const std::vector<ArchiveEntry>& entries) {
  std::string error;
  const std::string archive = PathJoin(g_root, "extract.tar");
  if (!WriteBaseline(entries, archive, &error)) {
    Check(false, "Extract: 准备归档", error);
    return;
  }
  std::vector<Member> members;
  if (!Scan(archive, &members, &error)) {
    Check(false, "Extract: 准备 Scan", error);
    return;
  }

  bool all_ok = true;
  bool zero_ok = false;
  std::string detail;
  for (const Member& member : members) {
    if (member.entry.type != EntryType::kRegularFile) {
      continue;
    }
    const std::string destination =
        PathJoin(PathJoin(g_root, "out"), member.entry.archive_path);
    if (!MakeParentDirectories(destination)) {
      all_ok = false;
      detail = "建目录失败: " + destination;
      break;
    }
    if (!ExtractData(archive, member, destination, &error)) {
      all_ok = false;
      detail = error;
      break;
    }
    std::string source_content;
    std::string destination_content;
    if (!ReadFile(PathJoin(g_root, member.entry.archive_path),
                  &source_content) ||
        !ReadFile(destination, &destination_content)) {
      all_ok = false;
      detail = "读文件失败: " + member.entry.archive_path;
      break;
    }
    if (source_content != destination_content) {
      all_ok = false;
      detail = "内容不一致: " + member.entry.archive_path;
      break;
    }
    if (member.entry.archive_path == "zero.bin") {
      zero_ok = destination_content.empty() && FileExists(destination);
    }
  }
  Check(all_ok, "ExtractData: 每个普通文件都写到目标文件且内容与源一致",
        detail);
  Check(zero_ok, "ExtractData: 0 字节文件也被创建出来（存在且为空）");

  // 失败的路径：非普通文件成员、偏移不对齐、payload 越界。
  for (const Member& member : members) {
    if (member.entry.type == EntryType::kDirectory) {
      CheckFailsWith(ExtractData(archive, member,
                                 PathJoin(g_root, "out/dir-as-file"), &error),
                     error, "只有普通文件",
                     "ExtractData: 目录成员不能当普通文件解出 payload");
      break;
    }
  }
  for (const Member& member : members) {
    if (member.entry.type != EntryType::kRegularFile) {
      continue;
    }
    Member misaligned = member;
    misaligned.data_offset += 1;
    CheckFailsWith(ExtractData(archive, misaligned,
                               PathJoin(g_root, "out/misaligned.bin"), &error),
                   error, "512", "ExtractData: 偏移不是 512 倍数时失败");
    Member too_big = member;
    too_big.data_size = 1u << 20;
    const std::string destination = PathJoin(g_root, "out/too-big.bin");
    CheckFailsWith(ExtractData(archive, too_big, destination, &error), error,
                   "payload 越界", "ExtractData: payload 越界时失败");
    Check(!FileExists(destination), "ExtractData: 越界失败时没有创建目标文件");
    break;
  }
}

// ---- 8. 条目数上限 ----

// 条目数上限用真实归档验证：写 100 万 + 1 条最小条目（512 MB 全零以外的
// 合法 header），Scan 必须在第 100 万 + 1 条上明确报错，而不是继续吃内存。
// 这条测试要写 512 MB，ASan 下内存与时间都不划算，用 --skip-big 跳过。
void TestMemberLimit() {
  std::string error;
  const std::string archive = PathJoin(g_root, "too-many.tar");
  const int fd =
      ::open(archive.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    Check(false, "条目数上限: 构造测试归档", "打开失败");
    return;
  }
  std::vector<char> buffer(1024 * 1024);
  std::size_t used = 0;
  std::string block;
  bool write_ok = true;
  for (std::uint64_t i = 0; i <= kMaxMembers; ++i) {
    Header header;
    header.typeflag = '0';
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%08llx",
                  static_cast<unsigned long long>(i));
    header.name.assign(digits, 8);
    header.mode = 0644;
    header.mtime = 1;
    if (!EncodeHeader(header, &block, &error)) {
      write_ok = false;
      break;
    }
    if (used + kBlockSize > buffer.size()) {
      if (!WriteAllRaw(fd, buffer.data(), used, &error)) {
        write_ok = false;
        break;
      }
      used = 0;
    }
    std::memcpy(buffer.data() + used, block.data(), kBlockSize);
    used += kBlockSize;
  }
  if (write_ok && used > 0) {
    write_ok = WriteAllRaw(fd, buffer.data(), used, &error);
  }
  if (write_ok) {
    const std::vector<char> zeros(2 * kBlockSize, '\0');
    write_ok = WriteAllRaw(fd, zeros.data(), zeros.size(), &error);
  }
  (void)::close(fd);
  if (!write_ok) {
    Check(false, "条目数上限: 构造测试归档", error);
    (void)::unlink(archive.c_str());
    return;
  }
  std::vector<Member> members;
  CheckFailsWith(Scan(archive, &members, &error), error, "上限",
                 "Scan: 超过 100 万条目时明确报错（不 OOM）");
  (void)::unlink(archive.c_str());
}

// ---- 9. 子命令：扫描真实目录树 / 列目录 / 还原 ----

std::string LookupUser(std::uint32_t uid) {
  char buffer[4096];
  struct passwd entry;
  struct passwd* result = nullptr;
  if (::getpwuid_r(static_cast<uid_t>(uid), &entry, buffer, sizeof(buffer),
                   &result) != 0 ||
      result == nullptr) {
    return std::string();  // 解析失败就留空：ustar 里 uid 才是权威
  }
  return std::string(result->pw_name);
}

std::string LookupGroup(std::uint32_t gid) {
  char buffer[4096];
  struct group entry;
  struct group* result = nullptr;
  if (::getgrgid_r(static_cast<gid_t>(gid), &entry, buffer, sizeof(buffer),
                   &result) != 0 ||
      result == nullptr) {
    return std::string();
  }
  return std::string(result->gr_name);
}

// 真实目录树 -> DFS 先序的 ArchiveEntry 列表。硬链接按 (dev, ino) 去重，
// 第二次出现时写成指向第一次出现路径的 kHardLink。
class TreeScanner {
 public:
  bool Scan(const std::string& root, std::vector<ArchiveEntry>* entries,
            std::string* error_message) {
    entries_ = entries;
    error_ = error_message;
    return AddPath(root, ".");
  }

 private:
  bool AddChildren(const std::string& directory,
                   const std::string& archive_path) {
    DIR* dir = ::opendir(directory.c_str());
    if (dir == nullptr) {
      *error_ = "opendir 失败: " + directory + ": " + std::strerror(errno);
      return false;
    }
    std::vector<std::string> names;
    while (struct dirent* item = ::readdir(dir)) {
      const std::string name = item->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      names.push_back(name);
    }
    (void)::closedir(dir);
    // 排序只为让归档内容可复现：readdir 的顺序是文件系统给的，不稳定。
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
      if (!AddPath(PathJoin(directory, name), PathJoin(archive_path, name))) {
        return false;
      }
    }
    return true;
  }

  bool AddPath(const std::string& full_path, const std::string& archive_path) {
    struct stat status;
    if (::lstat(full_path.c_str(), &status) != 0) {
      *error_ = "lstat 失败: " + full_path + ": " + std::strerror(errno);
      return false;
    }
    ArchiveEntry entry;
    entry.archive_path = archive_path;
    entry.source_path = full_path;
    entry.mode = static_cast<std::uint32_t>(status.st_mode & 07777);
    entry.uid = status.st_uid;
    entry.gid = status.st_gid;
    entry.mtime_sec = status.st_mtim.tv_sec;
    entry.mtime_nsec = static_cast<std::uint32_t>(status.st_mtim.tv_nsec);
    entry.user_name = LookupUser(entry.uid);
    entry.group_name = LookupGroup(entry.gid);
    if (S_ISDIR(status.st_mode)) {
      entry.type = EntryType::kDirectory;
      entries_->push_back(entry);
      return AddChildren(full_path, archive_path);
    }
    if (S_ISREG(status.st_mode)) {
      if (status.st_nlink > 1) {
        const std::string key =
            std::to_string(static_cast<unsigned long long>(status.st_dev)) +
            ":" +
            std::to_string(static_cast<unsigned long long>(status.st_ino));
        const auto found = inodes_.find(key);
        if (found != inodes_.end()) {
          entry.type = EntryType::kHardLink;
          entry.link_target = found->second;
          entry.size = 0;
          entries_->push_back(entry);
          return true;
        }
        inodes_.emplace(key, archive_path);
      }
      entry.type = EntryType::kRegularFile;
      entry.size = static_cast<std::uint64_t>(status.st_size);
      entries_->push_back(entry);
      return true;
    }
    if (S_ISLNK(status.st_mode)) {
      char target[4096];
      const ssize_t length =
          ::readlink(full_path.c_str(), target, sizeof(target) - 1);
      if (length < 0) {
        *error_ = "readlink 失败: " + full_path + ": " + std::strerror(errno);
        return false;
      }
      entry.type = EntryType::kSymlink;
      entry.link_target.assign(target, static_cast<std::size_t>(length));
      entries_->push_back(entry);
      return true;
    }
    if (S_ISFIFO(status.st_mode)) {
      entry.type = EntryType::kFifo;
      entries_->push_back(entry);
      return true;
    }
    if (S_ISCHR(status.st_mode) || S_ISBLK(status.st_mode)) {
      entry.type = S_ISCHR(status.st_mode) ? EntryType::kCharDevice
                                           : EntryType::kBlockDevice;
      entry.dev_major = backupproject::DeviceMajor(
          static_cast<std::uint64_t>(status.st_rdev));
      entry.dev_minor = backupproject::DeviceMinor(
          static_cast<std::uint64_t>(status.st_rdev));
      entries_->push_back(entry);
      return true;
    }
    if (S_ISSOCK(status.st_mode)) {
      *error_ = "unsupported special type: socket: " + full_path;
      return false;
    }
    *error_ = "不支持的文件类型: " + full_path;
    return false;
  }

  std::vector<ArchiveEntry>* entries_ = nullptr;
  std::string* error_ = nullptr;
  std::map<std::string, std::string> inodes_;
};

char TypeChar(EntryType type) {
  switch (type) {
    case EntryType::kRegularFile:
      return 'f';
    case EntryType::kDirectory:
      return 'd';
    case EntryType::kSymlink:
      return 'l';
    case EntryType::kHardLink:
      return 'h';
    case EntryType::kFifo:
      return 'p';
    case EntryType::kCharDevice:
      return 'c';
    case EntryType::kBlockDevice:
      return 'b';
    case EntryType::kSocket:
      return 's';
  }
  return '?';
}

int RunPack(const std::string& source, const std::string& archive,
            const std::string& writer) {
  if (writer != "baseline" && writer != "fast") {
    std::fprintf(stderr, "--pack 的第三个参数必须是 baseline 或 fast\n");
    return 2;
  }
  std::vector<ArchiveEntry> entries;
  std::string error;
  TreeScanner scanner;
  if (!scanner.Scan(source, &entries, &error)) {
    std::fprintf(stderr, "扫描失败: %s\n", error.c_str());
    return 1;
  }
  const bool ok = writer == "fast" ? WriteFast(entries, archive, &error)
                                   : WriteBaseline(entries, archive, &error);
  if (!ok) {
    std::fprintf(stderr, "写归档失败: %s\n", error.c_str());
    return 1;
  }
  std::printf("packed %zu entries -> %s (%s)\n", entries.size(),
              archive.c_str(), writer.c_str());
  return 0;
}

int RunList(const std::string& archive) {
  std::vector<Member> members;
  std::string error;
  if (!Scan(archive, &members, &error)) {
    std::fprintf(stderr, "Scan 失败: %s\n", error.c_str());
    return 1;
  }
  for (const Member& member : members) {
    std::printf("%c %llu %s", TypeChar(member.entry.type),
                static_cast<unsigned long long>(member.data_size),
                member.entry.archive_path.c_str());
    if (member.entry.type == EntryType::kSymlink ||
        member.entry.type == EntryType::kHardLink) {
      std::printf(" -> %s", member.entry.link_target.c_str());
    }
    std::printf("\n");
  }
  return 0;
}

int RunRestore(const std::string& archive, const std::string& destination) {
  std::vector<Member> members;
  std::string error;
  if (!Scan(archive, &members, &error)) {
    std::fprintf(stderr, "Scan 失败: %s\n", error.c_str());
    return 1;
  }
  if (::mkdir(destination.c_str(), 0700) != 0 && errno != EEXIST) {
    std::fprintf(stderr, "建目标目录失败: %s\n", destination.c_str());
    return 1;
  }
  // 目录先用 0700 建出来，最后统一恢复 mode / mtime：归档里可能是 0555 的
  // 目录，先设成最终权限后面的子项就写不进去了。
  for (const Member& member : members) {
    if (member.entry.type != EntryType::kDirectory) {
      continue;
    }
    const std::string path = PathJoin(destination, member.entry.archive_path);
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
      std::fprintf(stderr, "建目录失败: %s: %s\n", path.c_str(),
                   std::strerror(errno));
      return 1;
    }
  }
  for (const Member& member : members) {
    const std::string path = PathJoin(destination, member.entry.archive_path);
    if (!MakeParentDirectories(path)) {
      std::fprintf(stderr, "建父目录失败: %s\n", path.c_str());
      return 1;
    }
    switch (member.entry.type) {
      case EntryType::kDirectory:
        break;
      case EntryType::kRegularFile:
        if (!ExtractData(archive, member, path, &error)) {
          std::fprintf(stderr, "写文件失败: %s\n", error.c_str());
          return 1;
        }
        break;
      case EntryType::kSymlink:
        if (::symlink(member.entry.link_target.c_str(), path.c_str()) != 0) {
          std::fprintf(stderr, "建软链接失败: %s: %s\n", path.c_str(),
                       std::strerror(errno));
          return 1;
        }
        break;
      case EntryType::kHardLink: {
        const std::string target =
            PathJoin(destination, member.entry.link_target);
        if (::link(target.c_str(), path.c_str()) != 0) {
          std::fprintf(stderr, "建硬链接失败: %s: %s\n", path.c_str(),
                       std::strerror(errno));
          return 1;
        }
        break;
      }
      case EntryType::kFifo:
        if (::mkfifo(path.c_str(), 0600) != 0) {
          std::fprintf(stderr, "建 FIFO 失败: %s: %s\n", path.c_str(),
                       std::strerror(errno));
          return 1;
        }
        break;
      case EntryType::kCharDevice:
      case EntryType::kBlockDevice: {
        const mode_t kind =
            member.entry.type == EntryType::kCharDevice ? S_IFCHR : S_IFBLK;
        if (::mknod(path.c_str(), kind | 0600,
                    static_cast<dev_t>(MakeDevice(member.entry.dev_major,
                                                  member.entry.dev_minor))) !=
            0) {
          std::fprintf(stderr, "建设备节点失败（通常需要 root）: %s: %s\n",
                       path.c_str(), std::strerror(errno));
          return 1;
        }
        break;
      }
      case EntryType::kSocket:
        std::fprintf(stderr, "socket 不该出现在归档里: %s\n", path.c_str());
        return 1;
    }
  }
  // 权限与时间：从深到浅。创建子项会改父目录的 mtime，所以目录必须最后设。
  for (std::size_t i = members.size(); i-- > 0;) {
    const Member& member = members[i];
    const std::string path = PathJoin(destination, member.entry.archive_path);
    if (member.entry.type != EntryType::kSymlink) {
      if (::chmod(path.c_str(), static_cast<mode_t>(member.entry.mode)) != 0) {
        std::fprintf(stderr, "chmod 失败: %s: %s\n", path.c_str(),
                     std::strerror(errno));
        return 1;
      }
    }
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;  // 不动 atime
    times[1].tv_sec = static_cast<time_t>(member.entry.mtime_sec);
    times[1].tv_nsec = static_cast<long>(member.entry.mtime_nsec);
    if (::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
      std::fprintf(stderr, "utimensat 失败: %s: %s\n", path.c_str(),
                   std::strerror(errno));
      return 1;
    }
  }
  std::printf("restored %zu entries -> %s\n", members.size(),
              destination.c_str());
  return 0;
}

void PrintUsage() {
  std::fprintf(stderr,
               "用法:\n"
               "  ustar_test [--unit <目录>] [--skip-big]   跑单元测试\n"
               "  ustar_test --pack <源目录> <归档> baseline|fast\n"
               "  ustar_test --list <归档>\n"
               "  ustar_test --restore <归档> <目标目录>\n");
}

std::string MakeTempRoot(const std::string& requested, bool* owned) {
  if (!requested.empty()) {
    std::error_code error;
    std::filesystem::create_directories(requested, error);
    *owned = false;
    return requested;
  }
  char pattern[] = "/tmp/ustar-unit-XXXXXX";
  char* made = ::mkdtemp(pattern);
  *owned = made != nullptr;
  return made != nullptr ? std::string(made) : std::string("/tmp");
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv, argv + argc);
  if (args.size() >= 2) {
    const std::string& command = args[1];
    if (command == "--pack") {
      if (args.size() != 5) {
        PrintUsage();
        return 2;
      }
      return RunPack(args[2], args[3], args[4]);
    }
    if (command == "--list") {
      if (args.size() != 3) {
        PrintUsage();
        return 2;
      }
      return RunList(args[2]);
    }
    if (command == "--restore") {
      if (args.size() != 4) {
        PrintUsage();
        return 2;
      }
      return RunRestore(args[2], args[3]);
    }
    if (command == "--help") {
      PrintUsage();
      return 0;
    }
  }

  std::string unit_dir;
  bool skip_big = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--unit" && i + 1 < args.size()) {
      unit_dir = args[++i];
    } else if (args[i] == "--skip-big") {
      skip_big = true;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", args[i].c_str());
      PrintUsage();
      return 2;
    }
  }
  bool owns_root = false;
  g_root = MakeTempRoot(unit_dir, &owns_root);
  std::printf("== ustar 单元测试（临时目录 %s）==\n", g_root.c_str());

  MakeFixtureFiles();
  const std::vector<ArchiveEntry> entries = MakeFixtureEntries();

  TestSplitPath();
  TestTypeFlags();
  TestHeaderLayout();
  TestHeaderRoundTrip();
  TestDecodeRejects();
  TestScanBasics();
  TestScanRejects();
  TestWriterRoundTrip(entries, "baseline", false, "baseline.tar");
  TestWriterRoundTrip(entries, "fast", true, "fast.tar");
  TestExtract(entries);
  TestWriterFailures(entries);
  if (skip_big) {
    std::printf("SKIP: 条目数上限（100 万条，需要 512 MB 临时空间）\n");
  } else {
    TestMemberLimit();
  }

  // 两个写入器的字节是否一致：一致就说明 Fast 只改了 I/O 策略，没改格式。
  std::string baseline_bytes;
  std::string fast_bytes;
  ReadFile(PathJoin(g_root, "baseline.tar"), &baseline_bytes);
  ReadFile(PathJoin(g_root, "fast.tar"), &fast_bytes);
  std::printf("INFO: baseline 与 Fast 的归档字节%s（%zu vs %zu）\n",
              baseline_bytes == fast_bytes ? "完全相同" : "不同",
              baseline_bytes.size(), fast_bytes.size());

  std::printf("ustar_test: %d/%d checks passed\n", g_passed,
              g_passed + g_failed);
  if (owns_root) {
    std::error_code error;
    std::filesystem::remove_all(g_root, error);
  }
  return g_failed == 0 ? 0 : 1;
}

// tools/cert_tool_main.cpp
//
// backup-cert-tool —— 离线根密钥与服务器身份证书的管理工具（PR #23）。
//
//   backup-cert-tool root-init     --root-key <path> --root-id <id>
//   backup-cert-tool root-info     --root-key <path>
//   backup-cert-tool issue-server  --root-key <path> --server-id <id>
//                                  --server-pubkey <hex:<64 位十六进制>|@file>
//                                  --out <path>
//                                  [--serial N] [--days N] [--not-before N]
//   backup-cert-tool verify-server --cert <path> [--roots <file>]
//                                  [--issuer-pub <hex>] [--now N]
//   backup-cert-tool inspect-server --cert <path>
//
// 安全约定（写进代码而不是只写在文档里）：
//   * 根**私钥**只能从 --root-key <path> 读，**没有**任何命令行十六进制入口：
//     命令行会进 shell 历史、ps、审计日志，私钥绝不许走那条路；
//   * 工具任何子命令都**不打印**私钥内容、不做十六进制/base64 转储，
//     输出里固定带一行 content_printed = NO 作为可核对的证据；
//   * root-init 用 O_CREAT|O_EXCL|O_NOFOLLOW 打开文件，权限 0600，
//     已存在的文件一律拒绝覆盖（避免把旧根悄悄换掉）；
//   * 证书本身只有公钥材料，可以自由复制到服务器；私钥不行。
//
// 退出码：0 = 成功；1 = 业务失败（附原因）；2 = 用法错误。

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "bpcert.h"
#include "crypto.h"
#include "ed25519.h"
#include "trusted_root_store.h"
#include "x25519.h"

namespace {

using backupproject::crypto::Bpcert1;
using backupproject::crypto::Bpcert1Error;
using backupproject::crypto::Bpcert1ErrorName;
using backupproject::crypto::TrustedRoot;
using backupproject::crypto::TrustedRootStore;

struct Options {
  std::string command;
  std::vector<std::pair<std::string, std::string>> values;

  bool Has(const std::string& name) const {
    for (const auto& item : values) {
      if (item.first == name) return true;
    }
    return false;
  }
  std::string Get(const std::string& name,
                  const std::string& fallback = std::string()) const {
    for (const auto& item : values) {
      if (item.first == name) return item.second;
    }
    return fallback;
  }
};

// 严格的十进制整数解析：整个字符串必须被完全消费，且不得溢出。
//
// 为什么不能用 strtoll(..., nullptr, 10)：它把 "abc" 读成 0，把 "12abc" 读成
// 12， 并且正负号可以重复。在这里那不是宽松，而是静默地把用户的笔误
// 变成一个看起来合法的证书（2026 年签发一张 1970 年就过期的证书）。
bool ParseDecimalU64(const std::string& text, std::uint64_t* out,
                     std::string* error_message) {
  if (out == nullptr) return false;
  if (text.empty()) {
    if (error_message != nullptr) *error_message = "空字符串不是合法的数字";
    return false;
  }
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      if (error_message != nullptr) {
        *error_message = "只接受十进制非负整数，实际是 '" + text + "'";
      }
      return false;
    }
    const std::uint64_t digit_value = static_cast<std::uint64_t>(digit - '0');
    if (value > (UINT64_MAX - digit_value) / 10u) {
      if (error_message != nullptr) *error_message = "数字超出 uint64 范围";
      return false;
    }
    value = value * 10u + digit_value;
  }
  *out = value;
  return true;
}

// 时间戳与天数是 int64 语义（负数在协议层会被拒绝），这里只做上限校验。
bool ParseDecimalI64(const std::string& text, std::int64_t* out,
                     std::string* error_message) {
  std::uint64_t magnitude = 0;
  if (!ParseDecimalU64(text, &magnitude, error_message)) return false;
  if (magnitude > static_cast<std::uint64_t>(INT64_MAX)) {
    if (error_message != nullptr) *error_message = "数字超出 int64 范围";
    return false;
  }
  *out = static_cast<std::int64_t>(magnitude);
  return true;
}

void PrintUsage() {
  std::printf(
      "backup-cert-tool —— 离线根与服务器身份证书管理（PR #23）\n"
      "\n"
      "  root-init      --root-key <path> --root-id <id>\n"
      "                 生成一把新的离线根：私钥写入 "
      "<path>（0600，已存在则拒绝），\n"
      "                 公钥写到 <path>.pub（可直接当可信根文件用）。\n"
      "  root-info      --root-key <path>\n"
      "                 读回根的信息（只输出公钥与指纹）。\n"
      "  issue-server   --root-key <path> --server-id <id>\n"
      "                 --server-pubkey <hex:<64 位十六进制>|@file> --out "
      "<path>\n"
      "                 [--serial N] [--days N] [--not-before N]\n"
      "                 为已有服务器公钥签发身份证书（不改动任何私钥）。\n"
      "  verify-server  --cert <path> [--roots <file>] [--issuer-pub <hex>]\n"
      "                 [--now N]\n"
      "                 按可信根验签并检查时间窗；不给 --roots "
      "就用内置官方根。\n"
      "  inspect-server --cert <path>\n"
      "                 只打印证书内容（不做信任判断）。\n"
      "\n"
      "私钥只能通过 --root-key <文件路径> 提供；本工具**没有**、也不会加\n"
      "--root-key-hex 这类参数：私钥走命令行会进 shell 历史与 ps。\n");
}

bool ReadFile(const std::string& path, std::string* out,
              std::string* error_message) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    if (error_message != nullptr) *error_message = "打不开文件：" + path;
    return false;
  }
  out->clear();
  char buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, got);
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    if (error_message != nullptr) *error_message = "读取文件失败：" + path;
    return false;
  }
  return true;
}

// 独占创建 + 精确权限。exclusive=false
// 时允许覆盖（用于公钥/证书这类公开文件）。
bool WriteFile(const std::string& path, mode_t mode, const std::string& data,
               bool exclusive, std::string* error_message) {
  // O_NOFOLLOW 只挡符号链接，挡不住硬链接：提前把目标做成硬链接，写进去就会
  // 连带改到另一个路径上的文件。先 lstat 一次，链接数 > 1 直接拒绝。
  struct stat existing;
  if (::lstat(path.c_str(), &existing) == 0 && existing.st_nlink > 1) {
    if (error_message != nullptr) {
      *error_message = "目标文件有多个硬链接，拒绝写入：" + path;
    }
    return false;
  }
  const int flags =
      O_WRONLY | O_CREAT | O_NOFOLLOW | (exclusive ? O_EXCL : O_TRUNC);
  const int fd = ::open(path.c_str(), flags, mode);
  if (fd < 0) {
    if (error_message != nullptr) {
      *error_message = exclusive ? ("文件已存在或无法创建：" + path)
                                 : ("无法写入文件：" + path);
    }
    return false;
  }
  // umask 可能把权限改小，这里显式再设一次，保证"说 0600 就是 0600"。
  if (::fchmod(fd, mode) != 0) {
    ::close(fd);
    if (error_message != nullptr) *error_message = "无法设置文件权限：" + path;
    return false;
  }
  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n <= 0) {
      ::close(fd);
      if (error_message != nullptr) *error_message = "写文件失败：" + path;
      return false;
    }
    written += static_cast<std::size_t>(n);
  }
  if (::close(fd) != 0) {
    if (error_message != nullptr) *error_message = "关闭文件失败：" + path;
    return false;
  }
  return true;
}

// 回显参数名时只保留 '=' 之前的部分，并且截断长度。
// 原因：运维完全可能写成 --root-key-hex=<64 位种子>，整串回显就等于把私钥
// 写进了 stderr 与日志 —— 这是"私钥永不进日志"这条承诺上唯一被抓到的反例。
std::string RedactArgument(const std::string& raw) {
  const std::size_t equal = raw.find('=');
  std::string head = equal == std::string::npos ? raw : raw.substr(0, equal);
  if (head.size() > 32) {
    head = head.substr(0, 32) + "...";
  }
  return head;
}

std::string Trim(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                         text[begin] == '\r' || text[begin] == '\n')) {
    ++begin;
  }
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\r' || text[end - 1] == '\n')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string ModeOf(const std::string& path) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) return "unknown";
  char text[8];
  std::snprintf(text, sizeof(text), "%03o", info.st_mode & 0777);
  return text;
}

struct RootKey {
  std::string root_id;
  std::string seed;        // 32 字节私钥材料
  std::string public_key;  // 32 字节公钥
};

// 根密钥文件（纯文本，权限 0600）：
//     root-id: backup-project-official-root-a
//     seed-hex: <64 位十六进制>
bool LoadRootKey(const std::string& path, RootKey* out,
                 std::string* error_message) {
  std::string text;
  if (!ReadFile(path, &text, error_message)) {
    return false;
  }
  RootKey key;
  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t newline = text.find('\n', position);
    const std::string line = Trim(text.substr(
        position,
        newline == std::string::npos ? std::string::npos : newline - position));
    if (line.compare(0, 8, "root-id:") == 0) {
      key.root_id = Trim(line.substr(8));
    } else if (line.compare(0, 9, "seed-hex:") == 0) {
      const std::string hex = Trim(line.substr(9));
      if (!backupproject::crypto::FromHex(hex, &key.seed)) {
        if (error_message != nullptr)
          *error_message = "根文件里的 seed-hex 不是合法十六进制";
        return false;
      }
    } else if (!line.empty() && line[0] != '#') {
      // 忽略不认识的字段，便于将来加字段而不破坏旧文件。
    }
    if (newline == std::string::npos) break;
    position = newline + 1;
  }
  if (key.seed.size() != backupproject::crypto::kEd25519SeedSize) {
    if (error_message != nullptr) *error_message = "根私钥材料必须是 32 字节";
    return false;
  }
  if (!backupproject::crypto::Bpcert1IsValidIdentity(key.root_id)) {
    if (error_message != nullptr) *error_message = "根文件缺少合法的 root-id";
    return false;
  }
  if (!backupproject::crypto::Ed25519PublicKeyFromSeed(
          key.seed, &key.public_key, error_message)) {
    return false;
  }
  *out = key;
  return true;
}

bool ParseServerPublicKey(const std::string& text, std::string* out,
                          std::string* error_message) {
  std::string material = text;
  const bool from_file = !material.empty() && material[0] == '@';
  if (from_file) {
    if (!ReadFile(material.substr(1), &material, error_message)) {
      return false;
    }
    material = Trim(material);
  }
  std::string parse_error;
  if (backupproject::crypto::X25519ParseKeyText(material, out, &parse_error)) {
    return true;
  }
  // 32 字节原始二进制**只**对 @文件 成立。内联的 32 个十六进制字符一律按
  // "十六进制不完整"拒绝：否则同一段输入有两种读法，一个被截断的 64 位
  // 十六进制公钥会被当成 ASCII 原样签进证书，而且看不出来。
  if (from_file &&
      material.size() == backupproject::crypto::kBpcert1PublicKeySize) {
    *out = material;
    return true;
  }
  if (error_message != nullptr) {
    *error_message = "服务器公钥既不合法也不是 32 字节原始数据：" + parse_error;
  }
  return false;
}

int Fail(const std::string& message) {
  std::fprintf(stderr, "错误：%s\n", message.c_str());
  return 1;
}

int CommandRootInit(const Options& options) {
  const std::string path = options.Get("--root-key");
  const std::string root_id = options.Get("--root-id");
  if (path.empty() || root_id.empty()) {
    return Fail("root-init 需要 --root-key <path> 与 --root-id <id>");
  }
  std::string seed;
  std::string public_key;
  std::string error;
  if (!backupproject::crypto::Ed25519GenerateKeyPair(&seed, &public_key,
                                                     &error)) {
    return Fail("生成根密钥失败：" + error);
  }
  std::string key_text =
      "# backup-project 离线根私钥 —— 绝不可提交 / 上传 / 打印\n";
  key_text += "root-id: " + root_id + "\n";
  key_text +=
      "seed-hex: " +
      backupproject::crypto::ToHex(
          reinterpret_cast<const unsigned char*>(seed.data()), seed.size()) +
      "\n";
  if (!WriteFile(path, 0600, key_text, true, &error)) {
    return Fail(error);
  }
  // 显式写 unlimited：根的"不限期"必须是写出来的意图，不能靠省略字段表达。
  const std::string pub_text =
      root_id + " " +
      backupproject::crypto::Ed25519FormatPublicKeyHex(public_key) +
      " unlimited\n";
  if (!WriteFile(path + ".pub", 0644, pub_text, false, &error)) {
    return Fail(error);
  }
  std::printf("root_id            = %s\n", root_id.c_str());
  std::printf("root_key_file      = %s (mode %s)\n", path.c_str(),
              ModeOf(path).c_str());
  std::printf("root_public_file   = %s.pub\n", path.c_str());
  std::printf(
      "root_public_key    = %s\n",
      backupproject::crypto::Ed25519FormatPublicKeyHex(public_key).c_str());
  std::printf("root_fingerprint   = %s\n",
              backupproject::crypto::Ed25519Fingerprint(public_key).c_str());
  std::printf("content_printed    = NO\n");
  // 私钥材料用完即清，且从未出现在任何输出里。
  std::memset(&seed[0], 0, seed.size());
  return 0;
}

int CommandRootInfo(const Options& options) {
  const std::string path = options.Get("--root-key");
  if (path.empty()) {
    return Fail("root-info 需要 --root-key <path>");
  }
  RootKey key;
  std::string error;
  if (!LoadRootKey(path, &key, &error)) {
    return Fail(error);
  }
  std::printf("root_id            = %s\n", key.root_id.c_str());
  std::printf("root_key_file      = %s (mode %s)\n", path.c_str(),
              ModeOf(path).c_str());
  std::printf(
      "root_public_key    = %s\n",
      backupproject::crypto::Ed25519FormatPublicKeyHex(key.public_key).c_str());
  std::printf(
      "root_fingerprint   = %s\n",
      backupproject::crypto::Ed25519Fingerprint(key.public_key).c_str());
  std::printf("content_printed    = NO\n");
  std::memset(&key.seed[0], 0, key.seed.size());
  return 0;
}

int CommandIssueServer(const Options& options) {
  const std::string root_key_path = options.Get("--root-key");
  const std::string server_id = options.Get("--server-id");
  const std::string server_pub_text = options.Get("--server-pubkey");
  const std::string out_path = options.Get("--out");
  if (root_key_path.empty() || server_id.empty() || server_pub_text.empty() ||
      out_path.empty()) {
    return Fail(
        "issue-server 需要 --root-key --server-id --server-pubkey --out");
  }
  RootKey key;
  std::string error;
  if (!LoadRootKey(root_key_path, &key, &error)) {
    return Fail(error);
  }
  Bpcert1 certificate;
  certificate.server_id = server_id;
  certificate.issuer_id = key.root_id;
  if (!ParseServerPublicKey(server_pub_text, &certificate.server_public_key,
                            &error)) {
    return Fail(error);
  }
  const std::int64_t now = backupproject::crypto::Bpcert1NowUnixSeconds();
  certificate.not_before = now;
  if (options.Has("--not-before")) {
    std::string number_error;
    if (!ParseDecimalI64(options.Get("--not-before"), &certificate.not_before,
                         &number_error)) {
      return Fail("--not-before 不合法：" + number_error);
    }
  }
  std::int64_t days = 180;
  if (options.Has("--days")) {
    std::string number_error;
    if (!ParseDecimalI64(options.Get("--days"), &days, &number_error)) {
      return Fail("--days 不合法：" + number_error);
    }
  }
  if (days <= 0 || days > 3650) {
    return Fail("--days 必须在 1..3650 之间");
  }
  certificate.not_after = certificate.not_before + days * 24 * 60 * 60;
  certificate.serial_number =
      static_cast<std::uint64_t>(certificate.not_before);
  if (options.Has("--serial")) {
    std::string number_error;
    if (!ParseDecimalU64(options.Get("--serial"), &certificate.serial_number,
                         &number_error)) {
      return Fail("--serial 不合法：" + number_error);
    }
  }
  if (certificate.serial_number == 0) {
    return Fail("序列号不能为 0（--serial 显式给了 0？）");
  }
  std::string raw;
  if (!backupproject::crypto::Bpcert1Issue(certificate, key.seed, &raw,
                                           &error)) {
    return Fail(error);
  }
  if (!WriteFile(out_path, 0644, raw, false, &error)) {
    return Fail(error);
  }
  std::printf("server_id          = %s\n", certificate.server_id.c_str());
  std::printf("issuer_id          = %s\n", certificate.issuer_id.c_str());
  std::printf("serial_number      = %llu\n",
              static_cast<unsigned long long>(certificate.serial_number));
  std::printf("not_before         = %lld\n",
              static_cast<long long>(certificate.not_before));
  std::printf("not_after          = %lld\n",
              static_cast<long long>(certificate.not_after));
  std::printf("certificate_file   = %s (%zu 字节)\n", out_path.c_str(),
              raw.size());
  std::printf("certificate_sha256 = %s\n",
              backupproject::crypto::Bpcert1Fingerprint(raw).c_str());
  std::printf(
      "server_public_key  = %s\n",
      backupproject::crypto::X25519FormatKeyHex(certificate.server_public_key)
          .c_str());
  std::printf("content_printed    = NO\n");
  std::memset(&key.seed[0], 0, key.seed.size());
  return 0;
}

int CommandVerifyServer(const Options& options) {
  const std::string cert_path = options.Get("--cert");
  if (cert_path.empty()) {
    return Fail("verify-server 需要 --cert <path>");
  }
  std::string raw;
  std::string error;
  if (!ReadFile(cert_path, &raw, &error)) {
    return Fail(error);
  }
  Bpcert1 certificate;
  const Bpcert1Error parsed =
      backupproject::crypto::Bpcert1Parse(raw, &certificate);
  if (parsed != Bpcert1Error::kOk) {
    std::printf("parse_result       = %s\n", Bpcert1ErrorName(parsed));
    return Fail(std::string("证书结构不合法：") + Bpcert1ErrorName(parsed));
  }
  Bpcert1Error trust = Bpcert1Error::kOk;
  std::string matched_root;
  std::string message;
  if (options.Has("--issuer-pub")) {
    std::string issuer_key;
    std::string parse_error;
    if (!backupproject::crypto::Ed25519ParsePublicKeyText(
            options.Get("--issuer-pub"), &issuer_key, &parse_error)) {
      return Fail("--issuer-pub 不合法：" + parse_error);
    }
    trust = backupproject::crypto::Bpcert1VerifySignature(raw, issuer_key);
    matched_root = "(命令行直接给出的根公钥)";
    message = backupproject::crypto::Bpcert1ErrorMessage(trust);
  } else {
    TrustedRootStore store;
    if (options.Has("--roots")) {
      if (!TrustedRootStore::LoadFromFile(options.Get("--roots"), &store,
                                          &error)) {
        return Fail(error);
      }
    } else {
      store = TrustedRootStore::OfficialCloudStore();
    }
    trust = store.VerifyCertificate(raw, &matched_root, &message);
  }
  std::printf("parse_result       = ok\n");
  std::printf("trust_result       = %s\n", Bpcert1ErrorName(trust));
  std::printf("matched_root       = %s\n",
              matched_root.empty() ? "(none)" : matched_root.c_str());
  std::printf("message            = %s\n", message.c_str());
  std::printf("server_id          = %s\n", certificate.server_id.c_str());
  std::printf("issuer_id          = %s\n", certificate.issuer_id.c_str());
  std::printf("certificate_sha256 = %s\n",
              backupproject::crypto::Bpcert1Fingerprint(raw).c_str());
  std::int64_t now = backupproject::crypto::Bpcert1NowUnixSeconds();
  if (options.Has("--now")) {
    std::string number_error;
    if (!ParseDecimalI64(options.Get("--now"), &now, &number_error)) {
      return Fail("--now 不合法：" + number_error);
    }
  }
  Bpcert1Error window = Bpcert1Error::kOk;
  std::string window_message;
  const bool in_window = backupproject::crypto::Bpcert1CheckValidity(
      certificate, now, &window, &window_message);
  std::printf("validity_result    = %s\n",
              in_window ? "ok" : Bpcert1ErrorName(window));
  std::printf("validity_message   = %s\n", window_message.c_str());
  if (trust != Bpcert1Error::kOk) {
    return Fail(std::string("证书不可信：") + Bpcert1ErrorName(trust));
  }
  if (!in_window) {
    return Fail(std::string("证书时间窗不通过：") + Bpcert1ErrorName(window));
  }
  std::printf("verdict            = TRUSTED\n");
  return 0;
}

int CommandInspectServer(const Options& options) {
  const std::string cert_path = options.Get("--cert");
  if (cert_path.empty()) {
    return Fail("inspect-server 需要 --cert <path>");
  }
  std::string raw;
  std::string error;
  if (!ReadFile(cert_path, &raw, &error)) {
    return Fail(error);
  }
  Bpcert1 certificate;
  const Bpcert1Error parsed =
      backupproject::crypto::Bpcert1Parse(raw, &certificate);
  if (parsed != Bpcert1Error::kOk) {
    std::printf("parse_result       = %s\n", Bpcert1ErrorName(parsed));
    return Fail(std::string("证书结构不合法：") + Bpcert1ErrorName(parsed));
  }
  std::printf("format             = BPCERT1\n");
  std::printf("total_size         = %zu\n", raw.size());
  std::printf("body_size          = %zu\n",
              backupproject::crypto::Bpcert1Body(raw).size());
  std::printf("server_id          = %s\n", certificate.server_id.c_str());
  std::printf("issuer_id          = %s\n", certificate.issuer_id.c_str());
  std::printf("serial_number      = %llu\n",
              static_cast<unsigned long long>(certificate.serial_number));
  std::printf("not_before         = %lld\n",
              static_cast<long long>(certificate.not_before));
  std::printf("not_after          = %lld\n",
              static_cast<long long>(certificate.not_after));
  std::printf(
      "validity_days      = %.2f\n",
      static_cast<double>(certificate.not_after - certificate.not_before) /
          (24.0 * 60 * 60));
  std::printf(
      "server_public_key  = %s\n",
      backupproject::crypto::X25519FormatKeyHex(certificate.server_public_key)
          .c_str());
  std::printf("certificate_sha256 = %s\n",
              backupproject::crypto::Bpcert1Fingerprint(raw).c_str());
  std::printf("summary            = %s\n",
              backupproject::crypto::Bpcert1Describe(certificate).c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }
  Options options;
  options.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string name = argv[i];
    if (name.size() < 3 || name.compare(0, 2, "--") != 0) {
      std::fprintf(stderr, "错误：无法识别的参数 \"%s\"\n",
                   RedactArgument(name).c_str());
      return 2;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "错误：参数 %s 缺少取值\n",
                   RedactArgument(name).c_str());
      return 2;
    }
    options.values.emplace_back(name, argv[++i]);
  }
  // 明确拒绝任何"让私钥走命令行"的写法，包括将来可能有人加的别名。
  for (const auto& item : options.values) {
    if (item.first.find("root-key-hex") != std::string::npos ||
        item.first == "--seed" || item.first == "--seed-hex") {
      std::fprintf(stderr,
                   "错误：不接受把根私钥放在命令行上的参数（%s）。"
                   "请用 --root-key <文件路径>。\n",
                   item.first.c_str());
      return 2;
    }
  }

  if (options.command == "root-init") return CommandRootInit(options);
  if (options.command == "root-info") return CommandRootInfo(options);
  if (options.command == "issue-server") return CommandIssueServer(options);
  if (options.command == "verify-server") return CommandVerifyServer(options);
  if (options.command == "inspect-server") return CommandInspectServer(options);
  if (options.command == "--help" || options.command == "-h" ||
      options.command == "help") {
    PrintUsage();
    return 0;
  }
  std::fprintf(stderr, "错误：未知子命令 \"%s\"\n",
               RedactArgument(options.command).c_str());
  PrintUsage();
  return 2;
}

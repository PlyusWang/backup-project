// tools/cert_tool_main.cpp
//
// backup-cert-tool —— 离线根密钥与服务器身份证书的管理工具（PR #23）。
//
//   backup-cert-tool root-init     --root-key <path> --root-id <id>
//   backup-cert-tool root-info     --root-key <path>
//   backup-cert-tool issue-server  --root-key <path> --server-id <id>
//                                  --server-pubkey <x25519:hex|@file> --out <path>
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

void PrintUsage() {
  std::printf(
      "backup-cert-tool —— 离线根与服务器身份证书管理（PR #23）\n"
      "\n"
      "  root-init      --root-key <path> --root-id <id>\n"
      "                 生成一把新的离线根：私钥写入 <path>（0600，已存在则拒绝），\n"
      "                 公钥写到 <path>.pub（可直接当可信根文件用）。\n"
      "  root-info      --root-key <path>\n"
      "                 读回根的信息（只输出公钥与指纹）。\n"
      "  issue-server   --root-key <path> --server-id <id>\n"
      "                 --server-pubkey <x25519:hex|@file> --out <path>\n"
      "                 [--serial N] [--days N] [--not-before N]\n"
      "                 为已有服务器公钥签发身份证书（不改动任何私钥）。\n"
      "  verify-server  --cert <path> [--roots <file>] [--issuer-pub <hex>]\n"
      "                 [--now N]\n"
      "                 按可信根验签并检查时间窗；不给 --roots 就用内置官方根。\n"
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

// 独占创建 + 精确权限。exclusive=false 时允许覆盖（用于公钥/证书这类公开文件）。
bool WriteFile(const std::string& path, mode_t mode, const std::string& data,
               bool exclusive, std::string* error_message) {
  const int flags = O_WRONLY | O_CREAT | O_NOFOLLOW |
                    (exclusive ? O_EXCL : O_TRUNC);
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
        position, newline == std::string::npos ? std::string::npos
                                               : newline - position));
    if (line.compare(0, 8, "root-id:") == 0) {
      key.root_id = Trim(line.substr(8));
    } else if (line.compare(0, 9, "seed-hex:") == 0) {
      const std::string hex = Trim(line.substr(9));
      if (!backupproject::crypto::FromHex(hex, &key.seed)) {
        if (error_message != nullptr) *error_message = "根文件里的 seed-hex 不是合法十六进制";
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
  if (!backupproject::crypto::Ed25519PublicKeyFromSeed(key.seed, &key.public_key,
                                                       error_message)) {
    return false;
  }
  *out = key;
  return true;
}

bool ParseServerPublicKey(const std::string& text, std::string* out,
                          std::string* error_message) {
  std::string material = text;
  if (!material.empty() && material[0] == '@') {
    if (!ReadFile(material.substr(1), &material, error_message)) {
      return false;
    }
    material = Trim(material);
  }
  std::string parse_error;
  if (backupproject::crypto::X25519ParseKeyText(material, out, &parse_error)) {
    return true;
  }
  // 也接受 32 字节原始二进制文件。
  if (material.size() == backupproject::crypto::kBpcert1PublicKeySize) {
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
  std::string key_text = "# backup-project 离线根私钥 —— 绝不可提交 / 上传 / 打印\n";
  key_text += "root-id: " + root_id + "\n";
  key_text += "seed-hex: " + backupproject::crypto::ToHex(
                                  reinterpret_cast<const unsigned char*>(
                                      seed.data()),
                                  seed.size()) +
              "\n";
  if (!WriteFile(path, 0600, key_text, true, &error)) {
    return Fail(error);
  }
  const std::string pub_text =
      root_id + " " + backupproject::crypto::Ed25519FormatPublicKeyHex(public_key) + "\n";
  if (!WriteFile(path + ".pub", 0644, pub_text, false, &error)) {
    return Fail(error);
  }
  std::printf("root_id            = %s\n", root_id.c_str());
  std::printf("root_key_file      = %s (mode %s)\n", path.c_str(),
              ModeOf(path).c_str());
  std::printf("root_public_file   = %s.pub\n", path.c_str());
  std::printf("root_public_key    = %s\n",
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
  std::printf("root_public_key    = %s\n",
              backupproject::crypto::Ed25519FormatPublicKeyHex(key.public_key).c_str());
  std::printf("root_fingerprint   = %s\n",
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
  certificate.not_before = options.Has("--not-before")
                               ? std::strtoll(options.Get("--not-before").c_str(),
                                              nullptr, 10)
                               : now;
  const std::int64_t days = options.Has("--days")
                                ? std::strtoll(options.Get("--days").c_str(),
                                               nullptr, 10)
                                : 180;
  if (days <= 0 || days > 3650) {
    return Fail("--days 必须在 1..3650 之间");
  }
  certificate.not_after = certificate.not_before + days * 24 * 60 * 60;
  certificate.serial_number =
      options.Has("--serial")
          ? std::strtoull(options.Get("--serial").c_str(), nullptr, 10)
          : static_cast<std::uint64_t>(certificate.not_before);
  if (certificate.serial_number == 0) {
    return Fail("序列号不能为 0（--serial 显式给了 0？）");
  }
  std::string raw;
  if (!backupproject::crypto::Bpcert1Issue(certificate, key.seed, &raw, &error)) {
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
  std::printf("server_public_key  = %s\n",
              backupproject::crypto::X25519FormatKeyHex(
                  certificate.server_public_key)
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
  const Bpcert1Error parsed = backupproject::crypto::Bpcert1Parse(raw, &certificate);
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
  const std::int64_t now = options.Has("--now")
                               ? std::strtoll(options.Get("--now").c_str(),
                                              nullptr, 10)
                               : backupproject::crypto::Bpcert1NowUnixSeconds();
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
  const Bpcert1Error parsed = backupproject::crypto::Bpcert1Parse(raw, &certificate);
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
  std::printf("validity_days      = %.2f\n",
              static_cast<double>(certificate.not_after - certificate.not_before) /
                  (24.0 * 60 * 60));
  std::printf("server_public_key  = %s\n",
              backupproject::crypto::X25519FormatKeyHex(
                  certificate.server_public_key)
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
      std::fprintf(stderr, "错误：无法识别的参数 \"%s\"\n", name.c_str());
      return 2;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "错误：参数 %s 缺少取值\n", name.c_str());
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
  std::fprintf(stderr, "错误：未知子命令 \"%s\"\n", options.command.c_str());
  PrintUsage();
  return 2;
}

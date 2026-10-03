// server/keygen_main.cpp
//
// backup-server-keygen：生成 / 查看 BPSEC1 服务端长期身份密钥。
//
//   backup-server-keygen --output <文件>            生成新的身份私钥
//   backup-server-keygen --show --key-file <文件>   打印公钥与指纹
//
// 私钥文件是 32 字节原始 X25519 标量（已 clamp），权限 0600、O_NOFOLLOW、
// 必须是普通文件；已存在时**拒绝覆盖**（不会悄悄换掉一个正在服役的身份）。
// 这个工具**只打印公钥与指纹**，永远不打印私钥内容。
//
// 客户端拿到的 pin 就是这里打印的 "sha256:<指纹>" 或 "hex:<公钥>"。

#include <cstdio>
#include <string>

#include "crypto.h"
#include "secure_transport.h"

namespace {

void PrintUsage(std::FILE* out, const char* program) {
  std::fprintf(
      out,
      "用法:\n"
      "  %s --output <文件>              生成新的传输身份私钥（0600，不覆盖已有文件）\n"
      "  %s --show --key-file <文件>     打印已有私钥对应的公钥与指纹\n"
      "\n"
      "私钥只写在 --output 指定的文件里，不打印、不进日志、不进 Git。\n"
      "公钥不是秘密：客户端用 \"sha256:<指纹>\" 或 \"hex:<公钥>\" 作为\n"
      "--server-key 的值（见 docs/secure_transport.md）。\n"
      "\n"
      "退出码: 0 成功 / 1 运行失败 / 2 用法错误\n",
      program, program);
}

void PrintKeyMaterial(const backupproject::net::TransportIdentity& identity) {
  const std::string hex =
      backupproject::crypto::X25519FormatKeyHex(identity.public_key);
  const std::string fingerprint =
      backupproject::crypto::X25519Fingerprint(identity.public_key);
  std::printf("传输身份公钥 (hex): %s\n", hex.c_str());
  std::printf("指纹 (SHA-256):     %s\n", fingerprint.c_str());
  std::printf("\n客户端 pin（二选一）:\n");
  std::printf("  --server-key sha256:%s\n", fingerprint.c_str());
  std::printf("  --server-key hex:%s\n", hex.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  const char* program = argc > 0 ? argv[0] : "backup-server-keygen";
  std::string output_path;
  std::string key_file;
  bool show = false;

  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--help" || name == "-h") {
      PrintUsage(stdout, program);
      return 0;
    }
    if (name == "--show") {
      show = true;
      continue;
    }
    if (index + 1 >= argc) {
      std::fprintf(stderr, "Error: %s 需要一个值。\n\n", name.c_str());
      PrintUsage(stderr, program);
      return 2;
    }
    const std::string value = argv[++index];
    if (name == "--output") {
      output_path = value;
    } else if (name == "--key-file") {
      key_file = value;
    } else {
      std::fprintf(stderr, "Error: 未知选项 '%s'。\n\n", name.c_str());
      PrintUsage(stderr, program);
      return 2;
    }
  }

  std::string error;
  if (show) {
    if (key_file.empty()) {
      std::fprintf(stderr, "Error: --show 需要 --key-file <文件>。\n\n");
      PrintUsage(stderr, program);
      return 2;
    }
    if (!output_path.empty()) {
      std::fprintf(stderr, "Error: --show 不能和 --output 一起用。\n\n");
      PrintUsage(stderr, program);
      return 2;
    }
    backupproject::net::TransportIdentity identity;
    if (!backupproject::net::LoadTransportIdentity(key_file, &identity, &error)) {
      std::fprintf(stderr, "Error: %s\n", error.c_str());
      return 1;
    }
    std::printf("身份私钥文件: %s\n", key_file.c_str());
    PrintKeyMaterial(identity);
    return 0;
  }

  if (output_path.empty()) {
    std::fprintf(stderr, "Error: 需要 --output <文件>（或改用 --show）。\n\n");
    PrintUsage(stderr, program);
    return 2;
  }
  if (key_file.empty() == false) {
    std::fprintf(stderr, "Error: --key-file 只能和 --show 一起用。\n\n");
    PrintUsage(stderr, program);
    return 2;
  }

  backupproject::net::TransportIdentity identity;
  if (!backupproject::net::GenerateTransportIdentity(&identity, &error)) {
    std::fprintf(stderr, "Error: 生成身份密钥失败: %s\n", error.c_str());
    return 1;
  }
  if (!backupproject::net::SaveTransportIdentity(output_path, identity,
                                                 /*overwrite=*/false, &error)) {
    std::fprintf(stderr, "Error: %s\n", error.c_str());
    return 1;
  }
  std::printf("已生成传输身份私钥: %s（权限 0600，请勿复制到别处）\n",
              output_path.c_str());
  PrintKeyMaterial(identity);
  return 0;
}

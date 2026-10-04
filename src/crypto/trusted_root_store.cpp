// src/crypto/trusted_root_store.cpp
//
// 信任决策的实现。原则：**默认拒绝**，每一条通过的理由都要写清楚。

#include "trusted_root_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "ed25519.h"

namespace backupproject {
namespace crypto {
namespace {

// 官方云端根（Root-A）。这里只放**公钥**：私钥离线保存，从不进仓库、
// 不进 ECS、不进制品、不进日志。resources/security/official-root-ed25519.pub
// 是同一把根的文本形式，测试会断言两者一致（防止"改了文件忘了改代码"）。
constexpr char kOfficialCloudRootPublicKeyHex[] =
    "ed25519:0530792388326c3db8e0b60df197a94a3a549c888efdd0e6b254eac43aa66df9";

void SetError(std::string* error_message, const char* text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 严格解析 Unix 秒。strtoll 不带 endptr 检查会把 "not-a-number" 变成 0，
// 而 0 在这份格式里是"不限期"—— 一个笔误就把"根 2033 年过期"变成"根永久
// 有效"，这是实打实的 fail-open。所以：整串必须都是数字，否则报错。
bool ParseSecondsStrict(const std::string& text, std::int64_t* out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<std::int64_t>(value);
  return true;
}

}  // namespace

const char* TrustedRootStore::OfficialCloudRootId() {
  return "backup-project-official-root-a";
}

bool TrustedRootStore::AddRoot(const TrustedRoot& root,
                               std::string* error_message) {
  if (!Bpcert1IsValidIdentity(root.root_id)) {
    SetError(error_message, "根标识必须是 1..128 字节的可打印 ASCII");
    return false;
  }
  if (root.public_key.size() != kEd25519PublicKeySize) {
    SetError(error_message, "根公钥必须是 32 字节");
    return false;
  }
  if (root.not_before != 0 && root.not_after != 0 &&
      root.not_after <= root.not_before) {
    SetError(error_message, "根的有效期窗口不合法");
    return false;
  }
  if (FindRoot(root.root_id) != nullptr) {
    SetError(error_message, "根标识重复");
    return false;
  }
  roots_.push_back(root);
  return true;
}

const TrustedRoot* TrustedRootStore::FindRoot(
    const std::string& root_id) const {
  for (const TrustedRoot& root : roots_) {
    if (root.root_id == root_id) {
      return &root;
    }
  }
  return nullptr;
}

Bpcert1Error TrustedRootStore::VerifyCertificate(
    const std::string& raw_certificate, std::string* matched_root_id,
    std::string* message) const {
  if (matched_root_id != nullptr) {
    matched_root_id->clear();
  }
  // 空存储什么都不信 —— 这是最重要的一条默认。
  if (roots_.empty()) {
    if (message != nullptr) {
      *message = "本机没有配置任何可信根，拒绝一切服务器身份";
    }
    return Bpcert1Error::kUntrustedIssuer;
  }
  Bpcert1 certificate;
  const Bpcert1Error parsed = Bpcert1Parse(raw_certificate, &certificate);
  if (parsed != Bpcert1Error::kOk) {
    if (message != nullptr) {
      *message = Bpcert1ErrorMessage(parsed);
    }
    return parsed;
  }
  const TrustedRoot* root = FindRoot(certificate.issuer_id);
  if (root == nullptr) {
    if (message != nullptr) {
      *message = "证书的签发者不在本机可信根列表里";
    }
    return Bpcert1Error::kUntrustedIssuer;
  }
  if (matched_root_id != nullptr) {
    *matched_root_id = root->root_id;
  }
  if (root->revoked) {
    if (message != nullptr) {
      *message = "该根已被吊销";
    }
    return Bpcert1Error::kUntrustedIssuer;
  }
  // 根自己的有效期是"签发时刻"就要满足的：根过期之后再签出来的证书不算数。
  if (root->not_before != 0 && certificate.not_before < root->not_before) {
    if (message != nullptr) {
      *message = "证书签发时该根尚未生效";
    }
    return Bpcert1Error::kUntrustedIssuer;
  }
  if (root->not_after != 0 && certificate.not_before > root->not_after) {
    if (message != nullptr) {
      *message = "证书签发时该根已过期";
    }
    return Bpcert1Error::kUntrustedIssuer;
  }
  const std::string body = Bpcert1Body(raw_certificate);
  if (!Ed25519Verify(root->public_key, body.data(), body.size(),
                     certificate.signature, nullptr)) {
    if (message != nullptr) {
      *message = Bpcert1ErrorMessage(Bpcert1Error::kSignatureInvalid);
    }
    return Bpcert1Error::kSignatureInvalid;
  }
  if (message != nullptr) {
    *message = "证书可信（由根 " + root->root_id + " 签发）";
  }
  return Bpcert1Error::kOk;
}

bool TrustedRootStore::LoadFromText(const std::string& text,
                                    TrustedRootStore* out,
                                    std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  TrustedRootStore parsed;
  std::istringstream stream(text);
  std::string line;
  int line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    // 去掉注释与首尾空白。
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    std::istringstream fields(line);
    std::string root_id;
    std::string key_text;
    if (!(fields >> root_id)) {
      continue;  // 空行
    }
    if (!(fields >> key_text)) {
      SetError(error_message, "根文件某一行只有标识、没有公钥");
      return false;
    }
    TrustedRoot root;
    root.root_id = root_id;
    std::string parse_error;
    if (!Ed25519ParsePublicKeyText(key_text, &root.public_key, &parse_error)) {
      if (error_message != nullptr) {
        *error_message = "第 " + std::to_string(line_number) +
                         " 行的根公钥不合法：" + parse_error;
      }
      return false;
    }
    std::string extra;
    std::int64_t numbers[2] = {0, 0};
    int number_count = 0;
    bool saw_unlimited = false;
    while (fields >> extra) {
      if (extra == "revoked") {
        root.revoked = true;
      } else if (extra == "active") {
        root.revoked = false;
      } else if (extra == "unlimited") {
        saw_unlimited = true;
      } else if (number_count < 2) {
        if (!ParseSecondsStrict(extra, &numbers[number_count])) {
          if (error_message != nullptr) {
            *error_message =
                "第 " + std::to_string(line_number) +
                " 行的时间戳不是合法整数（不限期必须显式写 unlimited）";
          }
          return false;
        }
        ++number_count;
      } else {
        SetError(error_message, "根文件的字段太多，格式不认识");
        return false;
      }
    }
    // 有效期必须**显式**表达：要么两个整数，要么写 unlimited。
    // 只写一个数字、或者什么都不写，都不再被当成"不限期"。
    if (saw_unlimited && number_count != 0) {
      SetError(error_message, "根文件里 unlimited 与时间戳不能同时出现");
      return false;
    }
    if (!saw_unlimited && number_count != 2) {
      SetError(error_message,
               "根的有效期要么写成两个整数，要么显式写 unlimited");
      return false;
    }
    root.not_before = numbers[0];
    root.not_after = numbers[1];
    if (!parsed.AddRoot(root, error_message)) {
      return false;
    }
  }
  *out = parsed;
  return true;
}

bool TrustedRootStore::LoadFromFile(const std::string& path,
                                    TrustedRootStore* out,
                                    std::string* error_message) {
  std::ifstream input(path);
  if (!input) {
    if (error_message != nullptr) {
      *error_message = "打不开根文件：" + path;
    }
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return LoadFromText(buffer.str(), out, error_message);
}

TrustedRootStore TrustedRootStore::OfficialCloudStore() {
  TrustedRootStore store;
  TrustedRoot root;
  root.root_id = OfficialCloudRootId();
  std::string error;
  if (std::strlen(kOfficialCloudRootPublicKeyHex) == 0 ||
      !Ed25519ParsePublicKeyText(kOfficialCloudRootPublicKeyHex,
                                 &root.public_key, &error)) {
    // 内置根没配好（例如占位符还没替换）：返回空存储 —— 什么都不信。
    // 这是有意的失败方向：配置出错时"连不上"，而不是"随便连"。
    return store;
  }
  if (!store.AddRoot(root, &error)) {
    TrustedRootStore empty;
    return empty;
  }
  return store;
}

}  // namespace crypto
}  // namespace backupproject

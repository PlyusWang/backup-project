// tests/unit/sha512_test.cpp
//
// SHA-512 的官方向量与边界测试（FIPS 180-4 / RFC 6234）。
//
// 这个模块是 Ed25519 的哈希，所以测三层：
//   1. 官方向量（标准里给出的确定性答案）；
//   2. 流式一致性：任意切分必须与一次性调用逐字节相同；
//   3. 边界长度：正好卡在 128 字节分块与 112 字节长度域边界上的输入。
//
// 外部独立实现（sha512sum）的交叉验证放在 scripts/sha512_test.sh：
// 那一步不是"再跑一遍同一个实现"。
//
// 退出码 0 = 全部通过。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "crypto.h"

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const std::string& label, const std::string& detail = "") {
  if (ok) {
    ++g_passed;
    std::printf("[sha512]   ok   %s\n", label.c_str());
    return;
  }
  ++g_failed;
  if (detail.empty()) {
    std::printf("[sha512]   FAIL %s\n", label.c_str());
  } else {
    std::printf("[sha512]   FAIL %s（%s）\n", label.c_str(), detail.c_str());
  }
}

std::string Hex(const unsigned char* p, std::size_t n) {
  return backupproject::crypto::ToHex(p, n);
}

std::string OneShot(const std::string& data) {
  unsigned char out[backupproject::crypto::kSha512DigestSize];
  backupproject::crypto::Sha512::Digest(data.data(), data.size(), out);
  return Hex(out, sizeof(out));
}

// 以固定块长流式喂入。
std::string Streaming(const std::string& data, std::size_t chunk) {
  backupproject::crypto::Sha512 ctx;
  std::size_t off = 0;
  while (off < data.size()) {
    const std::size_t take =
        (data.size() - off) < chunk ? (data.size() - off) : chunk;
    ctx.Update(data.data() + off, take);
    off += take;
  }
  if (data.empty()) {
    ctx.Update(nullptr, 0);  // 空消息：显式确认 nullptr 安全
  }
  unsigned char out[backupproject::crypto::kSha512DigestSize];
  ctx.Final(out);
  return Hex(out, sizeof(out));
}

const char* const kEmpty =
    "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
    "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e";
const char* const kAbc =
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
// RFC 6234 / FIPS 180-4 的两分块示例（112 字节，正好压到长度域边界）
const char* const kTwoBlock =
    "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
    "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909";
// 一百万个 'a'
const char* const kMillionA =
    "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
    "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b";

const char* const kTwoBlockText =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
    "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";

}  // namespace

int main() {
  using backupproject::crypto::kSha512BlockSize;
  using backupproject::crypto::kSha512DigestSize;

  // ---- 1. 官方向量 ----
  Check(OneShot(std::string()) == kEmpty, "SHA512-V01 空消息官方向量");
  Check(OneShot("abc") == kAbc, "SHA512-V02 \"abc\" 官方向量",
        OneShot("abc").substr(0, 16));
  Check(OneShot(kTwoBlockText) == kTwoBlock,
        "SHA512-V03 112 字节两分块官方向量（压长度域边界）",
        OneShot(kTwoBlockText).substr(0, 16));
  {
    std::string million(1000000, 'a');
    Check(OneShot(million) == kMillionA, "SHA512-V04 一百万个 'a' 官方向量",
          OneShot(million).substr(0, 16));
  }

  // ---- 2. 流式一致性 ----
  {
    const std::size_t chunks[] = {1,   2,   3,   7,   63,  64,  65, 111,
                                  112, 113, 127, 128, 129, 255, 256, 1000};
    const std::string samples[] = {
        std::string(),
        std::string("a"),
        std::string("abc"),
        std::string(kTwoBlockText),
        std::string(300, 'x'),
        std::string(1000, 'y'),
    };
    bool all_ok = true;
    std::string first_bad;
    for (const std::string& sample : samples) {
      const std::string reference = OneShot(sample);
      for (std::size_t chunk : chunks) {
        const std::string streamed = Streaming(sample, chunk);
        if (streamed != reference) {
          all_ok = false;
          if (first_bad.empty()) {
            first_bad = "len=" + std::to_string(sample.size()) +
                        " chunk=" + std::to_string(chunk);
          }
        }
      }
    }
    Check(all_ok, "SHA512-S01 任意切分与一次性调用逐字节一致", first_bad);
  }

  // ---- 3. 边界长度：分块边界与长度域边界两侧 ----
  {
    bool all_ok = true;
    std::string first_bad;
    const std::size_t chunk_list[] = {1, 127, 128, 129};
    for (std::size_t len = 0; len <= 400; ++len) {
      std::string data;
      data.reserve(len);
      for (std::size_t i = 0; i < len; ++i) {
        data.push_back(static_cast<char>(i & 0xff));
      }
      const std::string reference = OneShot(data);
      for (std::size_t chunk : chunk_list) {
        if (Streaming(data, chunk) != reference) {
          all_ok = false;
          if (first_bad.empty()) {
            first_bad = "len=" + std::to_string(len) +
                        " chunk=" + std::to_string(chunk);
          }
        }
      }
    }
    Check(all_ok, "SHA512-B01 长度 0..400 逐字节与流式切分一致", first_bad);
  }

  // ---- 4. 对象复用：Reset 之后应当等价于新对象 ----
  {
    backupproject::crypto::Sha512 ctx;
    const char* junk = "this is discarded by Reset";
    ctx.Update(junk, std::strlen(junk));
    ctx.Reset();
    ctx.Update("abc", 3);
    unsigned char out[kSha512DigestSize];
    ctx.Final(out);
    Check(Hex(out, sizeof(out)) == kAbc, "SHA512-R01 Reset 之后等价于新对象");
  }

  // ---- 5. 常量自检 ----
  Check(kSha512BlockSize == 128 && kSha512DigestSize == 64,
        "SHA512-C01 分块 128 / 摘要 64");

  std::printf("[sha512] passed=%d failed=%d\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}

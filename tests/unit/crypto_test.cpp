// tests/unit/crypto_test.cpp
//
// 手写密码学原语的单元测试：官方向量 + 流式一致性 + 边界 + 负路径。
//
// 向量来源：
//   * SHA-256：FIPS 180-4 示例（"abc"、空串）、一百万个 'a'，外加 55/56/57、
//     63/64/65、119/120、127/128 字节的补位边界值（分组长度的临界点）
//   * HMAC-SHA256：RFC 4231 case 1/2/3/6/7，以及密钥长度 63/64/65 的临界值
//   * PBKDF2-HMAC-SHA256：公开的 password/salt
//   向量（c=1/2/4096、dkLen=32/40/64）
//   * DES：FIPS 46-3 教科书向量（key=133457799BBCDFF1）
//   * AES：FIPS 197 C.1（AES-128）、C.3（AES-256）
//   * CBC：NIST SP 800-38A F.2.1（AES-128-CBC 四块，手工 XOR 链复算）与
//     DES-CBC 两块的链值
//   * CTR：NIST SP 800-38A F.5.5（AES-256-CTR 四块）
//
// 写死向量之前都用 python3 hashlib 与 openssl 3.0.13 独立复算过。任务书里
// 第 12 条（dkLen=64 的后 32 字节）、第 14 条（DES-CBC）和第 17 条（CTR
// 第二块） 给的期望值与本机 openssl/hashlib 的结果不一致：12 条那个值实际属于
// P="passwordPASSWORDpassword"/c=4096/dkLen=40 的向量（测试里已单列），
// 14、17 条则与本机 openssl 复算结果不同。本测试一律采用独立复算得到的值。
//
// 退出码：0 表示全部通过；最后一行是给 scripts/crypto_test.sh 解析的汇总。

#include "crypto.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace bp = backupproject;
namespace crypto = backupproject::crypto;

namespace {

int g_passed = 0;
int g_failed = 0;

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 测试自己解析十六进制，刻意不复用被测的 FromHex：否则 FromHex 一旦写错，
// 所有期望值会跟着一起错，向量就失去意义了。
std::string Bytes(const char* hex) {
  const std::size_t size = std::strlen(hex);
  if (size % 2 != 0) {
    std::printf("测试数据长度不是偶数: %s\n", hex);
    std::exit(2);
  }
  std::string out(size / 2, '\0');
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int high = HexDigit(hex[i * 2]);
    const int low = HexDigit(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      std::printf("测试数据含非法十六进制字符: %s\n", hex);
      std::exit(2);
    }
    out[i] = static_cast<char>((high << 4) | low);
  }
  return out;
}

std::string ToHexString(const unsigned char* data, std::size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0fu]);
  }
  return out;
}

std::string ToHexString(const std::string& raw) {
  return ToHexString(reinterpret_cast<const unsigned char*>(raw.data()),
                     raw.size());
}

// 失败时打印实际值，方便定位。
void Show(const char* label, const std::string& raw) {
  std::printf("       %s(size=%zu)=%s\n", label, raw.size(),
              ToHexString(raw).c_str());
}

void ShowPrefix(const char* label, const std::string& raw) {
  const std::size_t take = raw.size() < 16 ? raw.size() : 16;
  std::printf(
      "       %s(size=%zu, 前 %zu 字节)=%s\n", label, raw.size(), take,
      ToHexString(reinterpret_cast<const unsigned char*>(raw.data()), take)
          .c_str());
}

template <typename F>
void Check(const char* name, F fn) {
  bool ok = false;
  try {
    ok = fn();
  } catch (const std::exception& error) {
    std::printf("       异常: %s\n", error.what());
    ok = false;
  } catch (...) {
    std::printf("       未知异常\n");
    ok = false;
  }
  if (ok) {
    ++g_passed;
    std::printf("[ PASS ] %s\n", name);
  } else {
    ++g_failed;
    std::printf("[ FAIL ] %s\n", name);
  }
}

// ---- 固定测试数据 ----

const char* const kDesKeyHex = "0123456789abcdef";
const char* const kDesIvHex = "1234567890abcdef";
const char* const kAes256KeyHex =
    "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4";
const char* const kAes256CtrIvHex = "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

std::string DeterministicBytes(std::size_t size) {
  // 只用于制造可复现的测试输入，不做任何密码学用途。
  std::string out(size, '\0');
  std::uint32_t state = 0x12345678u;
  for (std::size_t i = 0; i < size; ++i) {
    state = state * 1664525u + 1013904223u;
    out[i] = static_cast<char>(state >> 24);
  }
  return out;
}

}  // namespace

int main() {
  // ---------- SHA-256 ----------
  Check("sha256_fips_abc", [] {
    const char* expected =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::string hex = crypto::Sha256Hex("abc");
    if (hex != expected) {
      Show("Sha256Hex(abc)", hex);
      return false;
    }
    const std::string raw = crypto::Sha256Raw("abc");
    if (raw.size() != crypto::kSha256DigestSize ||
        ToHexString(raw) != expected) {
      Show("Sha256Raw(abc)", raw);
      return false;
    }
    unsigned char digest[crypto::kSha256DigestSize];
    crypto::Sha256::Hash("abc", 3, digest);
    if (ToHexString(digest, sizeof(digest)) != expected) return false;
    crypto::Sha256 hasher;
    hasher.Update("a", 1);
    hasher.Update("bc", 2);
    hasher.Final(digest);
    return ToHexString(digest, sizeof(digest)) == expected;
  });

  Check("sha256_empty_and_null", [] {
    const char* expected =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    if (crypto::Sha256Hex("") != expected) return false;
    unsigned char digest[crypto::kSha256DigestSize];
    crypto::Sha256::Hash(nullptr, 0, digest);
    if (ToHexString(digest, sizeof(digest)) != expected) return false;
    crypto::Sha256 hasher;
    hasher.Update(nullptr, 0);
    hasher.Final(digest);
    return ToHexString(digest, sizeof(digest)) == expected;
  });

  Check("sha256_million_a", [] {
    const std::string million(1000000, 'a');
    const char* expected =
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    if (crypto::Sha256Hex(million) != expected) return false;
    // 同一份数据分块喂进去，结果必须一致。
    crypto::Sha256 hasher;
    for (std::size_t offset = 0; offset < million.size(); offset += 4096) {
      const std::size_t take =
          std::min<std::size_t>(4096, million.size() - offset);
      hasher.Update(million.data() + offset, take);
    }
    unsigned char digest[crypto::kSha256DigestSize];
    hasher.Final(digest);
    return ToHexString(digest, sizeof(digest)) == expected;
  });

  Check("sha256_padding_boundaries", [] {
    // 55/56/57 与 63/64/65 是补位与分组长度的临界点；119/120/127/128
    // 再跨一个块。
    struct Vector {
      std::size_t size;
      const char* hex;
    };
    const Vector vectors[] = {
        {55,
         "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56,
         "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {57,
         "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"},
        {63,
         "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64,
         "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65,
         "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {119,
         "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120,
         "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
        {127,
         "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"},
        {128,
         "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
    };
    for (const Vector& vector : vectors) {
      const std::string data(vector.size, 'a');
      const std::string hex = crypto::Sha256Hex(data);
      if (hex != vector.hex) {
        std::printf("       size=%zu 期望 %s 实际 %s\n", vector.size,
                    vector.hex, hex.c_str());
        return false;
      }
    }
    return true;
  });

  Check("sha256_streaming_matches_oneshot", [] {
    // 0..300 全长度：一次性 vs 三种切分方式逐字节比对。
    for (std::size_t size = 0; size <= 300; ++size) {
      const std::string data = DeterministicBytes(size);
      const std::string expected = crypto::Sha256Raw(data);
      for (std::size_t chunk : {1u, 7u, 64u}) {
        crypto::Sha256 hasher;
        for (std::size_t offset = 0; offset < size; offset += chunk) {
          const std::size_t take = std::min(chunk, size - offset);
          hasher.Update(data.data() + offset, take);
        }
        unsigned char digest[crypto::kSha256DigestSize];
        hasher.Final(digest);
        const std::string got(reinterpret_cast<const char*>(digest),
                              sizeof(digest));
        if (got != expected) {
          std::printf("       size=%zu chunk=%zu 不一致\n", size, chunk);
          return false;
        }
      }
    }
    return true;
  });

  Check("hex_tools", [] {
    std::string parsed;
    if (!crypto::FromHex("00ff10Ab", &parsed)) return false;
    if (ToHexString(parsed) != "00ff10ab") return false;
    if (parsed.size() != 4) return false;
    // 空串合法，失败时不能改动手里的旧值。
    if (!crypto::FromHex("", &parsed)) return false;
    if (!parsed.empty()) return false;
    parsed = "sentinel";
    if (crypto::FromHex("abc", &parsed)) return false;  // 奇数长度
    if (crypto::FromHex("zz", &parsed)) return false;   // 非法字符
    if (crypto::FromHex("0 1", &parsed)) return false;  // 空白不接受
    if (crypto::FromHex("00", nullptr)) return false;
    if (parsed != "sentinel") return false;
    unsigned char raw[] = {0xde, 0xad, 0xbe, 0xef};
    if (crypto::ToHex(raw, sizeof(raw)) != "deadbeef") return false;
    if (!crypto::ToHex(nullptr, 0).empty()) return false;
    return crypto::ToHex(raw, 0).empty();
  });

  // ---------- HMAC-SHA256（RFC 4231）----------
  Check("hmac_rfc4231_case1", [] {
    const std::string key(20, static_cast<char>(0x0b));
    const std::string expected =
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
    if (crypto::HmacSha256Hex(key, "Hi There") != expected) {
      Show("hmac", crypto::HmacSha256Raw(key, "Hi There"));
      return false;
    }
    unsigned char digest[crypto::kSha256DigestSize];
    crypto::HmacSha256::Compute(key.data(), key.size(), "Hi There", 8, digest);
    return ToHexString(digest, sizeof(digest)) == expected;
  });

  Check("hmac_rfc4231_case2", [] {
    const std::string expected =
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
    const std::string got =
        crypto::HmacSha256Hex("Jefe", "what do ya want for nothing?");
    if (got != expected) Show("hmac", got);
    return got == expected;
  });

  Check("hmac_rfc4231_case3", [] {
    const std::string key(20, static_cast<char>(0xaa));
    const std::string data(50, static_cast<char>(0xdd));
    const std::string expected =
        "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe";
    const std::string got = crypto::HmacSha256Hex(key, data);
    if (got != expected) Show("hmac", got);
    return got == expected;
  });

  Check("hmac_rfc4231_case6_long_key", [] {
    // 密钥长于分组长度（64）：必须先哈希压缩。
    const std::string key(131, static_cast<char>(0xaa));
    const std::string data =
        "Test Using Larger Than Block-Size Key - Hash Key First";
    const std::string expected =
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54";
    const std::string got = crypto::HmacSha256Hex(key, data);
    if (got != expected) Show("hmac", got);
    return got == expected;
  });

  Check("hmac_rfc4231_case7_long_key_and_data", [] {
    const std::string key(131, static_cast<char>(0xaa));
    const std::string data =
        "This is a test using a larger than block-size key and a larger than "
        "block-size data. The key needs to be hashed before being used by the "
        "HMAC algorithm.";
    const std::string expected =
        "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2";
    const std::string got = crypto::HmacSha256Hex(key, data);
    if (got != expected) Show("hmac", got);
    return got == expected;
  });

  Check("hmac_key_length_boundaries", [] {
    // 63/64/65：密钥长度正好在"是否先哈希"的分界上。
    struct Vector {
      std::size_t key_size;
      const char* hex;
    };
    const Vector vectors[] = {
        {63,
         "ac9018d7751a26c7e7b4f7c1df7cc79e49ed4091aad2bb21b76c50f0a0fa58b3"},
        {64,
         "012a4dc984d8edf414b67036572b55d7e8fa4bc26604348df12cc9a88be67e99"},
        {65,
         "376c7a34048b2d4e2a6d7bec680400708ba8ae825c810bb291430271257e33ff"},
    };
    for (const Vector& vector : vectors) {
      const std::string key(vector.key_size, 'k');
      const std::string got = crypto::HmacSha256Hex(key, "mmmmmmmmmm");
      if (got != vector.hex) {
        std::printf("       key_size=%zu 期望 %s 实际 %s\n", vector.key_size,
                    vector.hex, got.c_str());
        return false;
      }
    }
    return true;
  });

  Check("hmac_empty_inputs", [] {
    // 空密钥等价于全零填充块，空消息也合法。
    const std::string expected =
        "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad";
    if (crypto::HmacSha256Hex("", "") != expected) return false;
    unsigned char digest[crypto::kSha256DigestSize];
    crypto::HmacSha256::Compute(nullptr, 0, nullptr, 0, digest);
    return ToHexString(digest, sizeof(digest)) == expected;
  });

  Check("hmac_streaming_matches_oneshot", [] {
    // 归档容器的真实用法：头 + 大 payload 分多次 Update。
    std::string error;
    std::string key;
    if (!crypto::RandomBytes(32, &key, &error)) {
      std::printf("       RandomBytes 失败: %s\n", error.c_str());
      return false;
    }
    const std::string payload = DeterministicBytes(70000);
    const std::string expected =
        crypto::HmacSha256Raw(key, std::string("HEADER") + payload);
    crypto::HmacSha256 hmac(key);
    hmac.Update("HEAD", 4);
    hmac.Update("ER", 2);
    for (std::size_t offset = 0; offset < payload.size(); offset += 7) {
      const std::size_t take =
          std::min<std::size_t>(7, payload.size() - offset);
      hmac.Update(payload.data() + offset, take);
    }
    unsigned char digest[crypto::kSha256DigestSize];
    hmac.Final(digest);
    const std::string got(reinterpret_cast<const char*>(digest),
                          sizeof(digest));
    if (got != expected) {
      ShowPrefix("分块 HMAC", got);
      ShowPrefix("一次 HMAC", expected);
      return false;
    }
    return true;
  });

  // ---------- PBKDF2-HMAC-SHA256 ----------
  Check("pbkdf2_c1_dklen32", [] {
    const std::string expected =
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b";
    std::string out;
    std::string error;
    if (!crypto::Pbkdf2HmacSha256("password", "salt", 1, 32, &out, &error)) {
      std::printf("       %s\n", error.c_str());
      return false;
    }
    if (ToHexString(out) != expected) Show("pbkdf2", out);
    return ToHexString(out) == expected && error.empty();
  });

  Check("pbkdf2_c2_dklen32", [] {
    const std::string expected =
        "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43";
    std::string out;
    if (!crypto::Pbkdf2HmacSha256("password", "salt", 2, 32, &out, nullptr)) {
      return false;
    }
    if (ToHexString(out) != expected) Show("pbkdf2", out);
    return ToHexString(out) == expected;
  });

  Check("pbkdf2_c4096_dklen32", [] {
    const std::string expected =
        "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a";
    std::string out;
    if (!crypto::Pbkdf2HmacSha256("password", "salt", 4096, 32, &out,
                                  nullptr)) {
      return false;
    }
    if (ToHexString(out) != expected) Show("pbkdf2", out);
    return ToHexString(out) == expected;
  });

  Check("pbkdf2_dklen64_and_partial_blocks", [] {
    // dkLen=64：两块拼接；任务书第 12 条给的后 32 字节有误，这里是真实值。
    const std::string expected64 =
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"
        "4dbf3a2f3dad3377264bb7b8e8330d4efc7451418617dabef683735361cdc18c";
    std::string out;
    if (!crypto::Pbkdf2HmacSha256("password", "salt", 1, 64, &out, nullptr)) {
      return false;
    }
    if (ToHexString(out) != expected64) {
      Show("pbkdf2 dkLen=64", out);
      return false;
    }
    // dkLen=40：最后一个块只取前 8 字节，验证截断逻辑。
    const std::string expected40 =
        "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1"
        "c635518c7dac47e9";
    if (!crypto::Pbkdf2HmacSha256("passwordPASSWORDpassword",
                                  "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096,
                                  40, &out, nullptr)) {
      return false;
    }
    if (ToHexString(out) != expected40) {
      Show("pbkdf2 dkLen=40", out);
      return false;
    }
    // 长度必须精确等于请求值。
    if (!crypto::Pbkdf2HmacSha256("password", "salt", 3, 48, &out, nullptr)) {
      return false;
    }
    return out.size() == 48;
  });

  Check("pbkdf2_negative_paths", [] {
    std::string out = "旧值";
    std::string error;
    if (crypto::Pbkdf2HmacSha256("p", "s", 0, 32, &out, &error)) return false;
    if (error.empty()) return false;
    if (!out.empty()) return false;
    if (crypto::Pbkdf2HmacSha256("p", "s", 1, 0, &out, &error)) return false;
    if (error.empty()) return false;
    if (crypto::Pbkdf2HmacSha256("p", "s", 1, 32, nullptr, &error))
      return false;
    if (error.empty()) return false;
    // error_message 允许为 nullptr。
    return crypto::Pbkdf2HmacSha256("p", "s", 1, 32, &out, nullptr);
  });

  // ---------- 定长比较 ----------
  Check("constant_time_equals", [] {
    const std::string a = "0123456789abcdef";
    const std::string same = a;
    const std::string diff = "0123456789abcdeF";
    if (!crypto::ConstantTimeEquals(a.data(), same.data(), a.size()))
      return false;
    if (crypto::ConstantTimeEquals(a.data(), diff.data(), a.size()))
      return false;
    if (!crypto::ConstantTimeEquals(a.data(), diff.data(), 15)) return false;
    if (!crypto::ConstantTimeEquals(nullptr, nullptr, 0)) return false;
    if (crypto::ConstantTimeEquals(nullptr, same.data(), 4)) return false;
    if (crypto::ConstantTimeEquals(same.data(), nullptr, 4)) return false;
    if (!crypto::ConstantTimeEquals(static_cast<const void*>(""), "",
                                    static_cast<std::size_t>(0))) {
      return false;
    }
    if (!crypto::ConstantTimeEquals(a, same)) return false;
    if (crypto::ConstantTimeEquals(a, diff)) return false;
    return !crypto::ConstantTimeEquals(a, a.substr(0, 15));
  });

  // ---------- OS 随机数 ----------
  Check("random_bytes_os", [] {
    std::string error;
    std::string first;
    std::string second;
    if (!crypto::RandomBytes(16, &first, &error)) {
      std::printf("       %s\n", error.c_str());
      return false;
    }
    if (first.size() != 16) return false;
    if (!crypto::RandomBytes(16, &second, &error)) return false;
    if (second.size() != 16) return false;
    if (first == second) return false;  // 16 字节重复的概率是 2^-128
    std::string big;
    if (!crypto::RandomBytes(300000, &big, &error)) return false;
    if (big.size() != 300000) return false;
    bool all_same = true;
    for (std::size_t i = 1; i < big.size(); ++i) {
      if (big[i] != big[0]) {
        all_same = false;
        break;
      }
    }
    if (all_same) return false;
    std::string empty = "x";
    if (!crypto::RandomBytes(0, &empty, &error)) return false;
    if (!empty.empty()) return false;
    return crypto::RandomBytes(8, nullptr, &error) == false && !error.empty();
  });

  Check("random_bytes_urandom_fallback", [] {
    // 直接验证回退实现本身：getrandom 不可用的环境里它是唯一的熵来源。
    std::string error;
    std::string first;
    std::string second;
    if (!crypto::RandomBytesFromUrandom(16, &first, &error)) {
      std::printf("       %s\n", error.c_str());
      return false;
    }
    if (first.size() != 16) return false;
    if (!crypto::RandomBytesFromUrandom(16, &second, &error)) return false;
    if (first == second) return false;
    std::string big;
    if (!crypto::RandomBytesFromUrandom(65536, &big, &error)) return false;
    if (big.size() != 65536) return false;
    std::string empty = "x";
    if (!crypto::RandomBytesFromUrandom(0, &empty, &error)) return false;
    return empty.empty();
  });

  // ---------- DES ----------
  Check("des_fips_single_block", [] {
    const std::string key = Bytes("133457799BBCDFF1");
    const std::string plain = Bytes("0123456789ABCDEF");
    const std::string expected = Bytes("85E813540F0AB405");
    const std::string encrypted = crypto::DesBlockEncrypt(key, plain);
    if (encrypted != expected) {
      Show("DesBlockEncrypt", encrypted);
      return false;
    }
    const std::string decrypted = crypto::DesBlockDecrypt(key, expected);
    if (decrypted != plain) {
      Show("DesBlockDecrypt", decrypted);
      return false;
    }
    // 密钥长度/块长度不合法时返回空串，而不是静默处理。
    if (!crypto::DesBlockEncrypt("short", plain).empty()) return false;
    if (!crypto::DesBlockEncrypt(key, "short").empty()) return false;
    return crypto::DesBlockDecrypt("", "").empty();
  });

  Check("des_cbc_manual_chain", [] {
    // 用 DesBlockEncrypt 手工做 CBC 链，验证 C_i = E(P_i xor C_{i-1})。
    // 期望值由 openssl 3.0.13 复算（openssl enc -des-cbc -nopad）。
    const std::string key = Bytes(kDesKeyHex);
    const std::string iv = Bytes(kDesIvHex);
    const std::string plain = Bytes(
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    const std::string expected = Bytes(
        "7f48a8d3d60494dd359970dca49cb3b182e514d334ddf0798aa88fcb0a12dbb9");
    std::string chained;
    std::string chain = iv;
    for (std::size_t block = 0; block < plain.size(); block += 8) {
      std::string xored(8, '\0');
      for (std::size_t i = 0; i < 8; ++i) {
        xored[i] = static_cast<char>(plain[block + i] ^ chain[i]);
      }
      chain = crypto::DesBlockEncrypt(key, xored);
      chained += chain;
    }
    if (chained != expected) {
      Show("手工 CBC", chained);
      Show("期望", expected);
      return false;
    }
    // 同一份数据走流式 CBC 接口：前 32 字节必须与手工链完全一致（流式接口按
    // PKCS#7 补了一整块，所以总共多 8 字节）。
    std::string out;
    std::string error;
    if (!crypto::DesCbcEncrypt(key, iv, plain, &out, &error)) {
      std::printf("       %s\n", error.c_str());
      return false;
    }
    if (out.size() != expected.size() + 8 ||
        out.compare(0, expected.size(), expected) != 0) {
      Show("DesCbcEncrypt", out);
      return false;
    }
    // 解密要能去掉 padding 回到原文。
    std::string back;
    if (!crypto::DesCbcDecrypt(key, iv, out, &back, &error)) return false;
    if (back != plain) {
      Show("DesCbcDecrypt", back);
      return false;
    }
    return true;
  });

  Check("aes128_cbc_manual_chain_nist_f21", [] {
    // NIST SP 800-38A F.2.1（CBC-AES128.Encrypt，四块）。
    const std::string key = Bytes("2b7e151628aed2a6abf7158809cf4f3c");
    const std::string iv = Bytes("000102030405060708090a0b0c0d0e0f");
    const std::string plain = Bytes(
        "6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef"
        "f69f2445df4f9b17ad2b417be66c3710");
    const std::string expected = Bytes(
        "7649abac8119b246cee98e9b12e9197d"
        "5086cb9b507219ee95db113a917678b2"
        "73bed6b8e3c1743b7116e69e22229516"
        "3ff1caa1681fac09120eca307586e1a7");
    std::string chained;
    std::string chain = iv;
    for (std::size_t block = 0; block < plain.size(); block += 16) {
      std::string xored(16, '\0');
      for (std::size_t i = 0; i < 16; ++i) {
        xored[i] = static_cast<char>(plain[block + i] ^ chain[i]);
      }
      chain = crypto::Aes128BlockEncrypt(key, xored);
      chained += chain;
    }
    if (chained != expected) {
      Show("手工 CBC", chained);
      Show("期望", expected);
      return false;
    }
    return true;
  });

  Check("des_cbc_pkcs7_roundtrip", [] {
    const std::string key = Bytes(kDesKeyHex);
    const std::string iv = Bytes(kDesIvHex);
    const std::size_t sizes[] = {0, 1, 7, 8, 9, 15, 16, 17, 1000};
    for (std::size_t size : sizes) {
      const std::string plain = DeterministicBytes(size);
      std::string cipher;
      std::string error;
      if (!crypto::DesCbcEncrypt(key, iv, plain, &cipher, &error)) {
        std::printf("       加密失败 size=%zu: %s\n", size, error.c_str());
        return false;
      }
      if (cipher.size() % 8 != 0 || cipher.size() <= plain.size()) return false;
      std::string back;
      if (!crypto::DesCbcDecrypt(key, iv, cipher, &back, &error)) {
        std::printf("       解密失败 size=%zu: %s\n", size, error.c_str());
        return false;
      }
      if (back != plain) {
        std::printf("       往返不一致 size=%zu\n", size);
        return false;
      }
    }
    return true;
  });

  Check("des_cbc_streaming_matches_oneshot", [] {
    std::string error;
    std::string data;
    if (!crypto::RandomBytes(100000, &data, &error)) {
      std::printf("       RandomBytes 失败: %s\n", error.c_str());
      return false;
    }
    const std::string key = Bytes(kDesKeyHex);
    const std::string iv = Bytes(kDesIvHex);
    std::string oneshot;
    if (!crypto::DesCbcEncrypt(key, iv, data, &oneshot, &error)) return false;
    for (std::size_t chunk : {1u, 7u, 8u, 13u, 4096u}) {
      crypto::DesCbcEncryptor encryptor(key, iv);
      std::string streamed;
      for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
        const std::size_t take = std::min(chunk, data.size() - offset);
        encryptor.Process(data.data() + offset, take, &streamed);
      }
      if (!encryptor.Finish(&streamed, &error)) return false;
      if (streamed != oneshot) {
        std::printf("       分块 %zu 与一次性结果不同\n", chunk);
        return false;
      }
      crypto::DesCbcDecryptor decryptor(key, iv);
      std::string plain;
      for (std::size_t offset = 0; offset < streamed.size(); offset += chunk) {
        const std::size_t take = std::min(chunk, streamed.size() - offset);
        decryptor.Process(streamed.data() + offset, take, &plain);
      }
      if (!decryptor.Finish(&plain, &error)) return false;
      if (plain != data) {
        std::printf("       分块 %zu 解密不一致\n", chunk);
        return false;
      }
    }
    // 空输入：Process 不产生输出，Finish 补满一块。
    crypto::DesCbcEncryptor empty_encryptor(key, iv);
    std::string empty_out;
    empty_encryptor.Process(nullptr, 0, &empty_out);
    if (!empty_encryptor.Finish(&empty_out, &error)) return false;
    if (empty_out.size() != 8) return false;
    std::string empty_plain;
    return crypto::DesCbcDecrypt(key, iv, empty_out, &empty_plain, &error) &&
           empty_plain.empty();
  });

  Check("des_cbc_decrypt_negative_paths", [] {
    const std::string key = Bytes(kDesKeyHex);
    const std::string iv = Bytes(kDesIvHex);
    std::string out;
    std::string error;
    // 长度不是 8 的倍数。
    if (crypto::DesCbcDecrypt(key, iv, Bytes("00112233445566778899"), &out,
                              &error)) {
      return false;
    }
    if (error.empty()) return false;
    // 空密文。
    if (crypto::DesCbcDecrypt(key, iv, std::string(), &out, &error))
      return false;
    // padding 全 0x00：先手工构造"解密后是全零块"的密文。
    const auto cipher_of_block = [&key, &iv](const std::string& plain_block) {
      std::string xored(8, '\0');
      for (std::size_t i = 0; i < 8; ++i) {
        xored[i] = static_cast<char>(plain_block[i] ^ iv[i]);
      }
      return crypto::DesBlockEncrypt(key, xored);
    };
    out = "未被清空";
    const std::string zero_padding = cipher_of_block(Bytes("0000000000000000"));
    if (crypto::DesCbcDecrypt(key, iv, zero_padding, &out, &error))
      return false;
    if (!out.empty()) return false;  // 失败时不能把未验证的明文交出去
    // padding 值 0x09 超过块长。
    const std::string nine_padding = cipher_of_block(Bytes("0000000000000009"));
    if (crypto::DesCbcDecrypt(key, iv, nine_padding, &out, &error))
      return false;
    // padding 值 0x06 与前面字节不符。
    const std::string mixed_padding =
        cipher_of_block(Bytes("0102030405050506"));
    if (crypto::DesCbcDecrypt(key, iv, mixed_padding, &out, &error))
      return false;
    // 密钥/IV 长度非法时也必须失败。
    if (crypto::DesCbcDecrypt("short", iv, zero_padding, &out, &error))
      return false;
    if (crypto::DesCbcDecrypt(key, "short", zero_padding, &out, &error))
      return false;
    // Finish 只能成功一次。
    crypto::DesCbcEncryptor encryptor(key, iv);
    std::string cipher;
    encryptor.Process("hello", 5, &cipher);
    if (!encryptor.Finish(&cipher, &error)) return false;
    return !encryptor.Finish(&cipher, &error) && !error.empty();
  });

  // ---------- AES ----------
  Check("aes128_block_fips197_c1", [] {
    const std::string key = Bytes("000102030405060708090a0b0c0d0e0f");
    const std::string plain = Bytes("00112233445566778899aabbccddeeff");
    const std::string expected = Bytes("69c4e0d86a7b0430d8cdb78070b4c55a");
    const std::string encrypted = crypto::Aes128BlockEncrypt(key, plain);
    if (encrypted != expected) {
      Show("Aes128BlockEncrypt", encrypted);
      return false;
    }
    const std::string decrypted = crypto::Aes128BlockDecrypt(key, expected);
    if (decrypted != plain) {
      Show("Aes128BlockDecrypt", decrypted);
      return false;
    }
    return true;
  });

  Check("aes256_block_fips197_c3", [] {
    const std::string key = Bytes(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const std::string plain = Bytes("00112233445566778899aabbccddeeff");
    const std::string expected = Bytes("8ea2b7ca516745bfeafc49904b496089");
    const std::string encrypted = crypto::Aes256BlockEncrypt(key, plain);
    if (encrypted != expected) {
      Show("Aes256BlockEncrypt", encrypted);
      return false;
    }
    const std::string decrypted = crypto::Aes256BlockDecrypt(key, expected);
    if (decrypted != plain) {
      Show("Aes256BlockDecrypt", decrypted);
      return false;
    }
    crypto::Aes256 cipher(key);
    if (!cipher.valid()) return false;
    unsigned char in[16];
    unsigned char out[16];
    std::memcpy(in, plain.data(), sizeof(in));
    cipher.EncryptBlock(in, out);
    if (ToHexString(out, sizeof(out)) != ToHexString(expected)) return false;
    cipher.DecryptBlock(out, in);
    return ToHexString(in, sizeof(in)) == ToHexString(plain);
  });

  Check("aes_block_roundtrip_random", [] {
    std::string error;
    std::string key128;
    std::string key256;
    if (!crypto::RandomBytes(16, &key128, &error)) return false;
    if (!crypto::RandomBytes(32, &key256, &error)) return false;
    const std::string block = DeterministicBytes(16);
    std::string encrypted = crypto::Aes128BlockEncrypt(key128, block);
    if (encrypted.size() != 16) return false;
    if (crypto::Aes128BlockDecrypt(key128, encrypted) != block) return false;
    encrypted = crypto::Aes256BlockEncrypt(key256, block);
    if (encrypted.size() != 16) return false;
    return crypto::Aes256BlockDecrypt(key256, encrypted) == block;
  });

  Check("aes256_ctr_nist_f55", [] {
    const std::string key = Bytes(kAes256KeyHex);
    const std::string iv = Bytes(kAes256CtrIvHex);
    const std::string plain = Bytes(
        "6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef"
        "f69f2445df4f9b17ad2b417be66c3710");
    const std::string expected = Bytes(
        "601ec313775789a5b7a7f504bbf3d228"
        "f443e3ca4d62b59aca84e990cacaf5c5"
        "2b0930daa23de94ce87017ba2d84988d"
        "dfc9c58db67aada613c2dd08457941a6");
    const std::string oneshot = crypto::Aes256CtrCrypt(key, iv, plain);
    if (oneshot != expected) {
      Show("Aes256CtrCrypt", oneshot);
      Show("期望", expected);
      return false;
    }
    // 逐块喂进去（块大小 16）结果必须一致。
    crypto::Aes256Ctr ctr(key, iv);
    std::string streamed;
    for (std::size_t offset = 0; offset < plain.size(); offset += 16) {
      ctr.Process(plain.data() + offset, 16, &streamed);
    }
    if (streamed != expected) return false;
    // 手工用单块接口拼计数器：E(ctr_i) xor P_i。
    const std::string counters[] = {
        Bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"),
        Bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdff00"),
        Bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdff01"),
        Bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdff02"),
    };
    std::string manual;
    for (std::size_t block = 0; block < 4; ++block) {
      const std::string keystream =
          crypto::Aes256BlockEncrypt(key, counters[block]);
      for (std::size_t i = 0; i < 16; ++i) {
        manual.push_back(
            static_cast<char>(plain[block * 16 + i] ^ keystream[i]));
      }
    }
    if (manual != expected) {
      Show("手工 CTR", manual);
      return false;
    }
    // 解密就是同一个操作。
    return crypto::Aes256CtrCrypt(key, iv, expected) == plain;
  });

  Check("aes256_ctr_counter_carry", [] {
    // 计数器末两字节 ff ff：第二块必须整体进位成 ...01 00 00（大端整数自增）。
    const std::string key = Bytes(kAes256KeyHex);
    const std::string iv = Bytes("0000000000000000000000000000ffff");
    const std::string next = Bytes("00000000000000000000000000010000");
    const std::string keystream_first = crypto::Aes256BlockEncrypt(key, iv);
    const std::string keystream_second = crypto::Aes256BlockEncrypt(key, next);
    const std::string cipher =
        crypto::Aes256CtrCrypt(key, iv, std::string(32, '\0'));
    if (cipher.size() != 32) return false;
    if (cipher.substr(0, 16) != keystream_first) {
      Show("第一块", cipher.substr(0, 16));
      return false;
    }
    if (cipher.substr(16, 16) != keystream_second) {
      Show("第二块", cipher.substr(16, 16));
      Show("期望", keystream_second);
      return false;
    }
    // 全 ff 计数器再加一必须回绕成全 0（128 位回绕，不能崩）。
    const std::string all_ff = std::string(16, static_cast<char>(0xff));
    const std::string wrapped =
        crypto::Aes256CtrCrypt(key, all_ff, std::string(32, '\0'));
    return wrapped.size() == 32 &&
           wrapped.substr(0, 16) == crypto::Aes256BlockEncrypt(key, all_ff) &&
           wrapped.substr(16, 16) ==
               crypto::Aes256BlockEncrypt(key, std::string(16, '\0'));
  });

  Check("aes256_ctr_roundtrip_lengths", [] {
    const std::string key = Bytes(kAes256KeyHex);
    const std::string iv = Bytes(kAes256CtrIvHex);
    const std::size_t sizes[] = {0, 1, 15, 16, 17, 255, 256, 4096, 100000};
    for (std::size_t size : sizes) {
      const std::string plain = DeterministicBytes(size);
      const std::string cipher = crypto::Aes256CtrCrypt(key, iv, plain);
      if (cipher.size() != plain.size()) return false;
      if (crypto::Aes256CtrCrypt(key, iv, cipher) != plain) {
        std::printf("       往返不一致 size=%zu\n", size);
        return false;
      }
      if (size > 0 && cipher == plain) return false;
    }
    return true;
  });

  Check("aes256_ctr_streaming_matches_oneshot", [] {
    std::string error;
    std::string data;
    if (!crypto::RandomBytes(100000, &data, &error)) {
      std::printf("       RandomBytes 失败: %s\n", error.c_str());
      return false;
    }
    const std::string key = Bytes(kAes256KeyHex);
    const std::string iv = Bytes(kAes256CtrIvHex);
    const std::string oneshot = crypto::Aes256CtrCrypt(key, iv, data);
    for (std::size_t chunk : {1u, 7u, 15u, 16u, 17u, 8192u}) {
      crypto::Aes256Ctr ctr(key, iv);
      std::string streamed;
      for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
        const std::size_t take = std::min(chunk, data.size() - offset);
        ctr.Process(data.data() + offset, take, &streamed);
      }
      if (streamed != oneshot) {
        std::printf("       分块 %zu 与一次性结果不同\n", chunk);
        return false;
      }
      if (crypto::Aes256CtrCrypt(key, iv, streamed) != data) return false;
    }
    // 0 字节输入不产生输出，也不报错。
    crypto::Aes256Ctr ctr(key, iv);
    std::string empty;
    ctr.Process(nullptr, 0, &empty);
    ctr.Process(data.data(), 0, &empty);
    return empty.empty() && ctr.valid();
  });

  Check("aes256_invalid_key_or_iv", [] {
    const std::size_t bad_sizes[] = {0, 1, 15, 31, 33, 64};
    for (std::size_t size : bad_sizes) {
      const std::string key(size, 'k');
      crypto::Aes256 cipher(key);
      if (cipher.valid()) return false;
      unsigned char in[16] = {0};
      unsigned char out[16];
      std::memset(out, 0xAA, sizeof(out));
      cipher.EncryptBlock(in, out);
      cipher.DecryptBlock(in, out);
      for (std::size_t i = 0; i < sizeof(out); ++i) {
        if (out[i] != 0) return false;  // 无效对象只写 0，绝不越界读
      }
      if (!crypto::Aes256BlockEncrypt(key, std::string(16, '\0')).empty()) {
        return false;
      }
      if (!crypto::Aes256CtrCrypt(key, std::string(16, '\0'), "data").empty()) {
        return false;
      }
    }
    // 密钥合法但 IV 非法。
    const std::string key(32, 'k');
    if (!crypto::Aes256CtrCrypt(key, "short", "data").empty()) return false;
    if (!crypto::Aes256CtrCrypt(key, std::string(17, 'i'), "data").empty()) {
      return false;
    }
    // 块长度非法的自由函数。
    if (!crypto::Aes256BlockEncrypt(key, "short").empty()) return false;
    if (!crypto::Aes128BlockEncrypt("short", std::string(16, '\0')).empty()) {
      return false;
    }
    crypto::Aes256 valid(key);
    if (!valid.valid()) return false;
    crypto::Aes256Ctr ctr(key, std::string(16, 'i'));
    return ctr.valid();
  });

  std::printf("crypto-test: %d/%d checks passed\n", g_passed,
              g_passed + g_failed);
  return g_failed == 0 ? 0 : 1;
}

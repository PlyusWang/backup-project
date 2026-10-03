// tests/review/counter_block_harness.cpp
//
// PR #21 独立审查轮（review-only 工具，不参与产品构建）。
//
// 为什么需要单独一个二进制：AES-CTR 的计数器块构造函数 CounterBlock() 与
// 会话密钥派生 DeriveSessionKeys() 都住在 src/network/secure_transport.cpp
// 的**匿名命名空间**里，产品头文件看不到它们。这个 harness 把那个 .cpp
// 直接 include 进同一个翻译单元，在不改产品代码、不复制实现的前提下验证：
//
//   1. 每个 (方向, 序号) 组合对应**唯一**的 16 字节计数器块（10 万个样本
//      两两不同）；
//   2. c2s 与 s2c 的 nonce 前缀不同，因此两个方向的计数器空间不相交
//      （等价说法：两个方向的计数器块集合的交集为空）；
//   3. 计数器块布局就是 nonce_prefix(4) || record_sequence(8, 大端) ||
//      block_counter(4)，且 block_counter 恒为 0（每条记录的计数器空间
//      互不重叠，不会像 "IV = base + seq" 那样让第 N 条记录的后半段和
//      第 N+1 条记录的开头撞在同一个计数器值上）。
//
// 编译时**不要**再单独链接 src/network/secure_transport.cpp（会重复定义）。
// 退出码：0 = 全部通过。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/network/secure_transport.cpp"
#include "crypto.h"
#include "hkdf.h"
#include "network_protocol.h"
#include "secure_transport.h"
#include "x25519.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name, const std::string& detail = "") {
  g_checks += 1;
  if (!ok) {
    g_failures += 1;
    std::printf("  FAIL %s%s\n", name.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
  }
}

// 匿名命名空间里的名字通过所属命名空间的隐式 using-directive 可见。
using namespace backupproject::net;

std::string BigEndianU64(std::uint64_t value) {
  std::string out;
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
  return out;
}

void TestNoncePrefixes() {
  const std::string shared_secret(64, 'S');
  const std::string client_random(32, 'C');
  const std::string server_random(32, 'R');
  std::string error;
  SessionKeys keys;
  Check(DeriveSessionKeys(shared_secret, client_random, server_random, &keys,
                          &error),
        "派生会话密钥", error);
  Check(keys.c2s_nonce.size() == 4 && keys.s2c_nonce.size() == 4,
        "两个方向的 nonce 前缀都是 4 字节",
        std::to_string(keys.c2s_nonce.size()) + "/" +
            std::to_string(keys.s2c_nonce.size()));
  Check(keys.c2s_nonce != keys.s2c_nonce, "c2s 与 s2c 的 nonce 前缀不同");

  SessionKeys again;
  Check(DeriveSessionKeys(shared_secret, client_random, server_random, &again,
                          &error) &&
            again.c2s_nonce == keys.c2s_nonce &&
            again.s2c_nonce == keys.s2c_nonce,
        "同样的输入派生同样的 nonce 前缀（确定性）");

  SessionKeys swapped;
  Check(DeriveSessionKeys(shared_secret, server_random, client_random, &swapped,
                          &error) &&
            swapped.c2s_nonce != keys.c2s_nonce,
        "换 client_random 之后 nonce 前缀也变");

  SessionKeys other_secret;
  Check(DeriveSessionKeys(std::string(64, 'T'), client_random, server_random,
                          &other_secret, &error) &&
            other_secret.c2s_nonce != keys.c2s_nonce,
        "换共享秘密之后 nonce 前缀也变");

  // 10 万个 (方向, 序号) 组合 -> 20 万个计数器块，必须两两不同。
  const std::size_t samples = 100000;
  std::vector<std::string> blocks;
  blocks.reserve(2 * samples + 16);
  for (std::size_t index = 0; index < samples; ++index) {
    blocks.push_back(CounterBlock(keys.c2s_nonce, index));
    blocks.push_back(CounterBlock(keys.s2c_nonce, index));
  }
  // 极端序号也要落在同一个"两两不同"的结论里。
  const std::uint64_t extremes[] = {
      0xFFFFFFFFull,         0x100000000ull,        0x7FFFFFFFFFFFFFFFull,
      0xFFFFFFFFFFFFFFFEull, 0xFFFFFFFFFFFFFFFFull,
  };
  for (const std::uint64_t sequence : extremes) {
    blocks.push_back(CounterBlock(keys.c2s_nonce, sequence));
    blocks.push_back(CounterBlock(keys.s2c_nonce, sequence));
  }

  bool all_blocks_are_16_bytes = true;
  for (const std::string& block : blocks) {
    if (block.size() != backupproject::crypto::kAesBlockSize) {
      all_blocks_are_16_bytes = false;
      break;
    }
  }
  Check(all_blocks_are_16_bytes, "每个计数器块都是 16 字节（一个 AES 分组）");

  std::sort(blocks.begin(), blocks.end());
  std::size_t duplicates = 0;
  for (std::size_t index = 1; index < blocks.size(); ++index) {
    if (blocks[index] == blocks[index - 1]) {
      duplicates += 1;
    }
  }
  Check(duplicates == 0,
        "20 万个计数器块两两不同（c2s/s2c 两个方向的集合不相交）",
        std::to_string(duplicates) + " 组重复");
  std::printf("  COUNTER_BLOCKS: %zu 个样本，%zu 组重复\n", blocks.size(),
              duplicates);

  // 布局：nonce_prefix(4) || seq(8, 大端) || 0(4)
  bool layout_ok = true;
  std::size_t checked = 0;
  for (std::size_t index = 0; index < samples; index += 997) {
    const std::string block = CounterBlock(keys.c2s_nonce, index);
    checked += 1;
    if (block.compare(0, 4, keys.c2s_nonce) != 0) {
      layout_ok = false;
    }
    if (block.compare(4, 8, BigEndianU64(index)) != 0) {
      layout_ok = false;
    }
    if (block.substr(12, 4) != std::string(4, '\0')) {
      layout_ok = false;
    }
  }
  Check(layout_ok,
        "计数器块布局 = nonce_prefix(4) || seq(8 大端) || block_counter(4)=0",
        std::to_string(checked) + " 个采样点");
}

}  // namespace

int main() {
  std::printf("BPSEC1 计数器块性质（review-only）\n");
  TestNoncePrefixes();
  const int passed = g_checks - g_failures;
  std::printf("review-counter-block: %d/%d checks passed\n", passed, g_checks);
  return g_failures == 0 ? 0 : 1;
}

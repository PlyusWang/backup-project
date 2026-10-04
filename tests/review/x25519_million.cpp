// tests/review/x25519_million.cpp
//
// **REVIEW-ONLY 工具。** RFC 7748 §5.2 的 1,000,000 次迭代向量：
//   k = u = 9；每轮 (k, u) = (X25519(k, u), k)。
// 期望值（RFC 7748 正文）：
//   7c3911e0ab2586fd864497297e575e6f3bc601c0883c30df5f4dd2d24f665424
//
// 同时报告真实速度，避免"跑不动就当覆盖过了"。

#include <chrono>
#include <cstdio>
#include <string>

#include "x25519.h"

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::atoi(argv[1]) : 1000000;
  std::string k(32, '\0');
  k[0] = 9;
  std::string u = k;
  std::string error;
  const auto start = std::chrono::steady_clock::now();
  for (int index = 0; index < iterations; ++index) {
    std::string next;
    if (!backupproject::crypto::X25519(k, u, &next, &error)) {
      std::printf("FAIL at iteration %d: %s\n", index, error.c_str());
      return 1;
    }
    u = k;
    k = next;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  static const char* digits = "0123456789abcdef";
  std::string hex;
  for (unsigned char byte : k) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 0x0F]);
  }
  std::printf("iterations=%d seconds=%.2f per_call_ms=%.3f\n", iterations,
              seconds, seconds * 1000.0 / iterations);
  std::printf("result=%s\n", hex.c_str());
  std::printf("expect=7c3911e0ab2586fd864497297e575e6f3bc601c0883c30df5f4dd2d24f665424\n");
  std::printf("%s\n", hex == "7c3911e0ab2586fd864497297e575e6f3bc601c0883c30df5f4dd2d24f665424"
                          ? "MILLION_ITERATION_PASS"
                          : "MILLION_ITERATION_FAIL");
  return hex == "7c3911e0ab2586fd864497297e575e6f3bc601c0883c30df5f4dd2d24f665424" ? 0 : 1;
}

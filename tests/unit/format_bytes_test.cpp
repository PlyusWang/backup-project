// tests/unit/format_bytes_test.cpp
//
// FormatByteSize 的边界测试：把展示规则钉死，避免哪天又冒出第二套实现
// （此前有 7 份，同一份归档在不同页面显示成三种样子）。
//
// 规则来自 include/format_bytes.h：1024 进制、B / KiB / MiB / GiB / TiB、
// 整单位省略 .0、非整数保留一位。

#include <cstdint>
#include <string>
#include <vector>

#include "format_bytes.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

constexpr std::uint64_t kKiB = 1024ull;
constexpr std::uint64_t kMiB = kKiB * 1024ull;
constexpr std::uint64_t kGiB = kMiB * 1024ull;
constexpr std::uint64_t kTiB = kGiB * 1024ull;

struct Case {
  std::uint64_t bytes;
  const char* expected;
};

}  // namespace

int main() {
  test_support::Section("FMT 1. 字节：不换算");
  const std::vector<Case> byte_cases = {
      {0, "0 B"},
      {1, "1 B"},
      {2, "2 B"},
      {512, "512 B"},
      {1023, "1023 B"},
  };
  for (const Case& item : byte_cases) {
    const std::string got = bp::FormatByteSize(item.bytes);
    test_support::Check(got == item.expected,
                        std::string("FMT T1 ") + std::to_string(item.bytes) +
                            " -> " + item.expected,
                        "got " + got);
  }

  test_support::Section("FMT 2. KiB 边界与整单位省略 .0");
  const std::vector<Case> kib_cases = {
      {kKiB, "1 KiB"},
      {kKiB + 1, "1.0 KiB"},
      {1536, "1.5 KiB"},
      {kKiB * 2, "2 KiB"},
      {kKiB * 10, "10 KiB"},
      {kKiB - 1, "1023 B"},
      {kKiB * 1023, "1023 KiB"},
  };
  for (const Case& item : kib_cases) {
    const std::string got = bp::FormatByteSize(item.bytes);
    test_support::Check(got == item.expected,
                        std::string("FMT T2 ") + std::to_string(item.bytes) +
                            " -> " + item.expected,
                        "got " + got);
  }

  test_support::Section("FMT 3. MiB / GiB / TiB 边界");
  const std::vector<Case> big_cases = {
      // 1048575 字节 = 1023.999 KiB，四舍五入到 0.1 之后进位成 1 MiB。
      {kMiB - 1, "1 MiB"},
      {kMiB, "1 MiB"},
      {kMiB + kMiB / 2, "1.5 MiB"},
      {kGiB, "1 GiB"},
      {kGiB + kGiB / 4, "1.3 GiB"},
      {kTiB, "1 TiB"},
      {kTiB + kTiB / 2, "1.5 TiB"},
      {kTiB * 2, "2 TiB"},
  };
  for (const Case& item : big_cases) {
    const std::string got = bp::FormatByteSize(item.bytes);
    test_support::Check(got == item.expected,
                        std::string("FMT T3 ") + std::to_string(item.bytes) +
                            " -> " + item.expected,
                        "got " + got);
  }

  test_support::Section("FMT 4. 四舍五入不进成 1024 单位");
  {
    // 1023.96 KiB 四舍五入到 0.1 会变成 1024.0 —— 必须进位成 1 MiB，
    // 不能输出 "1024.0 KiB"（读者还要自己再换算一次）。
    const std::uint64_t almost_one_mib = kMiB - 4;
    const std::string got = bp::FormatByteSize(almost_one_mib);
    test_support::Check(got == "1 MiB", "FMT T4 1023.996 KiB -> 1 MiB",
                        "got " + got);
    // 同样处理 GiB 边界。
    const std::string got_gib = bp::FormatByteSize(kGiB - 4);
    test_support::Check(got_gib == "1 GiB", "FMT T4 1023.996 MiB -> 1 GiB",
                        "got " + got_gib);
  }

  test_support::Section("FMT 5. 极大值不越界");
  {
    const std::string got = bp::FormatByteSize(UINT64_MAX);
    test_support::Check(!got.empty() && got.find("TiB") != std::string::npos,
                        "FMT T5 UINT64_MAX 落在 TiB 档",
                        "got " + got);
    const std::string got_tib = bp::FormatByteSize(kTiB * 1024ull);
    test_support::Check(got_tib.find("TiB") != std::string::npos,
                        "FMT T5 1024 TiB 仍是 TiB（不造 PiB）",
                        "got " + got_tib);
  }

  return test_support::Finish("format-bytes");
}
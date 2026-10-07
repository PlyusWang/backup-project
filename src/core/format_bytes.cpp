// src/core/format_bytes.cpp
//
// 见 include/format_bytes.h：展示规则与理由都写在那里，这里只有实现。

#include "format_bytes.h"

namespace backupproject {

namespace {

struct Unit {
  const char* name;
  std::uint64_t scale;
};

// 1024 进位的单位表。索引 0 是字节本身，所以它不做除法。
constexpr Unit kUnits[] = {
    {"B", 1ull},
    {"KiB", 1024ull},
    {"MiB", 1024ull * 1024ull},
    {"GiB", 1024ull * 1024ull * 1024ull},
    {"TiB", 1024ull * 1024ull * 1024ull * 1024ull},
};

constexpr std::size_t kLastUnit = sizeof(kUnits) / sizeof(kUnits[0]) - 1;

// 选择一个单位并渲染。whole 用整数除法取，小数部分用"四舍五入到 0.1"
// 现算，不用浮点：64 位边界上浮点会丢精度，而这里的输入可以到 UINT64_MAX。
std::string RenderUnit(std::uint64_t bytes, std::size_t unit) {
  const std::uint64_t scale = kUnits[unit].scale;
  std::uint64_t whole = bytes / scale;
  const std::uint64_t remainder = bytes % scale;
  if (remainder == 0) {
    return std::to_string(whole) + " " + kUnits[unit].name;
  }
  // remainder < scale <= 2^40，所以乘 10 不会溢出。
  const std::uint64_t tenths = (remainder * 10ull + scale / 2ull) / scale;
  if (tenths == 10) {
    // 四舍五入把 1023.x 抬成了整数：进到下一个单位去，免得出现
    // "1024 KiB" 这种读者要自己再换算一次的写法。
    //
    // 判断用除法，不用 whole * scale：后者在 接近 UINT64_MAX
    // 的输入上会溢出（整数乘溢出是未定义行为）。只有“下一个单位正好是
    // 这个值”时才升档，而那时它一定能被整除。最大单位（TiB）
    // 不升档，所以不会读到表外。
    whole += 1;
    if (unit + 1 <= kLastUnit && kUnits[unit + 1].scale % scale == 0 &&
        whole == kUnits[unit + 1].scale / scale) {
      ++unit;
      whole = 1;
    }
    return std::to_string(whole) + " " + kUnits[unit].name;
  }
  return std::to_string(whole) + "." + std::to_string(tenths) + " " +
         kUnits[unit].name;
}

}  // namespace

std::string FormatByteSize(std::uint64_t bytes) {
  if (bytes < kUnits[1].scale) {
    return std::to_string(bytes) + " B";
  }
  // 取最大的、且 bytes >= scale 的单位。不用循环里 break：从大到小找
  // 第一个满足条件的即可，边界（正好等于某个 scale）自然落在正确单位上。
  std::size_t unit = kLastUnit;
  while (unit > 1 && bytes < kUnits[unit].scale) {
    --unit;
  }
  return RenderUnit(bytes, unit);
}

}  // namespace backupproject

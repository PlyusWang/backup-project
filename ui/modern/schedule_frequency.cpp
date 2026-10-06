// schedule_frequency.cpp
//
// 见 schedule_frequency.h 的设计说明。这里只有整数换算与范围校验，
// 不读磁盘、不碰 Qt、也不重复任何一句"这个区间合不合法"的判断——
// 区间来自共享核心的 schedule_store.h。

#include "schedule_frequency.h"

#include "schedule_store.h"

namespace backup_modern {
namespace {

namespace bp = backupproject;

// 顺序即界面顺序。分钟放第一：它是唯一"永远不会被误解"的单位。
const FrequencyUnit kUnits[] = {
    {"minutes", "分钟", 1u},
    {"hours", "小时", 60u},
    {"days", "天", 1440u},
    {"weeks", "周", 10080u},
};
constexpr int kUnitCount = static_cast<int>(sizeof(kUnits) / sizeof(kUnits[0]));

}  // namespace

// 表是唯一真值：下拉框顺序与 LargestExactFrequencyUnit 的搜索方向都来自它。
int FrequencyUnitCount() { return kUnitCount; }

// 越界返回第一个单位而不是崩溃或返回空引用：index 来自 QML，模型变化后可能
// 已经过期，给一个合法值是这里更安全的选择。
const FrequencyUnit& FrequencyUnitAt(int index) {
  if (index < 0 || index >= kUnitCount) return kUnits[0];
  return kUnits[index];
}

// 返回指向静态表的指针（调用方不得释放或修改）。找不到返回 nullptr 而不是
// 回退到分钟 —— unit key 来自配置文件，未知值必须报错而不是猜。
const FrequencyUnit* FindFrequencyUnit(const std::string& key) {
  for (const FrequencyUnit& unit : kUnits) {
    if (key == unit.key) return &unit;
  }
  return nullptr;
}

const FrequencyUnit& LargestExactFrequencyUnit(std::uint32_t interval_minutes) {
  // 从大到小找第一个能整除的单位。10080 -> 周、2880 -> 天、120 -> 小时、
  // 90 -> 分钟（90 不能被 60 / 1440 / 10080 整除，只能老实显示 90 分钟）。
  for (int i = kUnitCount - 1; i >= 0; --i) {
    if (interval_minutes % kUnits[i].minutes == 0) return kUnits[i];
  }
  return kUnits[0];
}

// 三段校验的顺序不能换：先用“分钟上界”卡住数值（任何单位下的合法值都不会
// 超过它），再用除法规避溢出，最后才检查下界。
bool ParseFrequency(const std::string& value_text, const std::string& unit_key,
                    std::uint32_t* interval_minutes,
                    std::string* error_message) {
  if (interval_minutes == nullptr) {
    if (error_message != nullptr) {
      *error_message = "备份频率的输出指针不能为空";
    }
    return false;
  }
  const FrequencyUnit* unit = FindFrequencyUnit(unit_key);
  if (unit == nullptr) {
    if (error_message != nullptr) {
      *error_message = "未知的备份频率单位：" + unit_key;
    }
    return false;
  }

  // 数值本身仍然交给共享核心解析（CLI 的 --interval-minutes 走的是同一个
  // 函数）：纯十进制、无符号、无小数点、超范围立刻失败、边解析边夹。
  // 上界先用"分钟上界"卡住：数值单位最小是分钟，所以任何合法的值都不可能
  // 大于 kMaxIntervalMinutes。
  std::uint32_t value = 0;
  std::string parse_error;
  if (!bp::ParseBoundedScheduleNumber(value_text, 1, bp::kMaxIntervalMinutes,
                                      "备份频率", &value, &parse_error)) {
    if (error_message != nullptr) *error_message = parse_error;
    return false;
  }

  // 乘之前先除：value * unit->minutes 不会溢出，也不需要 128 位中间量。
  // 这一步同时是"超最大值"的唯一判据——比先乘再看结果可靠得多，因为
  // 先乘就已经回绕了。
  // 先除后乘：这一步同时是“超上限”的唯一判据，先乘的话结果已经回绕了。
  if (value > bp::kMaxIntervalMinutes / unit->minutes) {
    if (error_message != nullptr) {
      *error_message = "备份频率超出范围：每 " + value_text + " " +
                       unit->label + "（最多 " +
                       std::to_string(bp::kMaxIntervalMinutes) + " 分钟）";
    }
    return false;
  }

  const std::uint32_t minutes = value * unit->minutes;
  if (minutes < bp::kMinIntervalMinutes) {
    if (error_message != nullptr) {
      *error_message = "备份频率不能小于 " +
                       std::to_string(bp::kMinIntervalMinutes) + " 分钟";
    }
    return false;
  }
  *interval_minutes = minutes;
  return true;
}

void SplitFrequency(std::uint32_t interval_minutes, std::string* value_text,
                    std::string* unit_key) {
  const FrequencyUnit& unit = LargestExactFrequencyUnit(interval_minutes);
  if (value_text != nullptr) {
    *value_text = std::to_string(interval_minutes / unit.minutes);
  }
  if (unit_key != nullptr) *unit_key = unit.key;
}

}  // namespace backup_modern

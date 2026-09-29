// schedule_frequency.h
//
// "备份频率"的显示形态与存储形态之间的换算。
//
// 为什么需要这一层：计划配置里存的、backupctl 认的、共享核心校验的，都只有
// 一个字段 interval_minutes。而"周期 60 分钟"对人是难读的——它既可能是"每 1
// 小时"，也可能是"每 60 分钟"，用户每次都得自己心算。界面上改成"每 [1] [小时]"
// 之后就必须有人负责 值 × 单位 -> 分钟 以及反向的"取最大的整除单位"。
//
// 这个文件就是那个唯一的地方，而且是**纯 C++**（不依赖 Qt，也不依赖 QML）：
//   * 乘法在这里做范围与溢出校验，QML 里不会出现 parseInt(x) * 10080；
//   * 数值文本仍然交给共享核心的 ParseBoundedScheduleNumber 解析，
//     所以 "12abc" / 空串 / 超长数字 在 GUI 与 backupctl 里的结论完全一致；
//   * 存储 schema 一个字节都没变：核心、CLI、ScheduleStore 继续只看
//     interval_minutes。

#ifndef BACKUP_PROJECT_UI_MODERN_SCHEDULE_FREQUENCY_H_
#define BACKUP_PROJECT_UI_MODERN_SCHEDULE_FREQUENCY_H_

#include <cstdint>
#include <string>

namespace backup_modern {

struct FrequencyUnit {
  const char* key;        // 表单键，QML 用它传递选择结果
  const char* label;      // 中文单位名
  std::uint32_t minutes;  // 1 / 60 / 1440 / 10080
};

// 界面下拉的选项，顺序固定为 分钟 / 小时 / 天 / 周。
int FrequencyUnitCount();
const FrequencyUnit& FrequencyUnitAt(int index);
// 找不到时返回 nullptr（未知键必须明确失败，不能悄悄落成"分钟"）。
const FrequencyUnit* FindFrequencyUnit(const std::string& key);
// 给定分钟数应该用哪个单位显示（取最大的整除单位）。找不到时返回分钟。
const FrequencyUnit& LargestExactFrequencyUnit(std::uint32_t interval_minutes);

// "值 + 单位" -> interval_minutes。
//
// 失败原因写进 error_message（人可读，含具体数字）。会被拒绝的输入：
//   * 非纯十进制数字（含符号、小数点、字母、空串）；
//   * 数值为 0；
//   * 折算后超过 kMaxIntervalMinutes；
//   * 乘法会溢出的输入（在乘之前用最大值/单位分钟数夹住，不做回绕）。
bool ParseFrequency(const std::string& value_text, const std::string& unit_key,
                    std::uint32_t* interval_minutes,
                    std::string* error_message);

// interval_minutes -> "值 + 单位"，用于把落盘配置回显到界面。
// 10080 -> "1" + "weeks"；2880 -> "2" + "days"；120 -> "2" + "hours"；
// 90 -> "90" + "minutes"（不能整除时回退分钟，绝不显示"每 1.5 小时"）。
void SplitFrequency(std::uint32_t interval_minutes, std::string* value_text,
                    std::string* unit_key);

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_SCHEDULE_FREQUENCY_H_

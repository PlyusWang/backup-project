// archive_entry.cpp
//
// 归档条目模型里唯一需要平台知识的部分：Linux dev_t 的 major/minor 编码。
// 单独成文件是为了让 archive_entry.h 保持纯数据结构不难读。

#include "archive_entry.h"

#include <sys/sysmacros.h>

namespace backupproject {

std::uint32_t DeviceMajor(std::uint64_t device) {
  return static_cast<std::uint32_t>(major(static_cast<dev_t>(device)));
}

std::uint32_t DeviceMinor(std::uint64_t device) {
  return static_cast<std::uint32_t>(minor(static_cast<dev_t>(device)));
}

std::uint64_t MakeDevice(std::uint32_t major_value, std::uint32_t minor_value) {
  return static_cast<std::uint64_t>(
      makedev(static_cast<unsigned int>(major_value),
              static_cast<unsigned int>(minor_value)));
}

}  // namespace backupproject

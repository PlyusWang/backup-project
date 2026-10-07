// include/format_bytes.h
//
// 字节数的**展示**格式。产品里所有"给人看的大小"都必须走这一个函数：CLI、
// 服务端管理工具、Modern GUI 的四个页面共用同一份规则。
//
// 为什么单独一层：此前每个前端各写了一套（共 7 份），同一个归档在不同页面上
// 会显示成 "12.3 MiB" / "12.30 MB" / "12913459 B" 三种样子；同一个数值在不同
// 页面显示不同面貌，用户会以为看错了。格式规则属于产品表现层，就应该只有一份。
//
// 规则（1024 进制，全产品统一）：
//   * 小于 1 KiB 只报字节："0 B" / "1 B" / "1023 B"；
//   * 1 KiB 起按 1024 进位，单位依次 B / KiB / MiB / GiB / TiB；
//   * 单位换算后的值是整数时省略小数（"1 KiB" / "2 MiB"），否则保留一位
//     （"1.5 KiB"）。整数省略 .0 是为了让最常见的取值读起来干净；
//   * 大于等于 1024 TiB 时封顶在 TiB，不再造 PiB（本项目的单份归档到不了
//     那个量级，多一个单位只会增加读的人的负担）。
//
// 单位名是 KiB / MiB / GiB / TiB 而不是 KB / MB / GB：数值确实按 1024 换算，
// 用 KB 说 1024 进制是错的（那是 1000 进制的单位）。
//
// 与 Qt 无关：core 层不依赖 Qt，Qt 侧只做一次 QString::fromStdString。

#ifndef BACKUP_PROJECT_INCLUDE_FORMAT_BYTES_H_
#define BACKUP_PROJECT_INCLUDE_FORMAT_BYTES_H_

#include <cstdint>
#include <string>

namespace backupproject {

std::string FormatByteSize(std::uint64_t bytes);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FORMAT_BYTES_H_

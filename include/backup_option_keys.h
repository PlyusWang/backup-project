// backup_option_keys.h
//
// 打包 / 压缩 / 加密三组选项的 key 与 enum 的唯一映射表。
//
// 为什么要有这个文件：PR #16 的 Modern GUI 已经把这三张表写在自己的
// BackupController 里，CLI 当时还没有 v2 选项，ScheduleStore 也还不存在。
// 本 PR 之后有三类调用方需要同一份映射：
//
//   CLI        （--pack / --compression / --encryption）
//   Modern GUI （界面下拉框）
//   ScheduleStore（schedule.json 里的 "pack" / "compression" / "encryption"）
//
// 三处各写一张表的必然结果是"界面里存的 key 命令行不认"，所以这里提取成
// Qt 无关的共享核心：谁都不许再维护第二份 enum/key mapping。
//
// key 的拼写不在这里发明——PackMethodName / CompressionMethodName /
// EncryptionMethodName 已经是格式层的规范名字（"mypack" / "ustar" /
// "fast-ustar" / "none" / "huffman" / "lzss-huffman" /
// "des-cbc-hmac-sha256" / "aes-256-ctr-hmac-sha256"），
// 本模块只补反向解析与展示文本。
//
// 解析失败一律返回 false，绝不回退到默认值：用户明确选了某个算法却拿到另一个
// 算法的产物，比明确报错危险得多。

#ifndef BACKUP_PROJECT_INCLUDE_BACKUP_OPTION_KEYS_H_
#define BACKUP_PROJECT_INCLUDE_BACKUP_OPTION_KEYS_H_

#include <string>

#include "container_format.h"
#include "pack_stream.h"

namespace backupproject {

// 规范 key。与格式层的 *Name() 逐字相同；method 非法时返回 "unknown"。
const char* PackMethodKey(PackMethod method);
const char* CompressionMethodKey(CompressionMethod method);
const char* EncryptionMethodKey(EncryptionMethod method);

// 展示文本（ASCII）。CLI 帮助与日志用；GUI 的本地化文案属于界面层。
const char* PackMethodDisplayName(PackMethod method);
const char* CompressionMethodDisplayName(CompressionMethod method);
const char* EncryptionMethodDisplayName(EncryptionMethod method);

bool ParsePackMethodKey(const std::string& key, PackMethod* method);
bool ParseCompressionMethodKey(const std::string& key,
                               CompressionMethod* method);
bool ParseEncryptionMethodKey(const std::string& key, EncryptionMethod* method);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_OPTION_KEYS_H_

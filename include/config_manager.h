// config_manager.h
//
// 应用配置 config.json 的持久化：只负责读写这一个文件，**不校验也不创建**配置
// 里的仓库路径。
//
// 因此 Load 成功只代表"文件被读出来了"，不代表配置里的路径可用：调用方必须自己
// InspectPath 一次。这条边界是刻意的——"合法仓库路径"只有一处定义，放在这里
// 就会出现第二份、迟早分叉。
//
// 磁盘格式固定为 v1（UTF-8 文本，只有两个字段）：
//     {"version": 1, "backup_repository_path": "<路径>"}
// 读取器刻意不是通用 JSON 解析器：未知字段、重复字段、非 1 的 version 都报错。
// 宁可让用户看到"版本不支持"，也不要按旧规则解释一份新版本可能写出的配置。
//
// 线程：只保存一个路径、没有可变状态，Load / Save 都是 const，可被多个线程并发
// 调用；并发写的原子性由 WriteFileAtomicallyReplacing 保证。

#ifndef BACKUP_PROJECT_INCLUDE_CONFIG_MANAGER_H_
#define BACKUP_PROJECT_INCLUDE_CONFIG_MANAGER_H_

#include <string>

namespace backupproject {

struct AppConfig {
  std::string backup_repository_path;
};

enum class ConfigLoadStatus {
  kLoaded,
  kMissing,
  kError,
};

// 构造函数接收显式路径（不自己去查 XDG），以便测试注入临时文件。
// 构造不做任何 I/O，也不会失败：配置在不在，要等 Load 才知道。
class ConfigManager {
 public:
  explicit ConfigManager(std::string config_file_path);

  // 读取配置。三态返回是刻意的：kMissing（首次运行，正常）与 kError（配置坏了
  // 或读不了）对上层是完全不同的处理，合成一个 bool 会把"没有配置"变成"配置
  // 错误"。进入时先把 *config 复位成默认值，所以失败路径上调用方拿到的不会是
  // 上一次的内容。error_message 可以为 nullptr。
  ConfigLoadStatus Load(AppConfig* config, std::string* error_message) const;

  // 整份替换写入：先拼出完整文本，再用同目录唯一临时文件 + rename 原子替换，
  // 不存在"改了一半"的中间态；父目录按需创建（0700）。返回 false 时磁盘上的
  // 旧配置原封不动，调用方可以安全重试。
  bool Save(const AppConfig& config, std::string* error_message) const;

  // 构造时传入的路径，原样返回。
  const std::string& config_file_path() const;

 private:
  std::string config_file_path_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_CONFIG_MANAGER_H_

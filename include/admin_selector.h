// include/admin_selector.h
//
// 管理工具的用户选择器：**永远不猜**。
//
// 人工验收发现的问题：`backup-server-admin show-user 23` 里的 23 到底指
// "id=23 的账户"还是"用户名叫 23 的账户"？旧实现直接按数字当 id 解析，于是
// 用户可能看到的是另一个账户，而且界面上没有任何提示。这里把规则固定下来：
//
//   * id:<编号>      只按 id 找；
//   * name:<用户名>  只按用户名找（用户名允许纯数字，这是产品规则，不改）；
//   * 裸输入         只有在**不产生歧义**时才接受：同时命中 id 和用户名就拒绝，
//                    并打印两种明确写法；只命中一边时接受，但在 stderr 上提示
//                    这次是按哪一种解析的。
//
// 解析与判定是两个纯函数：调用方负责查库，这里负责"怎么读用户输入"和"两个
// 候选都在的时候怎么办"。规则只有一份，命令行、包装脚本、测试看到的是同一套。

#ifndef BACKUP_PROJECT_ADMIN_SELECTOR_H
#define BACKUP_PROJECT_ADMIN_SELECTOR_H

#include <cstdint>
#include <cstdlib>
#include <string>

#include "network_protocol.h"

namespace backupproject {
namespace admin {

enum class UserSelectorKind { kBare, kId, kName };

struct UserSelector {
  UserSelectorKind kind = UserSelectorKind::kBare;
  std::string text;     // 去掉前缀之后的原文
  std::int64_t id = 0;  // kind == kId 时有效
};

enum class UserResolution { kUseId, kUseName, kAmbiguous, kNotFound };

inline std::string TrimSelector(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' ||
                                  value[begin] == '\n')) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                         value[end - 1] == '\n')) {
    --end;
  }
  return value.substr(begin, end - begin);
}

inline bool IsAllDigits(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  for (const char character : value) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

// 解析用户输入。返回 false 时 error_message 里是给用户看的一句话。
inline bool ParseUserSelector(const std::string& raw, UserSelector* out,
                              std::string* error_message) {
  if (out == nullptr) {
    return false;
  }
  const std::string text = TrimSelector(raw);
  if (text.empty()) {
    if (error_message != nullptr) {
      *error_message = "缺少用户参数（写法：id:<编号> 或 name:<用户名>）";
    }
    return false;
  }
  if (text.rfind("id:", 0) == 0) {
    const std::string digits = TrimSelector(text.substr(3));
    if (!IsAllDigits(digits) || digits.size() > 18) {
      if (error_message != nullptr) {
        *error_message = "id: 后面必须是最多 18 位的数字，例如 id:23";
      }
      return false;
    }
    out->kind = UserSelectorKind::kId;
    out->text = digits;
    out->id =
        static_cast<std::int64_t>(std::strtoll(digits.c_str(), nullptr, 10));
    return true;
  }
  if (text.rfind("name:", 0) == 0) {
    const std::string name = TrimSelector(text.substr(5));
    std::string validation_error;
    if (!backupproject::net::IsValidUsername(name, &validation_error)) {
      if (error_message != nullptr) {
        *error_message = "name: 后面不是合法用户名：" + validation_error;
      }
      return false;
    }
    out->kind = UserSelectorKind::kName;
    out->text = name;
    return true;
  }
  out->kind = UserSelectorKind::kBare;
  out->text = text;
  // 裸输入如果全是数字，也要按编号解析一次：这正是"歧义"的来源——同一个数字
  // 既要当编号查一遍，也要当用户名查一遍，只有两边都能查到才拒绝执行。
  if (IsAllDigits(text) && text.size() <= 18) {
    out->id =
        static_cast<std::int64_t>(std::strtoll(text.c_str(), nullptr, 10));
  }
  return true;
}

// 关键判定：只有**一个**候选时才解析成功。显式写法永远只认自己那一种。
inline UserResolution DecideUserResolution(const UserSelector& selector,
                                           bool has_id_match,
                                           bool has_name_match) {
  switch (selector.kind) {
    case UserSelectorKind::kId:
      return has_id_match ? UserResolution::kUseId : UserResolution::kNotFound;
    case UserSelectorKind::kName:
      return has_name_match ? UserResolution::kUseName
                            : UserResolution::kNotFound;
    case UserSelectorKind::kBare:
      break;
  }
  if (has_id_match && has_name_match) {
    return UserResolution::kAmbiguous;
  }
  if (has_id_match) {
    return UserResolution::kUseId;
  }
  if (has_name_match) {
    return UserResolution::kUseName;
  }
  return UserResolution::kNotFound;
}

// 歧义时的原话。**两种写法都写出来**：只说"有歧义"等于把问题丢回给用户。
inline std::string AmbiguityMessage(const std::string& text) {
  // 第一句就是验收里要求的那句话：先说"有歧义"，再给出两种明确写法。
  // 括号里补一句为什么——因为"这个数字既能当编号用，也已经有同名的账户"，
  // 所以管理工具不敢替用户决定。
  return "输入 " + text + " 存在歧义。请使用 id:" + text + " 或 name:" + text +
         "。（这个数字既能当编号用，也已经有同名的账户）";
}

}  // namespace admin
}  // namespace backupproject

#endif  // BACKUP_PROJECT_ADMIN_SELECTOR_H

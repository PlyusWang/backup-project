// user_directory.h
//
// numeric uid/gid -> 名字的唯一解析入口。
//
// 为什么必须只有一份：uid/gid 是**数字**，名字只是给 user:/group: 规则和界面
// 展示用的便利字段。解析失败（NSS 不可用、账号不存在、缓冲不足）时正确的行为
// 是"留空 + 数字规则照常работа"，而不是报错、崩溃，或者最糟的一种——把
// "缓冲区太小"误判成"这个用户不存在"，于是 user:alice 规则莫名其妙不命中。
//
// 之前 tree_scanner 与 Modern GUI 预览各写了一份，还都用了
// _SC_GETPW_R_SIZE_MAX 去问 group 数据库的大小；现在两边都走这里。

#ifndef BACKUP_PROJECT_INCLUDE_USER_DIRECTORY_H_
#define BACKUP_PROJECT_INCLUDE_USER_DIRECTORY_H_

#include <cstdint>
#include <map>
#include <string>

namespace backupproject {

// 单次查询。成功返回 true 并写名字；找不到或查询失败返回 false 并清空 out。
bool LookupUserName(std::uint32_t uid, std::string* out);
bool LookupGroupName(std::uint32_t gid, std::string* out);

// 带缓存的解析器。同一棵树里几千个文件常常只有几个 uid，缓存把 getpwuid_r
// 的调用次数从"文件数"降到"不同 uid 数"。
//
// 返回的引用在下一次调用之前有效；对象析构后失效。
class UserDirectoryCache {
 public:
  const std::string& UserName(std::uint32_t uid);
  const std::string& GroupName(std::uint32_t gid);

 private:
  std::map<std::uint32_t, std::string> users_;
  std::map<std::uint32_t, std::string> groups_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_USER_DIRECTORY_H_

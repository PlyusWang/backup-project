// realtime_watcher.h
//
// PR #19：递归 inotify watcher（纯 C++17 / Linux，不依赖 Qt）。
//
// 职责边界（写死在接口上）：watcher 只回答"源树里哪里发生了**可能影响源树**的
// 变化"，并给出三类信号：
//
//   * 普通事件（create / modify / close_write / attrib / delete / move …）；
//   * 结构事件（目录 create/delete/move、IN_MOVE_SELF / IN_DELETE_SELF /
//     IN_IGNORED / IN_UNMOUNT）——调用方据此重建 watch set；
//   * overflow（IN_Q_OVERFLOW）——事件历史已经不可信，必须 resync。
//
// 它**不**决定最终备份集合：Filter / 类型判定 / socket 规则全部留在既有 core。
// 在 watcher 里复制一遍 Filter 语义，就是第二套事实来源。
//
// 设计要点：
//   * RAII 管理 fd（IN_NONBLOCK | IN_CLOEXEC），析构即关闭；
//   * 递归建立时只用 lstat，软链接目录一律不 follow；
//   * 任何一步失败都不假装健康：清理已建立的 watch，并报出具体 errno；
//   * 结构变化后整体重建：**新 fd 建成功之后才关旧的**，不允许出现"旧拆了、
//     新的没建起来"的空窗；
//   * rename cookie 只解析、只记录，不承担 correctness（tree 内 rename 由
//     "重建 watch set"覆盖，不需要手写 prefix rewrite）。

#ifndef BACKUP_PROJECT_INCLUDE_REALTIME_WATCHER_H_
#define BACKUP_PROJECT_INCLUDE_REALTIME_WATCHER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace backupproject {

// 一批 inotify 事件读完之后给调用方的摘要。
struct WatchBatch {
  // 本批是否读到了任何事件。
  bool any_event = false;
  // 本批事件里有多少条记录（inotify 记录数，不是"文件数"）。
  std::uint64_t event_count = 0;
  // 出现了结构变化（目录 create/delete/move、self 事件、ignored、unmount）。
  bool structural = false;
  // 出现了 IN_Q_OVERFLOW。
  bool overflow = false;
  // 源根本身丢了（DELETE_SELF / MOVE_SELF / UNMOUNT / root 的 IN_IGNORED）。
  bool root_lost = false;
  // 解析到的 rename cookie（成对 MOVED_FROM/MOVED_TO）。只做记录。
  std::uint32_t last_rename_cookie = 0;
  std::uint64_t rename_pairs = 0;
};

class InotifyWatcher {
 public:
  InotifyWatcher() = default;
  ~InotifyWatcher();
  InotifyWatcher(const InotifyWatcher&) = delete;
  InotifyWatcher& operator=(const InotifyWatcher&) = delete;

  // 递归建立 watch。root 必须是真实目录（不是软链接）。
  // 失败时不会留下部分 watch，error_message 里是具体原因。
  bool Attach(const std::string& root, std::string* error_message);

  // 关闭 fd 并清空 watch 表。
  void Detach();

  bool attached() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  const std::string& root() const { return root_; }
  std::size_t watch_count() const { return watches_.size(); }

  // 读一批事件（非阻塞）。EINTR 重试，EAGAIN 表示读完了。
  // 返回 false 表示这次读本身失败（调用方应进入 DEGRADED 并准备重建）。
  bool Drain(WatchBatch* batch, std::string* error_message);

  // 整体重建：在**新的 fd** 上完整建立 watch set，成功之后才替换旧的。
  bool Rebuild(std::string* error_message);

  // ---- 测试接缝（默认不影响产品路径）----
  //
  // 让下一次 Drain 报告一次 IN_Q_OVERFLOW（不必真的打爆内核队列）。
  void InjectOverflowForTesting();
  // 让下一次 Attach/Rebuild 的第一个 inotify_add_watch 以给定 errno 失败，
  // 用来验证 ENOSPC / EACCES 时"可见失败 + 半成品清理"。
  void InjectAddWatchFailureForTesting(int error_number);
  // 让下一次目录枚举在指定深度失败。
  void InjectReaddirFailureForTesting(int error_number);

 private:
  struct WatchTarget {
    int wd = -1;
    std::string path;
    bool is_root = false;
  };

  bool BuildWatches(int fd, const std::string& root,
                    std::vector<WatchTarget>* watches,
                    std::unordered_map<int, std::size_t>* index,
                    std::string* error_message);
  bool AddDirectory(int fd, const std::string& path, bool is_root,
                    std::vector<WatchTarget>* watches,
                    std::unordered_map<int, std::size_t>* index,
                    std::string* error_message);
  void ReleaseState();

  int fd_ = -1;
  std::string root_;
  std::vector<WatchTarget> watches_;
  std::unordered_map<int, std::size_t> index_;
  int root_wd_ = -1;

  bool injected_overflow_ = false;
  int injected_add_watch_errno_ = 0;
  int injected_readdir_errno_ = 0;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REALTIME_WATCHER_H_

// realtime_debouncer.h
//
// PR #19：Realtime 的 debounce / coalescing 状态机（纯状态，可注入时间）。
//
// 为什么单独一个文件：把 debounce 逻辑散在 QTimer / shell sleep / 事件回调里，
// 就会出现"每个 inotify 事件建一个 .bak"这种典型错误，而且无法测试。
//
// 冻结的算法（trailing edge + 硬上限）：
//
//   第一次事件 t0:  soft = t0 + debounce, hard = t0 + max_wait, dirty = true
//   后续事件 t:     soft = min(t + debounce, hard)
//   触发:           now >= soft 或 now >= hard
//
// 于是：
//   * 短 burst  -> 最后一次事件之后 debounce 窗口到期触发（trailing edge）；
//   * 持续写入  -> 最迟 max_wait 到期形成一次 checkpoint，不会被无限推迟。
//
// 一次 generation 被**消费**（真正交给 BackupService）之后 dirty 才会清除；
// 备份运行期间新到的事件自然进入下一个 generation。调用方据此保证最多
// 1 个 in-flight + 1 个 coalesced pending generation。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_REALTIME_DEBOUNCER_H_
#define BACKUP_PROJECT_INCLUDE_REALTIME_DEBOUNCER_H_

#include <cstdint>

namespace backupproject {

// 一次"已经稳定"的触发。它只描述**事件层面**发生了什么：
// event_count 是合并掉的文件系统事件条数，不是"改了几个文件"。
struct RealtimeGeneration {
  std::uint64_t generation = 0;
  std::uint64_t event_count = 0;
  bool overflow_seen = false;
  bool structural_seen = false;
  // 这一代是不是"重新同步"（首次启用 / 重启 / root 恢复 / overflow 之后）。
  bool resync = false;
  std::int64_t first_event_ms = 0;
  std::int64_t settled_ms = 0;

  bool empty() const { return event_count == 0 && !resync; }
};

class RealtimeDebouncer {
 public:
  RealtimeDebouncer(std::uint32_t debounce_ms, std::uint32_t max_wait_ms);

  // 记录 count 条普通事件（可能同时带结构变化 / overflow 标记）。
  void NoteEvents(std::uint64_t count, bool structural, bool overflow,
                  std::int64_t now_ms);

  // 记录一次"重新同步"触发：立即到期（进程重启 / 首次启用 / overflow 之后，
  // 事件历史已经不可信，唯一正确的做法是重新观察当前源树）。
  void NoteResync(std::int64_t now_ms);

  // 现在是否已经可以触发。
  bool Due(std::int64_t now_ms) const;

  bool dirty() const { return dirty_; }
  std::int64_t soft_deadline_ms() const { return soft_deadline_ms_; }
  std::int64_t hard_deadline_ms() const { return hard_deadline_ms_; }
  std::uint64_t event_count() const { return event_count_; }
  bool overflow_seen() const { return overflow_seen_; }
  bool structural_seen() const { return structural_seen_; }
  std::uint64_t generation() const { return generation_; }

  // 取出当前 generation 并清 dirty（只有真的交给 BackupService 时才调用）。
  RealtimeGeneration Consume(std::int64_t now_ms);

  // 距离下次可以触发还有多久（>=0）；没有 pending 时返回 -1。
  std::int64_t WaitMs(std::int64_t now_ms) const;

 private:
  void NoteEventInternal(std::uint64_t count, bool structural, bool overflow,
                         std::int64_t now_ms, bool resync);

  std::uint32_t debounce_ms_ = 0;
  std::uint32_t max_wait_ms_ = 0;

  bool dirty_ = false;
  std::int64_t first_event_ms_ = 0;
  std::int64_t soft_deadline_ms_ = 0;
  std::int64_t hard_deadline_ms_ = 0;
  std::uint64_t event_count_ = 0;
  bool overflow_seen_ = false;
  bool structural_seen_ = false;
  bool resync_ = false;
  std::uint64_t generation_ = 0;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REALTIME_DEBOUNCER_H_

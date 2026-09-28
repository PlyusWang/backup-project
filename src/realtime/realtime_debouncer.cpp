// realtime_debouncer.cpp
//
// 见 include/realtime_debouncer.h。

#include "realtime_debouncer.h"

#include <algorithm>

namespace backupproject {

RealtimeDebouncer::RealtimeDebouncer(std::uint32_t debounce_ms,
                                     std::uint32_t max_wait_ms)
    : debounce_ms_(debounce_ms), max_wait_ms_(max_wait_ms) {}

void RealtimeDebouncer::NoteEventInternal(std::uint64_t count, bool structural,
                                          bool overflow, std::int64_t now_ms,
                                          bool resync) {
  const std::int64_t debounce = static_cast<std::int64_t>(debounce_ms_);
  const std::int64_t max_wait = static_cast<std::int64_t>(max_wait_ms_);
  if (!dirty_) {
    // 第一次事件：软截止 = t0 + debounce，硬截止 = t0 + max_wait。
    first_event_ms_ = now_ms;
    soft_deadline_ms_ = now_ms + debounce;
    hard_deadline_ms_ = now_ms + max_wait;
    dirty_ = true;
  }
  if (resync) {
    // 重新同步立即到期：resync 的语义就是"现在就把当前源树观察一遍"。
    soft_deadline_ms_ = now_ms;
    hard_deadline_ms_ = now_ms;
    resync_ = true;
  } else {
    // 后续事件把软截止往后推，但绝不越过硬截止。
    soft_deadline_ms_ = std::min(now_ms + debounce, hard_deadline_ms_);
  }
  event_count_ += count;
  overflow_seen_ = overflow_seen_ || overflow;
  structural_seen_ = structural_seen_ || structural;
}

void RealtimeDebouncer::NoteEvents(std::uint64_t count, bool structural,
                                   bool overflow, std::int64_t now_ms) {
  if (count == 0 && !structural && !overflow) return;
  NoteEventInternal(count, structural, overflow, now_ms, /*resync=*/false);
}

void RealtimeDebouncer::NoteResync(std::int64_t now_ms) {
  NoteEventInternal(0, /*structural=*/true, /*overflow=*/false, now_ms,
                    /*resync=*/true);
}

bool RealtimeDebouncer::Due(std::int64_t now_ms) const {
  if (!dirty_) return false;
  return now_ms >= soft_deadline_ms_ || now_ms >= hard_deadline_ms_;
}

std::int64_t RealtimeDebouncer::WaitMs(std::int64_t now_ms) const {
  if (!dirty_) return -1;
  const std::int64_t deadline = std::min(soft_deadline_ms_, hard_deadline_ms_);
  if (now_ms >= deadline) return 0;
  return deadline - now_ms;
}

RealtimeGeneration RealtimeDebouncer::Consume(std::int64_t now_ms) {
  RealtimeGeneration out;
  if (!dirty_) return out;
  out.generation = ++generation_;
  out.event_count = event_count_;
  out.overflow_seen = overflow_seen_;
  out.structural_seen = structural_seen_;
  out.resync = resync_;
  out.first_event_ms = first_event_ms_;
  out.settled_ms = now_ms;

  // 当前 generation 被消费；备份运行期间新到的事件进入下一 generation。
  dirty_ = false;
  event_count_ = 0;
  overflow_seen_ = false;
  structural_seen_ = false;
  resync_ = false;
  first_event_ms_ = 0;
  soft_deadline_ms_ = 0;
  hard_deadline_ms_ = 0;
  return out;
}

}  // namespace backupproject

// realtime_debouncer_test.cpp
//
// PR #19：debounce / coalescing 状态机的专项测试（纯状态，注入时间）。

#include <cstdint>
#include <string>

#include "realtime_debouncer.h"
#include "test_support.h"

namespace bp = backupproject;

int main() {
  using bp::RealtimeDebouncer;

  test_support::Section("INC-RTD 1. trailing edge：一个事件一次触发");
  {
    RealtimeDebouncer debouncer(500, 5000);
    test_support::Check(!debouncer.dirty(), "INC-RTD T1 初始不 dirty");
    debouncer.NoteEvents(1, false, false, 1000);
    test_support::Check(debouncer.dirty(), "INC-RTD T1 事件后 dirty");
    test_support::Check(!debouncer.Due(1499), "INC-RTD T1 debounce 窗口内不触发");
    test_support::Check(debouncer.Due(1500), "INC-RTD T1 到期即触发");
    const bp::RealtimeGeneration generation = debouncer.Consume(1500);
    test_support::Check(generation.event_count == 1 && !generation.empty() &&
                            !generation.resync,
                        "INC-RTD T1 一次事件产生一个 generation");
    test_support::Check(!debouncer.dirty() && !debouncer.Due(2000),
                        "INC-RTD T1 消费之后回到干净状态");
  }

  test_support::Section("INC-RTD 2. 事件风暴合并成一个 generation");
  {
    RealtimeDebouncer debouncer(500, 5000);
    // 200 ms 内来 100 个事件：每 2 ms 一个。
    for (int index = 0; index < 100; ++index) {
      debouncer.NoteEvents(1, false, false, 1000 + index * 2);
    }
    test_support::Check(debouncer.event_count() == 100,
                        "INC-RTD T2 计数被累计成一个 generation");
    test_support::Check(!debouncer.Due(1298),
                        "INC-RTD T2 最后一个事件之后 500ms 之前不触发");
    test_support::Check(debouncer.Due(1298 + 500),
                        "INC-RTD T2 最后一个事件之后 500ms 触发");
    const bp::RealtimeGeneration generation = debouncer.Consume(1800);
    test_support::Check(generation.event_count == 100,
                        "INC-RTD T2 判别：100 个事件只形成一次触发");
  }

  test_support::Section("INC-RTD 3. 持续写入由 max_wait 兜底");
  {
    RealtimeDebouncer debouncer(500, 5000);
    // 每 50 ms 来一个事件（比 debounce 窗口 500 ms 更密），永不停止：
    // 软截止一直被推后，最后由硬截止把它钉死——否则持续写入会永远等不到触发。
    for (int index = 0; index < 100; ++index) {
      debouncer.NoteEvents(1, false, false, 1000 + index * 50);
    }
    test_support::Check(!debouncer.Due(5999),
                        "INC-RTD T3 硬截止之前仍然不触发（软截止一直等于硬截止）");
    test_support::Check(debouncer.Due(6000),
                        "INC-RTD T3 判别：max_wait 到期必然触发一次 checkpoint");
    const bp::RealtimeGeneration generation = debouncer.Consume(6000);
    test_support::Check(generation.first_event_ms == 1000 &&
                            generation.event_count == 100,
                        "INC-RTD T3 generation 记录了第一条事件与总条数");
  }

  test_support::Section("INC-RTD 4. 触发之后的事件进入下一 generation");
  {
    RealtimeDebouncer debouncer(500, 5000);
    debouncer.NoteEvents(3, false, false, 1000);
    const bp::RealtimeGeneration first = debouncer.Consume(1500);
    debouncer.NoteEvents(2, false, false, 1600);
    test_support::Check(debouncer.dirty() && !debouncer.Due(2000),
                        "INC-RTD T4 新事件开启新一代（自己的 debounce 窗口）");
    const bp::RealtimeGeneration second = debouncer.Consume(2100);
    test_support::Check(second.generation == first.generation + 1 &&
                            second.event_count == 2,
                        "INC-RTD T4 判别：两代事件各自成一次触发");
  }

  test_support::Section("INC-RTD 5. busy 期间只保留一个 pending generation");
  {
    RealtimeDebouncer debouncer(500, 5000);
    debouncer.NoteEvents(1, false, false, 1000);
    const bp::RealtimeGeneration in_flight = debouncer.Consume(1500);
    test_support::Check(in_flight.event_count == 1,
                        "INC-RTD T5 第一次触发进入 in-flight");
    // 备份运行期间持续来事件：全部合并成一个 pending generation。
    for (int index = 0; index < 50; ++index) {
      debouncer.NoteEvents(1, false, false, 1600 + index * 10);
    }
    test_support::Check(debouncer.event_count() == 50,
                        "INC-RTD T5 运行期间的事件合并计数");
    const bp::RealtimeGeneration pending = debouncer.Consume(2100);
    test_support::Check(pending.event_count == 50 &&
                            pending.generation == in_flight.generation + 1,
                        "INC-RTD T5 判别：50 个事件只留一个 pending generation，不丢不裂");
    test_support::Check(!debouncer.dirty(), "INC-RTD T5 消费之后没有残留");
  }

  test_support::Section("INC-RTD 6. overflow / 结构标记与 resync 语义");
  {
    RealtimeDebouncer debouncer(500, 5000);
    debouncer.NoteEvents(4, /*structural=*/true, /*overflow=*/true, 1000);
    debouncer.NoteEvents(1, false, false, 1100);
    const bp::RealtimeGeneration generation = debouncer.Consume(1700);
    test_support::Check(generation.overflow_seen && generation.structural_seen,
                        "INC-RTD T6 判别：overflow / 结构标记保留到 generation");
    test_support::Check(generation.event_count == 5,
                        "INC-RTD T6 计数包含 overflow 之后的事件");

    RealtimeDebouncer resync(500, 5000);
    resync.NoteResync(2000);
    test_support::Check(resync.dirty() && resync.Due(2000),
                        "INC-RTD T6 resync 立即到期（不等 debounce 窗口）");
    const bp::RealtimeGeneration resync_generation = resync.Consume(2000);
    test_support::Check(resync_generation.resync &&
                            resync_generation.event_count == 0,
                        "INC-RTD T6 resync generation 标记正确");
  }

  return test_support::Finish("realtime_debouncer_test");
}

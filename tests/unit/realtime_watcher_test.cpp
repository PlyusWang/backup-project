// realtime_watcher_test.cpp
//
// PR #19：递归 inotify watcher 的专项测试（真实 inotify + 窄测试接缝）。

#include "realtime_watcher.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>

#include "test_support.h"

namespace bp = backupproject;

namespace {

// 等一批事件；超时返回 false。
bool WaitForEvents(bp::InotifyWatcher* watcher, bp::WatchBatch* batch,
                   int timeout_ms, std::string* error) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    struct pollfd descriptor;
    descriptor.fd = watcher->fd();
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    const int ready = ::poll(&descriptor, 1, timeout_ms / 50 + 1);
    if (ready > 0) {
      if (!watcher->Drain(batch, error)) return false;
      if (batch->any_event) return true;
    } else if (ready < 0 && errno != EINTR) {
      *error = "poll failed";
      return false;
    }
  }
  *batch = bp::WatchBatch{};
  return false;
}

// 反复 Drain 直到 timeout，把几次结果合并起来（inotify 事件可能分批到达）。
bool CollectEvents(bp::InotifyWatcher* watcher, bp::WatchBatch* merged,
                   int total_timeout_ms, std::string* error) {
  *merged = bp::WatchBatch{};
  bool any = false;
  const int steps = 20;
  for (int step = 0; step < steps; ++step) {
    bp::WatchBatch batch;
    if (WaitForEvents(watcher, &batch, total_timeout_ms / steps + 1, error)) {
      merged->any_event = true;
      merged->event_count += batch.event_count;
      merged->structural = merged->structural || batch.structural;
      merged->overflow = merged->overflow || batch.overflow;
      merged->root_lost = merged->root_lost || batch.root_lost;
      any = true;
    }
  }
  return any;
}

}  // namespace

int main() {
  test_support::Section("INC-RTW 1. 递归建立与软链接边界");
  {
    const std::string work = test_support::FreshDir("realtime-watcher");
    const std::string source = work + "/source";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(source + "/a", 0755);
    test_support::Mkdir(source + "/a/b", 0755);
    test_support::Mkdir(source + "/c", 0755);
    // 软链接目录：绝不 follow，因此不会被 watch。
    test_support::CreateSymlink(source + "/c", source + "/a/link-to-c");

    bp::InotifyWatcher watcher;
    std::string error;
    test_support::Check(watcher.Attach(source, &error),
                        "INC-RTW T1 attach 成功", error);
    test_support::Check(watcher.attached() && watcher.fd() >= 0,
                        "INC-RTW T1 拿到 fd");
    test_support::Check(
        watcher.watch_count() == 4,
        "INC-RTW T1 判别：只 watch 真实目录（source + a + a/b + c），"
        "软链接目录不算",
        std::to_string(watcher.watch_count()));

    // 通过软链接改 c 里的文件：应该只由 c 自己的 watch 触发一次，而不是两次
    // （软链接没有被单独 watch）。
    bp::WatchBatch batch;
    test_support::WriteFile(source + "/c/through-link.txt", "x", 0644);
    error.clear();
    const bool got = CollectEvents(&watcher, &batch, 300, &error);
    test_support::Check(got && batch.event_count >= 1,
                        "INC-RTW T1 通过软链接写入也能被 c 的 watch 看到",
                        error);
    watcher.Detach();
    test_support::Check(!watcher.attached() && watcher.fd() < 0,
                        "INC-RTW T1 Detach 关闭 fd");
  }

  test_support::Section("INC-RTW 2. 普通事件与结构事件");
  {
    const std::string work = test_support::FreshDir("realtime-watcher-events");
    const std::string source = work + "/source";
    test_support::Mkdir(source, 0755);
    bp::InotifyWatcher watcher;
    std::string error;
    test_support::Check(watcher.Attach(source, &error), "INC-RTW T2 attach",
                        error);

    bp::WatchBatch batch;
    test_support::WriteFile(source + "/file.txt", "hello", 0644);
    error.clear();
    test_support::Check(
        CollectEvents(&watcher, &batch, 300, &error) && batch.event_count >= 1,
        "INC-RTW T2 create + close_write 被看到", error);
    test_support::Check(!batch.structural,
                        "INC-RTW T2 创建普通文件不算结构变化");

    test_support::Mkdir(source + "/newdir", 0755);
    error.clear();
    test_support::Check(
        CollectEvents(&watcher, &batch, 300, &error) && batch.structural,
        "INC-RTW T2 判别：mkdir 被标记为结构变化", error);

    ::unlink((source + "/file.txt").c_str());
    error.clear();
    test_support::Check(
        CollectEvents(&watcher, &batch, 300, &error) && batch.event_count >= 1,
        "INC-RTW T2 删除文件被看到", error);
    watcher.Detach();
  }

  test_support::Section("INC-RTW 3. 目录 move-in / move-out");
  {
    const std::string work = test_support::FreshDir("realtime-watcher-move");
    const std::string source = work + "/source";
    const std::string outside = work + "/outside";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(outside, 0755);
    test_support::Mkdir(outside + "/moved-in", 0755);
    test_support::WriteFile(outside + "/moved-in/inner.txt", "x", 0644);

    bp::InotifyWatcher watcher;
    std::string error;
    test_support::Check(watcher.Attach(source, &error), "INC-RTW T3 attach",
                        error);
    const std::size_t before = watcher.watch_count();

    // move-in：结构变化 + 重建之后，新目录的子项必须也被 watch。
    test_support::Check(::rename((outside + "/moved-in").c_str(),
                                 (source + "/moved-in").c_str()) == 0,
                        "INC-RTW T3 move-in 一个已填充的目录");
    bp::WatchBatch batch;
    error.clear();
    test_support::Check(
        CollectEvents(&watcher, &batch, 400, &error) && batch.structural,
        "INC-RTW T3 判别：move-in 被标记为结构变化", error);
    error.clear();
    test_support::Check(watcher.Rebuild(&error), "INC-RTW T3 rebuild 成功",
                        error);
    test_support::Check(watcher.watch_count() == before + 1,
                        "INC-RTW T3 重建之后 watch 数 +1（新目录）",
                        std::to_string(watcher.watch_count()));
    test_support::WriteFile(source + "/moved-in/after.txt", "y", 0644);
    error.clear();
    test_support::Check(
        CollectEvents(&watcher, &batch, 300, &error) && batch.event_count >= 1,
        "INC-RTW T3 判别：move-in 目录里新建文件能被看到（预填充目录也已 "
        "watch）",
        error);

    // move-out：重建之后，外部继续修改不得再触发。
    test_support::Check(::rename((source + "/moved-in").c_str(),
                                 (outside + "/moved-out").c_str()) == 0,
                        "INC-RTW T3 move-out 到 source 之外");
    error.clear();
    test_support::Check(
        CollectEvents(&watcher, &batch, 400, &error) && batch.structural,
        "INC-RTW T3 move-out 被标记为结构变化", error);
    error.clear();
    test_support::Check(watcher.Rebuild(&error),
                        "INC-RTW T3 move-out 后 rebuild", error);
    test_support::WriteFile(outside + "/moved-out/outside.txt", "z", 0644);
    error.clear();
    bp::WatchBatch after_move_out;
    const bool saw_outside =
        CollectEvents(&watcher, &after_move_out, 300, &error);
    test_support::Check(!saw_outside || !after_move_out.structural,
                        "INC-RTW T3 判别：移出 source 的目录不再触发结构事件",
                        std::to_string(after_move_out.event_count));
    watcher.Detach();
  }

  test_support::Section("INC-RTW 4. overflow 与 watch 失败接缝");
  {
    const std::string work = test_support::FreshDir("realtime-watcher-seams");
    const std::string source = work + "/source";
    test_support::Mkdir(source, 0755);
    bp::InotifyWatcher watcher;
    std::string error;
    test_support::Check(watcher.Attach(source, &error), "INC-RTW T4 attach",
                        error);

    watcher.InjectOverflowForTesting();
    bp::WatchBatch batch;
    error.clear();
    test_support::Check(watcher.Drain(&batch, &error) && batch.overflow,
                        "INC-RTW T4 判别：注入的 IN_Q_OVERFLOW 被如实上报",
                        error);

    // ENOSPC：必须可见失败 + 不留半成品。
    bp::InotifyWatcher failing;
    failing.InjectAddWatchFailureForTesting(ENOSPC);
    error.clear();
    test_support::Check(!failing.Attach(source, &error),
                        "INC-RTW T4 判别：add_watch ENOSPC -> attach 失败",
                        error);
    test_support::Check(
        !failing.attached() && failing.fd() < 0 && failing.watch_count() == 0,
        "INC-RTW T4 判别：失败之后没有留下 fd / watch");
    test_support::Check(error.find("inotify") != std::string::npos,
                        "INC-RTW T4 失败原因是具体的 inotify 错误", error);

    bp::InotifyWatcher readdir_failing;
    readdir_failing.InjectReaddirFailureForTesting(EACCES);
    error.clear();
    test_support::Check(!readdir_failing.Attach(source, &error),
                        "INC-RTW T4 判别：readdir 失败 -> attach 失败", error);

    // 软链接 root 直接拒绝。
    const std::string link = work + "/link";
    test_support::CreateSymlink(source, link);
    bp::InotifyWatcher linked;
    error.clear();
    test_support::Check(!linked.Attach(link, &error),
                        "INC-RTW T4 判别：软链接 root 被拒绝", error);
    watcher.Detach();
  }

  test_support::Section("INC-RTW 5. 源根丢失");
  {
    const std::string work =
        test_support::FreshDir("realtime-watcher-rootloss");
    const std::string source = work + "/source";
    test_support::Mkdir(source, 0755);
    bp::InotifyWatcher watcher;
    std::string error;
    test_support::Check(watcher.Attach(source, &error), "INC-RTW T5 attach",
                        error);
    test_support::Check(::rmdir(source.c_str()) == 0, "INC-RTW T5 删除源根");

    bp::WatchBatch batch;
    error.clear();
    const bool saw = CollectEvents(&watcher, &batch, 400, &error);
    test_support::Check(
        saw && batch.root_lost,
        "INC-RTW T5 判别：根丢失被标记（DELETE_SELF / IGNORED）", error);
    watcher.Detach();
  }

  test_support::Section(
      "INC-RTW 6. root attach 竞态：预检通过、建立 watch 时 root 已经变了");
  {
    // 这一节钉的是"成功但 0 watch"那个 TOCTOU：Attach 的预检看完 root 之后、
    // AddDirectory 的第二次 lstat 之前，root 被删 / 被换类型。旧实现会在这里
    // 返回成功、watch_count == 0、root_wd == -1；新实现必须硬失败，并且不留下
    // 任何半成品。钩子让这个窗口可重复命中，不靠概率 race。
    const std::string work =
        test_support::FreshDir("realtime-watcher-rootrace");

    struct RaceState {
      std::string root;
      int mode = 0;  // 0=删除 1=换成普通文件 2=换成软链接
      std::string other;
    };

    // 1) root 在窗口里被删除
    {
      const std::string source = work + "/removed";
      test_support::Mkdir(source, 0755);
      RaceState state;
      state.root = source;
      bp::InotifyWatcher watcher;
      watcher.SetRootPrecheckHookForTesting(
          [](void* context) {
            RaceState* state = static_cast<RaceState*>(context);
            if (state->mode == 0) {
              ::rmdir(state->root.c_str());
            } else if (state->mode == 1) {
              ::rmdir(state->root.c_str());
              test_support::WriteFile(state->root, "now a file", 0644);
            } else {
              ::rmdir(state->root.c_str());
              test_support::CreateSymlink(state->other, state->root);
            }
          },
          &state);
      std::string error;
      test_support::Check(!watcher.Attach(source, &error),
                          "INC-RTW T6 判别：root 在预检后消失 -> attach 失败",
                          error);
      test_support::Check(
          !watcher.attached() && watcher.fd() < 0 && watcher.watch_count() == 0,
          "INC-RTW T6 判别：失败之后没有 fd、没有 watch、"
          "watch_count 不会是 0 却自称健康");
      test_support::Check(error.find("watch root") != std::string::npos,
                          "INC-RTW T6 失败原因点名 root", error);
      watcher.SetRootPrecheckHookForTesting(nullptr, nullptr);
    }

    // 2) root 在窗口里被换成普通文件 / 软链接
    for (int mode = 1; mode <= 2; ++mode) {
      const std::string source = work + (mode == 1 ? "/file" : "/link");
      test_support::Mkdir(source, 0755);
      const std::string other = work + "/other-target";
      test_support::Mkdir(other, 0755);
      RaceState state;
      state.root = source;
      state.mode = mode;
      state.other = other;
      bp::InotifyWatcher watcher;
      watcher.SetRootPrecheckHookForTesting(
          [](void* context) {
            RaceState* state = static_cast<RaceState*>(context);
            if (state->mode == 1) {
              ::rmdir(state->root.c_str());
              test_support::WriteFile(state->root, "now a file", 0644);
            } else {
              ::rmdir(state->root.c_str());
              test_support::CreateSymlink(state->other, state->root);
            }
          },
          &state);
      std::string error;
      const bool attached = watcher.Attach(source, &error);
      test_support::Check(!attached,
                          mode == 1
                              ? "INC-RTW T6 判别：root 变成普通文件 -> 失败"
                              : "INC-RTW T6 判别：root 变成软链接 -> 失败",
                          error);
      test_support::Check(!watcher.attached() && watcher.watch_count() == 0,
                          "INC-RTW T6 判别的依据：拒绝之后没有留下 watch");
      watcher.SetRootPrecheckHookForTesting(nullptr, nullptr);
    }

    // 3) 恢复：root 回来之后 attach 成功，并且真的能收到事件（上层据此
    //    "retry attach + resync"）。
    {
      const std::string source = work + "/recovered";
      test_support::Mkdir(source, 0755);
      RaceState state;
      state.root = source;
      bp::InotifyWatcher watcher;
      watcher.SetRootPrecheckHookForTesting(
          [](void* context) {
            RaceState* state = static_cast<RaceState*>(context);
            ::rmdir(state->root.c_str());
          },
          &state);
      std::string error;
      test_support::Check(!watcher.Attach(source, &error),
                          "INC-RTW T6 恢复前的第一次 attach 被拒", error);
      watcher.SetRootPrecheckHookForTesting(nullptr, nullptr);
      test_support::Mkdir(source, 0755);
      error.clear();
      test_support::Check(watcher.Attach(source, &error),
                          "INC-RTW T6 判别：root 恢复之后 attach 成功", error);
      test_support::Check(watcher.watch_count() >= 1,
                          "INC-RTW T6 判别：恢复之后 root 真的被 watch 上",
                          std::to_string(watcher.watch_count()));
      bp::WatchBatch batch;
      test_support::WriteFile(source + "/after-recovery.txt", "x", 0644);
      error.clear();
      const bool saw = CollectEvents(&watcher, &batch, 400, &error);
      test_support::Check(saw, "INC-RTW T6 判别：恢复之后事件真的到得了",
                          error);
      watcher.Detach();
    }

    // 4) Rebuild 的同一条不变量：root 在重建窗口里消失 -> 失败，而且旧状态
    //    （旧 fd / 旧 watch）原样保留。
    {
      // 根目录必须是空的：钩子用 rmdir 让它在窗口里消失，非空目录删不掉。
      const std::string source = work + "/rebuild";
      test_support::Mkdir(source, 0755);
      bp::InotifyWatcher watcher;
      std::string error;
      test_support::Check(watcher.Attach(source, &error),
                          "INC-RTW T6 rebuild 前的 attach", error);
      const std::size_t before = watcher.watch_count();
      RaceState state;
      state.root = source;
      watcher.SetRootPrecheckHookForTesting(
          [](void* context) {
            RaceState* state = static_cast<RaceState*>(context);
            ::rmdir(state->root.c_str());
          },
          &state);
      error.clear();
      test_support::Check(
          !watcher.Rebuild(&error),
          "INC-RTW T6 判别：root 在 rebuild 窗口里消失 -> 重建失败", error);
      watcher.SetRootPrecheckHookForTesting(nullptr, nullptr);
      test_support::Check(watcher.attached() && watcher.watch_count() == before,
                          "INC-RTW T6 判别：重建失败之后旧状态原样保留",
                          std::to_string(watcher.watch_count()));
      watcher.Detach();
    }
  }

  return test_support::Finish("realtime_watcher_test");
}

// tests/unit/cleanup_short_circuit_test.cpp
//
// 失败路径清理的专项测试：证明 fsync / close 的失败不会**短路**掉后面的
// 清理步骤，并且诊断里报的是第一次失败的原因。
//
// 为什么必须有这个测试：`if (fsync(fd) != 0 || close(fd) != 0)` 在 fsync
// 失败时根本不会执行 close —— 每失败一次泄漏一个 fd，而 ENOSPC / EIO 恰恰
// 就是会连续失败的那种场景（长驻的实时备份 / 计划任务进程会稳定耗尽 fd 表）。
// 光看代码不容易发现，因为成功路径完全正常。
//
// 注入方式：tests/review/cleanup_fault_interposer.cpp 在链接期覆盖
// fsync() / close()（放在 libc 前面，见 scripts/cleanup_contract_test.sh），
// 可以指定"第几次调用失败"以及失败时返回的 errno。不 sleep、不撞运气。
//
// 三个用例分别覆盖：
//   1) fsync 失败 -> fd 仍被 close（原来的短路写法在这里必然失败）；
//   2) fsync 与 close 都失败 -> 诊断是**第一次**失败（fsync/ENOSPC）的原因，
//      没有被后续清理改写成 close/EIO；
//   3) 两个 fd 的 close 都失败 -> 两个 fd 都尝试关闭，诊断同样是第一次的
//      errno（EIO），而不是第二个的 ENOSPC。

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "incremental_delta.h"
#include "realtime_store.h"
#include "source_digest.h"
#include "test_support.h"

namespace bp = backupproject;

// 注入器接口（tests/review/cleanup_fault_interposer.cpp）。
extern "C" void CleanupFaultArm(std::uint32_t fsync_fail_mask,
                                std::uint32_t close_fail_mask);
extern "C" std::uint32_t CleanupFaultFsyncCalls();
extern "C" std::uint32_t CleanupFaultCloseCalls();
extern "C" int CleanupFaultFsyncErrno();
extern "C" int CleanupFaultCloseErrno();
extern "C" int CleanupFaultCallRealClose(int fd);

namespace {

// 第 1 次调用失败 / 第 2 次调用失败。
constexpr std::uint32_t kFailFirst = 0x1u;
constexpr std::uint32_t kFailSecond = 0x2u;

bp::RealtimeConfig MakeConfig(const std::string& source_path) {
  bp::RealtimeConfig config;
  config.enabled = false;
  config.trigger = bp::BackupTrigger::kRealtime;
  config.source_path = source_path;
  config.debounce_ms = bp::kDefaultRealtimeDebounceMs;
  config.max_wait_ms = bp::kDefaultRealtimeMaxWaitMs;
  config.retain_count = bp::kDefaultRealtimeRetainCount;
  config.strategy = bp::BackupStrategy::kFull;
  config.pack_method = bp::PackMethod::kMyPack;
  config.compression_method = bp::CompressionMethod::kNone;
  config.encryption_method = bp::EncryptionMethod::kNone;
  return config;
}

// 用例 1 与 2 的被测入口：RealtimeStore::Save 的收尾就是
// "fsync -> close -> unlink 半成品" 那一段。
void CheckRealtimeStoreSaveCleanup(const std::string& work) {
  const std::string file = work + "/realtime.json";
  bp::RealtimeStore store(file);
  const bp::RealtimeConfig config = MakeConfig(work + "/src");

  // ---- S1：fsync 失败时 fd 仍然必须被 close ----
  test_support::Section("CLEANUP-S1 fsync 失败不再短路掉 close");
  {
    CleanupFaultArm(kFailFirst, 0);
    std::string error;
    const bool ok = store.Save(config, &error);
    const std::uint32_t fsync_calls = CleanupFaultFsyncCalls();
    const std::uint32_t close_calls = CleanupFaultCloseCalls();
    test_support::Check(!ok, "CLEANUP-S1 T1 fsync 失败时 Save 必须失败");
    test_support::Check(fsync_calls == 1,
                        "CLEANUP-S1 T2 fsync 真的被调用了一次");
    test_support::Check(close_calls == 1,
                        "CLEANUP-S1 T3 fsync 失败后 close 仍然被执行",
                        "close 调用次数 = " + std::to_string(close_calls));
    test_support::Check(error.find(std::strerror(ENOSPC)) != std::string::npos,
                        "CLEANUP-S1 T4 诊断里带着 fsync 的 errno", error);
    test_support::Check(!test_support::Exists(file),
                        "CLEANUP-S1 T5 失败后不留半成品文件");
  }

  // ---- S2：两个都失败时，诊断必须是第一次失败的原因 ----
  test_support::Section("CLEANUP-S2 后面的清理不得改写第一次失败的 errno");
  {
    CleanupFaultArm(kFailFirst, kFailFirst);
    std::string error;
    const bool ok = store.Save(config, &error);
    test_support::Check(!ok, "CLEANUP-S2 T1 两级都失败时 Save 必须失败");
    test_support::Check(CleanupFaultFsyncCalls() == 1 &&
                            CleanupFaultCloseCalls() == 1,
                        "CLEANUP-S2 T2 fsync 与 close 各被调用一次");
    test_support::Check(CleanupFaultFsyncErrno() == ENOSPC,
                        "CLEANUP-S2 T3 fsync 注入的是 ENOSPC");
    test_support::Check(CleanupFaultCloseErrno() == EIO,
                        "CLEANUP-S2 T4 close 注入的是 EIO（两者可区分）");
    test_support::Check(error.find(std::strerror(ENOSPC)) != std::string::npos,
                        "CLEANUP-S2 T5 诊断保留第一次失败（fsync）的原因",
                        error);
    test_support::Check(error.find(std::strerror(EIO)) == std::string::npos,
                        "CLEANUP-S2 T6 诊断没有被后续清理改写", error);
  }

  // 关闭注入，确认正常路径不受影响。
  CleanupFaultArm(0, 0);
  {
    std::string error;
    const bool ok = store.Save(config, &error);
    test_support::Check(ok, "CLEANUP-S3 T1 关闭注入后 Save 正常成功", error);
    bp::RealtimeConfig loaded;
    std::string load_error;
    const bool loaded_ok =
        store.Load(&loaded, &load_error) == bp::RealtimeLoadStatus::kLoaded;
    test_support::Check(loaded_ok && loaded.source_path == config.source_path,
                        "CLEANUP-S3 T2 写出去的读得回来", load_error);
  }
}

// 用例 3：抽取 delta payload 的收尾要关**两个** fd。
void CheckExtractDeltaPayloadCleanup(const std::string& work) {
  test_support::Section("CLEANUP-S4 两个 fd 都必须尝试关闭");

  const std::string source = work + "/src";
  const std::string delta = work + "/one.delta";
  if (!test_support::Mkdir(source, 0755)) {
    test_support::Check(false, "CLEANUP-S4 前置：建源目录");
    return;
  }
  test_support::WriteFile(source + "/a.txt", "alpha", 0644);

  bp::DeltaEnvelope envelope;
  envelope.parent_file_name = "base.bak";
  envelope.parent_snapshot_id = bp::ContentDigestOfBytes("parent");
  envelope.parent_manifest_digest = std::string(64, 'a');
  envelope.base_generation_id = std::string(64, 'b');
  envelope.source_identity =
      bp::SourceIdentityDigest(source, "repository-identity");
  envelope.filter_identity = bp::FilterIdentityDigest({}, {});
  envelope.strategy_identity = bp::StrategyIdentityDigest(
      bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
      bp::EncryptionMethod::kNone);
  envelope.current_manifest_digest = std::string(64, 'c');
  envelope.created_unix_seconds = 1700000000;

  bp::ArchiveEntry root;
  root.archive_path = ".";
  root.source_path = source;
  root.type = bp::EntryType::kDirectory;
  root.mode = 0755;
  root.uid = 1000;
  root.gid = 1000;

  std::vector<bp::ArchiveEntry> entries;
  entries.push_back(root);
  bp::BackupOptions options;
  std::string error;
  if (!bp::WriteDeltaFile(delta, envelope, entries, options, &error)) {
    test_support::Check(false, "CLEANUP-S4 前置：写出一份合法 delta", error);
    return;
  }
  test_support::Check(test_support::Exists(delta),
                      "CLEANUP-S4 T1 前置 delta 已生成");

  const std::string container = work + "/extracted.container";

  // ---- S4a：两个 close 都失败 -> 两个都尝试了，诊断保留第一次的 EIO ----
  CleanupFaultArm(0, kFailFirst | kFailSecond);
  const bool ok = bp::ExtractDeltaPayload(delta, container, &error);
  const std::uint32_t close_calls = CleanupFaultCloseCalls();
  test_support::Check(!ok, "CLEANUP-S4 T2 close 失败时抽取必须失败");
  test_support::Check(close_calls == 2,
                      "CLEANUP-S4 T3 两个 fd 都尝试关闭（不是 1 次）",
                      "close 调用次数 = " + std::to_string(close_calls));
  test_support::Check(CleanupFaultCloseErrno() == ENOSPC,
                      "CLEANUP-S4 T4 第二次 close 注入的是 ENOSPC");
  test_support::Check(error.find(std::strerror(EIO)) != std::string::npos,
                      "CLEANUP-S4 T5 诊断保留第一次失败的 errno", error);
  test_support::Check(error.find(std::strerror(ENOSPC)) == std::string::npos,
                      "CLEANUP-S4 T6 诊断没有被第二次 close 改写", error);
  test_support::Check(!test_support::Exists(container),
                      "CLEANUP-S4 T7 失败后不留半成品 container");

  // ---- S4b：只有第二个 close 失败 -> 第一个已经成功关闭 ----
  error.clear();
  CleanupFaultArm(0, kFailSecond);
  const bool ok_second = bp::ExtractDeltaPayload(delta, container, &error);
  const std::uint32_t close_calls_second = CleanupFaultCloseCalls();
  test_support::Check(!ok_second, "CLEANUP-S4 T8 第二次 close 失败时也必须失败");
  test_support::Check(close_calls_second == 2,
                      "CLEANUP-S4 T9 第二次 close 也是真的被调用了",
                      "close 调用次数 = " +
                          std::to_string(close_calls_second));
  test_support::Check(error.find(std::strerror(ENOSPC)) != std::string::npos,
                      "CLEANUP-S4 T10 这次诊断报的是 ENOSPC", error);
  test_support::Check(!test_support::Exists(container),
                      "CLEANUP-S4 T11 失败后仍然不留半成品");

  // ---- S4c：关闭注入 -> 正常抽取成功，产物可用 ----
  error.clear();
  CleanupFaultArm(0, 0);
  const bool ok_clean = bp::ExtractDeltaPayload(delta, container, &error);
  test_support::Check(ok_clean, "CLEANUP-S4 T12 关闭注入后抽取成功", error);
  test_support::Check(test_support::Exists(container),
                      "CLEANUP-S4 T13 正常路径产物存在");
  const int probe = ::open(container.c_str(), O_RDONLY | O_CLOEXEC);
  test_support::Check(probe >= 0, "CLEANUP-S4 T14 产物可以正常打开");
  if (probe >= 0) {
    CleanupFaultCallRealClose(probe);
  }
}

}  // namespace

int main() {
  const std::string work = test_support::FreshDir("cleanup-short-circuit");
  CheckRealtimeStoreSaveCleanup(work);
  CheckExtractDeltaPayloadCleanup(work);
  return test_support::Finish("cleanup-short-circuit");
}

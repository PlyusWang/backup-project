// tests/unit/remote_client_test.cpp
//
// PR #20：RemoteArchiveClient 的专项测试。
//
// 服务端是真的（Run() 起固定 worker 池），客户端是真的，文件系统是真的。
// 这里断言的是"客户端库拿到手的行为"：会话、进度回调、流式上传下载、
// 默认不覆盖目标、下载失败不发布文件。

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "file_io.h"
#include "remote_backup_client.h"
#include "remote_server.h"
#include "test_support.h"

namespace bp = backupproject;
namespace crypto = backupproject::crypto;
namespace net = backupproject::net;

namespace {

std::string RandomHex(std::size_t bytes) {
  std::string raw;
  std::string error;
  if (!crypto::RandomBytes(bytes, &raw, &error)) {
    return std::string();
  }
  return crypto::ToHex(reinterpret_cast<const unsigned char*>(raw.data()),
                       raw.size());
}

std::string Sha256OfFile(const std::string& path) {
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) {
    return std::string();
  }
  crypto::Sha256 hasher;
  std::vector<char> buffer(64 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = input.gcount();
    if (got > 0) {
      hasher.Update(buffer.data(), static_cast<std::size_t>(got));
    }
  }
  unsigned char digest[crypto::kSha256DigestSize];
  hasher.Final(digest);
  return crypto::ToHex(digest, crypto::kSha256DigestSize);
}

std::string ReadWholeFile(const std::string& path) {
  std::string content;
  test_support::ReadFile(path, &content);
  return content;
}

// ---- 下载发布时刻的确定性注入点 --------------------------------------------
//
// 真实竞态是"目标在下载开始之后、发布之前才被创建"，靠 sleep 猜时机既不稳定
// 也不可复现。PublishNoReplace 的第一步就是 link(temp, target)，而 file_io 已经
// 把这个 syscall 做成了可注入点（它本来就是为"三个发布分支"准备的）。注入的
// 钩子在**发布那一刻**先把目标造出来，再调用真的 link()——内核返回的 EEXIST
// 就是真实竞态里会得到的那一个结果。
// file_io 的发布原语注入点：与 tests/unit/file_io_test.cpp 用的是同一个 seam。
namespace file_io_syscalls = backupproject::file_io_syscalls;

const char* const kRaceSentinel = "SENTINEL: another process owns this path\n";
const char* g_race_target = nullptr;
int g_race_injections = 0;
file_io_syscalls::LinkFn g_real_link = nullptr;

int InjectTargetBeforePublish(const char* existing_path, const char* new_path) {
  if (g_race_target != nullptr && new_path != nullptr &&
      std::strcmp(new_path, g_race_target) == 0 &&
      ::access(new_path, F_OK) != 0) {
    const int fd = ::open(new_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      (void)::write(fd, kRaceSentinel, std::strlen(kRaceSentinel));
      ::close(fd);
      g_race_injections += 1;
    }
  }
  return g_real_link(existing_path, new_path);
}

// 目录里没有以 prefix 开头的条目：用来证明下载没有留下自己的临时文件。
bool NoEntriesWithPrefix(const std::string& directory,
                         const std::string& prefix) {
  for (const std::string& name : test_support::DirEntries(directory)) {
    if (name.compare(0, prefix.size(), prefix) == 0) {
      return false;
    }
  }
  return true;
}

// 递归收集后缀是 suffix 的文件：用来证明服务端没有留下上传中间产物。
void CollectFilesWithSuffix(const std::string& directory,
                            const std::string& suffix,
                            std::vector<std::string>* found) {
  for (const std::string& name : test_support::DirEntries(directory)) {
    const std::string path = directory + "/" + name;
    struct stat info;
    if (!test_support::StatOf(path, &info)) {
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      CollectFilesWithSuffix(path, suffix, found);
      continue;
    }
    if (path.size() >= suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
      found->push_back(path);
    }
  }
}

struct Fixture {
  net::RemoteServerConfig config;
  std::string base;
  std::string root;
};

bool SetupFixture(Fixture* fixture, const std::string& name) {
  fixture->base = test_support::FreshDir(name);
  if (fixture->base.empty()) {
    return false;
  }
  fixture->root = fixture->base + "/data";
  const std::string secret = RandomHex(32);
  if (secret.empty() ||
      !test_support::WriteFile(fixture->base + "/secrets.env",
                               "BACKUP_TOKEN_SECRET=" + secret + "\n", 0600)) {
    return false;
  }
  fixture->config.bind_address = "127.0.0.1";
  fixture->config.port = 0;
  fixture->config.root_directory = fixture->root;
  fixture->config.database_path = fixture->base + "/state/metadata.sqlite3";
  fixture->config.secret_file_path = fixture->base + "/secrets.env";
  fixture->config.quiet = true;
  fixture->config.worker_count = 2;
  return true;
}

}  // namespace

int main() {
  test_support::Section("CLI 1. 会话：ping / register / login / logout");
  {
    Fixture fixture;
    test_support::Check(SetupFixture(&fixture, "client-session"),
                        "CLI T1 夹具就绪");
    net::RemoteServer server;
    std::string error;
    test_support::Check(
        server.Configure(fixture.config, &error) && server.Start(&error),
        "CLI T1 服务端启动", error);
    std::string run_error;
    bool run_result = true;
    std::thread runner([&server, &run_error, &run_result] {
      run_result = server.Run(&run_error);
    });

    net::RemoteArchiveClient client;
    net::RemoteEndpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = server.bound_port();
    test_support::Check(client.Connect(endpoint, &error), "CLI T1 连接成功",
                        error);
    std::string software;
    std::uint16_t version = 0;
    std::uint64_t server_time = 0;
    test_support::Check(client.Ping(&software, &version, &server_time, &error) &&
                            software == net::kServerSoftwareName &&
                            version == net::kProtocolVersion && server_time > 0,
                        "CLI T1 ping 往返", error);

    std::vector<net::RemoteSnapshotInfo> snapshots;
    test_support::Check(!client.List(&snapshots, &error),
                        "CLI T1 未登录时 list 失败");
    test_support::Check(error.find("session") != std::string::npos ||
                            !error.empty(),
                        "CLI T1 未登录时给出可读原因");

    const std::string username = "cli-" + RandomHex(4);
    const std::string password = RandomHex(16);
    test_support::Check(client.Register(username, password, &error),
                        "CLI T1 注册成功", error);
    error.clear();
    test_support::Check(!client.Register(username, password, &error),
                        "CLI T1 重复注册失败");
    error.clear();
    test_support::Check(!client.Login(username, password + "x", &error),
                        "CLI T1 口令错误登录失败");
    error.clear();
    test_support::Check(client.Login(username, password, &error) &&
                            client.authenticated(),
                        "CLI T1 登录成功", error);
    snapshots.clear();
    test_support::Check(client.List(&snapshots, &error) && snapshots.empty(),
                        "CLI T1 新账号列表为空", error);
    test_support::Check(client.Logout(&error) && !client.authenticated(),
                        "CLI T1 登出之后会话结束", error);
    error.clear();
    test_support::Check(!client.List(&snapshots, &error),
                        "CLI T1 登出之后 list 再次失败");

    client.Disconnect();
    server.RequestStop();
    runner.join();
    test_support::Check(run_result, "CLI T1 服务端正常停止", run_error);
    server.Stop();
  }

  test_support::Section("CLI 2. 上传 / 下载 / 删除（真实文件）");
  {
    Fixture fixture;
    SetupFixture(&fixture, "client-transfer");
    net::RemoteServer server;
    std::string error;
    server.Configure(fixture.config, &error);
    server.Start(&error);
    std::string run_error;
    bool run_result = true;
    std::thread runner([&server, &run_error, &run_result] {
      run_result = server.Run(&run_error);
    });

    // 300 KiB 的本地文件（超过一个 256 KiB 的块，因此一定走多块路径）。
    const std::string local_path = fixture.base + "/local.bak";
    const std::string content = RandomHex(150 * 1024);
    test_support::Check(test_support::WriteFile(local_path, content, 0600),
                        "CLI T2 准备本地归档");
    const std::string expected_sha = Sha256OfFile(local_path);

    net::RemoteArchiveClient client;
    net::RemoteEndpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = server.bound_port();
    client.Connect(endpoint, &error);
    const std::string username = "xfer-" + RandomHex(4);
    const std::string password = RandomHex(16);
    client.Register(username, password, &error);
    test_support::Check(client.Login(username, password, &error),
                        "CLI T2 登录", error);

    std::vector<net::RemoteTransferProgress> progress;
    const auto recorder = [&progress](const net::RemoteTransferProgress& step) {
      progress.push_back(step);
    };
    net::RemoteSnapshotInfo uploaded;
    test_support::Check(client.UploadArchiveFile(local_path, "local.bak",
                                                 recorder, &uploaded, &error),
                        "CLI T2 上传成功", error);
    test_support::Check(uploaded.sha256 == expected_sha &&
                            uploaded.size_bytes == content.size() &&
                            net::IsValidSnapshotId(uploaded.snapshot_id, &error),
                        "CLI T2 判别：上传回来的摘要 / 长度与本地文件一致");
    test_support::Check(progress.size() >= 2 &&
                            progress.front().bytes_done == 0 &&
                            progress.back().bytes_done == content.size() &&
                            progress.back().bytes_total == content.size(),
                        "CLI T2 判别：进度回调从 0 走到总量");

    std::vector<net::RemoteSnapshotInfo> snapshots;
    test_support::Check(client.List(&snapshots, &error) &&
                            snapshots.size() == 1 &&
                            snapshots[0].snapshot_id == uploaded.snapshot_id &&
                            snapshots[0].display_name == "local.bak",
                        "CLI T2 列表里有这一条", error);

    const std::string target = fixture.base + "/downloaded.bak";
    net::RemoteSnapshotInfo downloaded;
    test_support::Check(client.DownloadArchiveFile(uploaded.snapshot_id, target,
                                                   false, recorder, &downloaded,
                                                   &error),
                        "CLI T2 下载成功", error);
    test_support::Check(ReadWholeFile(target) == content &&
                            Sha256OfFile(target) == expected_sha,
                        "CLI T2 判别：下载回来的文件与本地文件逐字节一致");
    test_support::Check(!test_support::Exists(target + ".part"),
                        "CLI T2 判别：下载之后没有留下 .part");

    const std::string before = ReadWholeFile(target);
    error.clear();
    test_support::Check(!client.DownloadArchiveFile(uploaded.snapshot_id, target,
                                                    false, nullptr, nullptr,
                                                    &error),
                        "CLI T2 默认不覆盖已存在的目标");
    test_support::Check(ReadWholeFile(target) == before,
                        "CLI T2 判别：被拒绝的下载没有动过目标文件");
    error.clear();
    test_support::Check(client.DownloadArchiveFile(uploaded.snapshot_id, target,
                                                   true, nullptr, nullptr,
                                                   &error),
                        "CLI T2 --force 时允许覆盖", error);

    test_support::Check(client.Delete(uploaded.snapshot_id, &error),
                        "CLI T2 删除成功", error);
    snapshots.clear();
    client.List(&snapshots, &error);
    test_support::Check(snapshots.empty(), "CLI T2 删除之后列表为空");
    error.clear();
    test_support::Check(!client.Delete(uploaded.snapshot_id, &error),
                        "CLI T2 再删一次失败（NOT_FOUND）");

    client.Disconnect();
    server.RequestStop();
    runner.join();
    server.Stop();
  }

  test_support::Section("CLI 3. 失败路径：不上传空文件、不发布坏下载");
  {
    Fixture fixture;
    SetupFixture(&fixture, "client-failures");
    net::RemoteServer server;
    std::string error;
    server.Configure(fixture.config, &error);
    server.Start(&error);
    std::string run_error;
    bool run_result = true;
    std::thread runner([&server, &run_error, &run_result] {
      run_result = server.Run(&run_error);
    });

    net::RemoteArchiveClient client;
    net::RemoteEndpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = server.bound_port();
    client.Connect(endpoint, &error);
    const std::string username = "fail-" + RandomHex(4);
    const std::string password = RandomHex(16);
    client.Register(username, password, &error);
    client.Login(username, password, &error);

    error.clear();
    test_support::Check(!client.UploadArchiveFile(fixture.base + "/missing.bak",
                                                  "missing.bak", nullptr,
                                                  nullptr, &error),
                        "CLI T3 不存在的文件上传失败");
    const std::string empty_path = fixture.base + "/empty.bak";
    test_support::WriteFile(empty_path, std::string(), 0600);
    error.clear();
    test_support::Check(!client.UploadArchiveFile(empty_path, "empty.bak",
                                                  nullptr, nullptr, &error),
                        "CLI T3 空文件上传失败");

    const std::string local_path = fixture.base + "/good.bak";
    test_support::WriteFile(local_path, RandomHex(2048), 0600);
    net::RemoteSnapshotInfo uploaded;
    client.UploadArchiveFile(local_path, "good.bak", nullptr, &uploaded, &error);

    error.clear();
    net::RemoteSnapshotInfo ignored;
    test_support::Check(!client.DownloadArchiveFile(std::string(32, 'a'),
                                                    fixture.base + "/nope.bak",
                                                    false, nullptr, &ignored,
                                                    &error),
                        "CLI T3 下载不存在的快照失败");

    // 把服务端的 blob 换成同长度的另一份内容：客户端必须拒绝发布。
    const std::string blob = fixture.root + "/users/1/" +
                             uploaded.snapshot_id + ".bak";
    test_support::Check(test_support::WriteFile(
                            blob, std::string(uploaded.size_bytes, 'Z'), 0600),
                        "CLI T3 夹具：篡改服务端 blob（长度不变）");
    const std::string tampered_target = fixture.base + "/tampered.bak";
    error.clear();
    test_support::Check(!client.DownloadArchiveFile(uploaded.snapshot_id,
                                                    tampered_target, false,
                                                    nullptr, &ignored, &error),
                        "CLI T3 篡改过的 blob 下载被拒绝", error);
    test_support::Check(!test_support::Exists(tampered_target) &&
                            !test_support::Exists(tampered_target + ".part"),
                        "CLI T3 判别：失败的下载没有发布文件，也没有留下 .part");

    client.Disconnect();
    server.RequestStop();
    runner.join();
    server.Stop();
  }

  test_support::Section("CLI 4. 下载发布：不覆盖由内核保证（含 TOCTOU 竞态）");
  {
    Fixture fixture;
    SetupFixture(&fixture, "client-publish");
    net::RemoteServer server;
    std::string error;
    server.Configure(fixture.config, &error);
    server.Start(&error);
    std::string run_error;
    bool run_result = true;
    std::thread runner([&server, &run_error, &run_result] {
      run_result = server.Run(&run_error);
    });

    net::RemoteArchiveClient client;
    net::RemoteEndpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = server.bound_port();
    client.Connect(endpoint, &error);
    const std::string username = "pub-" + RandomHex(4);
    const std::string password = RandomHex(16);
    client.Register(username, password, &error);
    client.Login(username, password, &error);

    const std::string payload = RandomHex(64 * 1024);
    const std::string local_path = fixture.base + "/publish.bak";
    test_support::Check(test_support::WriteFile(local_path, payload, 0600),
                        "CLI T4 夹具：本地归档已写好");
    net::RemoteSnapshotInfo uploaded;
    test_support::Check(client.UploadArchiveFile(local_path, "publish.bak",
                                                 nullptr, &uploaded, &error),
                        "CLI T4 上传夹具归档", error);
    test_support::Check(uploaded.size_bytes == payload.size(),
                        "CLI T4 判别：上传回来的长度与本地一致");

    // (a) TOCTOU：目标在"下载开始之后、发布之前"才被另一个进程创建。
    //     老实现最后走的是普通 rename(part, target)，会把目标直接覆盖掉。
    const std::string race_target = fixture.base + "/race.bak";
    test_support::Check(!test_support::Exists(race_target),
                        "CLI T4 夹具：下载开始前目标还不存在");
    g_race_target = race_target.c_str();
    g_race_injections = 0;
    g_real_link = file_io_syscalls::LinkHook();
    file_io_syscalls::LinkHook() = &InjectTargetBeforePublish;
    error.clear();
    const bool raced =
        client.DownloadArchiveFile(uploaded.snapshot_id, race_target, false,
                                   nullptr, nullptr, &error);
    file_io_syscalls::LinkHook() = g_real_link;
    g_race_target = nullptr;
    test_support::Check(!raced,
                        "CLI T4 判别：发布前才出现的目标让这次下载失败", error);
    test_support::Check(g_race_injections == 1,
                        "CLI T4 判别：注入正好发生在发布那一刻");
    std::string race_content;
    test_support::ReadFile(race_target, &race_content);
    test_support::Check(race_content == kRaceSentinel,
                        "CLI T4 判别：目标仍是 SENTINEL，一个字节都没被覆盖");
    test_support::Check(NoEntriesWithPrefix(fixture.base, "race.bak.part-"),
                        "CLI T4 判别：被拒绝的发布没有留下唯一临时文件");
    // 发布失败之后连接必须仍然可用（服务端已经收到 DOWNLOAD_END）。
    error.clear();
    net::RemoteSnapshotInfo still_usable;
    const std::string second_target = fixture.base + "/second.bak";
    test_support::Check(client.DownloadArchiveFile(uploaded.snapshot_id,
                                                   second_target, false, nullptr,
                                                   &still_usable, &error),
                        "CLI T4 判别：一次被拒绝的发布之后连接仍然可用", error);

    // (b) 固定的 target + ".part" 是用户自己的文件：新实现不再占用这个名字，
    //     所以既不能截断它，也不能删掉它（老实现会 O_TRUNC 再 rename 走）。
    const std::string fixed_target = fixture.base + "/fixed.bak";
    const std::string fixed_part = fixed_target + ".part";
    const std::string part_sentinel = "SENTINEL: pre-existing .part file\n";
    test_support::Check(
        test_support::WriteFile(fixed_part, part_sentinel, 0600),
        "CLI T4 夹具：目标旁边预先放一个 target.part");
    error.clear();
    net::RemoteSnapshotInfo downloaded;
    test_support::Check(client.DownloadArchiveFile(
                            uploaded.snapshot_id, fixed_target, false, nullptr,
                            &downloaded, &error),
                        "CLI T4 有 target.part 时下载正常完成", error);
    std::string fixed_content;
    std::string part_content;
    test_support::ReadFile(fixed_target, &fixed_content);
    test_support::ReadFile(fixed_part, &part_content);
    test_support::Check(fixed_content == payload &&
                            Sha256OfFile(fixed_target) == uploaded.sha256,
                        "CLI T4 判别：下载回来的字节与上传的一致");
    test_support::Check(part_content == part_sentinel,
                        "CLI T4 判别：用户已有的 target.part 一字未动");
    test_support::Check(NoEntriesWithPrefix(fixture.base, "fixed.bak.part-"),
                        "CLI T4 判别：成功的下载没有留下自己的临时文件");

    // (c) --force：明确同意覆盖时才做原子替换，同样不留临时文件。
    test_support::Check(
        test_support::WriteFile(fixed_target, RandomHex(4096), 0600),
        "CLI T4 夹具：把目标换成另一份内容");
    error.clear();
    net::RemoteSnapshotInfo forced;
    test_support::Check(client.DownloadArchiveFile(uploaded.snapshot_id,
                                                   fixed_target, true, nullptr,
                                                   &forced, &error),
                        "CLI T4 --force 覆盖同一个目标成功", error);
    std::string forced_content;
    std::string part_after_force;
    test_support::ReadFile(fixed_target, &forced_content);
    test_support::ReadFile(fixed_part, &part_after_force);
    test_support::Check(forced_content == payload &&
                            Sha256OfFile(fixed_target) == uploaded.sha256,
                        "CLI T4 判别：覆盖之后的内容就是下载回来的那一份");
    test_support::Check(part_after_force == part_sentinel,
                        "CLI T4 判别：覆盖发布也没有动用户的 target.part");
    test_support::Check(NoEntriesWithPrefix(fixture.base, "fixed.bak.part-"),
                        "CLI T4 判别：覆盖发布之后没有残留临时文件");

    client.Disconnect();
    server.RequestStop();
    runner.join();
    server.Stop();
  }

  test_support::Section(
      "CLI 5. 上传事务：UPLOAD_BEGIN 之后的本地失败必须终止事务");
  {
    Fixture fixture;
    SetupFixture(&fixture, "client-upload-abort");
    net::RemoteServer server;
    std::string error;
    server.Configure(fixture.config, &error);
    server.Start(&error);
    std::string run_error;
    bool run_result = true;
    std::thread runner([&server, &run_error, &run_result] {
      run_result = server.Run(&run_error);
    });

    net::RemoteArchiveClient client;
    net::RemoteEndpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = server.bound_port();
    client.Connect(endpoint, &error);
    const std::string username = "upabort-" + RandomHex(4);
    const std::string password = RandomHex(16);
    client.Register(username, password, &error);
    client.Login(username, password, &error);

    // 夹具：一个比一个分块大的归档。它会在"UPLOAD_BEGIN 已经被接受、第一个
    // 字节还没读"的那一刻被截短——客户端正好在这个位置回调进度
    // （{"upload", 0, size}），所以触发点是确定的，不需要 sleep。
    const std::string shrink_path = fixture.base + "/shrink.bak";
    test_support::Check(
        test_support::WriteFile(shrink_path, RandomHex(200 * 1024), 0600),
        "CLI T5 夹具：本地归档已写好");
    int truncations = 0;
    const std::string shrink_target = shrink_path;
    const net::RemoteProgressCallback shrink_on_begin =
        [&truncations, shrink_target](const net::RemoteTransferProgress& p) {
          if (p.phase != "upload" || p.bytes_done != 0) {
            return;
          }
          if (::truncate(shrink_target.c_str(), 0) == 0) {
            truncations += 1;
          }
        };
    error.clear();
    net::RemoteSnapshotInfo uploaded;
    const bool upload_ok =
        client.UploadArchiveFile(shrink_path, "shrink.bak", shrink_on_begin,
                                 &uploaded, &error);
    test_support::Check(!upload_ok,
                        "CLI T5 判别：中途被截短的本地文件让上传失败");
    test_support::Check(truncations == 1,
                        "CLI T5 判别：截短正好发生在 UPLOAD_BEGIN 之后、"
                        "第一个字节之前");
    test_support::Check(error.find("shrank") != std::string::npos,
                        "CLI T5 判别：报出来的是本地那个错误", error);
    test_support::Check(client.last_status() == 0,
                        "CLI T5 判别：这不是服务端的状态拒绝");
    test_support::Check(client.session_resumable() && !client.connected(),
                        "CLI T5 判别：连接被主动关掉，token 留在手里");

    // 关键一条：紧接着的 List 必须成功。老实现把服务端留在
    // UPLOAD_IN_PROGRESS，同一个 socket 上的 LIST 会被按"不在已认证状态"
    // 拒绝，用户只能重新登录。
    error.clear();
    std::vector<net::RemoteSnapshotInfo> snapshots;
    test_support::Check(client.List(&snapshots, &error),
                        "CLI T5 判别：上传失败之后同一个客户端还能立刻 List",
                        error);
    test_support::Check(snapshots.empty(),
                        "CLI T5 判别：失败的上传没有留下元数据行");

    // 事务真的结束了：下一次上传正常完成。
    const std::string good_path = fixture.base + "/good.bak";
    test_support::Check(
        test_support::WriteFile(good_path, RandomHex(32 * 1024), 0600),
        "CLI T5 夹具：一份正常的本地归档");
    error.clear();
    net::RemoteSnapshotInfo good_uploaded;
    test_support::Check(client.UploadArchiveFile(good_path, "good.bak", nullptr,
                                                 &good_uploaded, &error),
                        "CLI T5 判别：事务终止之后下一次上传正常完成", error);
    test_support::Check(!good_uploaded.snapshot_id.empty(),
                        "CLI T5 判别：上传回来的快照 id 有效");

    client.Disconnect();
    server.RequestStop();
    runner.join();
    server.Stop();
    // 服务端的上传临时文件必须已经消失（AbortUpload）。这一步放在服务端完全
    // 停下之后断言：worker 都 join 过了，清理不可能还没跑到。
    std::vector<std::string> leftovers;
    CollectFilesWithSuffix(fixture.root, ".part", &leftovers);
    test_support::Check(leftovers.empty(),
                        "CLI T5 判别：服务端没有留下上传临时文件");
  }

  test_support::Section(
      "CLI 6. 下载事务：DOWNLOAD_BEGIN 之后的本地失败必须终止事务");
  {
    Fixture fixture;
    SetupFixture(&fixture, "client-download-abort");
    net::RemoteServer server;
    std::string error;
    server.Configure(fixture.config, &error);
    server.Start(&error);
    std::string run_error;
    bool run_result = true;
    std::thread runner([&server, &run_error, &run_result] {
      run_result = server.Run(&run_error);
    });

    net::RemoteArchiveClient client;
    net::RemoteEndpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = server.bound_port();
    client.Connect(endpoint, &error);
    const std::string username = "dnabort-" + RandomHex(4);
    const std::string password = RandomHex(16);
    client.Register(username, password, &error);
    client.Login(username, password, &error);

    const std::string payload = RandomHex(8 * 1024);
    const std::string local_path = fixture.base + "/abort-source.bak";
    test_support::Check(test_support::WriteFile(local_path, payload, 0600),
                        "CLI T6 夹具：本地归档已写好");
    net::RemoteSnapshotInfo uploaded;
    test_support::Check(client.UploadArchiveFile(local_path, "abort-source.bak",
                                                 nullptr, &uploaded, &error),
                        "CLI T6 上传夹具归档", error);

    // DOWNLOAD_BEGIN 会成功（快照真的存在），随后 mkstemp 必然失败：目标所在
    // 的目录根本不存在。触发点是确定的，不需要 sleep、也不需要竞态。
    const std::string bad_target =
        fixture.base + "/no-such-directory/deeper/out.bak";
    error.clear();
    net::RemoteSnapshotInfo downloaded;
    const bool download_ok = client.DownloadArchiveFile(
        uploaded.snapshot_id, bad_target, false, nullptr, &downloaded, &error);
    test_support::Check(!download_ok,
                        "CLI T6 判别：目标目录不存在导致下载失败");
    test_support::Check(error.find("temporary file") != std::string::npos,
                        "CLI T6 判别：报出来的是本地建临时文件失败", error);
    test_support::Check(client.last_status() == 0,
                        "CLI T6 判别：这不是服务端的状态拒绝");
    test_support::Check(client.connected(),
                        "CLI T6 判别：下载事务用协议收尾，连接没有被丢掉");
    test_support::Check(!test_support::Exists(bad_target) &&
                            NoEntriesWithPrefix(fixture.base, "out.bak.part-"),
                        "CLI T6 判别：失败的下载一个文件都没留下");

    // 关键一条：紧接着的 List 必须成功。老实现把服务端留在
    // DOWNLOAD_IN_PROGRESS，同一个 socket 上的 LIST 会被按"不在已认证状态"
    // 拒绝。
    error.clear();
    std::vector<net::RemoteSnapshotInfo> snapshots;
    test_support::Check(client.List(&snapshots, &error),
                        "CLI T6 判别：下载失败之后同一条连接仍然可用", error);
    test_support::Check(snapshots.size() == 1,
                        "CLI T6 判别：快照列表仍然正常");
    // 事务真的结束了：同一条连接上还能继续下载。
    const std::string good_target = fixture.base + "/abort-after.bak";
    error.clear();
    net::RemoteSnapshotInfo again;
    test_support::Check(client.DownloadArchiveFile(uploaded.snapshot_id,
                                                   good_target, false, nullptr,
                                                   &again, &error),
                        "CLI T6 判别：收尾之后还能正常下载", error);
    test_support::Check(Sha256OfFile(good_target) == uploaded.sha256,
                        "CLI T6 判别：收尾之后下载回来的字节仍然正确");

    client.Disconnect();
    server.RequestStop();
    runner.join();
    server.Stop();
  }

  return test_support::Finish("remote_client_test");
}

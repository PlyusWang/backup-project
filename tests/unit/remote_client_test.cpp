// tests/unit/remote_client_test.cpp
//
// PR #20：RemoteArchiveClient 的专项测试。
//
// 服务端是真的（Run() 起固定 worker 池），客户端是真的，文件系统是真的。
// 这里断言的是"客户端库拿到手的行为"：会话、进度回调、流式上传下载、
// 默认不覆盖目标、下载失败不发布文件。

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
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

  return test_support::Finish("remote_client_test");
}

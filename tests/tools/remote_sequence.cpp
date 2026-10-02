// tests/tools/remote_sequence.cpp
//
// 真实 BPNET1（跑在 BPSEC1 加密传输之上）上的请求序列驱动器（ECS 真机验证用）。
//
// 它跑的就是人工验收的那一串动作，而且**全程用同一条连接**（这正是人工验收里
// 出问题的地方）：注册 -> 错误口令登录 ×4 -> 正确登录 -> LIST ×10 -> 上传 ->
// LIST -> 下载并逐字节比对 -> 删除快照 -> 用错口令注销（必须失败）-> LIST（必须
// 成功）-> 用对口令注销 -> 再用原凭据登录（必须失败）。
//
// PR #21：服务端在说第一句 BPNET1 之前先做 BPSEC1 握手，客户端**必须**事先知道
// 服务端身份公钥（不做 TOFU）。pin 从 --server-key 或环境变量
// BACKUP_REMOTE_SERVER_KEY 读，取值 "sha256:<指纹>" 或 "hex:<公钥>"，用
// backup-server-keygen --show --key-file <身份私钥文件> 取得。
//
// 另外可以用 --idle-seconds N 在两次 LIST 之间插入 N 秒空闲：服务端默认
// --io-timeout 30，所以 N=31 就能在真机上证明"空闲被服务端关掉之后，第一次操作
// 仍然成功"（修复前这里会以 peer closed 失败并把登录态一起丢掉）。
//
// 口令只从环境变量 BACKUP_REMOTE_PASSWORD 读：不进 argv，也不打印。

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "remote_backup_client.h"

namespace {

namespace net = backupproject::net;

struct Options {
  net::RemoteEndpoint endpoint;
  std::string server_key;
  std::string username;
  std::string upload_path;
  std::string download_path;
  int idle_seconds = 0;
  bool skip_register = false;
};

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "用法: %s --host <地址> --port <端口> --user <用户名> \\\n"
               "          --server-key <sha256:指纹|hex:公钥> \\\n"
               "          --upload <本地文件> --download <目标路径> \\\n"
               "          [--idle-seconds N] [--skip-register]\n"
               "口令从环境变量 BACKUP_REMOTE_PASSWORD 读。\n"
               "服务端 pin 也可以放在环境变量 BACKUP_REMOTE_SERVER_KEY 里\n"
               "（BPSEC1 不做首次连接自动信任，没有 pin 就不能连接）。\n",
               program);
}

bool ParseOptions(int argc, char* argv[], Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    const bool need_value =
        name == "--host" || name == "--port" || name == "--user" ||
        name == "--server-key" || name == "--upload" || name == "--download" ||
        name == "--idle-seconds";
    if (!need_value) {
      if (name == "--skip-register") {
        options->skip_register = true;
        continue;
      }
      std::fprintf(stderr, "未知参数 %s\n", name.c_str());
      return false;
    }
    if (index + 1 >= argc) {
      std::fprintf(stderr, "%s 需要一个值\n", name.c_str());
      return false;
    }
    const std::string value = argv[++index];
    if (name == "--host") {
      options->endpoint.host = value;
    } else if (name == "--port") {
      options->endpoint.port = static_cast<std::uint16_t>(std::atoi(value.c_str()));
    } else if (name == "--user") {
      options->username = value;
    } else if (name == "--server-key") {
      options->server_key = value;
    } else if (name == "--upload") {
      options->upload_path = value;
    } else if (name == "--download") {
      options->download_path = value;
    } else if (name == "--idle-seconds") {
      options->idle_seconds = std::atoi(value.c_str());
    }
  }
  // pin 优先级：--server-key -> 环境变量 BACKUP_REMOTE_SERVER_KEY。与
  // backupctl remote 用的是同一条规则，这样真机验证脚本只要导出一个环境变量。
  options->endpoint.server_key_pin = options->server_key;
  if (options->endpoint.server_key_pin.empty()) {
    const char* from_environment = std::getenv("BACKUP_REMOTE_SERVER_KEY");
    if (from_environment != nullptr && from_environment[0] != '\0') {
      options->endpoint.server_key_pin = from_environment;
    }
  }
  if (options->endpoint.host.empty() || options->endpoint.port == 0 ||
      options->username.empty()) {
    return false;
  }
  if (options->endpoint.server_key_pin.empty()) {
    std::fprintf(stderr,
                 "缺少服务端 pin：--server-key <sha256:指纹|hex:公钥> 或环境"
                 "变量 BACKUP_REMOTE_SERVER_KEY（BPSEC1 不做 TOFU）。\n");
    return false;
  }
  return true;
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(input),
              std::istreambuf_iterator<char>());
  return true;
}

struct Sequence {
  int passed = 0;
  int failed = 0;

  void Check(bool ok, const std::string& label, const std::string& detail) {
    if (ok) {
      ++passed;
      std::printf("  PASS  %s\n", label.c_str());
    } else {
      ++failed;
      std::printf("  FAIL  %s -- %s\n", label.c_str(), detail.c_str());
    }
    std::fflush(stdout);
  }
};

std::string Unauthorized() {
  return net::RemoteStatusMessage(
      static_cast<std::uint32_t>(net::Status::kUnauthorized));
}

}  // namespace

int main(int argc, char* argv[]) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    PrintUsage(argc > 0 ? argv[0] : "remote-sequence");
    return 2;
  }
  const char* password_env = std::getenv("BACKUP_REMOTE_PASSWORD");
  if (password_env == nullptr || password_env[0] == '\0') {
    std::fprintf(stderr, "需要环境变量 BACKUP_REMOTE_PASSWORD\n");
    return 2;
  }
  const std::string password = password_env;

  std::printf("[remote-sequence] endpoint=%s:%u user=%s idle=%d\n",
              options.endpoint.host.c_str(),
              static_cast<unsigned>(options.endpoint.port),
              options.username.c_str(), options.idle_seconds);
  Sequence run;
  net::RemoteArchiveClient client;
  std::string error;

  if (!options.skip_register) {
    net::RemoteArchiveClient registrar;
    std::string register_error;
    const bool connected = registrar.Connect(options.endpoint, &register_error);
    const bool registered =
        connected && registrar.Register(options.username, password, &register_error);
    // 账户已经存在也算通过：同一个序列要能反复跑。
    const bool already = register_error ==
                         net::RemoteStatusMessage(static_cast<std::uint32_t>(
                             net::Status::kAlreadyExists));
    run.Check(registered || already, "SEQ-01 REGISTER（或已存在）", register_error);
  }

  // ---- 错误口令 ×4：每一次都必须是 UNAUTHORIZED，不能断线、不能静默 ----
  {
    net::RemoteArchiveClient probe;
    std::string connect_error;
    const bool connected = probe.Connect(options.endpoint, &connect_error);
    int unauthorized = 0;
    std::string last_error;
    for (int attempt = 0; attempt < 4 && connected; ++attempt) {
      std::string attempt_error;
      const bool ok = probe.Login(options.username, password + "-wrong",
                                  &attempt_error);
      if (!ok && attempt_error == Unauthorized()) {
        ++unauthorized;
      }
      last_error = attempt_error;
    }
    run.Check(connected && unauthorized == 4,
              "SEQ-02 错误口令登录 ×4 全部 UNAUTHORIZED", last_error);
  }

  // ---- 正确登录 + LIST ×10 ----
  const bool logged_in = client.Connect(options.endpoint, &error) &&
                         client.Login(options.username, password, &error);
  run.Check(logged_in && client.authenticated(), "SEQ-03 正确登录", error);
  if (!logged_in) {
    std::printf("[remote-sequence] SEQUENCE_FAILED\n");
    return 1;
  }
  {
    int listed = 0;
    std::string last_error;
    for (int index = 0; index < 10; ++index) {
      std::vector<net::RemoteSnapshotInfo> snapshots;
      if (client.List(&snapshots, &last_error)) {
        ++listed;
      }
    }
    run.Check(listed == 10 && client.authenticated(),
              "SEQ-04 LIST ×10 全部成功且会话有效", last_error);
  }

  // ---- 空闲（默认不插；--idle-seconds 31 对上产品默认的 30 秒 io-timeout）----
  if (options.idle_seconds > 0) {
    std::printf("[remote-sequence] 空闲 %d 秒……\n", options.idle_seconds);
    ::sleep(static_cast<unsigned>(options.idle_seconds));
    std::vector<net::RemoteSnapshotInfo> snapshots;
    std::string list_error;
    const bool ok = client.List(&snapshots, &list_error) &&
                    client.authenticated();
    run.Check(ok, "SEQ-05 空闲之后第一次 LIST 就成功（会话不丢）", list_error);
  }

  // ---- 上传 / LIST / 下载 / 删除快照 ----
  std::string snapshot_id;
  if (!options.upload_path.empty()) {
    net::RemoteSnapshotInfo uploaded;
    const bool uploaded_ok = client.UploadArchiveFile(
        options.upload_path, "ecs-sequence.bak", nullptr, &uploaded, &error);
    run.Check(uploaded_ok, "SEQ-06 上传真实文件", error);
    snapshot_id = uploaded.snapshot_id;
    std::vector<net::RemoteSnapshotInfo> snapshots;
    std::string list_error;
    bool seen = false;
    if (client.List(&snapshots, &list_error)) {
      for (const net::RemoteSnapshotInfo& info : snapshots) {
        if (info.snapshot_id == snapshot_id) {
          seen = true;
        }
      }
    }
    run.Check(seen, "SEQ-07 列表里出现刚上传的那一条", list_error);
  }
  if (!options.download_path.empty() && !snapshot_id.empty()) {
    net::RemoteSnapshotInfo downloaded;
    const bool downloaded_ok =
        client.DownloadArchiveFile(snapshot_id, options.download_path, true,
                                   nullptr, &downloaded, &error);
    std::string local;
    std::string fetched;
    const bool identical = downloaded_ok &&
                           ReadWholeFile(options.upload_path, &local) &&
                           ReadWholeFile(options.download_path, &fetched) &&
                           local == fetched;
    run.Check(identical, "SEQ-08 下载回来的字节与上传的一致", error);
    if (!snapshot_id.empty()) {
      run.Check(client.Delete(snapshot_id, &error), "SEQ-09 删除快照", error);
    }
  }

  // ---- 注销：错口令必须失败且不动会话；对口令成功；之后原凭据登录失败 ----
  {
    std::string wrong_error;
    const bool wrong_rejected =
        !client.DeleteAccount(password + "-wrong", &wrong_error);
    run.Check(wrong_rejected && client.authenticated(),
              "SEQ-10 错误口令注销被拒绝，会话保留", wrong_error);
    std::vector<net::RemoteSnapshotInfo> snapshots;
    std::string list_error;
    run.Check(client.List(&snapshots, &list_error) && client.authenticated(),
              "SEQ-11 被拒绝之后 LIST 仍然成功", list_error);

    std::string delete_error;
    const bool deleted = client.DeleteAccount(password, &delete_error);
    run.Check(deleted && !client.authenticated(),
              "SEQ-12 正确口令注销成功，会话立即清空", delete_error);

    net::RemoteArchiveClient fresh;
    std::string relogin_error;
    const bool relogin_failed =
        fresh.Connect(options.endpoint, &error) &&
        !fresh.Login(options.username, password, &relogin_error) &&
        relogin_error == Unauthorized();
    run.Check(relogin_failed, "SEQ-13 注销之后原凭据再也登录不上", relogin_error);
  }

  std::printf("[remote-sequence] passed=%d failed=%d\n", run.passed, run.failed);
  if (run.failed != 0) {
    std::printf("[remote-sequence] SEQUENCE_FAILED\n");
    return 1;
  }
  std::printf("[remote-sequence] SEQUENCE_ALL_PASS\n");
  return 0;
}

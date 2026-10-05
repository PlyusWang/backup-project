// tests/unit/remote_sequence_test.cpp
//
// PR #20（人工验收缺陷修复）：请求序列回归。
//
// 人工验收看到的现象是"奇数次失败、偶数次有响应"：
//
//     第 1 次操作  cannot read the response: peer closed the connection
//     第 2 次操作  正常（或 UNAUTHORIZED）
//
// 根因（本文件第一版就是它的复现）：服务端会在 io_timeout 之后主动关掉**空闲**
// 连接（慢连接保护），而客户端手里那条 fd 依然"有效"——往里写不会立刻报错，
// 响应却永远不会来。客户端随后把整条会话（包括 token）一起丢掉，于是"下一次
// 重新登录就好了"，表现成奇偶交替。
//
// 这个文件用真实的 backup-server + 真实的 RemoteArchiveClient 把修复钉死：
//
//   * 每一次独立用户操作，第一次请求就得到确定结果；
//   * 失败就是失败，**绝不自动重发**（服务端按操作码计数，重发会让计数 +2）；
//   * 一次传输层失败不会让用户"被退出登录"（token 保留，下一次自动恢复会话）；
//   * token 真的无效（过期 / 账户已注销）时，才丢掉它并如实报错。
//
// 夹具刻意把 io_timeout 设成 2 秒：产品默认是 30 秒，机制完全相同，但测试可以
// 在几秒内确定性地复现"空闲被服务端关掉"。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "network_protocol.h"
#include "remote_backup_client.h"
#include "remote_server.h"
#include "remote_test_support.h"
#include "test_support.h"

namespace bp = backupproject;
namespace net = backupproject::net;

namespace {

constexpr int kIdleSeconds = 3;         // > io_timeout_seconds
constexpr unsigned int kIdleTimeout = 2;

std::string RandomHex(std::size_t bytes) {
  std::string raw;
  std::string error;
  if (!bp::crypto::RandomBytes(bytes, &raw, &error)) {
    return std::string();
  }
  return bp::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

struct Fixture {
  net::RemoteServerConfig config;
  std::string base;
  std::string server_log;
  // PR #21：BPSEC1 的服务端身份私钥与客户端 pin 文本（同一次生成）。
  std::string transport_key_file;
  std::string pin;
};

bool SetupFixture(Fixture* fixture, const std::string& name) {
  const std::string base = test_support::FreshDir(name);
  if (base.empty()) {
    return false;
  }
  fixture->base = base;
  fixture->server_log = base + "/server.log";
  const std::string secret = RandomHex(32);
  if (secret.empty() ||
      !test_support::WriteFile(base + "/secrets.env",
                               "BACKUP_TOKEN_SECRET=" + secret + "\n", 0600)) {
    return false;
  }
  // BPSEC1 身份密钥：服务端每个连接的第一步就是它，缺了 Start() 直接失败。
  fixture->transport_key_file = base + "/transport.key";
  net::TransportIdentity identity;
  std::string identity_error;
  if (!remote_test_support::PrepareTransportIdentity(
          fixture->transport_key_file, &identity, &fixture->pin,
          &identity_error)) {
    return false;
  }
  fixture->config.bind_address = "127.0.0.1";
  fixture->config.port = 0;
  fixture->config.root_directory = base + "/data";
  fixture->config.database_path = base + "/state/db.sqlite3";
  fixture->config.secret_file_path = base + "/secrets.env";
  fixture->config.transport_key_file_path = fixture->transport_key_file;
  fixture->config.log_file_path = fixture->server_log;
  fixture->config.io_timeout_seconds = static_cast<int>(kIdleTimeout);
  fixture->config.quiet = true;
  return true;
}

// 每个用例一个自己的服务端 + 一个自己的端点：用例之间不共享任何状态，
// 一个用例失败不会污染下一个。
class ServerFixture {
 public:
  bool Start(const std::string& name) {
    if (!SetupFixture(&fixture_, name)) {
      return false;
    }
    // §33 的生产默认值（5 次失败 -> 锁 60 秒）会让 SEQ-1 的最后一条断言
    // （「六次错误口令之后同一条连接还能正常登录」）与产品合同直接冲突：锁定期间
    // **正确口令也拒绝**（否则攻击者只要在锁定窗口里碰对一次就绕过了节流）。
    // 本套件测的是「一条连接上的请求 / 响应序列」，不是节流语义；节流由
    // scripts/login_throttle_test.sh 专门覆盖（7/7，含「锁定期间正确口令也拒绝」）。
    // 所以这里**显式关掉节流**，而不是把 SEQ-1 的断言改成一句更弱的话。
    // （max_login_failures <= 0 表示不计数，见 server/remote_server.cpp:808/821。）
    fixture_.config.max_login_failures = 0;
    if (!server_.Configure(fixture_.config, &error_)) {
      return false;
    }
    if (!server_.Start(&error_)) {
      return false;
    }
    runner_ = std::thread([this] { run_result_ = server_.Run(&run_error_); });
    endpoint_.host = "127.0.0.1";
    endpoint_.port = server_.bound_port();
    endpoint_.timeout_seconds = 15;
    // BPSEC1 不做 TOFU：这个端点必须带上本次 fixture 的 pin。
    endpoint_.server_key_pin = fixture_.pin;
    return true;
  }

  ~ServerFixture() {
    server_.RequestStop();
    if (runner_.joinable()) {
      runner_.join();
    }
    server_.Stop();
  }

  net::RemoteServer* server() { return &server_; }
  const net::RemoteEndpoint& endpoint() const { return endpoint_; }
  const std::string& error() const { return error_; }

 private:
  Fixture fixture_;
  net::RemoteServer server_;
  net::RemoteEndpoint endpoint_;
  std::thread runner_;
  bool run_result_ = false;
  std::string error_;
  std::string run_error_;
};

std::string UnauthorizedMessage() {
  return net::RemoteStatusMessage(
      static_cast<std::uint32_t>(net::Status::kUnauthorized));
}

bool IsTransportFailure(const std::string& message) {
  return message.find("cannot read the response") != std::string::npos ||
         message.find("cannot send the request") != std::string::npos ||
         message.find("cannot connect") != std::string::npos ||
         message.find("closed") != std::string::npos;
}

}  // namespace

int main() {
  test_support::Section("SEQ-1 错误口令登录 × 6：6/6 都必须拿到 UNAUTHORIZED");
  {
    ServerFixture fixture;
    test_support::Check(fixture.Start("seq-1"), "SEQ-1 夹具就绪",
                        fixture.error());
    net::RemoteArchiveClient client;
    std::string error;
    test_support::Check(client.Connect(fixture.endpoint(), &error),
                        "SEQ-1 连接成功", error);
    test_support::Check(client.Register("seq-user-1", "seq-password-1", &error),
                        "SEQ-1 注册成功", error);
    int unauthorized = 0;
    int transport = 0;
    int unexpected = 0;
    std::string last_error;
    for (int attempt = 0; attempt < 6; ++attempt) {
      std::string attempt_error;
      const bool ok = client.Login("seq-user-1", "wrong-password", &attempt_error);
      last_error = attempt_error;
      if (ok) {
        ++unexpected;
      } else if (attempt_error == UnauthorizedMessage()) {
        ++unauthorized;
      } else if (IsTransportFailure(attempt_error)) {
        ++transport;
      } else {
        ++unexpected;
      }
    }
    test_support::Check(unauthorized == 6 && transport == 0 && unexpected == 0,
                        "SEQ-1 六次错误口令都是 UNAUTHORIZED（没有断线、没有静默）",
                        "unauthorized=" + std::to_string(unauthorized) +
                            " transport=" + std::to_string(transport) +
                            " unexpected=" + std::to_string(unexpected) +
                            " last=" + last_error);
    std::string login_error;
    test_support::Check(client.Login("seq-user-1", "seq-password-1", &login_error) &&
                            client.authenticated(),
                        "SEQ-1 六次失败之后同一条连接仍然能正常登录", login_error);
  }

  test_support::Section("SEQ-2 正确登录 + LIST × 10：10/10，且不重发");
  {
    ServerFixture fixture;
    test_support::Check(fixture.Start("seq-2"), "SEQ-2 夹具就绪",
                        fixture.error());
    net::RemoteArchiveClient client;
    std::string error;
    test_support::Check(client.Connect(fixture.endpoint(), &error) &&
                            client.Register("seq-user-2", "seq-password-2", &error) &&
                            client.Login("seq-user-2", "seq-password-2", &error),
                        "SEQ-2 注册并登录", error);
    const std::uint64_t before =
        fixture.server()->request_count_for_testing(
            static_cast<std::uint16_t>(net::Opcode::kList));
    int passed = 0;
    bool stayed_authenticated = true;
    std::string last_error;
    for (int index = 0; index < 10; ++index) {
      std::vector<net::RemoteSnapshotInfo> listed;
      std::string list_error;
      if (client.List(&listed, &list_error)) {
        ++passed;
      } else {
        last_error = list_error;
      }
      if (!client.authenticated()) {
        stayed_authenticated = false;
      }
    }
    const std::uint64_t after =
        fixture.server()->request_count_for_testing(
            static_cast<std::uint16_t>(net::Opcode::kList));
    test_support::Check(passed == 10 && stayed_authenticated,
                        "SEQ-2 连续 10 次 LIST 全部成功且会话全程有效",
                        "passed=" + std::to_string(passed) + " " + last_error);
    test_support::Check(after - before == 10,
                        "SEQ-2 服务端恰好收到 10 次 LIST（一次都没有重发）",
                        std::to_string(after - before));
  }

  test_support::Section("SEQ-3 交替序列：每一步结果与输入一致");
  {
    ServerFixture fixture;
    test_support::Check(fixture.Start("seq-3"), "SEQ-3 夹具就绪",
                        fixture.error());
    net::RemoteArchiveClient client;
    std::string error;
    test_support::Check(client.Connect(fixture.endpoint(), &error) &&
                            client.Register("seq-user-3", "seq-password-3", &error),
                        "SEQ-3 连接并注册", error);
    bool all_ok = true;
    std::string detail;

    std::string wrong_error;
    const bool wrong_rejected =
        !client.Login("seq-user-3", "nope", &wrong_error) &&
        wrong_error == UnauthorizedMessage();
    all_ok = all_ok && wrong_rejected;
    detail += wrong_rejected ? "" : "wrong-login ";

    std::string right_error;
    const bool right_accepted =
        client.Login("seq-user-3", "seq-password-3", &right_error);
    all_ok = all_ok && right_accepted && client.authenticated();
    detail += right_accepted ? "" : "correct-login ";

    for (int index = 0; index < 2; ++index) {
      std::vector<net::RemoteSnapshotInfo> listed;
      std::string list_error;
      const bool listed_ok = client.List(&listed, &list_error);
      all_ok = all_ok && listed_ok;
      detail += listed_ok ? "" : "list ";
    }

    std::string logout_error;
    client.Logout(&logout_error);
    const bool logged_out = !client.authenticated() && !client.session_resumable();
    all_ok = all_ok && logged_out;
    detail += logged_out ? "" : "logout ";

    std::string wrong_error2;
    const bool wrong_again =
        !client.Login("seq-user-3", "nope", &wrong_error2) &&
        wrong_error2 == UnauthorizedMessage();
    all_ok = all_ok && wrong_again;
    detail += wrong_again ? "" : "wrong-login-2 ";

    std::string right_error2;
    const bool right_again =
        client.Login("seq-user-3", "seq-password-3", &right_error2);
    all_ok = all_ok && right_again;

    std::vector<net::RemoteSnapshotInfo> listed;
    std::string last_error;
    const bool final_list = client.List(&listed, &last_error);
    all_ok = all_ok && final_list;
    test_support::Check(all_ok,
                        "SEQ-3 错误登录 / 正确登录 / LIST×2 / 退出 / 错误登录 / "
                        "正确登录 / LIST 全部符合预期",
                        detail + last_error);
  }

  test_support::Section("SEQ-4 注销账户序列：错口令不动会话，对口令才清空");
  {
    ServerFixture fixture;
    test_support::Check(fixture.Start("seq-4"), "SEQ-4 夹具就绪",
                        fixture.error());
    net::RemoteArchiveClient client;
    std::string error;
    test_support::Check(client.Connect(fixture.endpoint(), &error) &&
                            client.Register("seq-user-4", "seq-password-4", &error) &&
                            client.Login("seq-user-4", "seq-password-4", &error),
                        "SEQ-4 注册并登录", error);
    std::vector<net::RemoteSnapshotInfo> listed;
    test_support::Check(client.List(&listed, &error), "SEQ-4 注销前 LIST 成功",
                        error);

    std::string wrong_error;
    const bool wrong_delete =
        !client.DeleteAccount("wrong-password", &wrong_error);
    test_support::Check(wrong_delete &&
                            client.authenticated() &&
                            client.session_resumable(),
                        "SEQ-4 错误口令注销被拒绝，且会话与 token 都保留",
                        wrong_error);

    std::string list_error;
    const bool still_works = client.List(&listed, &list_error);
    test_support::Check(still_works && client.authenticated(),
                        "SEQ-4 被拒绝之后 LIST 仍然成功（会话没有被打断）",
                        list_error);

    std::string delete_error;
    const bool deleted = client.DeleteAccount("seq-password-4", &delete_error);
    test_support::Check(deleted && !client.authenticated() &&
                            !client.session_resumable(),
                        "SEQ-4 正确口令注销成功，会话与 token 立即清空",
                        delete_error);

    net::RemoteArchiveClient fresh;
    std::string relogin_error;
    test_support::Check(fresh.Connect(fixture.endpoint(), &error) &&
                            !fresh.Login("seq-user-4", "seq-password-4",
                                         &relogin_error) &&
                            relogin_error == UnauthorizedMessage(),
                        "SEQ-4 注销之后原凭据再也登录不上", relogin_error);
  }

  test_support::Section("SEQ-5 20 轮：登录 + LIST，每 5 轮插入一次空闲重连");
  {
    ServerFixture fixture;
    test_support::Check(fixture.Start("seq-5"), "SEQ-5 夹具就绪",
                        fixture.error());
    net::RemoteArchiveClient seed;
    std::string error;
    test_support::Check(seed.Connect(fixture.endpoint(), &error) &&
                            seed.Register("seq-user-5", "seq-password-5", &error),
                        "SEQ-5 预置账户", error);
    seed.Disconnect();

    int rounds_ok = 0;
    std::string first_failure;
    for (int round = 0; round < 20; ++round) {
      net::RemoteArchiveClient client;
      std::string round_error;
      bool round_ok = client.Connect(fixture.endpoint(), &round_error) &&
                      client.Login("seq-user-5", "seq-password-5", &round_error);
      if (round == 0 || round % 5 == 0) {
        // 让服务端按 io_timeout 关掉这条空闲连接，再继续用同一个 client：
        // 修复之后这不该产生任何可见失败。
        ::sleep(kIdleSeconds);
      }
      for (int index = 0; index < 2 && round_ok; ++index) {
        std::vector<net::RemoteSnapshotInfo> listed;
        std::string list_error;
        round_ok = client.List(&listed, &list_error) && client.authenticated();
        if (!round_ok && first_failure.empty()) {
          first_failure = "round " + std::to_string(round) + ": " + list_error;
        }
      }
      if (round_ok) {
        ++rounds_ok;
      } else if (first_failure.empty()) {
        first_failure = "round " + std::to_string(round) + ": " + round_error;
      }
      client.Disconnect();
    }
    test_support::Check(rounds_ok == 20,
                        "SEQ-5 20 轮（含 5 次空闲重连）全部符合预期",
                        "ok=" + std::to_string(rounds_ok) + " " + first_failure);
  }

  test_support::Section("IDLE 服务端关掉空闲连接之后，第一次操作就必须成功");
  {
    ServerFixture fixture;
    test_support::Check(fixture.Start("idle-1"), "IDLE 夹具就绪",
                        fixture.error());
    net::RemoteArchiveClient client;
    std::string error;
    test_support::Check(client.Connect(fixture.endpoint(), &error) &&
                            client.Register("idle-user", "idle-password", &error) &&
                            client.Login("idle-user", "idle-password", &error),
                        "IDLE 注册并登录", error);
    const std::uint64_t before =
        fixture.server()->request_count_for_testing(
            static_cast<std::uint16_t>(net::Opcode::kList));
    ::sleep(kIdleSeconds);
    std::vector<net::RemoteSnapshotInfo> listed;
    std::string list_error;
    const bool listed_ok = client.List(&listed, &list_error);
    test_support::Check(listed_ok,
                        "IDLE-1 空闲超时之后**第一次** LIST 就成功（不再是 peer closed）",
                        list_error);
    test_support::Check(client.authenticated() && client.session_resumable(),
                        "IDLE-2 会话与 token 都还在（没有把用户退出登录）");
    const std::uint64_t after =
        fixture.server()->request_count_for_testing(
            static_cast<std::uint16_t>(net::Opcode::kList));
    test_support::Check(after - before == 1,
                        "IDLE-3 只发了一次 LIST（重连与恢复会话发生在发送之前）",
                        std::to_string(after - before));
  }

  test_support::Section("NORETRY 服务端处理完就断开：如实失败、不重发、会话保留");
  {
    ServerFixture fixture;
    test_support::Check(fixture.Start("noretry"), "NORETRY 夹具就绪",
                        fixture.error());
    net::RemoteArchiveClient client;
    std::string error;
    test_support::Check(client.Connect(fixture.endpoint(), &error) &&
                            client.Register("noretry-user", "noretry-password",
                                            &error) &&
                            client.Login("noretry-user", "noretry-password",
                                         &error),
                        "NORETRY 注册并登录", error);
    const std::uint64_t before =
        fixture.server()->request_count_for_testing(
            static_cast<std::uint16_t>(net::Opcode::kList));
    fixture.server()->FailNextResponseForTesting();
    std::vector<net::RemoteSnapshotInfo> listed;
    std::string first_error;
    const bool first = client.List(&listed, &first_error);
    test_support::Check(!first && IsTransportFailure(first_error),
                        "NORETRY-1 读不到响应就如实失败（不假装成功）", first_error);
    const std::uint64_t mid =
        fixture.server()->request_count_for_testing(
            static_cast<std::uint16_t>(net::Opcode::kList));
    test_support::Check(mid - before == 1,
                        "NORETRY-2 客户端没有自动重发（服务端只收到 1 次 LIST）",
                        std::to_string(mid - before));
    test_support::Check(client.session_resumable(),
                        "NORETRY-3 传输层失败之后 token 仍然保留（不等于退出登录）");

    std::vector<net::RemoteSnapshotInfo> listed_again;
    std::string second_error;
    const bool second = client.List(&listed_again, &second_error);
    const std::uint64_t after =
        fixture.server()->request_count_for_testing(
            static_cast<std::uint16_t>(net::Opcode::kList));
    test_support::Check(second && client.authenticated(),
                        "NORETRY-4 第二次操作自己重连 + 恢复会话并成功",
                        second_error);
    test_support::Check(after - mid == 1,
                        "NORETRY-5 第二次也恰好一次请求", std::to_string(after - mid));
  }

  test_support::Section("RESUME-INVALID token 真的无效时才丢掉它");
  {
    ServerFixture fixture;
    test_support::Check(fixture.Start("resume-invalid"), "RESUME 夹具就绪",
                        fixture.error());
    net::RemoteArchiveClient client;
    std::string error;
    test_support::Check(client.Connect(fixture.endpoint(), &error) &&
                            client.Register("resume-user", "resume-password",
                                            &error) &&
                            client.Login("resume-user", "resume-password", &error),
                        "RESUME 注册并登录", error);
    // 另一条连接把这个账户注销掉：旧 token 的签名依然有效，但账户已经不存在。
    net::RemoteArchiveClient other;
    std::string other_error;
    test_support::Check(other.Connect(fixture.endpoint(), &other_error) &&
                            other.Login("resume-user", "resume-password",
                                        &other_error) &&
                            other.DeleteAccount("resume-password", &other_error),
                        "RESUME 从另一条连接注销该账户", other_error);
    // 强制走"重连 + 用旧 token 恢复会话"这条路。
    client.DisconnectSocket();
    test_support::Check(!client.authenticated(),
                        "RESUME 连接断开之后这条连接不再是 authenticated");
    std::vector<net::RemoteSnapshotInfo> listed;
    std::string list_error;
    const bool listed_ok = client.List(&listed, &list_error);
    test_support::Check(!listed_ok && list_error == UnauthorizedMessage(),
                        "RESUME-1 旧 token 被服务端明确拒绝（UNAUTHORIZED）",
                        list_error);
    test_support::Check(!client.session_resumable(),
                        "RESUME-2 只有这一种情况才丢掉 token（会话真的失效了）");
  }

  return test_support::Finish("remote_sequence_test");
}

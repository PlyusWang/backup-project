// tests/unit/remote_account_test.cpp
//
// PR #20（closure）：账户注销（DELETE_ACCOUNT）的端到端测试。
//
// 这里是**真实 TCP 环回 + 真实 SQLite + 真实磁盘**：起一个真的 backup-server，
// 用真实的 RemoteArchiveClient 注册 / 登录 / 上传，然后注销账户。
//
// 覆盖的 13 条（与交付报告里的编号一一对应）：
//
//   1. 未登录的注销请求（原始帧）-> UNAUTHORIZED
//   2. 错误当前口令 -> 拒绝，账户与数据一个字节都没动
//   3. 正确 token + 正确口令 -> 成功，且返回 kOk
//   4. 注销之后 login 原账户 -> 失败（服务端没有这一行了）
//   5. 注销之后旧 token 立即失效（另一条已经登录的连接再也做不了任何事）
//   6. 注销之后 snapshots 元数据 = 0
//   7. 注销之后该用户的 blob = 0（磁盘上真的没有了，trash 里也没有残留）
//   8. User A 注销：User B 的数据完全不受影响
//   9. user id 不重用（旧 token 不可能命中新注册的账户）
//  10. DB 事务故障注入 -> 不留"半删除"状态（元数据与 blob 都回到原样）
//  11. 失败之后重试成功（失败不是终态）
//  12. 口令与 token 不写服务端日志
//  13. 注销是幂等的"不可表达删别人"：协议里没有指定目标的字段，第二次
//      提交只会得到 UNAUTHORIZED（账户已不在）

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "network_protocol.h"
#include "remote_auth.h"
#include "remote_backup_client.h"
#include "remote_maintenance.h"
#include "remote_metadata_store.h"
#include "remote_server.h"
#include "remote_test_support.h"
#include "test_support.h"

namespace bp = backupproject;
namespace net = backupproject::net;

namespace {

constexpr int kClientTimeoutSeconds = 20;

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
  std::string root;
  std::string database;
  std::string secret_file;
  std::string log_file;
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
  fixture->root = base + "/data";
  fixture->database = base + "/state/metadata.sqlite3";
  fixture->secret_file = base + "/secrets.env";
  fixture->log_file = base + "/state/server.log";
  const std::string secret = RandomHex(32);
  if (secret.empty()) {
    return false;
  }
  if (!test_support::WriteFile(fixture->secret_file,
                               "BACKUP_TOKEN_SECRET=" + secret + "\n", 0600)) {
    return false;
  }
  // BPSEC1 身份密钥：服务端握手第一步就要用，缺了它 Start() 直接失败。
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
  fixture->config.root_directory = fixture->root;
  fixture->config.database_path = fixture->database;
  fixture->config.secret_file_path = fixture->secret_file;
  fixture->config.transport_key_file_path = fixture->transport_key_file;
  fixture->config.log_file_path = fixture->log_file;
  fixture->config.quiet = true;
  return true;
}

int ConnectToLoopback(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  timeval timeout;
  timeout.tv_sec = kClientTimeoutSeconds;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
    ::close(fd);
    return -1;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

struct RawResponse {
  std::uint32_t status = 0;
  std::string payload;
};

// 原始帧调用：用它来验证"客户端本地就挡住了"的那些路径在服务端**也**挡得住。
// BPSEC1 之后"原始"指的是绕过 RemoteArchiveClient，而不是绕过加密层：帧仍然
// 必须走通道，否则服务端在记录层就断连，根本走不到业务判别。
bool RawRequest(net::SecureChannel* channel, int fd, std::uint16_t opcode,
                const std::string& payload, std::uint64_t request_id,
                RawResponse* out) {
  std::string error;
  if (!remote_test_support::SendTestFrame(channel, fd, opcode, 0, request_id,
                                          payload, &error)) {
    return false;
  }
  net::FrameHeader header;
  std::string body;
  if (remote_test_support::ReceiveTestFrame(channel, fd, &header, &body,
                                            &error) !=
      net::FrameReadStatus::kOk) {
    return false;
  }
  out->status = header.status;
  out->payload = body;
  return true;
}

std::string PasswordPayload(const std::string& password) {
  net::PayloadBuilder builder;
  std::string error;
  builder.AppendString(password, net::kMaxPasswordBytes, &error);
  return builder.data();
}

bool TrashHasAccountLeftovers(const std::string& root) {
  const std::vector<std::string> names =
      test_support::DirEntries(root + "/trash");
  for (const std::string& name : names) {
    if (name.compare(0, 8, "account-") == 0) {
      return true;
    }
  }
  return false;
}

std::string UserBlobPath(const std::string& root, std::int64_t user_id,
                         const std::string& snapshot_id) {
  return root + "/users/" + std::to_string(user_id) + "/" + snapshot_id +
         ".bak";
}

// inspector：与备份服务端**不同的** SQLite 连接，直接读同一个数据库文件。
// 用它证明"注销之后元数据真的是 0 行"，而不是相信服务端的自述。
struct Inspector {
  net::RemoteMetadataStore store;

  bool Open(const std::string& database) {
    std::string error;
    return store.Open(database, &error);
  }

  bool FindByName(const std::string& username, net::RemoteUserSummary* out) {
    std::vector<net::RemoteUserSummary> users;
    std::string error;
    if (store.ListUsers(&users, &error) != net::StoreResult::kOk) {
      return false;
    }
    for (const net::RemoteUserSummary& user : users) {
      if (user.username == username) {
        *out = user;
        return true;
      }
    }
    return false;
  }

  std::uint64_t CountFor(std::int64_t user_id) {
    net::RemoteUserSummary found;
    std::vector<net::RemoteUserSummary> users;
    std::string error;
    if (store.ListUsers(&users, &error) != net::StoreResult::kOk) {
      return 0xFFFFFFFFull;
    }
    for (const net::RemoteUserSummary& user : users) {
      if (user.user_id == user_id) {
        return user.snapshot_count;
      }
    }
    return 0;
  }

  bool SnapshotExists(std::int64_t user_id, const std::string& snapshot_id) {
    net::RemoteSnapshotRecord record;
    std::string error;
    return store.FindSnapshot(user_id, snapshot_id, &record, &error) ==
           net::StoreResult::kOk;
  }
};

}  // namespace

int main() {
  test_support::Section("ACC 0. 夹具：真实服务端 + 真实数据库 + 真实磁盘");
  Fixture fixture;
  test_support::Check(SetupFixture(&fixture, "remote-account"),
                      "ACC T0 夹具就绪");
  net::RemoteServer server;
  std::string error;
  test_support::Check(server.Configure(fixture.config, &error),
                      "ACC T0 配置被接受", error);
  test_support::Check(server.Start(&error), "ACC T0 服务端启动", error);
  std::string run_error;
  bool run_result = false;
  std::thread runner([&server, &run_error, &run_result] {
    run_result = server.Run(&run_error);
  });
  const std::uint16_t port = server.bound_port();
  test_support::Check(port > 0, "ACC T0 服务端绑定了端口");

  net::RemoteEndpoint endpoint;
  endpoint.host = "127.0.0.1";
  endpoint.port = port;
  endpoint.timeout_seconds = kClientTimeoutSeconds;
  // BPSEC1 不做 TOFU：没有 pin 的客户端连不上，所以这里必须填。
  endpoint.server_key_pin = fixture.pin;

  Inspector inspector;
  test_support::Check(inspector.Open(fixture.database),
                      "ACC T0 独立的元数据检查连接已打开");

  const std::string user_a = "acc-user-a";
  const std::string user_b = "acc-user-b";
  const std::string password_a = "Acc-Pw-A-" + RandomHex(8);
  const std::string password_b = "Acc-Pw-B-" + RandomHex(8);

  test_support::Section("ACC 1. 未登录的注销请求必须被拒绝（原始帧）");
  {
    const int fd = ConnectToLoopback(port);
    test_support::Check(fd >= 0, "ACC T1 能连上服务端");
    net::SecureChannel channel;
    std::string handshake_error;
    const bool handshaken =
        fd >= 0 && remote_test_support::HandshakeTestClient(fd, fixture.pin,
                                                           &channel,
                                                           &handshake_error);
    test_support::Check(fd < 0 || handshaken, "ACC T1 原始连接的 BPSEC1 握手完成",
                        handshake_error);
    if (fd >= 0 && !handshaken) {
      // 握手没成时服务端还在等 ClientHello，显式 shutdown 才能让它及时收尾。
      ::shutdown(fd, SHUT_RDWR);
    }
    RawResponse response;
    const bool answered =
        handshaken &&
        RawRequest(&channel, fd,
                   static_cast<std::uint16_t>(net::Opcode::kDeleteAccount),
                   PasswordPayload(password_a), 1, &response);
    test_support::Check(answered &&
                            response.status ==
                                static_cast<std::uint32_t>(
                                    net::Status::kUnauthorized),
                        "ACC T1 未认证的 DELETE_ACCOUNT -> UNAUTHORIZED",
                        answered ? net::StatusName(response.status)
                                 : std::string("no answer"));
    if (fd >= 0) {
      ::close(fd);
    }
  }

  test_support::Section("ACC 2. 两个用户各上传一份真实归档");
  net::RemoteArchiveClient client_a;
  net::RemoteArchiveClient other_session_a;
  net::RemoteArchiveClient client_b;
  std::string snapshot_a;
  std::string snapshot_b;
  {
    const std::string archive_a = fixture.base + "/a.bak";
    const std::string archive_b = fixture.base + "/b.bak";
    test_support::Check(
        test_support::WriteFile(archive_a, std::string(4096, 'A'), 0600),
        "ACC T2 归档 A 已写出");
    test_support::Check(
        test_support::WriteFile(archive_b, std::string(2048, 'B'), 0600),
        "ACC T2 归档 B 已写出");

    test_support::Check(client_a.Connect(endpoint, &error) &&
                            client_a.Register(user_a, password_a, &error),
                        "ACC T2 注册用户 A", error);
    test_support::Check(client_a.Login(user_a, password_a, &error),
                        "ACC T2 登录用户 A", error);
    net::RemoteSnapshotInfo uploaded_a;
    test_support::Check(client_a.UploadArchiveFile(archive_a, "a.bak", nullptr,
                                                   &uploaded_a, &error),
                        "ACC T2 用户 A 上传成功", error);
    snapshot_a = uploaded_a.snapshot_id;

    // 第二条**独立的**连接，也以用户 A 登录：它手里的 token 就是"注销之前
    // 签发的旧 token"。
    test_support::Check(other_session_a.Connect(endpoint, &error) &&
                            other_session_a.Login(user_a, password_a, &error),
                        "ACC T2 用户 A 的第二条会话（旧 token）已建立", error);

    test_support::Check(client_b.Connect(endpoint, &error) &&
                            client_b.Register(user_b, password_b, &error) &&
                            client_b.Login(user_b, password_b, &error),
                        "ACC T2 注册并登录用户 B", error);
    net::RemoteSnapshotInfo uploaded_b;
    test_support::Check(client_b.UploadArchiveFile(archive_b, "b.bak", nullptr,
                                                   &uploaded_b, &error),
                        "ACC T2 用户 B 上传成功", error);
    snapshot_b = uploaded_b.snapshot_id;
  }

  net::RemoteUserSummary summary_a;
  net::RemoteUserSummary summary_b;
  test_support::Check(inspector.FindByName(user_a, &summary_a) &&
                          inspector.FindByName(user_b, &summary_b),
                      "ACC T2 两个用户都能在元数据库里查到");
  const std::string blob_a = UserBlobPath(fixture.root, summary_a.user_id,
                                          snapshot_a);
  const std::string blob_b = UserBlobPath(fixture.root, summary_b.user_id,
                                          snapshot_b);
  test_support::Check(test_support::Exists(blob_a) &&
                          test_support::Exists(blob_b),
                      "ACC T2 两份 blob 都在磁盘上（服务端生成的路径）");

  test_support::Section("ACC 3. 错误当前口令 -> 拒绝，什么都不动");
  {
    std::string wrong_error;
    const bool rejected =
        !client_a.DeleteAccount(password_a + "-wrong", &wrong_error);
    test_support::Check(rejected && client_a.authenticated(),
                        "ACC T3 错误口令被拒绝，会话与账户都还在", wrong_error);
    std::vector<net::RemoteSnapshotInfo> listed;
    test_support::Check(client_a.List(&listed, &error) && listed.size() == 1,
                        "ACC T3 拒绝之后用户 A 仍然能列出自己的备份", error);
    test_support::Check(inspector.SnapshotExists(summary_a.user_id, snapshot_a) &&
                            test_support::Exists(blob_a),
                        "ACC T3 拒绝之后元数据与 blob 都原样");
    test_support::Check(!TrashHasAccountLeftovers(fixture.root),
                        "ACC T3 拒绝之后 trash 里没有账户目录残留");
  }

  test_support::Section("ACC 4. 正确 token + 正确口令 -> 成功");
  {
    test_support::Check(client_a.DeleteAccount(password_a, &error),
                        "ACC T4 注销成功", error);
    test_support::Check(!client_a.authenticated(),
                        "ACC T4 注销之后本机会话立即失效");
    test_support::Check(!inspector.FindByName(user_a, &summary_a),
                        "ACC T4 users 行已经不存在");
    test_support::Check(inspector.CountFor(summary_a.user_id) == 0 &&
                            !inspector.SnapshotExists(summary_a.user_id,
                                                      snapshot_a),
                        "ACC T4 该用户的 snapshots 元数据 = 0");
    test_support::Check(!test_support::Exists(blob_a) &&
                            !test_support::Exists(fixture.root + "/users/" +
                                                  std::to_string(
                                                      summary_a.user_id)),
                        "ACC T4 该用户的 blob 与目录 = 0（磁盘上真的没有了）");
    test_support::Check(!TrashHasAccountLeftovers(fixture.root),
                        "ACC T4 trash 里没有隔离目录残留");
  }

  test_support::Section("ACC 5. 旧 token 立即失效（另一条已登录的连接）");
  {
    std::vector<net::RemoteSnapshotInfo> listed;
    const bool listed_ok = other_session_a.List(&listed, &error);
    test_support::Check(!listed_ok &&
                            error == net::RemoteStatusMessage(
                                         static_cast<std::uint32_t>(
                                             net::Status::kUnauthorized)),
                        "ACC T5 注销前签发的 token 再也读不到任何东西", error);
    std::string delete_error;
    test_support::Check(!other_session_a.Delete(snapshot_a, &delete_error),
                        "ACC T5 旧 token 也删不掉任何东西（没有可删的行）");
    // 同一个账户连续两次注销：第二次必须也是 UNAUTHORIZED，而不是"又成功了一次"。
    std::string second_error;
    test_support::Check(!client_a.DeleteAccount(password_a, &second_error),
                        "ACC T5 同一个账户的第二次注销被拒绝", second_error);
  }

  test_support::Section("ACC 6. 注销之后原账户无法再登录");
  {
    net::RemoteArchiveClient fresh;
    std::string login_error;
    test_support::Check(fresh.Connect(endpoint, &error) &&
                            !fresh.Login(user_a, password_a, &login_error),
                        "ACC T6 原账户登录失败（通用失败，不区分原因）",
                        login_error);
  }

  test_support::Section("ACC 7. 跨用户隔离：User B 完全不受影响");
  {
    std::vector<net::RemoteSnapshotInfo> listed;
    const bool ok = client_b.List(&listed, &error);
    test_support::Check(ok && listed.size() == 1 &&
                            listed[0].snapshot_id == snapshot_b,
                        "ACC T7 用户 B 的列表还是他自己那一条", error);
    test_support::Check(test_support::Exists(blob_b) &&
                            inspector.SnapshotExists(summary_b.user_id,
                                                     snapshot_b),
                        "ACC T7 用户 B 的 blob 与元数据都还在");
    test_support::Check(inspector.FindByName(user_b, &summary_b),
                        "ACC T7 用户 B 的账户还在");
  }

  test_support::Section("ACC 8. user id 不重用");
  {
    const std::int64_t deleted_id = summary_a.user_id;
    net::RemoteArchiveClient fresh;
    const std::string user_c = "acc-user-c";
    const std::string password_c = "Acc-Pw-C-" + RandomHex(8);
    test_support::Check(fresh.Connect(endpoint, &error) &&
                            fresh.Register(user_c, password_c, &error),
                        "ACC T8 注销之后再注册一个新账户", error);
    net::RemoteUserSummary summary_c;
    test_support::Check(inspector.FindByName(user_c, &summary_c),
                        "ACC T8 新账户能在元数据库里查到");
    test_support::Check(summary_c.user_id != deleted_id,
                        "ACC T8 新账户不会复用被注销账户的 user id",
                        "deleted=" + std::to_string(deleted_id) + " new=" +
                            std::to_string(summary_c.user_id));
    // 清理：把 C 也注销掉，后面的小节从"只有 B"开始。
    test_support::Check(fresh.Login(user_c, password_c, &error) &&
                            fresh.DeleteAccount(password_c, &error),
                        "ACC T8 新账户可以被正常注销", error);
  }

  test_support::Section("ACC 9. DB 事务故障注入 -> 不留半删除状态");
  {
    server.FailNextAccountDeleteForTesting();
    std::string inject_error;
    const bool rejected = !client_b.DeleteAccount(password_b, &inject_error);
    test_support::Check(rejected, "ACC T9 注入的元数据失败让注销如实失败",
                        inject_error);
    test_support::Check(client_b.authenticated(),
                        "ACC T9 失败之后会话仍然有效（注销没有半途改状态）");
    test_support::Check(inspector.FindByName(user_b, &summary_b) &&
                            inspector.SnapshotExists(summary_b.user_id,
                                                     snapshot_b),
                        "ACC T9 失败之后元数据完好：账户与快照行都还在");
    test_support::Check(test_support::Exists(blob_b),
                        "ACC T9 失败之后 blob 回到了正式位置（隔离已回滚）");
    test_support::Check(!TrashHasAccountLeftovers(fixture.root),
                        "ACC T9 失败之后 trash 里没有隔离目录残留");
    std::vector<net::RemoteSnapshotInfo> listed;
    test_support::Check(client_b.List(&listed, &error) && listed.size() == 1,
                        "ACC T9 失败之后用户 B 照常列出自己的备份", error);
  }

  test_support::Section("ACC 10. 失败之后重试成功（失败不是终态）");
  {
    test_support::Check(client_b.DeleteAccount(password_b, &error),
                        "ACC T10 重试注销成功", error);
    test_support::Check(!inspector.FindByName(user_b, &summary_b) &&
                            !test_support::Exists(blob_b) &&
                            !TrashHasAccountLeftovers(fixture.root),
                        "ACC T10 重试之后用户 B 的数据也清理干净了");
  }

  test_support::Section("ACC 11. 口令与 token 不写服务端日志");
  {
    std::string log_text;
    test_support::Check(test_support::ReadFile(fixture.log_file, &log_text),
                        "ACC T11 服务端日志可读");
    test_support::Check(log_text.find(password_a) == std::string::npos &&
                            log_text.find(password_b) == std::string::npos,
                        "ACC T11 日志里没有出现任何口令");
    test_support::Check(log_text.find("account deleted: user id=") !=
                            std::string::npos,
                        "ACC T11 日志里记录了注销这件事（只记 id 与数量）");
    test_support::Check(log_text.find("delete-account rejected") !=
                            std::string::npos,
                        "ACC T11 日志里记录了被拒绝的注销尝试");
  }

  server.RequestStop();
  if (runner.joinable()) {
    runner.join();
  }
  server.Stop();
  test_support::Check(run_result, "ACC T12 服务端正常停止", run_error);

  return test_support::Finish("remote_account_test");
}

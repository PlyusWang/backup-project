// tests/unit/remote_metadata_store_test.cpp
//
// PR #20：SQLite 元数据库的专项测试。
//
// 重点是**归属隔离**：A 的快照对 B 必须完全不存在——不是"看得见但没权限"，
// 而是"按 id 也查不到"。这条在协议层还会再测一遍（socket 层），
// 这里先把存储层的语义钉死。

#include <cstdint>
#include <string>
#include <vector>

#include "remote_auth.h"
#include "remote_metadata_store.h"
#include "test_support.h"

namespace bp = backupproject;
namespace net = backupproject::net;

namespace {

// 存储层不关心摘要怎么来的，所以直接造一条长度合法的记录，
// 免得每个用例都花几百毫秒做一次 PBKDF2。
net::PasswordRecord FakePasswordRecord() {
  net::PasswordRecord record;
  record.salt = std::string(net::kPasswordSaltBytes, 's');
  record.hash = std::string(net::kPasswordHashBytes, 'h');
  record.iterations = net::kPasswordIterations;
  return record;
}

net::RemoteSnapshotRecord MakeSnapshot(const std::string& id,
                                       std::int64_t user_id,
                                       const std::string& display_name,
                                       std::uint64_t size_bytes,
                                       std::int64_t created_at) {
  net::RemoteSnapshotRecord record;
  record.snapshot_id = id;
  record.user_id = user_id;
  record.display_name = display_name;
  record.size_bytes = size_bytes;
  record.sha256 = std::string(64, 'a');
  record.created_at = created_at;
  record.storage_name = id + ".bak";
  return record;
}

std::string SnapshotId(char fill) { return std::string(32, fill); }

}  // namespace

int main() {
  test_support::Section("STORE 1. 打开、建表、重复打开");
  {
    const std::string base = test_support::FreshDir("store-open");
    const std::string path = base + "/metadata.sqlite3";
    net::RemoteMetadataStore store;
    std::string error;
    test_support::Check(!store.IsOpen(), "STORE T1 新建对象未打开");
    test_support::Check(store.Open(path, &error), "STORE T1 打开数据库", error);
    test_support::Check(store.IsOpen(), "STORE T1 打开之后 IsOpen");
    error.clear();
    test_support::Check(!store.Open(path, &error),
                        "STORE T1 重复打开同一个对象被拒绝");
    error.clear();
    test_support::Check(!store.Open("", &error), "STORE T1 空路径被拒绝");

    // 表已经建好：直接查一下 users 的行数。
    std::uint64_t count = 0;
    test_support::Check(store.CountSnapshots(1, &count, &error) ==
                                net::StoreResult::kOk &&
                            count == 0,
                        "STORE T1 schema 可用（新库没有快照）", error);
    store.Close();
    test_support::Check(!store.IsOpen(), "STORE T1 关闭之后 IsOpen 为假");

    net::RemoteMetadataStore reopened;
    error.clear();
    test_support::Check(reopened.Open(path, &error),
                        "STORE T1 关闭之后可以重新打开（WAL 文件不影响）",
                        error);
    reopened.Close();
  }

  test_support::Section("STORE 2. 用户：创建、唯一、查找");
  {
    const std::string base = test_support::FreshDir("store-user");
    net::RemoteMetadataStore store;
    std::string error;
    store.Open(base + "/metadata.sqlite3", &error);

    std::int64_t alice_id = 0;
    const net::StoreResult created = store.CreateUser(
        "alice", FakePasswordRecord(), 1000, &alice_id, &error);
    test_support::Check(created == net::StoreResult::kOk && alice_id > 0,
                        "STORE T2 创建用户成功", net::StoreResultName(created));
    std::int64_t duplicate_id = 0;
    error.clear();
    test_support::Check(store.CreateUser("alice", FakePasswordRecord(), 1001,
                                         &duplicate_id, &error) ==
                            net::StoreResult::kAlreadyExists,
                        "STORE T2 同名用户返回 ALREADY_EXISTS");

    net::RemoteUserRecord found;
    error.clear();
    test_support::Check(store.FindUser("alice", &found, &error) ==
                                net::StoreResult::kOk &&
                            found.user_id == alice_id &&
                            found.username == "alice" &&
                            found.password.salt ==
                                FakePasswordRecord().salt &&
                            found.password.hash ==
                                FakePasswordRecord().hash &&
                            found.password.iterations == net::kPasswordIterations,
                        "STORE T2 查到用户且口令记录逐字节一致", error);

    net::RemoteUserRecord by_id;
    error.clear();
    test_support::Check(store.FindUserById(alice_id, &by_id, &error) ==
                                net::StoreResult::kOk &&
                            by_id.username == "alice",
                        "STORE T2 按 id 查用户", error);

    error.clear();
    test_support::Check(store.FindUser("nobody", &found, &error) ==
                            net::StoreResult::kNotFound,
                        "STORE T2 不存在的用户返回 NOT_FOUND");

    net::PasswordRecord bad;
    bad.salt = "short";
    bad.hash = "short";
    error.clear();
    test_support::Check(store.CreateUser("bob", bad, 1000, nullptr, &error) ==
                            net::StoreResult::kError,
                        "STORE T2 畸形口令记录被拒绝写入");
    store.Close();
  }

  test_support::Section("STORE 3. 快照归属隔离");
  {
    const std::string base = test_support::FreshDir("store-isolation");
    net::RemoteMetadataStore store;
    std::string error;
    store.Open(base + "/metadata.sqlite3", &error);

    std::int64_t alice = 0;
    std::int64_t bob = 0;
    store.CreateUser("alice", FakePasswordRecord(), 1000, &alice, &error);
    store.CreateUser("bob", FakePasswordRecord(), 1000, &bob, &error);

    const std::string alice_snapshot = SnapshotId('a');
    const std::string bob_snapshot = SnapshotId('b');
    error.clear();
    test_support::Check(store.InsertSnapshot(
                            MakeSnapshot(alice_snapshot, alice, "alice.bak",
                                         1024, 2000),
                            &error) == net::StoreResult::kOk,
                        "STORE T3 A 的文件插入成功", error);
    error.clear();
    test_support::Check(store.InsertSnapshot(
                            MakeSnapshot(bob_snapshot, bob, "bob.bak", 2048,
                                         2001),
                            &error) == net::StoreResult::kOk,
                        "STORE T3 B 的文件插入成功", error);

    std::vector<net::RemoteSnapshotRecord> alice_list;
    std::vector<net::RemoteSnapshotRecord> bob_list;
    store.ListSnapshots(alice, &alice_list, &error);
    store.ListSnapshots(bob, &bob_list, &error);
    test_support::Check(alice_list.size() == 1 &&
                            alice_list[0].snapshot_id == alice_snapshot,
                        "STORE T3 判别：A 的列表只有 A 的");
    test_support::Check(bob_list.size() == 1 &&
                            bob_list[0].snapshot_id == bob_snapshot,
                        "STORE T3 判别：B 的列表里没有 A 的快照");

    net::RemoteSnapshotRecord record;
    error.clear();
    test_support::Check(store.FindSnapshot(bob, alice_snapshot, &record, &error) ==
                            net::StoreResult::kNotFound,
                        "STORE T3 判别：B 按 id 查 A 的快照 = NOT_FOUND", error);
    error.clear();
    net::RemoteSnapshotRecord removed;
    test_support::Check(store.DeleteSnapshot(bob, alice_snapshot, &removed,
                                             &error) ==
                            net::StoreResult::kNotFound,
                        "STORE T3 判别：B 删不掉 A 的快照");
    std::uint64_t alice_count = 0;
    store.CountSnapshots(alice, &alice_count, &error);
    test_support::Check(alice_count == 1,
                        "STORE T3 判别：越权删除之后 A 的行还在");

    error.clear();
    test_support::Check(store.InsertSnapshot(
                            MakeSnapshot(alice_snapshot, bob, "copy.bak", 1,
                                         2002),
                            &error) == net::StoreResult::kAlreadyExists,
                        "STORE T3 重复的 snapshot id 被拒绝");
    store.Close();
  }

  test_support::Section("STORE 4. 删除语义与 storage_name");
  {
    const std::string base = test_support::FreshDir("store-delete");
    net::RemoteMetadataStore store;
    std::string error;
    store.Open(base + "/metadata.sqlite3", &error);
    std::int64_t alice = 0;
    store.CreateUser("alice", FakePasswordRecord(), 1000, &alice, &error);
    const std::string id = SnapshotId('c');
    store.InsertSnapshot(MakeSnapshot(id, alice, "  ../weird/name.bak  ", 4096,
                                      3000),
                         &error);

    net::RemoteSnapshotRecord removed;
    error.clear();
    test_support::Check(store.DeleteSnapshot(alice, id, &removed, &error) ==
                                net::StoreResult::kOk &&
                            removed.storage_name == id + ".bak" &&
                            removed.display_name == "  ../weird/name.bak  " &&
                            removed.size_bytes == 4096,
                        "STORE T4 判别：删除返回的行就是被删掉的那一行", error);
    std::uint64_t count = 0;
    store.CountSnapshots(alice, &count, &error);
    test_support::Check(count == 0, "STORE T4 删除之后计数为 0");

    error.clear();
    net::RemoteSnapshotRecord again;
    test_support::Check(store.DeleteSnapshot(alice, id, &again, &error) ==
                            net::StoreResult::kNotFound,
                        "STORE T4 再删一次 = NOT_FOUND");
    store.Close();
  }

  test_support::Section("STORE 5. 显示名只做 metadata（路径类名字原样保存）");
  {
    const std::string base = test_support::FreshDir("store-names");
    net::RemoteMetadataStore store;
    std::string error;
    store.Open(base + "/metadata.sqlite3", &error);
    std::int64_t alice = 0;
    store.CreateUser("alice", FakePasswordRecord(), 1000, &alice, &error);

    const char* names[] = {"../x", "../../etc/passwd", "/absolute/path.bak",
                           "a/b.bak", "a\\b.bak", "备份_2026.bak",
                           "%2e%2e%2fescaped.bak"};
    bool all_ok = true;
    std::vector<net::RemoteSnapshotRecord> list;
    for (int index = 0; index < 7; ++index) {
      const std::string id = SnapshotId(static_cast<char>('a' + index));
      net::RemoteSnapshotRecord record = MakeSnapshot(
          id, alice, names[index], static_cast<std::uint64_t>(index), 4000 + index);
      if (store.InsertSnapshot(record, &error) != net::StoreResult::kOk) {
        all_ok = false;
      }
    }
    store.ListSnapshots(alice, &list, &error);
    test_support::Check(all_ok && list.size() == 7,
                        "STORE T5 七种可疑显示名都作为 metadata 存下来了");
    bool names_intact = list.size() == 7;
    for (std::size_t index = 0; index < list.size() && names_intact; ++index) {
      if (list[index].display_name != names[index]) {
        names_intact = false;
      }
      // 磁盘名只由服务端生成，与显示名无关。
      if (list[index].storage_name != list[index].snapshot_id + ".bak") {
        names_intact = false;
      }
    }
    test_support::Check(names_intact,
                        "STORE T5 判别：显示名逐字节保留，storage_name 与它无关");
    store.Close();
  }

  test_support::Section("STORE 6. 持久化与故障注入");
  {
    const std::string base = test_support::FreshDir("store-persist");
    const std::string path = base + "/metadata.sqlite3";
    std::int64_t alice = 0;
    const std::string id = SnapshotId('d');
    {
      net::RemoteMetadataStore store;
      std::string error;
      store.Open(path, &error);
      store.CreateUser("alice", FakePasswordRecord(), 1000, &alice, &error);
      store.InsertSnapshot(MakeSnapshot(id, alice, "keep.bak", 77, 5000), &error);
      store.Close();
    }
    {
      net::RemoteMetadataStore store;
      std::string error;
      test_support::Check(store.Open(path, &error),
                          "STORE T6 重新打开已有数据库", error);
      std::vector<net::RemoteSnapshotRecord> list;
      store.ListSnapshots(alice, &list, &error);
      test_support::Check(list.size() == 1 && list[0].size_bytes == 77,
                          "STORE T6 判别：关闭再打开之后数据仍在");

      // 故障注入：这一次插入必须失败，而且不能留下半行。
      store.FailNextInsertForTesting();
      error.clear();
      const net::StoreResult injected = store.InsertSnapshot(
          MakeSnapshot(SnapshotId('e'), alice, "fail.bak", 1, 5001), &error);
      test_support::Check(injected == net::StoreResult::kError,
                          "STORE T6 注入的插入失败被如实返回",
                          net::StoreResultName(injected));
      std::uint64_t count = 0;
      store.CountSnapshots(alice, &count, &error);
      test_support::Check(count == 1,
                          "STORE T6 判别：失败的插入没有留下记录");
      // 注入只生效一次。
      error.clear();
      test_support::Check(store.InsertSnapshot(
                              MakeSnapshot(SnapshotId('e'), alice, "ok.bak", 1,
                                           5002),
                              &error) == net::StoreResult::kOk,
                          "STORE T6 下一次插入恢复正常", error);
      store.Close();
    }
    {
      net::RemoteMetadataStore closed;
      std::string error;
      test_support::Check(closed.CreateUser("x", FakePasswordRecord(), 1, nullptr,
                                            &error) == net::StoreResult::kError,
                          "STORE T6 未打开的库上操作返回 kError");
      std::uint64_t count = 0;
      error.clear();
      test_support::Check(closed.CountSnapshots(1, &count, &error) ==
                              net::StoreResult::kError,
                          "STORE T6 未打开的库上查询返回 kError");
    }
  }

  return test_support::Finish("remote_metadata_store_test");
}

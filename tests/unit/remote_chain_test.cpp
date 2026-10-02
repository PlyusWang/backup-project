// tests/unit/remote_chain_test.cpp
//
// PR #21：远端增量链的单元测试。
//
// 覆盖三块：
//   1. 材料包 BPSNAP1（打包 / 检查 / 解包 / 篡改 / 不覆盖）；
//   2. 链解析与 head 选择（纯元数据，不走网络）；
//   3. SQLite schema 迁移（1 -> 2）与依赖感知删除（真实 SQLite 文件）。
//
// 端到端（真实服务端 + BPSEC1 + full -> delta -> delta -> restore）在
// scripts/remote_incremental_test.sh 里跑。
//
// 退出码：0 = 全部通过。

#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "crypto.h"
#include "remote_incremental.h"
#include "remote_metadata_store.h"
#include "snapshot_bundle.h"

using backupproject::net::RemoteSnapshotInfo;
using backupproject::net::RemoteMetadataStore;
using backupproject::net::SnapshotBundleInfo;
using backupproject::net::StoreResult;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name, const std::string& detail = "") {
  g_checks += 1;
  if (!ok) {
    g_failures += 1;
    std::printf("  FAIL %s%s\n", name.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
  }
}

std::string WorkRoot() {
  std::string root = "/tmp/remote-chain-test-" +
                     std::to_string(static_cast<long>(::getpid()));
  return root;
}

bool MakeDirectory(const std::string& path) {
  const std::string command = "mkdir -p '" + path + "'";
  return std::system(command.c_str()) == 0;
}

bool WriteFile(const std::string& path, const std::string& content) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written =
      content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), file);
  return std::fclose(file) == 0 && written == content.size();
}

bool ReadFile(const std::string& path, std::string* out) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  out->clear();
  char buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, got);
  }
  std::fclose(file);
  return true;
}

bool FileExists(const std::string& path) {
  return ::access(path.c_str(), F_OK) == 0;
}

std::string Hex64(char fill) { return std::string(64, fill); }

// ---- 1. 材料包 ----

void TestSnapshotBundle() {
  std::printf("[chain] 材料包 BPSNAP1\n");
  const std::string root = WorkRoot() + "/bundle";
  const std::string repo = root + "/repo";
  const std::string out = root + "/out";
  Check(MakeDirectory(repo) && MakeDirectory(out), "建测试目录");

  const std::string archive = "remote-aaaaaaaaaaaa-100-g0.bak";
  const std::string manifest = archive + ".manifest";
  const std::string identity = archive + ".identity";
  Check(WriteFile(repo + "/" + archive, std::string(1000, 'A') + "payload-1"),
        "写入 .bak");
  Check(WriteFile(repo + "/" + manifest, "BPMANIFEST3 1\nentry\n"),
        "写入 .manifest");
  Check(WriteFile(repo + "/" + identity, "BPIDENT2\nsnapshot_id=deadbeef\n"),
        "写入 .identity");

  const std::string bundle = root + "/snapshot.bundle";
  SnapshotBundleInfo info;
  std::string error;
  Check(backupproject::net::BuildSnapshotBundle(repo, archive, bundle, &info,
                                                &error),
        "打包成功", error);
  Check(info.members.size() == 3, "三个成员");
  Check(info.archive_name == archive, "归档名正确");
  Check(info.bundle_size > 1000, "材料包大小合理");
  Check(!backupproject::net::BuildSnapshotBundle(repo, archive, bundle, &info,
                                                 &error),
        "重复打包到同一路径被拒绝（不覆盖）");

  SnapshotBundleInfo inspected;
  Check(backupproject::net::InspectSnapshotBundle(bundle, &inspected, &error),
        "检查材料包", error);
  Check(inspected.members.size() == 3 &&
            inspected.members[0].name == archive &&
            inspected.members[1].name == manifest &&
            inspected.members[2].name == identity,
        "成员顺序与名字正确");
  Check(inspected.members[0].sha256.size() == 64, "成员摘要长度正确");

  Check(backupproject::net::ExtractSnapshotBundle(bundle, out, &inspected,
                                                  &error),
        "解包成功", error);
  std::string a;
  std::string b;
  Check(ReadFile(repo + "/" + archive, &a) && ReadFile(out + "/" + archive, &b) &&
            a == b,
        "解包后 .bak 逐字节一致");
  Check(ReadFile(repo + "/" + manifest, &a) &&
            ReadFile(out + "/" + manifest, &b) && a == b,
        "解包后 .manifest 逐字节一致");
  Check(!backupproject::net::ExtractSnapshotBundle(bundle, out, &inspected,
                                                   &error),
        "重复解包到同一目录被拒绝（不覆盖）");

  // 篡改：把成员数据区的一个字节翻掉。
  std::string bytes;
  Check(ReadFile(bundle, &bytes), "读回材料包");
  std::string tampered = bytes;
  tampered[tampered.size() - 10] =
      static_cast<char>(tampered[tampered.size() - 10] ^ 0x40);
  const std::string tampered_path = root + "/tampered.bundle";
  Check(WriteFile(tampered_path, tampered), "写出被篡改的材料包");
  const std::string out2 = root + "/out2";
  Check(MakeDirectory(out2), "建第二个输出目录");
  SnapshotBundleInfo ignored;
  Check(!backupproject::net::ExtractSnapshotBundle(tampered_path, out2, &ignored,
                                                   &error),
        "成员内容被改 -> 解包失败（SHA-256 不符）");
  Check(!FileExists(out2 + "/" + archive), "失败时不留下一半的三件套");

  // 篡改：截断。
  const std::string truncated_path = root + "/truncated.bundle";
  Check(WriteFile(truncated_path, bytes.substr(0, bytes.size() / 2)),
        "写出被截断的材料包");
  Check(!backupproject::net::ExtractSnapshotBundle(truncated_path, out2, &ignored,
                                                   &error),
        "截断的材料包被拒绝");

  // 篡改：magic。
  std::string bad_magic = bytes;
  bad_magic[0] = 'X';
  const std::string bad_magic_path = root + "/bad-magic.bundle";
  Check(WriteFile(bad_magic_path, bad_magic), "写出坏 magic 的材料包");
  Check(!backupproject::net::InspectSnapshotBundle(bad_magic_path, &ignored,
                                                   &error),
        "magic 不对的材料包被拒绝");

  // 缺一件：三件套不齐时打包必须失败。
  const std::string archive2 = "remote-bbbbbbbbbbbb-200-g0.bak";
  Check(WriteFile(repo + "/" + archive2, "only-the-archive"),
        "只写 .bak 不写副文件");
  SnapshotBundleInfo ignored2;
  Check(!backupproject::net::BuildSnapshotBundle(repo, archive2,
                                                 root + "/missing.bundle",
                                                 &ignored2, &error),
        "缺副文件时打包失败");

  // 超大声明：成员长度超过上限时必须拒绝，而且**不能按声明值分配内存**。
  {
    std::string header;
    header.append("BPSNAP1\0", 8);
    auto append_u16 = [&header](std::uint16_t value) {
      header.push_back(static_cast<char>((value >> 8) & 0xFF));
      header.push_back(static_cast<char>(value & 0xFF));
    };
    auto append_u64 = [&header](std::uint64_t value) {
      for (int shift = 56; shift >= 0; shift -= 8) {
        header.push_back(static_cast<char>((value >> shift) & 0xFF));
      }
    };
    append_u16(1);  // 版本
    append_u16(3);  // 成员数
    const std::string first_name = archive;
    append_u16(static_cast<std::uint16_t>(first_name.size()));
    header.append(first_name);
    append_u64(backupproject::net::kSnapshotBundleMaxMemberBytes + 1);
    header.append(32, '\0');  // 假摘要
    const std::string oversized_path = root + "/oversized.bundle";
    Check(WriteFile(oversized_path, header), "写出超大声明的材料包");
    SnapshotBundleInfo oversized_info;
    Check(!backupproject::net::InspectSnapshotBundle(oversized_path,
                                                     &oversized_info, &error),
          "声明的成员长度超过上限时被拒绝（不按声明值分配）");
  }

  // 名字作弊：成员名带路径分隔符时必须拒绝（用真实包改名字字段）。
  std::string path_trick = bytes;
  const std::size_t first_name_offset = 14;  // magic(8) + version(2) + count(2) + len(2)
  path_trick[first_name_offset + 4] = '/';
  const std::string path_trick_path = root + "/path-trick.bundle";
  Check(WriteFile(path_trick_path, path_trick), "写出名字带 / 的材料包");
  Check(!backupproject::net::InspectSnapshotBundle(path_trick_path, &ignored,
                                                   &error),
        "成员名不是单组件时被拒绝");

  const std::string cleanup = "rm -rf '" + root + "'";
  (void)std::system(cleanup.c_str());
}

// ---- 2. 链解析与 head 选择 ----

RemoteSnapshotInfo MakeSnapshot(const std::string& id,
                                const std::string& parent,
                                std::uint64_t generation,
                                const std::string& lineage,
                                std::int64_t created_at,
                                std::uint16_t kind) {
  RemoteSnapshotInfo info;
  info.snapshot_id = id;
  info.parent_snapshot_id = parent;
  info.generation = generation;
  info.lineage = lineage;
  info.created_at = created_at;
  info.snapshot_kind = kind;
  info.size_bytes = 100;
  return info;
}

void TestChainResolution() {
  std::printf("[chain] 远端链解析\n");
  const std::string lineage = Hex64('a');
  const std::string other_lineage = Hex64('b');
  std::vector<RemoteSnapshotInfo> snapshots;
  snapshots.push_back(MakeSnapshot("11111111111111111111111111111111", "", 0,
                                   lineage, 100, 0));
  snapshots.push_back(MakeSnapshot("22222222222222222222222222222222",
                                   "11111111111111111111111111111111", 1,
                                   lineage, 200, 1));
  snapshots.push_back(MakeSnapshot("33333333333333333333333333333333",
                                   "22222222222222222222222222222222", 2,
                                   lineage, 300, 1));
  snapshots.push_back(MakeSnapshot("44444444444444444444444444444444", "", 0,
                                   other_lineage, 400, 0));

  std::vector<RemoteSnapshotInfo> chain;
  std::string error;
  Check(backupproject::net::ResolveRemoteChain(
            snapshots, "33333333333333333333333333333333", &chain, &error),
        "解析三跳链", error);
  Check(chain.size() == 3, "链长度 3");
  Check(chain.front().snapshot_id == "11111111111111111111111111111111" &&
            chain.back().snapshot_id == "33333333333333333333333333333333",
        "链的顺序是 base -> target");

  Check(!backupproject::net::ResolveRemoteChain(
            snapshots, "99999999999999999999999999999999", &chain, &error),
        "目标不存在时失败");

  // 父不存在（链断在中间）。
  std::vector<RemoteSnapshotInfo> broken;
  broken.push_back(MakeSnapshot("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 3, lineage,
                                500, 1));
  Check(!backupproject::net::ResolveRemoteChain(
            broken, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &chain, &error),
        "父不在列表里（链断了）时失败");

  // 代数不连续。
  std::vector<RemoteSnapshotInfo> jump;
  jump.push_back(MakeSnapshot("11111111111111111111111111111111", "", 0, lineage,
                              100, 0));
  jump.push_back(MakeSnapshot("22222222222222222222222222222222",
                              "11111111111111111111111111111111", 5, lineage,
                              200, 1));
  Check(!backupproject::net::ResolveRemoteChain(
            jump, "22222222222222222222222222222222", &chain, &error),
        "代数跳跃（元数据自相矛盾）时失败");

  // 深度上界。
  std::vector<RemoteSnapshotInfo> deep;
  std::string previous;
  for (int index = 0; index < 80; ++index) {
    char id[33];
    std::snprintf(id, sizeof(id), "%032d", index);
    deep.push_back(MakeSnapshot(id, previous, static_cast<std::uint64_t>(index),
                                lineage, 100 + index, index == 0 ? 0 : 1));
    previous = id;
  }
  Check(!backupproject::net::ResolveRemoteChain(deep, previous, &chain, &error),
        "超过最大深度时失败");

  // head 选择：没有被引用为父的那一个；两个候选时取更新的。
  RemoteSnapshotInfo head;
  std::string reason;
  Check(backupproject::net::FindRemoteLineageHead(snapshots, lineage, &head,
                                                  &reason),
        "找到 head", reason);
  Check(head.snapshot_id == "33333333333333333333333333333333",
        "head 是最新的叶子");
  Check(!backupproject::net::FindRemoteLineageHead(snapshots, Hex64('c'), &head,
                                                   &reason),
        "没有这条链的 head 时返回 false");

  std::vector<RemoteSnapshotInfo> forked = snapshots;
  forked.push_back(MakeSnapshot("55555555555555555555555555555555",
                                "22222222222222222222222222222222", 2, lineage,
                                900, 1));
  Check(backupproject::net::FindRemoteLineageHead(forked, lineage, &head,
                                                  &reason) &&
            head.snapshot_id == "55555555555555555555555555555555",
        "分叉时取创建时间更新的叶子");
}

// ---- 3. schema 迁移与依赖感知删除 ----

bool ExecSql(sqlite3* database, const char* sql) {
  char* message = nullptr;
  const int code = sqlite3_exec(database, sql, nullptr, nullptr, &message);
  if (message != nullptr) {
    sqlite3_free(message);
  }
  return code == SQLITE_OK;
}

int ReadUserVersion(const std::string& path) {
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return -1;
  }
  int version = -1;
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database, "PRAGMA user_version;", -1, &statement,
                         nullptr) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_ROW) {
    version = sqlite3_column_int(statement, 0);
  }
  if (statement != nullptr) {
    sqlite3_finalize(statement);
  }
  sqlite3_close(database);
  return version;
}

// 手工造一个 PR #20 的 schema 版本 1 数据库（含一行 legacy 快照）。
bool CreateLegacyDatabase(const std::string& path) {
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(path.c_str(), &database,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
      SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return false;
  }
  const char* statements[] = {
      "PRAGMA journal_mode=WAL;",
      "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT UNIQUE NOT NULL,"
      " password_salt BLOB NOT NULL, password_hash BLOB NOT NULL,"
      " password_iterations INTEGER NOT NULL, created_at INTEGER NOT NULL);",
      "CREATE TABLE snapshots (id TEXT PRIMARY KEY, user_id INTEGER NOT NULL,"
      " display_name TEXT NOT NULL, size_bytes INTEGER NOT NULL,"
      " sha256 TEXT NOT NULL, created_at INTEGER NOT NULL,"
      " storage_name TEXT NOT NULL, FOREIGN KEY(user_id) REFERENCES users(id));",
      "CREATE TABLE deleted_users (id INTEGER PRIMARY KEY, deleted_at INTEGER NOT NULL);",
      "CREATE INDEX snapshots_by_user ON snapshots(user_id, created_at, id);",
      "INSERT INTO users VALUES (1, 'legacy-user', X'00', X'00', 1, 1);",
      "INSERT INTO snapshots VALUES ('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 1,"
      " 'legacy snapshot', 123,"
      " '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',"
      " 1000, 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bak');",
      "PRAGMA user_version=1;",
  };
  bool ok = true;
  for (const char* sql : statements) {
    if (!ExecSql(database, sql)) {
      ok = false;
      break;
    }
  }
  sqlite3_close(database);
  return ok;
}

void TestMigrationAndChainMetadata() {
  std::printf("[chain] SQLite 迁移与依赖感知删除\n");
  const std::string root = WorkRoot() + "/migrate";
  Check(MakeDirectory(root), "建测试目录");
  const std::string legacy_path = root + "/legacy.sqlite3";
  Check(CreateLegacyDatabase(legacy_path), "造出 PR#20 的 v1 数据库");
  Check(ReadUserVersion(legacy_path) == 1, "迁移前版本是 1");

  RemoteMetadataStore store;
  std::string error;
  Check(store.Open(legacy_path, &error), "打开旧库（自动迁移）", error);
  Check(ReadUserVersion(legacy_path) == 2, "迁移后版本是 2");
  Check(store.IsOpen(), "迁移后库仍然可用");

  const std::string legacy_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  std::vector<backupproject::net::RemoteSnapshotRecord> records;
  Check(store.ListSnapshots(1, &records, &error) == StoreResult::kOk,
        "列出 legacy 快照", error);
  Check(records.size() == 1, "legacy 行还在（没有清库）");
  bool legacy_ok = false;
  if (records.size() == 1) {
    legacy_ok = records[0].snapshot_kind == 0 && records[0].parent_id.empty() &&
                records[0].generation == 0 && records[0].lineage.empty() &&
                records[0].display_name == "legacy snapshot";
  }
  Check(legacy_ok, "legacy 行被当作 standalone full（kind 0 / 无父 / gen 0）");

  // 插入一条增量子节点。
  backupproject::net::RemoteSnapshotRecord child;
  child.snapshot_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  child.user_id = 1;
  child.display_name = "delta 1";
  child.size_bytes = 456;
  child.sha256 = Hex64('c');
  child.created_at = 2000;
  child.storage_name = child.snapshot_id + ".bak";
  child.snapshot_kind = 1;
  child.parent_id = legacy_id;
  child.generation = 1;
  child.lineage = Hex64('d');
  Check(store.InsertSnapshot(child, &error) == StoreResult::kOk, "插入增量行",
        error);

  std::uint64_t children = 0;
  Check(store.CountSnapshotChildren(1, legacy_id, &children, &error) ==
                StoreResult::kOk &&
            children == 1,
        "子节点计数为 1", error);

  backupproject::net::RemoteSnapshotRecord removed;
  const StoreResult blocked =
      store.DeleteSnapshot(1, legacy_id, &removed, &error);
  Check(blocked == StoreResult::kHasDependents,
        "删有后代的父被拒绝（kHasDependents）",
        backupproject::net::StoreResultName(blocked));
  Check(store.CountSnapshots(1, &children, &error) == StoreResult::kOk &&
            children == 2,
        "被拒绝后两行都还在");

  Check(store.DeleteSnapshot(1, child.snapshot_id, &removed, &error) ==
            StoreResult::kOk,
        "删叶子成功", error);
  Check(store.DeleteSnapshot(1, legacy_id, &removed, &error) == StoreResult::kOk,
        "后代替删除之后可以删父", error);
  Check(store.CountSnapshots(1, &children, &error) == StoreResult::kOk &&
            children == 0,
        "删完之后没有残留");
  store.Close();

  // 迁移必须幂等：再打开一次不能出问题，也不能把列再加一遍。
  RemoteMetadataStore again;
  Check(again.Open(legacy_path, &error), "再次打开（迁移幂等）", error);
  Check(ReadUserVersion(legacy_path) == 2, "版本仍然是 2");
  again.Close();

  // 只读打开不能升级：用一个手工造的 v1 库验证管理工具的 fail-closed。
  const std::string legacy2 = root + "/legacy2.sqlite3";
  Check(CreateLegacyDatabase(legacy2), "再造一个 v1 数据库");
  RemoteMetadataStore read_only;
  Check(!read_only.OpenExistingReadOnly(legacy2, &error),
        "只读打开 v1 库被拒绝（不偷偷升级）", error);
  Check(ReadUserVersion(legacy2) == 1, "被拒绝之后版本仍然是 1（一个字节都没写）");

  const std::string cleanup = "rm -rf '" + root + "'";
  (void)std::system(cleanup.c_str());
}

// ---- 4. 材料包的确定性变异 ----

std::uint64_t NextRandom(std::uint64_t* state) {
  std::uint64_t value = *state;
  value ^= value << 13;
  value ^= value >> 7;
  value ^= value << 17;
  *state = value;
  return value;
}

void TestBundleFuzz() {
  // 材料包里没有"被忽略的字节"：magic、版本、成员数、每个成员的名字/长度/
  // SHA-256/数据都被校验。所以任何一处变异都必须让解包失败，而且失败之后
  // 目标目录里不能留下任何东西（不能留下"解了一半"的三件套）。
  std::printf("[chain] 材料包确定性变异\n");
  const std::string root = WorkRoot() + "/bundle-fuzz";
  const std::string repo = root + "/repo";
  Check(MakeDirectory(repo), "建测试目录");

  const std::string archive = "remote-cccccccccccc-300-g0.bak";
  Check(WriteFile(repo + "/" + archive, std::string(500, 'M')), "写入 .bak");
  Check(WriteFile(repo + "/" + archive + ".manifest", "BPMANIFEST3 1\nx\n"),
        "写入 .manifest");
  Check(WriteFile(repo + "/" + archive + ".identity", "BPIDENT2\nsnapshot_id=x\n"),
        "写入 .identity");

  const std::string bundle = root + "/valid.bundle";
  SnapshotBundleInfo info;
  std::string error;
  Check(backupproject::net::BuildSnapshotBundle(repo, archive, bundle, &info,
                                                &error),
        "打出基准材料包", error);
  std::string bytes;
  Check(ReadFile(bundle, &bytes), "读回材料包");

  const int rounds = 200;
  int rejected = 0;
  int leftovers = 0;
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  for (int round = 0; round < rounds; ++round) {
    std::string mutated = bytes;
    const int mutations = 1 + static_cast<int>(NextRandom(&state) % 3);
    for (int index = 0; index < mutations; ++index) {
      const std::size_t offset =
          static_cast<std::size_t>(NextRandom(&state) % mutated.size());
      mutated[offset] = static_cast<char>(mutated[offset] ^
                                          (1u << (NextRandom(&state) % 8)));
    }
    const std::string mutated_path =
        root + "/mutated-" + std::to_string(round) + ".bundle";
    if (!WriteFile(mutated_path, mutated)) {
      Check(false, "写出变异材料包");
      return;
    }
    const std::string out = root + "/out-" + std::to_string(round);
    if (!MakeDirectory(out)) {
      Check(false, "建解包目录");
      return;
    }
    SnapshotBundleInfo result;
    std::string extract_error;
    if (!backupproject::net::ExtractSnapshotBundle(mutated_path, out, &result,
                                                   &extract_error)) {
      rejected += 1;
    }
    if (FileExists(out + "/" + archive) ||
        FileExists(out + "/" + archive + ".manifest") ||
        FileExists(out + "/" + archive + ".identity")) {
      leftovers += 1;
    }
    const std::string cleanup = "rm -rf '" + out + "' '" + mutated_path + "'";
    (void)std::system(cleanup.c_str());
  }
  Check(rejected == rounds, "200 个变异材料包全部被拒绝",
        std::to_string(rejected) + "/" + std::to_string(rounds));
  Check(leftovers == 0, "被拒绝时目标目录里没有留下任何文件");
  const std::string cleanup = "rm -rf '" + root + "'";
  (void)std::system(cleanup.c_str());
}

// ---- 5. 迁移的 fail-closed：不认识的版本一个字节都不改 ----

void TestUnknownSchemaVersion() {
  std::printf("[chain] 未知 schema 版本必须 fail closed\n");
  const std::string root = WorkRoot() + "/unknown-schema";
  Check(MakeDirectory(root), "建测试目录");
  const std::string path = root + "/future.sqlite3";
  Check(CreateLegacyDatabase(path), "造一个 v1 数据库");
  {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database,
                        SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
      ExecSql(database, "PRAGMA user_version=99;");
    }
    if (database != nullptr) {
      sqlite3_close(database);
    }
  }
  std::string before;
  Check(ReadFile(path, &before), "读迁移前的库");
  RemoteMetadataStore store;
  std::string error;
  Check(!store.Open(path, &error), "版本 99 的库被拒绝打开", error);
  Check(error.find("99") != std::string::npos, "错误信息里点明了版本号", error);
  std::string after;
  Check(ReadFile(path, &after), "读迁移后的库");
  Check(before == after, "被拒绝之后库文件一个字节都没变");
  // 迁移失败必须整体回滚：把库文件设成只读，ALTER 必然失败，此时
  // 版本号与数据都必须保持原样（不能出现"加了列却没写版本"的半迁移状态）。
  {
    const std::string read_only = root + "/readonly.sqlite3";
    Check(CreateLegacyDatabase(read_only), "再造一个 v1 数据库");
    std::string before_bytes;
    Check(ReadFile(read_only, &before_bytes), "读只读迁移前的库");
    Check(::chmod(read_only.c_str(), 0444) == 0, "把库文件设为只读");
    RemoteMetadataStore blocked;
    std::string blocked_error;
    Check(!blocked.Open(read_only, &blocked_error),
          "只读库上的迁移失败", blocked_error);
    Check(ReadUserVersion(read_only) == 1, "迁移失败后版本号仍然是 1");
    Check(::chmod(read_only.c_str(), 0644) == 0, "恢复权限以便比对");
    std::string after_bytes;
    Check(ReadFile(read_only, &after_bytes), "读只读迁移后的库");
    Check(before_bytes == after_bytes, "迁移失败后库文件一个字节都没变");
    blocked.Close();
  }

  const std::string cleanup = "rm -rf '" + root + "'";
  (void)std::system(cleanup.c_str());
}

}  // namespace

int main() {
  std::printf("远端增量链单元测试\n");
  TestSnapshotBundle();
  TestChainResolution();
  TestMigrationAndChainMetadata();
  TestBundleFuzz();
  TestUnknownSchemaVersion();
  const int passed = g_checks - g_failures;
  std::printf("remote-chain-test: %d/%d checks passed\n", passed, g_checks);
  return g_failures == 0 ? 0 : 1;
}

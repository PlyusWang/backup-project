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

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "archive_pipeline.h"
#include "crypto.h"
#include "file_io.h"
#include "filter.h"
#include "incremental_backup.h"
#include "incremental_delta.h"
#include "remote_backup_client.h"
#include "remote_incremental.h"
#include "remote_metadata_store.h"
#include "remote_server.h"
#include "remote_test_support.h"
#include "snapshot_bundle.h"
using backupproject::net::RemoteMetadataStore;
using backupproject::net::RemoteSnapshotInfo;
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

// ----
// RT0-a：红队用例需要的基础工具（放在最前面，既有代码的清理路径也用它）----
//
// 为什么不用 std::system：glibc 给 system() 标了 warn_unused_result，而
// "(void)system(...)" 在 -O1 下仍然会警告；清目录用递归 POSIX 调用也顺带
// 证明"测试自己没有留下需要 shell 才能删的东西"。

int RunShell(const std::string& command) {
  return std::system(command.c_str());
}

bool MakeDirectoryTree(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::string current;
  std::size_t index = 0;
  if (path[0] == '/') {
    current = "/";
    index = 1;
  }
  while (index < path.size()) {
    std::size_t slash = path.find('/', index);
    if (slash == std::string::npos) {
      slash = path.size();
    }
    const std::string part = path.substr(index, slash - index);
    if (!part.empty()) {
      if (current.empty()) {
        current = part;
      } else if (current == "/") {
        current += part;
      } else {
        current += "/" + part;
      }
      if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
      }
    }
    index = slash + 1;
  }
  struct stat info;
  return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool RemoveTree(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return true;
  }
  if (!S_ISDIR(info.st_mode)) {
    return ::unlink(path.c_str()) == 0;
  }
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    return false;
  }
  bool ok = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    if (!RemoveTree(path + "/" + name)) {
      ok = false;
    }
  }
  ::closedir(directory);
  if (::rmdir(path.c_str()) != 0) {
    ok = false;
  }
  return ok;
}

std::string WorkRoot() {
  std::string root =
      "/tmp/remote-chain-test-" + std::to_string(static_cast<long>(::getpid()));
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
      content.empty() ? 0
                      : std::fwrite(content.data(), 1, content.size(), file);
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
  Check(inspected.members.size() == 3 && inspected.members[0].name == archive &&
            inspected.members[1].name == manifest &&
            inspected.members[2].name == identity,
        "成员顺序与名字正确");
  Check(inspected.members[0].sha256.size() == 64, "成员摘要长度正确");

  Check(backupproject::net::ExtractSnapshotBundle(bundle, out, &inspected,
                                                  &error),
        "解包成功", error);
  std::string a;
  std::string b;
  Check(ReadFile(repo + "/" + archive, &a) &&
            ReadFile(out + "/" + archive, &b) && a == b,
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
  Check(!backupproject::net::ExtractSnapshotBundle(tampered_path, out2,
                                                   &ignored, &error),
        "成员内容被改 -> 解包失败（SHA-256 不符）");
  Check(!FileExists(out2 + "/" + archive), "失败时不留下一半的三件套");

  // 篡改：截断。
  const std::string truncated_path = root + "/truncated.bundle";
  Check(WriteFile(truncated_path, bytes.substr(0, bytes.size() / 2)),
        "写出被截断的材料包");
  Check(!backupproject::net::ExtractSnapshotBundle(truncated_path, out2,
                                                   &ignored, &error),
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
  Check(!backupproject::net::BuildSnapshotBundle(
            repo, archive2, root + "/missing.bundle", &ignored2, &error),
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
  const std::size_t first_name_offset =
      14;  // magic(8) + version(2) + count(2) + len(2)
  path_trick[first_name_offset + 4] = '/';
  const std::string path_trick_path = root + "/path-trick.bundle";
  Check(WriteFile(path_trick_path, path_trick), "写出名字带 / 的材料包");
  Check(!backupproject::net::InspectSnapshotBundle(path_trick_path, &ignored,
                                                   &error),
        "成员名不是单组件时被拒绝");

  const std::string cleanup = "rm -rf '" + root + "'";
  RunShell(cleanup.c_str());
}

// ---- 2. 链解析与 head 选择 ----

RemoteSnapshotInfo MakeSnapshot(const std::string& id,
                                const std::string& parent,
                                std::uint64_t generation,
                                const std::string& lineage,
                                std::int64_t created_at, std::uint16_t kind) {
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
  snapshots.push_back(
      MakeSnapshot("11111111111111111111111111111111", "", 0, lineage, 100, 0));
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
  jump.push_back(
      MakeSnapshot("11111111111111111111111111111111", "", 0, lineage, 100, 0));
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
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return false;
  }
  const char* statements[] = {
      "PRAGMA journal_mode=WAL;",
      "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT UNIQUE NOT "
      "NULL,"
      " password_salt BLOB NOT NULL, password_hash BLOB NOT NULL,"
      " password_iterations INTEGER NOT NULL, created_at INTEGER NOT NULL);",
      "CREATE TABLE snapshots (id TEXT PRIMARY KEY, user_id INTEGER NOT NULL,"
      " display_name TEXT NOT NULL, size_bytes INTEGER NOT NULL,"
      " sha256 TEXT NOT NULL, created_at INTEGER NOT NULL,"
      " storage_name TEXT NOT NULL, FOREIGN KEY(user_id) REFERENCES "
      "users(id));",
      "CREATE TABLE deleted_users (id INTEGER PRIMARY KEY, deleted_at INTEGER "
      "NOT NULL);",
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

  // PR #21 的契约（两层组合出来的结论，store 本身并不"认识" legacy）：
  //   * 服务端 UPLOAD_BEGIN 对 kind=incremental 强制要求非空 lineage；
  //   * store 又要求 parent_lineage == record.lineage；
  //   * 迁移后的 legacy 行 lineage 是 DEFAULT ''。
  // 所以一个增量永远不可能声明 ''，也就永远挂不到 legacy 行下面 —— 它结构上
  // 就是链的终点。客户端也不会把它选成 head（head 按 lineage 过滤）。
  {
    backupproject::net::RemoteSnapshotRecord onto_legacy;
    onto_legacy.snapshot_id = "cccccccccccccccccccccccccccccccc";
    onto_legacy.user_id = 1;
    onto_legacy.display_name = "delta onto legacy";
    onto_legacy.size_bytes = 111;
    onto_legacy.sha256 = Hex64('e');
    onto_legacy.created_at = 1500;
    onto_legacy.storage_name = onto_legacy.snapshot_id + ".bak";
    onto_legacy.snapshot_kind = 1;
    onto_legacy.parent_id = legacy_id;
    onto_legacy.generation = 1;
    onto_legacy.lineage = Hex64('d');
    const StoreResult onto_legacy_result =
        store.InsertSnapshot(onto_legacy, &error);
    Check(onto_legacy_result == StoreResult::kChainConflict,
          "以 lineage='' 的遗留行为父的增量被拒绝（kChainConflict：lineage "
          "不同）",
          backupproject::net::StoreResultName(onto_legacy_result));
    std::uint64_t count = 0;
    Check(store.CountSnapshots(1, &count, &error) == StoreResult::kOk &&
              count == 1,
          "被拒绝之后 legacy 行仍然只有一行");
  }

  // 依赖感知删除改用一条**自己的**链：同一个 lineage 才允许建边。
  const std::string root_id = "dddddddddddddddddddddddddddddddd";
  const std::string chain_lineage = Hex64('d');
  backupproject::net::RemoteSnapshotRecord full_root;
  full_root.snapshot_id = root_id;
  full_root.user_id = 1;
  full_root.display_name = "chain root";
  full_root.size_bytes = 222;
  full_root.sha256 = Hex64('f');
  full_root.created_at = 1800;
  full_root.storage_name = root_id + ".bak";
  full_root.snapshot_kind = 0;
  full_root.generation = 0;
  full_root.lineage = chain_lineage;
  Check(store.InsertSnapshot(full_root, &error) == StoreResult::kOk, "插入链根",
        error);

  backupproject::net::RemoteSnapshotRecord child;
  child.snapshot_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  child.user_id = 1;
  child.display_name = "delta 1";
  child.size_bytes = 456;
  child.sha256 = Hex64('c');
  child.created_at = 2000;
  child.storage_name = child.snapshot_id + ".bak";
  child.snapshot_kind = 1;
  child.parent_id = root_id;
  child.generation = 1;
  child.lineage = chain_lineage;
  Check(store.InsertSnapshot(child, &error) == StoreResult::kOk, "插入增量行",
        error);

  std::uint64_t children = 0;
  Check(store.CountSnapshotChildren(1, root_id, &children, &error) ==
                StoreResult::kOk &&
            children == 1,
        "子节点计数为 1", error);

  backupproject::net::RemoteSnapshotRecord removed;
  const StoreResult blocked =
      store.DeleteSnapshot(1, root_id, &removed, &error);
  Check(blocked == StoreResult::kHasDependents,
        "删有后代的父被拒绝（kHasDependents）",
        backupproject::net::StoreResultName(blocked));
  Check(store.CountSnapshots(1, &children, &error) == StoreResult::kOk &&
            children == 3,
        "被拒绝后三行都还在");

  Check(store.DeleteSnapshot(1, child.snapshot_id, &removed, &error) ==
            StoreResult::kOk,
        "删叶子成功", error);
  Check(store.DeleteSnapshot(1, root_id, &removed, &error) == StoreResult::kOk,
        "后代替删除之后可以删父", error);
  Check(store.CountSnapshots(1, &children, &error) == StoreResult::kOk &&
            children == 1,
        "删完之后只剩 legacy 行");
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
  Check(ReadUserVersion(legacy2) == 1,
        "被拒绝之后版本仍然是 1（一个字节都没写）");

  RemoveTree(root);
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
  Check(WriteFile(repo + "/" + archive + ".identity",
                  "BPIDENT2\nsnapshot_id=x\n"),
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
      mutated[offset] =
          static_cast<char>(mutated[offset] ^ (1u << (NextRandom(&state) % 8)));
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
    RunShell(cleanup.c_str());
  }
  Check(rejected == rounds, "200 个变异材料包全部被拒绝",
        std::to_string(rejected) + "/" + std::to_string(rounds));
  Check(leftovers == 0, "被拒绝时目标目录里没有留下任何文件");
  const std::string cleanup = "rm -rf '" + root + "'";
  RunShell(cleanup.c_str());
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
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READWRITE,
                        nullptr) == SQLITE_OK) {
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
    Check(!blocked.Open(read_only, &blocked_error), "只读库上的迁移失败",
          blocked_error);
    Check(ReadUserVersion(read_only) == 1, "迁移失败后版本号仍然是 1");
    Check(::chmod(read_only.c_str(), 0644) == 0, "恢复权限以便比对");
    std::string after_bytes;
    Check(ReadFile(read_only, &after_bytes), "读只读迁移后的库");
    Check(before_bytes == after_bytes, "迁移失败后库文件一个字节都没变");
    blocked.Close();
  }

  const std::string cleanup = "rm -rf '" + root + "'";
  RunShell(cleanup.c_str());
}

// ============================================================================
// PR #21 独立红队审查轮新增用例：公共工具（RT0）
//
// 这一节只新增构造器 / 夹具 / 断言辅助，不改变上面任何既有用例的语义。
// 用例编号与任务书一一对应：RT1 材料包恶意构造、RT2 解包本地攻击、
// RT3 缓存索引、RT4 缓存隔离、RT5 元数据<->归档交叉校验、RT6 迁移半途失败、
// RT7 大数据量迁移、RT8 并发迁移。
// ============================================================================

// RunShell / MakeDirectoryTree / RemoveTree 定义在文件开头的 RT0-a 一节。
std::vector<std::string> DirectoryEntries(const std::string& path) {
  std::vector<std::string> names;
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    return names;
  }
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name != "." && name != "..") {
      names.push_back(name);
    }
  }
  ::closedir(directory);
  std::sort(names.begin(), names.end());
  return names;
}

int CountDirectoryEntries(const std::string& path) {
  return static_cast<int>(DirectoryEntries(path).size());
}

bool IsSymlink(const std::string& path) {
  struct stat info;
  return ::lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
}

std::string ReadLinkTarget(const std::string& path) {
  char buffer[4096];
  const ssize_t got = ::readlink(path.c_str(), buffer, sizeof(buffer) - 1);
  if (got < 0) {
    return std::string();
  }
  buffer[got] = '\0';
  return std::string(buffer);
}

bool CreateSymlinkTo(const std::string& target, const std::string& path) {
  return ::symlink(target.c_str(), path.c_str()) == 0;
}

std::uint64_t FileSizeBytes(const std::string& path) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(info.st_size);
}

std::string Sha256HexOf(const std::string& data) {
  backupproject::crypto::Sha256 hasher;
  hasher.Update(data.data(), data.size());
  unsigned char digest[backupproject::crypto::kSha256DigestSize];
  hasher.Final(digest);
  return backupproject::crypto::ToHex(digest, sizeof(digest));
}

std::string RawSha256Of(const std::string& data) {
  backupproject::crypto::Sha256 hasher;
  hasher.Update(data.data(), data.size());
  unsigned char digest[backupproject::crypto::kSha256DigestSize];
  hasher.Final(digest);
  return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

// 一份"受管"的快照三件套名字（.bak 后缀，单组件）。
std::string ManagedArchiveName(const std::string& stem) {
  return "remote-" + stem + ".bak";
}

std::string RandomHexLocal(std::size_t bytes) {
  std::string raw;
  std::string error;
  if (!backupproject::crypto::RandomBytes(bytes, &raw, &error)) {
    return std::string();
  }
  return backupproject::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

bool CopyFileBytes(const std::string& from, const std::string& to) {
  std::string content;
  if (!ReadFile(from, &content)) {
    return false;
  }
  return WriteFile(to, content);
}

void Note(const std::string& text) {
  std::printf("  NOTE  %s\n", text.c_str());
  std::fflush(stdout);
}

// 每个用例自己的小计：结束时打印 "RTn ... <checks> checks"，
// 报告里可以直接引用这一行。
void EndCase(const std::string& name, int checks_before, int failures_before) {
  const int checks = g_checks - checks_before;
  const int failures = g_failures - failures_before;
  std::printf("[%s] %d/%d checks passed%s\n", name.c_str(), checks - failures,
              checks, failures == 0 ? "" : "  <-- 有用例失败");
  std::fflush(stdout);
}

// ---- BPSNAP1 原始字节构造器 ----

struct RawBundleMember {
  std::string name;
  std::uint64_t declared_size = 0;
  std::string digest;  // 原始 32 字节
  std::string data;
  int declared_name_length = -1;  // < 0 表示用 name.size()
};

void AppendBeU16(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
  out->push_back(static_cast<char>(value & 0xFF));
}

void AppendBeU64(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

std::string BuildRawBundle(
    const std::vector<RawBundleMember>& members, int count_override = -1,
    const std::string& magic = std::string("BPSNAP1\0", 8),
    std::uint16_t version = 1, const std::string& trailer = std::string()) {
  std::string out = magic;
  AppendBeU16(&out, version);
  AppendBeU16(&out, static_cast<std::uint16_t>(
                        count_override < 0 ? static_cast<int>(members.size())
                                           : count_override));
  for (const RawBundleMember& member : members) {
    const std::size_t name_length =
        member.declared_name_length < 0
            ? member.name.size()
            : static_cast<std::size_t>(member.declared_name_length);
    AppendBeU16(&out, static_cast<std::uint16_t>(name_length));
    out.append(member.name.data(), member.name.size());
    AppendBeU64(&out, member.declared_size);
    out.append(member.digest);
    out.append(member.data);
  }
  out.append(trailer);
  return out;
}

RawBundleMember RawMember(const std::string& name, const std::string& data) {
  RawBundleMember member;
  member.name = name;
  member.declared_size = data.size();
  member.digest = RawSha256Of(data);
  member.data = data;
  return member;
}

// ---- 本地增量引擎：造一条真实的两跳链（full -> delta）----

struct LocalChain {
  std::string source_directory;
  std::string repository_directory;
  std::string full_name;
  std::string delta_name;
  std::string full_snapshot_id;
  std::string delta_snapshot_id;
};

bool BuildLocalChain(const std::string& root, const std::string& tag,
                     LocalChain* chain, std::string* error_message) {
  chain->source_directory = root + "/" + tag + "-src";
  chain->repository_directory = root + "/" + tag + "-repo";
  if (!MakeDirectoryTree(chain->source_directory) ||
      !MakeDirectoryTree(chain->repository_directory)) {
    if (error_message != nullptr) {
      *error_message = "无法建本地链目录";
    }
    return false;
  }
  if (!WriteFile(chain->source_directory + "/a.txt",
                 std::string(tag.size() * 7 + 3, 'a') + "-first") ||
      !WriteFile(chain->source_directory + "/b.txt", "second-" + tag)) {
    if (error_message != nullptr) {
      *error_message = "无法准备源文件";
    }
    return false;
  }
  backupproject::Filter filter;
  backupproject::BackupOptions options;
  backupproject::IncrementalOutcome outcome;
  chain->full_name = ManagedArchiveName(tag + "-g0");
  const std::string repository_identity = "rt-repo-" + tag;
  if (!backupproject::RunIncrementalBackup(
          chain->source_directory, chain->repository_directory,
          chain->full_name, repository_identity, filter, options,
          std::vector<std::string>(), std::vector<std::string>(), std::string(),
          &outcome, error_message)) {
    return false;
  }
  if (outcome.kind != backupproject::IncrementalOutcome::Kind::kFullBaseline) {
    if (error_message != nullptr) {
      *error_message = "第一次备份没有产出完整基线";
    }
    return false;
  }
  if (!WriteFile(chain->source_directory + "/a.txt",
                 std::string(tag.size() * 11 + 5, 'b') + "-changed") ||
      !WriteFile(chain->source_directory + "/c.txt", "added-" + tag)) {
    if (error_message != nullptr) {
      *error_message = "无法修改源文件";
    }
    return false;
  }
  chain->delta_name = ManagedArchiveName(tag + "-g1");
  if (!backupproject::RunIncrementalBackup(
          chain->source_directory, chain->repository_directory,
          chain->delta_name, repository_identity, filter, options,
          std::vector<std::string>(), std::vector<std::string>(),
          chain->full_name, &outcome, error_message)) {
    return false;
  }
  if (outcome.kind != backupproject::IncrementalOutcome::Kind::kDelta) {
    if (error_message != nullptr) {
      *error_message = "第二次备份没有产出 delta";
    }
    return false;
  }
  backupproject::SnapshotIdentity full_identity;
  backupproject::SnapshotIdentity delta_identity;
  std::string identity_error;
  if (!backupproject::LoadVerifiedSnapshotIdentity(
          chain->repository_directory, chain->full_name, &full_identity,
          nullptr, &identity_error) ||
      !full_identity.sidecars_verified) {
    if (error_message != nullptr) {
      *error_message = "完整基线身份验证不过：" + identity_error;
    }
    return false;
  }
  if (!backupproject::LoadVerifiedSnapshotIdentity(
          chain->repository_directory, chain->delta_name, &delta_identity,
          nullptr, &identity_error) ||
      !delta_identity.sidecars_verified) {
    if (error_message != nullptr) {
      *error_message = "delta 身份验证不过：" + identity_error;
    }
    return false;
  }
  chain->full_snapshot_id = full_identity.snapshot_id;
  chain->delta_snapshot_id = delta_identity.snapshot_id;
  return true;
}

// ---- 真实服务端夹具（真 TCP 环回 + BPSEC1 + 真实元数据库）----

struct RedTeamServer {
  std::string base;
  std::string pin;
  std::string username;
  std::string password;
  bool started = false;
  backupproject::net::RemoteServer server;
  std::thread runner;
  backupproject::net::RemoteArchiveClient client;
};

RedTeamServer* g_red_team_server = nullptr;

bool StartRedTeamServer(RedTeamServer* fixture, const std::string& tag,
                        std::string* error_message) {
  fixture->base = WorkRoot() + "/rt-server-" + tag;
  if (!MakeDirectoryTree(fixture->base + "/srv/data") ||
      !MakeDirectoryTree(fixture->base + "/srv/state")) {
    if (error_message != nullptr) {
      *error_message = "无法建服务端目录";
    }
    return false;
  }
  std::string secret;
  if (!backupproject::crypto::RandomBytes(32, &secret, error_message)) {
    return false;
  }
  const std::string secret_hex = backupproject::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(secret.data()), secret.size());
  const std::string secret_path = fixture->base + "/srv/secrets.env";
  if (!WriteFile(secret_path, "BACKUP_TOKEN_SECRET=" + secret_hex + "\n") ||
      ::chmod(secret_path.c_str(), 0600) != 0) {
    if (error_message != nullptr) {
      *error_message = "无法写 secret 文件";
    }
    return false;
  }
  const std::string key_path = fixture->base + "/srv/transport.key";
  backupproject::net::TransportIdentity identity;
  if (!remote_test_support::PrepareTransportIdentity(
          key_path, &identity, &fixture->pin, error_message)) {
    return false;
  }
  backupproject::net::RemoteServerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;  // 内核分配端口：测试之间不抢
  config.root_directory = fixture->base + "/srv/data";
  config.database_path = fixture->base + "/srv/state/metadata.sqlite3";
  config.secret_file_path = secret_path;
  config.transport_key_file_path = key_path;
  config.quiet = true;
  // 共享客户端自己占一个 worker；RT9 还要同时开两条原始连接，所以留够。
  config.worker_count = 4;
  if (!fixture->server.Configure(config, error_message) ||
      !fixture->server.Start(error_message)) {
    return false;
  }
  fixture->runner = std::thread([fixture] {
    std::string local_error;
    (void)fixture->server.Run(&local_error);
  });
  fixture->username = "rt-" + RandomHexLocal(4);
  fixture->password = RandomHexLocal(16);
  backupproject::net::RemoteEndpoint endpoint;
  endpoint.host = "127.0.0.1";
  endpoint.port = fixture->server.bound_port();
  endpoint.server_key_pin = fixture->pin;
  if (!fixture->client.Connect(endpoint, error_message) ||
      !fixture->client.Register(fixture->username, fixture->password,
                                error_message) ||
      !fixture->client.Login(fixture->username, fixture->password,
                             error_message)) {
    return false;
  }
  fixture->started = true;
  return true;
}

void StopRedTeamServer(RedTeamServer* fixture) {
  if (!fixture->started) {
    return;
  }
  fixture->client.Disconnect();
  fixture->server.RequestStop();
  if (fixture->runner.joinable()) {
    fixture->runner.join();
  }
  fixture->server.Stop();
  fixture->started = false;
}

// 整个测试进程共用一个服务端：用例之间只共享"一台服务器 + 一个账户"，
// 缓存目录按指纹区分，互不干扰。
RedTeamServer* SharedServer() {
  if (g_red_team_server != nullptr) {
    return g_red_team_server;
  }
  RedTeamServer* fixture = new RedTeamServer();
  std::string error;
  if (!StartRedTeamServer(fixture, "shared", &error)) {
    std::printf("  FAIL RT0 共享服务端启动 -- %s\n", error.c_str());
    g_checks += 1;
    g_failures += 1;
    delete fixture;
    return nullptr;
  }
  g_red_team_server = fixture;
  return g_red_team_server;
}

void ShutdownSharedServer() {
  if (g_red_team_server != nullptr) {
    StopRedTeamServer(g_red_team_server);
    delete g_red_team_server;
    g_red_team_server = nullptr;
  }
}

// 造一个属于共享服务端账户的本地缓存（指纹决定目录，用一个字符区分用例）。
bool PrepareSharedCache(char fingerprint_fill, const std::string& tag,
                        backupproject::net::RemoteCacheLayout* layout,
                        std::string* error_message) {
  RedTeamServer* fixture = SharedServer();
  if (fixture == nullptr) {
    if (error_message != nullptr) {
      *error_message = "共享服务端不可用";
    }
    return false;
  }
  return backupproject::net::PrepareRemoteCache(
      fixture->base + "/cache-" + tag, Hex64(fingerprint_fill),
      fixture->username, layout, error_message);
}

// 把仓库目录里的三件套打包上传。
bool UploadTrio(RedTeamServer* fixture, const std::string& repository_directory,
                const std::string& archive_name,
                const std::string& display_name, std::uint16_t kind,
                const std::string& parent_snapshot_id,
                const std::string& lineage,
                backupproject::net::RemoteSnapshotInfo* uploaded,
                std::string* error_message) {
  const std::string bundle_path =
      repository_directory + "/.rt-upload-" + archive_name + ".bundle";
  (void)::unlink(bundle_path.c_str());
  backupproject::net::SnapshotBundleInfo bundle_info;
  if (!backupproject::net::BuildSnapshotBundle(repository_directory,
                                               archive_name, bundle_path,
                                               &bundle_info, error_message)) {
    return false;
  }
  backupproject::net::RemoteUploadOptions options;
  options.snapshot_kind = kind;
  options.parent_snapshot_id = parent_snapshot_id;
  options.lineage = lineage;
  const bool ok = fixture->client.UploadSnapshotFile(
      bundle_path, display_name, options, nullptr, uploaded, error_message);
  (void)::unlink(bundle_path.c_str());
  return ok;
}

std::uint64_t DownloadBeginCount(RedTeamServer* fixture) {
  return fixture->server.request_count_for_testing(
      static_cast<std::uint16_t>(backupproject::net::Opcode::kDownloadBegin));
}

// 从 from_directory 出发、按目录层数生成相对路径，指回 target_absolute。
// 手写而不是猜 "../" 的个数：缓存目录是 <root>/<指纹前16>/<用户名>。
std::string RelativeEscapePath(const std::string& from_directory,
                               const std::string& target_absolute) {
  int depth = 0;
  for (std::size_t index = 0; index < from_directory.size(); ++index) {
    if (from_directory[index] == '/') {
      depth += 1;
    }
  }
  std::string out;
  for (int index = 0; index < depth; ++index) {
    out += "../";
  }
  std::string trimmed = target_absolute;
  while (!trimmed.empty() && trimmed[0] == '/') {
    trimmed.erase(0, 1);
  }
  return out + trimmed;
}
// ============================================================================
// RT1. BPSNAP1 材料包的恶意构造（逐条构造，不做随机变异）
// ============================================================================

struct BundleProbe {
  bool wrote = false;
  bool inspected = false;
  bool extracted = false;
  int entries = 0;
  std::vector<std::string> names;
  std::string inspect_error;
  std::string extract_error;
};

BundleProbe ProbeBundle(const std::string& root, const std::string& tag,
                        const std::string& bytes) {
  BundleProbe probe;
  const std::string bundle_path = root + "/" + tag + ".bundle";
  const std::string out = root + "/out-" + tag;
  RemoveTree(out);
  MakeDirectoryTree(out);
  probe.wrote = WriteFile(bundle_path, bytes);
  backupproject::net::SnapshotBundleInfo inspect_info;
  probe.inspected = backupproject::net::InspectSnapshotBundle(
      bundle_path, &inspect_info, &probe.inspect_error);
  backupproject::net::SnapshotBundleInfo extract_info;
  probe.extracted = backupproject::net::ExtractSnapshotBundle(
      bundle_path, out, &extract_info, &probe.extract_error);
  probe.names = DirectoryEntries(out);
  probe.entries = static_cast<int>(probe.names.size());
  return probe;
}

// 每一条构造都要满足三条：Inspect 或 Extract 失败；目标目录里一个文件都不留；
// 没有在目标目录之外创建任何东西（逃逸）。如果它居然解包成功，还要满足
// 核心不变式：解出来的必须**恰好**是那一组三件套，不能变成任意 archive 容器。
void ExpectBundleRejected(const std::string& root, const std::string& tag,
                          const std::string& bytes,
                          const std::string& escape_path,
                          const std::string& what) {
  const BundleProbe probe = ProbeBundle(root, tag, bytes);
  Check(probe.wrote, "RT1." + tag + " 写出材料包");
  const bool rejected = !probe.inspected || !probe.extracted;
  const std::string detail =
      probe.inspected ? ("Extract 居然成功或失败原因=" + probe.extract_error)
                      : ("Inspect 失败原因=" + probe.inspect_error);
  Check(rejected, "RT1." + tag + " " + what + " -> 被拒绝", detail);
  Check(probe.entries == 0, "RT1." + tag + " 失败时目标目录不留任何文件",
        std::to_string(probe.entries) + " 个条目");
  if (!escape_path.empty()) {
    Check(!FileExists(escape_path),
          "RT1." + tag + " 没有在目标目录之外创建文件");
  }
  if (probe.extracted) {
    Check(probe.entries == 3, "RT1." + tag + " 解包只发布三件套（不变式）");
  }
}

void TestBundleMaliciousConstructions() {
  std::printf("[chain] RT1 材料包 BPSNAP1 的恶意构造\n");
  const int checks_before = g_checks;
  const int failures_before = g_failures;

  const std::string root = WorkRoot() + "/rt1-bundle";
  RemoveTree(root);
  Check(MakeDirectoryTree(root + "/repo"), "RT1 建测试目录");

  const std::string archive = "remote-rt1-100-g0.bak";
  const std::string manifest = archive + ".manifest";
  const std::string identity = archive + ".identity";
  const std::string bak_data(2048, 'B');
  const std::string manifest_data = "BPMANIFEST3 1\nentry\n";
  const std::string identity_data = "BPIDENT2\nsnapshot_id=deadbeef\n";

  std::vector<RawBundleMember> baseline;
  baseline.push_back(RawMember(archive, bak_data));
  baseline.push_back(RawMember(manifest, manifest_data));
  baseline.push_back(RawMember(identity, identity_data));
  const std::string baseline_bytes = BuildRawBundle(baseline);

  // 构造器本身必须先能被产品接受：否则下面所有"被拒绝"都不说明问题。
  {
    const BundleProbe probe = ProbeBundle(root, "baseline", baseline_bytes);
    Check(probe.inspected && probe.extracted,
          "RT1.0 构造器产出的基准包被产品接受", probe.extract_error);
    Check(probe.entries == 3, "RT1.0 基准包恰好解开三件套");
  }

  // ---- 成员名 ----
  struct NameAttack {
    std::string tag;
    std::string name;
    int declared_name_length;
    std::string escape_path;
  };
  std::vector<NameAttack> name_attacks;
  name_attacks.push_back({"01-traversal", "../x.bak", -1, root + "/x.bak"});
  name_attacks.push_back({"02-absolute", "/abs.bak", -1, root + "/abs.bak"});
  name_attacks.push_back(
      {"03-subdir", "a/b.bak", -1, root + "/out-03-subdir/a"});
  name_attacks.push_back({"04-backslash", "a\\b.bak", -1, std::string()});
  name_attacks.push_back(
      {"05-nul", std::string("a\0b.bak", 7), -1, std::string()});
  name_attacks.push_back({"06-tab", "a\tb.bak", -1, std::string()});
  name_attacks.push_back({"07-newline", "a\nb.bak", -1, std::string()});
  name_attacks.push_back({"08-empty", std::string(), 0, std::string()});
  name_attacks.push_back(
      {"09-overlong", std::string(252, 'x') + ".bak", -1, std::string()});

  for (const NameAttack& attack : name_attacks) {
    std::vector<RawBundleMember> members;
    members.push_back(RawMember(attack.name, "ARCHIVE-DATA"));
    members.push_back(RawMember(attack.name + ".manifest", "MANIFEST-DATA"));
    members.push_back(RawMember(attack.name + ".identity", "IDENTITY-DATA"));
    if (attack.declared_name_length >= 0) {
      members[0].declared_name_length = attack.declared_name_length;
      members[1].declared_name_length = attack.declared_name_length;
      members[2].declared_name_length = attack.declared_name_length;
    }
    ExpectBundleRejected(root, attack.tag, BuildRawBundle(members),
                         attack.escape_path, "成员名不合法");
  }

  // ---- 三件套的组成与顺序 ----
  {
    std::vector<RawBundleMember> missing_manifest;
    missing_manifest.push_back(RawMember(archive, bak_data));
    missing_manifest.push_back(RawMember(archive + ".x", manifest_data));
    missing_manifest.push_back(RawMember(identity, identity_data));
    ExpectBundleRejected(root, "10-no-manifest",
                         BuildRawBundle(missing_manifest), std::string(),
                         "缺 .manifest");

    std::vector<RawBundleMember> missing_identity;
    missing_identity.push_back(RawMember(archive, bak_data));
    missing_identity.push_back(RawMember(manifest, manifest_data));
    missing_identity.push_back(RawMember(manifest, manifest_data));
    ExpectBundleRejected(root, "11-no-identity",
                         BuildRawBundle(missing_identity), std::string(),
                         "缺 .identity");

    std::vector<RawBundleMember> duplicate_bak;
    duplicate_bak.push_back(RawMember(archive, bak_data));
    duplicate_bak.push_back(RawMember(archive, bak_data));
    duplicate_bak.push_back(RawMember(identity, identity_data));
    ExpectBundleRejected(root, "12-duplicate-bak",
                         BuildRawBundle(duplicate_bak), std::string(),
                         "重复 .bak");

    std::vector<RawBundleMember> fourth;
    fourth.push_back(RawMember(archive, bak_data));
    fourth.push_back(RawMember(manifest, manifest_data));
    fourth.push_back(RawMember(identity, identity_data));
    fourth.push_back(RawMember("extra.bin", "EXTRA"));
    ExpectBundleRejected(root, "13-fourth-member", BuildRawBundle(fourth),
                         std::string(), "第四个未知成员（成员数 4）");

    std::vector<RawBundleMember> shuffled;
    shuffled.push_back(RawMember(archive, bak_data));
    shuffled.push_back(RawMember(identity, identity_data));
    shuffled.push_back(RawMember(manifest, manifest_data));
    ExpectBundleRejected(root, "14-shuffled", BuildRawBundle(shuffled),
                         std::string(), "三件套顺序被打乱");
  }

  // ---- 长度与摘要 ----
  const std::string empty_digest = RawSha256Of(std::string());
  {
    std::vector<RawBundleMember> empty_member;
    RawBundleMember zero;
    zero.name = archive;
    zero.declared_size = 0;
    zero.digest = empty_digest;  // 摘要与"空内容"自洽：这不是校验能挡住的错误
    zero.data = std::string();
    empty_member.push_back(zero);
    empty_member.push_back(RawMember(manifest, manifest_data));
    empty_member.push_back(RawMember(identity, identity_data));
    const BundleProbe probe =
        ProbeBundle(root, "20-empty-member", BuildRawBundle(empty_member));
    Check(!probe.inspected || !probe.extracted,
          "RT1.20 必需成员长度为 0 -> 被拒绝",
          probe.extracted ? "Extract 居然接受了 0 字节的 .bak"
                          : ("Inspect: " + probe.inspect_error));
    Check(probe.entries == 0, "RT1.20 失败时目标目录不留任何文件",
          std::to_string(probe.entries) + " 个条目");
    if (probe.extracted) {
      // 安全网还在：0 字节的 .bak 交到增量引擎手里一定过不了身份验证，
      // 所以这是"格式层契约不对称"，不是"能拿它当可信材料用"。
      backupproject::SnapshotIdentity empty_identity;
      std::string identity_error;
      const bool loaded = backupproject::LoadVerifiedSnapshotIdentity(
          root + "/out-20-empty-member", archive, &empty_identity, nullptr,
          &identity_error);
      Check(!loaded,
            "RT1.20 0 字节的 .bak 交到引擎手里仍然不可信（安全网仍在）",
            identity_error);
      Note(
          "RT1.20 本轮修掉的缺陷 F1（读/写契约不对称）：修复前写侧拒绝空成员"
          "（BuildSnapshotBundle 里 member.size == 0 即失败），读侧却接受 "
          "declared_size == 0，ExtractSnapshotBundle 会发布 0 字节的 .bak 与"
          "两个副文件。现在 ReadBundleIndex 同样拒绝空成员，本用例的 Check "
          "就是这条回归。安全影响有限（引擎随后仍会拒绝），但读侧不该接受"
          "写侧绝不产出的东西。");
    }

    std::vector<RawBundleMember> oversized;
    RawBundleMember huge = RawMember(archive, bak_data);
    huge.declared_size = backupproject::net::kSnapshotBundleMaxMemberBytes + 1;
    oversized.push_back(huge);
    oversized.push_back(RawMember(manifest, manifest_data));
    oversized.push_back(RawMember(identity, identity_data));
    ExpectBundleRejected(root, "21-max-plus-one", BuildRawBundle(oversized),
                         std::string(), "声明长度超过单个成员上限");

    std::vector<RawBundleMember> u64_max;
    RawBundleMember biggest = RawMember(archive, bak_data);
    biggest.declared_size = 0xFFFFFFFFFFFFFFFFull;
    u64_max.push_back(biggest);
    RawBundleMember biggest2 = RawMember(manifest, manifest_data);
    biggest2.declared_size = 0xFFFFFFFFFFFFFFFFull;
    u64_max.push_back(biggest2);
    RawBundleMember biggest3 = RawMember(identity, identity_data);
    biggest3.declared_size = 0xFFFFFFFFFFFFFFFFull;
    u64_max.push_back(biggest3);
    ExpectBundleRejected(root, "22-u64-max", BuildRawBundle(u64_max),
                         std::string(), "长度字段本身是 0xFFFFFFFFFFFFFFFF");

    // 格式里没有"三个成员长度之和"字段（头只有 12 字节），而每个成员的上限是
    // 16 GiB，三者之和 48 GiB 远小于 2^64：长度和溢出在这个格式里不可表达。
    Check(backupproject::net::kSnapshotBundleHeaderSize == 12,
          "RT1.23 头部只有 12 字节（没有总和字段）");
    Check(3 * backupproject::net::kSnapshotBundleMaxMemberBytes <
              0xFFFFFFFFFFFFFFFFull,
          "RT1.23 三个成员的上限之和仍然远小于 2^64（求和不可能溢出）");
  }

  // 摘要被截断：成员头只写了 20 字节摘要就 EOF。
  {
    std::string bytes = std::string("BPSNAP1\0", 8);
    AppendBeU16(&bytes, 1);
    AppendBeU16(&bytes, 3);
    AppendBeU16(&bytes, static_cast<std::uint16_t>(archive.size()));
    bytes += archive;
    AppendBeU64(&bytes, 10);
    bytes.append(20, '\0');  // 摘要只有 20/32 字节
    ExpectBundleRejected(root, "24-truncated-digest", bytes, std::string(),
                         "成员头在摘要中间被截断");
  }
  {
    std::vector<RawBundleMember> wrong_digest;
    RawBundleMember member = RawMember(archive, bak_data);
    member.digest = RawSha256Of("not-the-data");
    wrong_digest.push_back(member);
    wrong_digest.push_back(RawMember(manifest, manifest_data));
    wrong_digest.push_back(RawMember(identity, identity_data));
    ExpectBundleRejected(root, "25-wrong-digest", BuildRawBundle(wrong_digest),
                         std::string(), "成员摘要与内容不符");
  }

  // ---- 尾部追加字节：确定结论 ----
  //
  // 结论（依据在下面的断言与注释里）：**解析层明确忽略尾部字节**——
  // ReadBundleIndex 读完声明的 count 个成员就返回，不检查是否已到文件尾；
  // InspectSnapshotBundle 把整个文件长度记进 bundle_size，ExtractSnapshotBundle
  // 也只读三个成员的数据区。所以：
  //   * 解析层既不发现也不拒绝；
  //   * 但整包 SHA-256 会变，而客户端下载路径在发布前会校验"实际字节的
  //     SHA-256 == 服务端声明值"（src/network/remote_backup_client.cpp:
  //     DownloadArchiveFile），服务端声明的摘要与长度来自它自己存的字节，
  //     因此尾部追加这一层会被下载校验挡住（信任链第 1 层）。
  {
    const std::string trailer = "TRAILING-JUNK-AFTER-THE-THIRD-MEMBER";
    const std::string with_trailer = baseline_bytes + trailer;
    const BundleProbe probe = ProbeBundle(root, "30-trailing", with_trailer);
    Check(probe.inspected, "RT1.30 尾部追加：解析层忽略（Inspect 成功）",
          probe.inspect_error);
    Check(probe.extracted, "RT1.30 尾部追加：Extract 成功且只取三个成员",
          probe.extract_error);
    Check(probe.entries == 3, "RT1.30 尾部追加：目标目录恰好三件套",
          std::to_string(probe.entries) + " 个条目");
    std::string extracted_bak;
    const std::string out = root + "/out-30-trailing";
    Check(ReadFile(out + "/" + archive, &extracted_bak) &&
              extracted_bak == bak_data,
          "RT1.30 尾部追加：解出来的 .bak 与原件逐字节一致");
    backupproject::net::SnapshotBundleInfo info;
    std::string error;
    Check(backupproject::net::InspectSnapshotBundle(
              root + "/30-trailing.bundle", &info, &error) &&
              info.bundle_size == with_trailer.size() &&
              info.bundle_size == FileSizeBytes(root + "/30-trailing.bundle"),
          "RT1.30 尾部追加：bundle_size 报告的是整个文件长度");
    Check(Sha256HexOf(with_trailer) != Sha256HexOf(baseline_bytes),
          "RT1.30 尾部追加：整包 SHA-256 与干净包不同（下载校验会挡住）");
  }

  RemoveTree(root);
  EndCase("RT1", checks_before, failures_before);
}

// ============================================================================
// RT2. 解包时的本地文件系统攻击
// ============================================================================

const char* g_rt_race_target = nullptr;
int g_rt_race_injections = 0;
backupproject::file_io_syscalls::LinkFn g_rt_real_link = nullptr;

int RtInjectTargetBeforePublish(const char* existing_path,
                                const char* new_path) {
  if (g_rt_race_target != nullptr && new_path != nullptr &&
      std::strcmp(new_path, g_rt_race_target) == 0 &&
      ::access(new_path, F_OK) != 0) {
    const int fd = ::open(new_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      const ssize_t written = ::write(fd, "RT-RACE-SENTINEL", 16);
      (void)written;
      ::close(fd);
      g_rt_race_injections += 1;
    }
  }
  return g_rt_real_link(existing_path, new_path);
}

void TestExtractionLocalAttacks() {
  std::printf("[chain] RT2 解包时的本地文件系统攻击\n");
  const int checks_before = g_checks;
  const int failures_before = g_failures;

  const std::string root = WorkRoot() + "/rt2-extract";
  RemoveTree(root);
  Check(MakeDirectoryTree(root + "/repo"), "RT2 建测试目录");

  const std::string archive = "remote-rt2-100-g0.bak";
  const std::string manifest = archive + ".manifest";
  const std::string identity = archive + ".identity";
  const std::string bak_data(4096, 'K');
  const std::string manifest_data = "BPMANIFEST3 1\nentry\n";
  const std::string identity_data = "BPIDENT2\nsnapshot_id=cafebabe\n";
  Check(WriteFile(root + "/repo/" + archive, bak_data) &&
            WriteFile(root + "/repo/" + manifest, manifest_data) &&
            WriteFile(root + "/repo/" + identity, identity_data),
        "RT2 准备真实三件套");
  backupproject::net::SnapshotBundleInfo bundle_info;
  std::string error;
  const std::string bundle_path = root + "/ok.bundle";
  Check(backupproject::net::BuildSnapshotBundle(
            root + "/repo", archive, bundle_path, &bundle_info, &error),
        "RT2 打出真实材料包", error);

  // (a) 目标文件已存在（内容不同）-> 不许覆盖
  {
    const std::string out = root + "/out-exists";
    Check(MakeDirectoryTree(out) && WriteFile(out + "/" + archive, "EXISTING"),
          "RT2.1 造出已存在的目标");
    backupproject::net::SnapshotBundleInfo info;
    std::string extract_error;
    Check(!backupproject::net::ExtractSnapshotBundle(bundle_path, out, &info,
                                                     &extract_error),
          "RT2.1 目标已存在 -> 解包失败", extract_error);
    std::string content;
    Check(ReadFile(out + "/" + archive, &content) && content == "EXISTING",
          "RT2.1 已有文件一个字节都没被改");
    Check(
        !FileExists(out + "/" + manifest) && !FileExists(out + "/" + identity),
        "RT2.1 失败时没有留下半套");
  }

  // (b)(c)(d) 目标三件套之一是符号链接 -> 不许跟随写入
  struct LinkAttack {
    std::string tag;
    std::string member;
  };
  const std::vector<LinkAttack> link_attacks = {
      {"2-bak", archive}, {"3-manifest", manifest}, {"4-identity", identity}};
  for (const LinkAttack& attack : link_attacks) {
    const std::string out = root + "/out-link-" + attack.tag;
    Check(MakeDirectoryTree(out), "RT2." + attack.tag + " 建目标目录");
    const std::string outside = root + "/outside-" + attack.tag + ".txt";
    Check(WriteFile(outside, "OUTSIDE-CONTENT"),
          "RT2." + attack.tag + " 写外部文件");
    Check(CreateSymlinkTo(outside, out + "/" + attack.member),
          "RT2." + attack.tag + " 把目标成员做成符号链接");
    backupproject::net::SnapshotBundleInfo info;
    std::string extract_error;
    Check(!backupproject::net::ExtractSnapshotBundle(bundle_path, out, &info,
                                                     &extract_error),
          "RT2." + attack.tag + " 目标是符号链接 -> 解包失败", extract_error);
    std::string outside_content;
    Check(ReadFile(outside, &outside_content) &&
              outside_content == "OUTSIDE-CONTENT",
          "RT2." + attack.tag + " 符号链接指向的文件没有被写入");
    Check(IsSymlink(out + "/" + attack.member),
          "RT2." + attack.tag + " 符号链接本身还在（没被换成普通文件）");
    const int entries = CountDirectoryEntries(out);
    Check(entries == 1, "RT2." + attack.tag + " 失败时只留下那个符号链接",
          std::to_string(entries) + " 个条目");
  }

  // (e) 目标目录里已经有同名 .part- 临时文件
  {
    const std::string out = root + "/out-part";
    Check(MakeDirectoryTree(out), "RT2.5 建目标目录");
    const std::string pid = std::to_string(static_cast<long>(::getpid()));
    // 产品给每个成员用的临时名是 <成员最终路径>.part-<pid>-<成员序号>，
    // 所以三个成员各有各的临时名（不是都以 .bak 结尾）。
    const std::string stale_parts[3] = {archive + ".part-" + pid + "-0",
                                        manifest + ".part-" + pid + "-1",
                                        identity + ".part-" + pid + "-2"};
    for (int index = 0; index < 3; ++index) {
      Check(WriteFile(out + "/" + stale_parts[index], "STALE-PART-CONTENT"),
            "RT2.5 预置同名 .part- 临时文件");
    }
    backupproject::net::SnapshotBundleInfo info;
    std::string extract_error;
    Check(backupproject::net::ExtractSnapshotBundle(bundle_path, out, &info,
                                                    &extract_error),
          "RT2.5 已有 .part- 文件时解包仍然成功（清理后重建）", extract_error);
    std::string a;
    std::string b;
    Check(ReadFile(out + "/" + archive, &a) && a == bak_data,
          "RT2.5 产物是包里的字节，不是预置的临时文件内容");
    Check(ReadFile(out + "/" + manifest, &b) && b == manifest_data,
          "RT2.5 .manifest 也是包里的字节");
    bool part_left = false;
    for (const std::string& name : DirectoryEntries(out)) {
      if (name.find(".part-") != std::string::npos) {
        part_left = true;
      }
    }
    Check(!part_left,
          "RT2.5 收尾之后目录里没有 .part- "
          "残留（三个预置临时文件都被清理并重建）");
    Check(CountDirectoryEntries(out) == 3, "RT2.5 目录里恰好三件套");
  }

  // (f) 目标目录本身是符号链接 -> 明确行为：落到真实目录，不越出，不替换链接
  {
    const std::string real = root + "/out-real-dir";
    const std::string link = root + "/out-dir-link";
    Check(MakeDirectoryTree(real) && CreateSymlinkTo(real, link),
          "RT2.6 造一个指向真实目录的符号链接");
    backupproject::net::SnapshotBundleInfo info;
    std::string extract_error;
    const bool ok = backupproject::net::ExtractSnapshotBundle(
        bundle_path, link, &info, &extract_error);
    Check(ok, "RT2.6 目标目录是符号链接 -> 解包成功（落到真实目录）",
          extract_error);
    Check(CountDirectoryEntries(real) == 3, "RT2.6 真实目录里恰好三件套",
          std::to_string(CountDirectoryEntries(real)) + " 个条目");
    Check(IsSymlink(link), "RT2.6 符号链接本身没有被替换成目录");
    Check(!FileExists(root + "/" + archive),
          "RT2.6 没有在符号链接旁边创建文件");
  }

  // (g) 目标目录不存在
  {
    backupproject::net::SnapshotBundleInfo info;
    std::string extract_error;
    Check(!backupproject::net::ExtractSnapshotBundle(
              bundle_path, root + "/out-does-not-exist", &info, &extract_error),
          "RT2.7 目标目录不存在 -> 解包失败", extract_error);
    Check(!FileExists(root + "/out-does-not-exist"),
          "RT2.7 失败时没有把目标目录建出来");
  }

  // (h) 发布瞬间被替换：用 file_io 的 link 注入点在"publish 那一刻"造出目标
  {
    const std::string out = root + "/out-inject";
    Check(MakeDirectoryTree(out), "RT2.8 建目标目录");
    const std::string race_target = out + "/" + archive;
    g_rt_race_target = race_target.c_str();
    g_rt_race_injections = 0;
    g_rt_real_link = backupproject::file_io_syscalls::LinkHook();
    backupproject::file_io_syscalls::LinkHook() = &RtInjectTargetBeforePublish;
    backupproject::net::SnapshotBundleInfo info;
    std::string extract_error;
    const bool ok = backupproject::net::ExtractSnapshotBundle(
        bundle_path, out, &info, &extract_error);
    backupproject::file_io_syscalls::LinkHook() = g_rt_real_link;
    g_rt_real_link = nullptr;
    g_rt_race_target = nullptr;
    Check(!ok, "RT2.8 发布前目标被抢走 -> 解包失败", extract_error);
    Check(g_rt_race_injections == 1, "RT2.8 注入恰好命中一次发布",
          std::to_string(g_rt_race_injections));
    std::string content;
    Check(ReadFile(out + "/" + archive, &content) &&
              content == "RT-RACE-SENTINEL",
          "RT2.8 抢走目标的那份文件没有被覆盖");
    Check(
        !FileExists(out + "/" + manifest) && !FileExists(out + "/" + identity),
        "RT2.8 失败时没有留下半套");
  }

  // (i) 真实并发替换：另一个线程在解包期间反复创建/删除目标 .bak。
  //     不变式：要么解包失败且不留半套，要么成功的三件套齐全且字节正确。
  {
    const std::string out = root + "/out-race";
    Check(MakeDirectoryTree(out), "RT2.9 建竞态目录");
    const std::string target = out + "/" + archive;
    // 竞争者只**创建/截断**目标，从不删除它：这样"半套"的判定才是干净的
    // ——.manifest / .identity 只可能来自产品自己的发布，而产品发布 .bak 在前，
    // 因此只要副文件出现，三件套就必须齐全。
    // 两种节奏交替，保证"产品输"和"产品赢"两条路径都被真实覆盖：
    //   phase 0 = 竞争者贴着目标打（产品几乎必输）；
    //   phase 1 = 竞争者放慢（每 200 微秒一次，产品有机会赢下发布）。
    std::atomic<int> phase(0);
    std::atomic<bool> stop(false);
    std::thread racer([&stop, &target, &phase] {
      while (!stop.load()) {
        const int fd =
            ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
          const ssize_t written = ::write(fd, "RACER", 5);
          (void)written;
          ::close(fd);
        }
        if (phase.load() == 1) {
          ::usleep(20000);  // 放慢到 20 毫秒一次：给产品赢下发布的机会
        }
      }
    });
    int rounds = 0;
    int ok_rounds = 0;
    int half_trio = 0;
    int sidecar_mismatch = 0;
    int unexpected_bytes = 0;
    for (int round = 0; round < 150; ++round) {
      phase.store(round % 2);
      RemoveTree(out);
      MakeDirectoryTree(out);
      backupproject::net::SnapshotBundleInfo info;
      std::string extract_error;
      const bool ok = backupproject::net::ExtractSnapshotBundle(
          bundle_path, out, &info, &extract_error);
      rounds += 1;
      const bool has_bak = FileExists(out + "/" + archive);
      const bool has_manifest = FileExists(out + "/" + manifest);
      const bool has_identity = FileExists(out + "/" + identity);
      if ((has_manifest || has_identity) &&
          !(has_bak && has_manifest && has_identity)) {
        half_trio += 1;
      }
      if (has_manifest) {
        std::string content;
        if (!ReadFile(out + "/" + manifest, &content) ||
            content != manifest_data) {
          sidecar_mismatch += 1;
        }
      }
      if (has_identity) {
        std::string content;
        if (!ReadFile(out + "/" + identity, &content) ||
            content != identity_data) {
          sidecar_mismatch += 1;
        }
      }
      if (ok) {
        ok_rounds += 1;
        // .bak 在竞争者的控制下，它的内容只可能是三种之一：包里的字节、
        // 竞争者刚写下的 "RACER"、或 O_TRUNC 之后被读到的空文件。
        std::string content;
        if (ReadFile(out + "/" + archive, &content) && content != bak_data &&
            content != "RACER" && !content.empty()) {
          unexpected_bytes += 1;
        }
      }
    }
    stop.store(true);
    racer.join();
    Check(rounds == 150, "RT2.9 竞态轮次完整", std::to_string(rounds));
    Check(half_trio == 0, "RT2.9 任何时刻都没有出现过半套三件套",
          std::to_string(half_trio) + " 次");
    Check(sidecar_mismatch == 0,
          "RT2.9 两个副文件只要在，内容就逐字节正确（竞争者碰不到它们）",
          std::to_string(sidecar_mismatch) + " 次");
    Check(unexpected_bytes == 0,
          "RT2.9 目标 .bak 的内容只会是包里的字节或竞争者写的 RACER",
          std::to_string(unexpected_bytes) + " 次");
    Note("RT2.9 竞态观测：150 轮里解包成功 " + std::to_string(ok_rounds) +
         " 次，其余被并发替换挡掉（失败是允许的结果，半套不是）");
  }

  // (j) TOCTOU：解包期间 bundle 文件本身被改写。
  //
  // ExtractSnapshotBundle 会**解析两遍**：第一遍 ReadBundleIndex 校验成员名与
  // 长度，第二遍（写数据那一段）重新读成员头，然后直接
  // target_directory + "/" + name 拼路径 —— 第二遍没有任何单组件名字校验，
  // 也没有和第一遍的成员表比对。于是"文件在两次解析之间被改写"就能把写入
  // 引到目标目录之外。这里用一个原地改写名字字段的线程把它打出来。
  {
    const std::string toctou_root = root + "/toctou";
    const std::string escaped_directory = toctou_root + "/escaped";
    const std::string out_dir = toctou_root + "/out";
    Check(MakeDirectoryTree(escaped_directory) && MakeDirectoryTree(out_dir),
          "RT2.10 建 TOCTOU 观察目录");
    const std::string long_archive = "remote-" + std::string(170, 'p') + ".bak";
    const std::string long_manifest = long_archive + ".manifest";
    const std::string long_identity = long_archive + ".identity";
    Check(WriteFile(root + "/repo/" + long_archive,
                    std::string(1024 * 1024, 'T')) &&
              WriteFile(root + "/repo/" + long_manifest, manifest_data) &&
              WriteFile(root + "/repo/" + long_identity, identity_data),
          "RT2.10 准备一份大 .bak 的三件套");
    const std::string toctou_bundle = root + "/toctou.bundle";
    backupproject::net::SnapshotBundleInfo toctou_info;
    Check(
        backupproject::net::BuildSnapshotBundle(
            root + "/repo", long_archive, toctou_bundle, &toctou_info, &error),
        "RT2.10 打出 TOCTOU 用材料包", error);
    std::string bundle_bytes;
    Check(ReadFile(toctou_bundle, &bundle_bytes), "RT2.10 读回材料包");
    const std::size_t name_offset = bundle_bytes.find(long_identity);
    Check(name_offset != std::string::npos, "RT2.10 定位第三件名字的字节偏移");
    const std::string relative =
        RelativeEscapePath(out_dir, escaped_directory + "/");
    const std::string evil_name =
        relative +
        std::string(long_identity.size() - relative.size() - 4, 'x') + ".bak";
    Check(evil_name.size() == long_identity.size(),
          "RT2.10 恶意名字与合法名字等长（原地改写不会移位）",
          std::to_string(evil_name.size()) + " vs " +
              std::to_string(long_identity.size()));
    std::atomic<bool> stop(false);
    std::thread rewriter(
        [&stop, &toctou_bundle, &evil_name, &long_identity, name_offset] {
          while (!stop.load()) {
            const int fd = ::open(toctou_bundle.c_str(), O_WRONLY);
            if (fd < 0) {
              continue;
            }
            const ssize_t bad = ::pwrite(fd, evil_name.data(), evil_name.size(),
                                         static_cast<off_t>(name_offset));
            const ssize_t good =
                ::pwrite(fd, long_identity.data(), long_identity.size(),
                         static_cast<off_t>(name_offset));
            (void)bad;
            (void)good;
            ::close(fd);
          }
        });
    int rounds = 0;
    int successes = 0;
    int escapes = 0;
    std::string first_escape;
    for (int round = 0; round < 40; ++round) {
      RemoveTree(escaped_directory);
      MakeDirectoryTree(escaped_directory);
      RemoveTree(out_dir);
      MakeDirectoryTree(out_dir);
      backupproject::net::SnapshotBundleInfo info;
      std::string extract_error;
      if (backupproject::net::ExtractSnapshotBundle(toctou_bundle, out_dir,
                                                    &info, &extract_error)) {
        successes += 1;
      }
      rounds += 1;
      const std::vector<std::string> escaped =
          DirectoryEntries(escaped_directory);
      if (!escaped.empty()) {
        escapes += 1;
        if (first_escape.empty()) {
          first_escape = escaped.front();
        }
      }
    }
    stop.store(true);
    rewriter.join();
    Check(rounds == 40, "RT2.10 轮次完整", std::to_string(rounds));
    Check(escapes == 0, "RT2.10 解包期间 bundle 被改写也不能写到目标目录之外",
          std::to_string(escapes) + " 次逃逸；第一个逃逸文件：" + first_escape);
    Note(
        "RT2.10 本轮修掉的缺陷 F2（TOCTOU 路径逃逸）：修复前第二遍解析"
        "（写数据那一段）重新读成员名却没校验，直接用 target_directory + "
        "name 拼路径，40 轮里 6-9 轮能把文件写到目标目录之外。现在第二遍"
        "只把读到的东西与第一遍已校验的成员表逐项比对、路径取自已校验的"
        "索引项，所以改写只会导致失败。本轮实测：" +
        std::to_string(successes) + " 轮解包成功、" + std::to_string(escapes) +
        " 轮逃逸。");
  }

  RemoveTree(root);
  EndCase("RT2", checks_before, failures_before);
}

// ============================================================================
// RT3. 缓存索引 .remote-index.tsv：不是信任根，但不能成为路径注入根
// ============================================================================

std::string StatSignature(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return "missing";
  }
  return std::to_string(static_cast<long long>(info.st_size)) + ":" +
         std::to_string(static_cast<long long>(info.st_mtime)) + ":" +
         std::to_string(static_cast<long long>(info.st_ino));
}

struct IndexProbe {
  bool restored = false;
  std::uint64_t downloads = 0;
  std::string error;
  std::string restored_text;
  std::string link_target;  // mode == 2 时，调用之后符号链接仍然指向哪里
};

// mode: 0 = 写入 index_content；1 = 删掉索引；2 = 索引是指向诱饵的符号链接；
//       3 = 索引是一个目录。
IndexProbe ProbeIndexRestore(RedTeamServer* fixture,
                             const backupproject::net::RemoteCacheLayout& cache,
                             const std::string& index_content, int mode,
                             const std::string& snapshot_id,
                             const std::string& dest,
                             const std::string& symlink_target) {
  IndexProbe probe;
  const std::string index_path = cache.cache_directory + "/.remote-index.tsv";
  RemoveTree(index_path);
  if (mode == 0) {
    probe.restored = WriteFile(index_path, index_content);
  } else if (mode == 2) {
    probe.restored = CreateSymlinkTo(symlink_target, index_path);
  } else if (mode == 3) {
    probe.restored = MakeDirectoryTree(index_path);
  } else {
    probe.restored = true;
  }
  RemoveTree(dest);
  MakeDirectoryTree(dest);
  const std::uint64_t before = DownloadBeginCount(fixture);
  backupproject::net::RemoteRestoreOutcome outcome;
  std::string error;
  const bool ok = backupproject::net::RunRemoteRestore(
      &fixture->client, cache, snapshot_id, dest,
      backupproject::RestoreOptions(), &outcome, &error);
  probe.downloads = DownloadBeginCount(fixture) - before;
  probe.restored = ok;
  probe.error = error;
  std::string text;
  if (ReadFile(dest + "/a.txt", &text)) {
    probe.restored_text = text;
  }
  if (mode == 2) {
    probe.link_target = ReadLinkTarget(index_path);
  }
  return probe;
}

void TestRemoteIndexAttacks() {
  std::printf("[chain] RT3 缓存索引 .remote-index.tsv 审查\n");
  const int checks_before = g_checks;
  const int failures_before = g_failures;

  RedTeamServer* fixture = SharedServer();
  Check(fixture != nullptr, "RT3 共享服务端可用");
  if (fixture == nullptr) {
    EndCase("RT3", checks_before, failures_before);
    return;
  }
  std::string error;
  backupproject::net::RemoteCacheLayout cache;
  Check(PrepareSharedCache('a', "index", &cache, &error), "RT3 建缓存目录",
        error);

  // 两份真实材料：A（源内容 CONTENT-A）与 B（源内容 CONTENT-B），同一个缓存。
  const std::string source_a = fixture->base + "/rt3-src-a";
  const std::string source_b = fixture->base + "/rt3-src-b";
  Check(MakeDirectoryTree(source_a) && MakeDirectoryTree(source_b) &&
            WriteFile(source_a + "/a.txt", "CONTENT-A") &&
            WriteFile(source_b + "/a.txt", "CONTENT-B"),
        "RT3 准备两份源");
  backupproject::net::RemoteBackupRequest request;
  request.client = &fixture->client;
  request.cache = cache;
  backupproject::net::RemoteBackupOutcome outcome_a;
  request.source_directory = source_a;
  Check(backupproject::net::RunRemoteBackup(request, &outcome_a, &error),
        "RT3 备份 A", error);
  backupproject::net::RemoteBackupOutcome outcome_b;
  request.source_directory = source_b;
  Check(backupproject::net::RunRemoteBackup(request, &outcome_b, &error),
        "RT3 备份 B", error);
  const std::string id_a = outcome_a.snapshot_id;
  const std::string name_a = outcome_a.archive_name;
  const std::string id_b = outcome_b.snapshot_id;
  const std::string name_b = outcome_b.archive_name;
  Check(id_a.size() == 32 && id_b.size() == 32 && id_a != id_b,
        "RT3 两份材料的服务端 id 都是 32 位且不同");
  Check(FileExists(cache.cache_directory + "/" + name_a) &&
            FileExists(cache.cache_directory + "/" + name_b),
        "RT3 两份材料都在缓存里");

  const std::string index_path = cache.cache_directory + "/.remote-index.tsv";
  std::string index_content;
  Check(ReadFile(index_path, &index_content), "RT3 索引文件存在");
  Check(index_content.find(id_a + "\t" + name_a + "\n") != std::string::npos,
        "RT3 索引里记下了 A 的映射");
  Check(index_content.size() > 0 && index_content.back() == '\n',
        "RT3 索引以换行结尾（没有半行）");

  // 诱饵：把 B 的三件套复制到缓存目录之外。
  const std::string decoy_directory = fixture->base + "/rt3-decoy";
  RemoveTree(decoy_directory);
  Check(MakeDirectoryTree(decoy_directory), "RT3 建诱饵目录");
  for (const std::string& member :
       {name_b, name_b + ".manifest", name_b + ".identity"}) {
    Check(CopyFileBytes(cache.cache_directory + "/" + member,
                        decoy_directory + "/" + member),
          "RT3 复制诱饵 " + member);
  }
  const std::string decoy_trap =
      RelativeEscapePath(cache.cache_directory, decoy_directory + "/" + name_b);
  const std::string decoy_signature =
      StatSignature(decoy_directory + "/" + name_b);
  const std::string decoy_content = [&] {
    std::string text;
    ReadFile(decoy_directory + "/" + name_b, &text);
    return text;
  }();

  // 先证明"正常情况下这条缓存是命中的"：没有恶意行时必须不下载。
  {
    const IndexProbe probe =
        ProbeIndexRestore(fixture, cache, id_a + "\t" + name_a + "\n", 0, id_a,
                          fixture->base + "/rt3-dest-ok", std::string());
    Check(probe.restored && probe.downloads == 0,
          "RT3.0 干净的索引 -> 缓存命中（不重新下载）",
          std::to_string(probe.downloads) + " 次下载 " + probe.error);
    Check(probe.restored_text == "CONTENT-A", "RT3.0 恢复出来的是 A 的内容");
  }

  struct IndexAttack {
    std::string tag;
    std::string content;
    int mode;
    std::string what;
    // 期望这次是 cache miss（重新下载）还是命中？唯一的"命中"是那条
    // "恶意行 + 合法行"的用例：恶意行必须被忽略，合法行必须继续生效。
    bool expect_download = true;
  };
  std::vector<IndexAttack> attacks;
  attacks.push_back({"1-traversal-valid-trio", id_a + "\t" + decoy_trap + "\n",
                     0, "相对路径穿越到缓存外的合法三件套"});
  attacks.push_back({"2-absolute-path",
                     id_a + "\t" + decoy_directory + "/" + name_b + "\n", 0,
                     "绝对路径"});
  attacks.push_back({"3-no-bak-suffix", id_a + "\t../../etc/passwd\n", 0,
                     "以 ../../etc/passwd 为名字（无 .bak）"});
  attacks.push_back(
      {"4-trailing-tab", id_a + "\t" + name_a + ".bak\t\n", 0, "名字里带 TAB"});
  attacks.push_back(
      {"5-line-injection",
       id_a + "\t" + decoy_trap + "\n" + id_a + "\t" + name_a + "\n", 0,
       "恶意行 + 合法行（恶意行不许覆盖合法行）",
       /*expect_download=*/false});
  attacks.push_back(
      {"6-empty-id", std::string("\t") + name_a + "\n", 0, "空 id"});
  attacks.push_back({"7-empty-name", id_a + "\t\n", 0, "空名字"});
  attacks.push_back({"8-overlong-name",
                     id_a + "\t" + std::string(4000, 'x') + ".bak\n", 0,
                     "超长名字（4000+ 字节）"});
  attacks.push_back(
      {"9-non-hex-id", "zzzz\t" + name_a + "\n", 0, "id 不是 32 位十六进制"});
  attacks.push_back({"10-uppercase-id",
                     [&] {
                       std::string upper = id_a;
                       for (char& character : upper) {
                         if (character >= 'a' && character <= 'f') {
                           character = static_cast<char>(character - 'a' + 'A');
                         }
                       }
                       return upper;
                     }() +
                         "\t" + name_a + "\n",
                     0, "id 是大写十六进制"});
  attacks.push_back(
      {"11-half-line", id_a + "\n", 0, "半截行（没有 TAB 分隔）"});
  attacks.push_back({"12-truncated-name",
                     id_a + "\t" + name_a.substr(0, 12) + "\n", 0,
                     "名字被截断"});
  attacks.push_back(
      {"13-non-utf8",
       std::string("\xFF\xFE\xFF\xFE\t") + std::string("\xC3\x28") + "\n", 0,
       "非 UTF-8 字节"});
  attacks.push_back({"14-empty-file", std::string(), 0, "空索引文件"});
  attacks.push_back({"15-missing-file", std::string(), 1, "索引文件不存在"});
  attacks.push_back(
      {"16-symlinked-index", std::string(), 2, "索引本身是符号链接"});
  attacks.push_back(
      {"17-index-is-directory", std::string(), 3, "索引是一个目录"});

  for (const IndexAttack& attack : attacks) {
    const IndexProbe probe = ProbeIndexRestore(
        fixture, cache, attack.content, attack.mode, id_a,
        fixture->base + "/rt3-dest-" + attack.tag,
        attack.mode == 2 ? (decoy_directory + "/" + name_b) : std::string());
    Check(probe.restored,
          "RT3." + attack.tag + " " + attack.what +
              " -> 仍然退化成 cache miss 并恢复成功",
          probe.error);
    if (attack.expect_download) {
      Check(probe.downloads >= 1,
            "RT3." + attack.tag + " 索引行被忽略（重新下载而不是用诱饵）",
            std::to_string(probe.downloads) + " 次下载");
    } else {
      Check(probe.downloads == 0,
            "RT3." + attack.tag + " 恶意行被忽略、同一 id 的合法行仍然生效",
            std::to_string(probe.downloads) + " 次下载");
    }
    if (attack.mode == 2) {
      // 读取路径用 O_NOFOLLOW：符号链接不会被跟随（诱饵没被用过、也没被改）。
      // 之后的 SaveRemoteIndex 会用 rename 原子替换这个路径，这是允许的
      // ——rename 替换的是链接本身，不是它指向的文件。
      Check(!IsSymlink(index_path) ||
                probe.link_target == decoy_directory + "/" + name_b,
            "RT3." + attack.tag +
                " 不跟随符号链接（链接要么原样保留，要么被产品自己原子替换）",
            probe.link_target +
                " / exists=" + (FileExists(index_path) ? "1" : "0"));
    }
    Check(probe.restored_text == "CONTENT-A",
          "RT3." + attack.tag + " 恢复出来的是被请求的那份材料（不是 B）",
          probe.restored_text);
    Check(StatSignature(decoy_directory + "/" + name_b) == decoy_signature,
          "RT3." + attack.tag +
              " 缓存外的诱饵没有被读取/改动（inode/长度/mtime 不变）");
    std::string decoy_now;
    Check(ReadFile(decoy_directory + "/" + name_b, &decoy_now) &&
              decoy_now == decoy_content,
          "RT3." + attack.tag + " 诱饵内容逐字节不变");
  }

  // 全部都是"合法形状但互相矛盾"的重复行：map 语义是最后一行胜出。
  // 这一条不构成路径逃逸，但它说明"索引写错"的后果：根快照没有父子边可供
  // 交叉校验，所以一条写错的映射会让 restore 静默换一份材料。
  {
    const IndexProbe probe = ProbeIndexRestore(
        fixture, cache,
        id_a + "\t" + name_a + "\n" + id_a + "\t" + name_b + "\n", 0, id_a,
        fixture->base + "/rt3-dest-duplicate", std::string());
    Check(probe.restored, "RT3.18 同一 id 两行矛盾映射 -> 不崩溃且恢复成功",
          probe.error);
    Check(probe.downloads == 0,
          "RT3.18 两行都是合法形状 -> 命中缓存（最后一行胜出）");
    Note("RT3.18 观测：重复行最后一行胜出，restore(" + id_a +
         ") 拿到的文本是 '" + probe.restored_text +
         "'（A 的内容是 CONTENT-A）。索引不是信任根，但根快照没有父子边可以"
         "交叉校验，这一条映射错误不会被 EnsureChainMaterial 发现——建议在"
         "缓存命中路径上再用服务端声明的 (size, sha256) 对一次材料身份。");
  }

  // ---- 巨大索引：不接受成为内存放大器 / 崩溃点 ----
  {
    const std::string huge =
        std::string(8 * 1024 * 1024, 'z');  // 8 MiB 垃圾，没有任何 TAB
    const IndexProbe probe =
        ProbeIndexRestore(fixture, cache, huge, 0, id_a,
                          fixture->base + "/rt3-dest-huge", std::string());
    Check(probe.restored && probe.downloads >= 1,
          "RT3.19 8 MiB 垃圾索引 -> 退化成 cache miss、不崩溃", probe.error);
    std::string status;
    if (ReadFile("/proc/self/status", &status)) {
      const std::size_t at = status.find("VmHWM:");
      if (at != std::string::npos) {
        const std::size_t end = status.find('\n', at);
        Note("RT3.19 读 8 MiB 索引之后进程峰值 RSS：" +
             status.substr(at, end - at));
      }
    }
    Note(
        "RT3.19 本轮收口的观察项：修复前 LoadRemoteIndex 把整个索引文件读进"
        "内存（没有长度上限，8 MiB 索引能顶到约 40 MB RSS）。现在超过 "
        "kMaxRemoteIndexBytes（4 MiB）就整体当 cache miss 重新下载，本用例"
        "断言的「退化 + 重新下载」路径不变。索引在缓存目录里，属于本地可写面，"
        "所以这条始终只是内存放大，不是越权读。");
  }

  // ---- 索引更新：temp + 原子替换 ----
  {
    RemoveTree(index_path);
    std::atomic<bool> stop(false);
    std::atomic<int> reads(0);
    std::atomic<int> malformed(0);
    std::thread reader([&] {
      while (!stop.load()) {
        std::string content;
        if (!ReadFile(index_path, &content)) {
          continue;
        }
        reads.fetch_add(1);
        if (content.empty() || content.back() != '\n') {
          malformed.fetch_add(1);
          continue;
        }
        std::size_t start = 0;
        while (start < content.size()) {
          std::size_t end = content.find('\n', start);
          if (end == std::string::npos) {
            malformed.fetch_add(1);
            break;
          }
          const std::string line = content.substr(start, end - start);
          start = end + 1;
          const std::size_t tab = line.find('\t');
          if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
            malformed.fetch_add(1);
          }
        }
      }
    });
    const std::string source_c = fixture->base + "/rt3-src-c";
    MakeDirectoryTree(source_c);
    backupproject::net::RemoteBackupRequest request_c;
    request_c.client = &fixture->client;
    request_c.cache = cache;
    request_c.source_directory = source_c;
    int backups = 0;
    for (int round = 0; round < 12; ++round) {
      WriteFile(source_c + "/file-" + std::to_string(round) + ".txt",
                std::string(32 + round, 'c'));
      backupproject::net::RemoteBackupOutcome outcome;
      std::string backup_error;
      if (backupproject::net::RunRemoteBackup(request_c, &outcome,
                                              &backup_error)) {
        backups += 1;
      }
    }
    stop.store(true);
    reader.join();
    Check(backups >= 10, "RT3.20 连续 12 次备份（每次都更新索引）",
          std::to_string(backups));
    Check(reads.load() > 0, "RT3.20 并发读者确实读到了索引",
          std::to_string(reads.load()) + " 次");
    Check(malformed.load() == 0,
          "RT3.20 读者从来没有看到半个索引（temp + rename 原子替换）",
          std::to_string(malformed.load()) + " 次半截内容");
    std::string final_content;
    Check(ReadFile(index_path, &final_content) && !final_content.empty() &&
              final_content.back() == '\n',
          "RT3.20 最终索引完整（以换行结尾）");
    bool leftover = false;
    for (const std::string& name : DirectoryEntries(cache.cache_directory)) {
      if (name.find(".remote-index.tsv.part-") != std::string::npos) {
        leftover = true;
      }
    }
    Check(!leftover, "RT3.20 更新之后没有留下索引临时文件");
    Note("RT3.20 观测：12 次备份期间读者读了 " + std::to_string(reads.load()) +
         " 次，全部是完整内容；SaveRemoteIndex 走的是"
         " <索引>.part-<pid> + fsync + "
         "rename（src/network/remote_incremental.cpp）。");
  }

  EndCase("RT3", checks_before, failures_before);
}

// ============================================================================
// RT4. 缓存隔离：指纹 / 用户名 / pin 不同 -> 不复用材料
// ============================================================================

void TestCacheIsolation() {
  std::printf("[chain] RT4 缓存隔离\n");
  const int checks_before = g_checks;
  const int failures_before = g_failures;

  const std::string root = WorkRoot() + "/rt4-isolation";
  RemoveTree(root);
  const std::string fingerprint_a = Hex64('a');
  const std::string fingerprint_b = Hex64('b');
  backupproject::net::RemoteCacheLayout same_a;
  backupproject::net::RemoteCacheLayout different_user;
  backupproject::net::RemoteCacheLayout different_fingerprint;
  backupproject::net::RemoteCacheLayout repeat;
  std::string error;
  Check(backupproject::net::PrepareRemoteCache(root, fingerprint_a, "user-1",
                                               &same_a, &error),
        "RT4 建缓存 A", error);
  Check(backupproject::net::PrepareRemoteCache(root, fingerprint_a, "user-2",
                                               &different_user, &error),
        "RT4 建缓存 B（同指纹不同用户名）", error);
  Check(backupproject::net::PrepareRemoteCache(root, fingerprint_b, "user-1",
                                               &different_fingerprint, &error),
        "RT4 建缓存 C（同用户名不同指纹）", error);
  Check(backupproject::net::PrepareRemoteCache(root, fingerprint_a, "user-1",
                                               &repeat, &error),
        "RT4 再次建缓存 A", error);
  Check(same_a.cache_directory != different_user.cache_directory,
        "RT4 同指纹不同用户名 -> 目录不同");
  Check(same_a.cache_directory != different_fingerprint.cache_directory,
        "RT4 同用户名不同指纹 -> 目录不同");
  Check(same_a.cache_directory == repeat.cache_directory,
        "RT4 同 root/指纹/用户名 -> 目录相同（幂等）");
  Check(same_a.cache_directory.find(fingerprint_a.substr(0, 16)) !=
            std::string::npos,
        "RT4 目录里带指纹前 16 位");
  Check(same_a.cache_directory.find("user-1") != std::string::npos,
        "RT4 目录里带用户名");
  Check(same_a.repository_identity != different_user.repository_identity &&
            same_a.repository_identity !=
                different_fingerprint.repository_identity,
        "RT4 逻辑身份（repository_identity）三者互不相同");
  Check(!backupproject::net::PrepareRemoteCache(root, std::string(), "user-1",
                                                &repeat, &error),
        "RT4 指纹为空被拒绝");
  Check(!backupproject::net::PrepareRemoteCache(
            root, fingerprint_a.substr(0, 63), "user-1", &repeat, &error),
        "RT4 指纹长度不对被拒绝");
  Check(!backupproject::net::PrepareRemoteCache(root, fingerprint_a,
                                                std::string(), &repeat, &error),
        "RT4 用户名为空被拒绝");

  // 行为断言：A 的材料在 B/C 的命名空间里查不到（会重新下载）。
  RedTeamServer* fixture = SharedServer();
  Check(fixture != nullptr, "RT4 共享服务端可用");
  if (fixture == nullptr) {
    EndCase("RT4", checks_before, failures_before);
    return;
  }
  backupproject::net::RemoteCacheLayout cache_a;
  Check(PrepareSharedCache('a', "isolation", &cache_a, &error),
        "RT4 建用例缓存", error);
  const std::string source = fixture->base + "/rt4-src";
  MakeDirectoryTree(source);
  WriteFile(source + "/a.txt", "ISOLATION");
  backupproject::net::RemoteBackupRequest request;
  request.client = &fixture->client;
  request.cache = cache_a;
  request.source_directory = source;
  backupproject::net::RemoteBackupOutcome outcome;
  Check(backupproject::net::RunRemoteBackup(request, &outcome, &error),
        "RT4 备份到用例缓存", error);

  // 同一 root、不同用户名 -> 另一个目录，材料不可见
  backupproject::net::RemoteCacheLayout user_cache;
  Check(backupproject::net::PrepareRemoteCache(
            fixture->base + "/cache-isolation-other-user", fingerprint_a,
            fixture->username + "-other", &user_cache, &error),
        "RT4 建另一个用户名的缓存", error);
  Check(!FileExists(user_cache.cache_directory + "/" + outcome.archive_name),
        "RT4 A 的归档名在另一个用户名的命名空间里不存在");

  backupproject::net::RemoteCacheLayout pin_cache;
  Check(backupproject::net::PrepareRemoteCache(
            fixture->base + "/cache-isolation-other-pin", fingerprint_b,
            fixture->username, &pin_cache, &error),
        "RT4 建另一个 pin 的缓存", error);
  Check(!FileExists(pin_cache.cache_directory + "/" + outcome.archive_name),
        "RT4 A 的归档名在另一个指纹的命名空间里不存在");

  const std::uint64_t before = DownloadBeginCount(fixture);
  backupproject::net::RemoteRestoreOutcome restore_same;
  Check(backupproject::net::RunRemoteRestore(
            &fixture->client, cache_a, outcome.snapshot_id,
            fixture->base + "/rt4-dest-same", backupproject::RestoreOptions(),
            &restore_same, &error),
        "RT4 用自己的缓存恢复成功", error);
  Check(DownloadBeginCount(fixture) == before,
        "RT4 自己的缓存命中：没有重新下载");

  backupproject::net::RemoteRestoreOutcome restore_pin;
  const std::uint64_t before_pin = DownloadBeginCount(fixture);
  Check(backupproject::net::RunRemoteRestore(
            &fixture->client, pin_cache, outcome.snapshot_id,
            fixture->base + "/rt4-dest-pin", backupproject::RestoreOptions(),
            &restore_pin, &error),
        "RT4 用另一个指纹的缓存恢复成功（重新下载）", error);
  Check(DownloadBeginCount(fixture) > before_pin,
        "RT4 不同指纹不复用材料：确实重新下载了");

  backupproject::net::RemoteRestoreOutcome restore_user;
  const std::uint64_t before_user = DownloadBeginCount(fixture);
  Check(backupproject::net::RunRemoteRestore(
            &fixture->client, user_cache, outcome.snapshot_id,
            fixture->base + "/rt4-dest-user", backupproject::RestoreOptions(),
            &restore_user, &error),
        "RT4 用另一个用户名的缓存恢复成功（重新下载）", error);
  Check(DownloadBeginCount(fixture) > before_user,
        "RT4 不同用户名不复用材料：确实重新下载了");

  EndCase("RT4", checks_before, failures_before);
}

// ============================================================================
// RT5. 元数据 vs 归档的双向交叉校验
// ============================================================================

// 把一份 delta 的**信封**改掉，并重新组装成 BKPINC1（文件名不变，所以
// manifest 副文件仍然匹配）。recompute_identity 为真时同时把 .identity 里的
// snapshot_id 改成新的自摘要——于是这份材料在引擎眼里是"自洽、可信"的，
// 只有 EnsureChainMaterial 的交叉校验能发现它声明的父与元数据链不一致。
bool RewriteDeltaEnvelope(const std::string& repository_directory,
                          const std::string& delta_name,
                          const std::string& new_parent_file_name,
                          bool rewrite_identity, bool recompute_digest,
                          std::string* error_message) {
  const std::string delta_path = repository_directory + "/" + delta_name;
  backupproject::DeltaEnvelope envelope;
  if (!backupproject::ReadDeltaEnvelope(delta_path, &envelope, error_message)) {
    return false;
  }
  envelope.parent_file_name = new_parent_file_name;
  if (recompute_digest) {
    envelope.snapshot_id = backupproject::ComputeDeltaSnapshotId(envelope);
  } else {
    // 天真篡改：只改内容，不改自摘要。解析层必须因此拒绝。
    envelope.snapshot_id.clear();
  }
  const std::string text = backupproject::SerializeDeltaEnvelope(envelope);
  std::string payload;
  const std::string payload_path = repository_directory + "/.rt-payload.bin";
  if (!backupproject::ExtractDeltaPayload(delta_path, payload_path,
                                          error_message) ||
      !ReadFile(payload_path, &payload)) {
    return false;
  }
  (void)::unlink(payload_path.c_str());
  if (!recompute_digest) {
    // 天真篡改的字节级做法：直接改原文件里 parent_file_name 的值，
    // 长度不变，其余字节一个都不动。
    std::string original;
    if (!ReadFile(delta_path, &original)) {
      return false;
    }
    const std::size_t at = original.find("parent_file_name=");
    if (at == std::string::npos) {
      return false;
    }
    const std::size_t value_at = at + std::strlen("parent_file_name=");
    if (value_at >= original.size()) {
      return false;
    }
    original[value_at] = original[value_at] == 'x' ? 'y' : 'x';
    return WriteFile(delta_path, original);
  }
  std::string bytes;
  bytes.append(reinterpret_cast<const char*>(backupproject::kDeltaMagic),
               backupproject::kDeltaMagicSize);
  const std::uint16_t version = backupproject::kDeltaFormatVersion;
  bytes.push_back(static_cast<char>(version & 0xFF));
  bytes.push_back(static_cast<char>((version >> 8) & 0xFF));
  const std::uint16_t header_size =
      static_cast<std::uint16_t>(backupproject::kDeltaFixedHeaderSize);
  bytes.push_back(static_cast<char>(header_size & 0xFF));
  bytes.push_back(static_cast<char>((header_size >> 8) & 0xFF));
  const std::uint32_t envelope_len = static_cast<std::uint32_t>(text.size());
  for (int index = 0; index < 4; ++index) {
    bytes.push_back(static_cast<char>((envelope_len >> (8 * index)) & 0xFF));
  }
  const std::uint64_t payload_len = payload.size();
  for (int index = 0; index < 8; ++index) {
    bytes.push_back(static_cast<char>((payload_len >> (8 * index)) & 0xFF));
  }
  bytes += text;
  bytes += payload;
  if (!WriteFile(delta_path, bytes)) {
    return false;
  }
  if (!rewrite_identity) {
    return true;
  }
  std::string identity_text;
  if (!ReadFile(delta_path + ".identity", &identity_text)) {
    return false;
  }
  const std::size_t position = identity_text.find("snapshot_id=");
  if (position == std::string::npos) {
    return false;
  }
  const std::size_t line_end = identity_text.find('\n', position);
  identity_text = identity_text.substr(0, position) +
                  "snapshot_id=" + envelope.snapshot_id +
                  identity_text.substr(line_end);
  return WriteFile(delta_path + ".identity", identity_text);
}

void TestMetadataArchiveCrossCheck() {
  std::printf("[chain] RT5 元数据 <-> 归档 双向交叉校验\n");
  const int checks_before = g_checks;
  const int failures_before = g_failures;

  RedTeamServer* fixture = SharedServer();
  Check(fixture != nullptr, "RT5 共享服务端可用");
  if (fixture == nullptr) {
    EndCase("RT5", checks_before, failures_before);
    return;
  }
  const std::string root = fixture->base;
  std::string error;

  // ---- 方向二（纯元数据）：LIST 的父子边错误 -> ResolveRemoteChain 自洽性检查
  // ----
  {
    const std::string lineage = Hex64('a');
    std::vector<RemoteSnapshotInfo> snapshots;
    snapshots.push_back(MakeSnapshot("11111111111111111111111111111111", "", 0,
                                     lineage, 100, 0));
    snapshots.push_back(MakeSnapshot("22222222222222222222222222222222",
                                     "11111111111111111111111111111111", 1,
                                     lineage, 200, 1));
    snapshots.push_back(MakeSnapshot("33333333333333333333333333333333",
                                     "22222222222222222222222222222222", 2,
                                     lineage, 300, 1));
    snapshots.push_back(MakeSnapshot("44444444444444444444444444444444",
                                     "22222222222222222222222222222222", 2,
                                     lineage, 400, 1));
    // 错误边：同样是 generation 2，父却被指向链根（gen 0）。
    snapshots.push_back(MakeSnapshot("55555555555555555555555555555555",
                                     "11111111111111111111111111111111", 2,
                                     lineage, 500, 1));
    std::vector<RemoteSnapshotInfo> chain;
    std::string chain_error;
    Check(backupproject::net::ResolveRemoteChain(
              snapshots, "33333333333333333333333333333333", &chain,
              &chain_error) &&
              chain.size() == 3,
          "RT5.1 控制组：正确的父子边解析出三跳链", chain_error);
    Check(!backupproject::net::ResolveRemoteChain(
              snapshots, "55555555555555555555555555555555", &chain,
              &chain_error),
          "RT5.1 同 generation 但父指向别的 id -> 自洽性检查失败", chain_error);
    Check(chain_error.find("代数") != std::string::npos ||
              chain_error.find("父子") != std::string::npos,
          "RT5.1 失败理由是父子关系/代数不连续", chain_error);

    std::vector<RemoteSnapshotInfo> bad_root;
    bad_root.push_back(MakeSnapshot("66666666666666666666666666666666", "", 3,
                                    lineage, 600, 0));
    Check(
        !backupproject::net::ResolveRemoteChain(
            bad_root, "66666666666666666666666666666666", &chain, &chain_error),
        "RT5.2 根不是 generation 0 的完整快照 -> 失败", chain_error);

    std::vector<RemoteSnapshotInfo> loop;
    loop.push_back(MakeSnapshot("77777777777777777777777777777777",
                                "88888888888888888888888888888888", 1, lineage,
                                700, 1));
    loop.push_back(MakeSnapshot("88888888888888888888888888888888",
                                "77777777777777777777777777777777", 2, lineage,
                                800, 1));
    Check(!backupproject::net::ResolveRemoteChain(
              loop, "77777777777777777777777777777777", &chain, &chain_error),
          "RT5.2 元数据成环 -> 失败（深度上限兜住）", chain_error);
  }

  // ---- 方向一（端到端）：元数据正确，delta 信封的父被改 ----
  const std::string lineage_a = Hex64('c');
  const std::string lineage_b = Hex64('d');
  LocalChain chain_a;
  LocalChain chain_b;
  Check(BuildLocalChain(root, "rt5a", &chain_a, &error), "RT5 造本地链 A",
        error);
  Check(BuildLocalChain(root, "rt5b", &chain_b, &error), "RT5 造本地链 B",
        error);

  backupproject::net::RemoteSnapshotInfo full_a;
  backupproject::net::RemoteSnapshotInfo full_b;
  Check(
      UploadTrio(fixture, chain_a.repository_directory, chain_a.full_name,
                 "chain-a-root", 0, std::string(), lineage_a, &full_a, &error),
      "RT5 上传链 A 的根", error);
  Check(
      UploadTrio(fixture, chain_b.repository_directory, chain_b.full_name,
                 "chain-b-root", 0, std::string(), lineage_b, &full_b, &error),
      "RT5 上传链 B 的根", error);
  Check(full_a.generation == 0 && full_a.parent_snapshot_id.empty() &&
            full_b.generation == 0,
        "RT5 两份根都被登记成 generation 0 的完整快照");

  // (1)
  // 自洽篡改：信封里的父文件名指向另一份"幽灵"归档，自摘要与身份副文件同步。
  const std::string ghost_parent = ManagedArchiveName("rt5-ghost-g0");
  Check(RewriteDeltaEnvelope(chain_a.repository_directory, chain_a.delta_name,
                             ghost_parent, /*rewrite_identity=*/true,
                             /*recompute_digest=*/true, &error),
        "RT5 把链 A 的 delta 信封改成自洽的篡改版", error);
  {
    backupproject::SnapshotIdentity identity;
    std::string identity_error;
    const bool loaded = backupproject::LoadVerifiedSnapshotIdentity(
        chain_a.repository_directory, chain_a.delta_name, &identity, nullptr,
        &identity_error);
    Check(loaded && identity.sidecars_verified,
          "RT5 引擎层接受了这份自洽篡改（sidecars_verified = true）",
          loaded ? identity.sidecar_diagnostic : identity_error);
    Check(identity.parent_file_name == ghost_parent &&
              identity.parent_file_name != chain_a.full_name,
          "RT5 交叉校验条件为真：信封父名 != 元数据链上的父名");
  }
  backupproject::net::RemoteSnapshotInfo tampered_child;
  Check(UploadTrio(fixture, chain_a.repository_directory, chain_a.delta_name,
                   "chain-a-delta-tampered", 1, full_a.snapshot_id, lineage_a,
                   &tampered_child, &error),
        "RT5 上传自洽篡改的 delta（服务端只存字节，照收）", error);
  Check(tampered_child.parent_snapshot_id == full_a.snapshot_id &&
            tampered_child.generation == 1,
        "RT5 服务端登记的父子边是**正确**的（被改的只有归档里的信封）");
  {
    backupproject::net::RemoteCacheLayout cache;
    Check(PrepareSharedCache('e', "crosscheck-a", &cache, &error),
          "RT5 建方向一的缓存", error);
    const std::string dest = root + "/rt5-dest-a";
    MakeDirectoryTree(dest);
    backupproject::net::RemoteRestoreOutcome outcome;
    std::string restore_error;
    const bool restored = backupproject::net::RunRemoteRestore(
        &fixture->client, cache, tampered_child.snapshot_id, dest,
        backupproject::RestoreOptions(), &outcome, &restore_error);
    Check(!restored, "RT5 元数据与归档不一致 -> 恢复必须失败", restore_error);
    Check(restore_error.find(ghost_parent) != std::string::npos,
          "RT5 拒绝理由点名了信封里那个假的父文件名（确实是交叉校验抛的）",
          restore_error);
    Check(!FileExists(dest + "/a.txt"), "RT5 被拒绝时目标目录没有任何产出");
    Note(
        "RT5 方向一（自洽篡改）实际拒绝层 = EnsureChainMaterial 的交叉校验；"
        "原文：" +
        restore_error);
  }

  // (2) 天真篡改：只改信封一个字节，不重算自摘要 -> 引擎层先拒绝。
  Check(RewriteDeltaEnvelope(chain_b.repository_directory, chain_b.delta_name,
                             ghost_parent, /*rewrite_identity=*/false,
                             /*recompute_digest=*/false, &error),
        "RT5 把链 B 的 delta 信封改成不自洽的篡改版", error);
  {
    backupproject::SnapshotIdentity identity;
    std::string identity_error;
    const bool loaded = backupproject::LoadVerifiedSnapshotIdentity(
        chain_b.repository_directory, chain_b.delta_name, &identity, nullptr,
        &identity_error);
    Check(!loaded || !identity.sidecars_verified,
          "RT5 不自洽的篡改在引擎层就被拒绝",
          loaded ? identity.sidecar_diagnostic : identity_error);
    Note(std::string(
             "RT5 方向一（天真篡改）实际拒绝层 = 引擎的材料验证；原因：") +
         (loaded ? identity.sidecar_diagnostic : identity_error));
  }
  backupproject::net::RemoteSnapshotInfo naive_child;
  Check(UploadTrio(fixture, chain_b.repository_directory, chain_b.delta_name,
                   "chain-b-delta-naive", 1, full_b.snapshot_id, lineage_b,
                   &naive_child, &error),
        "RT5 上传不自洽的篡改 delta", error);
  {
    backupproject::net::RemoteCacheLayout cache;
    Check(PrepareSharedCache('f', "crosscheck-b", &cache, &error),
          "RT5 建方向一的第二个缓存", error);
    const std::string dest = root + "/rt5-dest-b";
    MakeDirectoryTree(dest);
    backupproject::net::RemoteRestoreOutcome outcome;
    std::string restore_error;
    const bool restored = backupproject::net::RunRemoteRestore(
        &fixture->client, cache, naive_child.snapshot_id, dest,
        backupproject::RestoreOptions(), &outcome, &restore_error);
    Check(!restored, "RT5 不自洽的材料 -> 恢复必须失败", restore_error);
    Check(!FileExists(cache.cache_directory + "/" + chain_b.delta_name),
          "RT5 验证不通过的材料被从缓存里撤掉（没有留下半套）");
    Note(
        "RT5 方向一（天真篡改）端到端拒绝层 = FetchSnapshotMaterial 的"
        "材料自身验证；原文：" +
        restore_error);
  }

  // (3) 方向二的端到端版本：元数据正确，但缓存索引把"父 id"指向另一份
  //     完全合法、但属于别的快照的材料 -> 交叉校验必须发现。
  {
    LocalChain chain_c;
    Check(BuildLocalChain(root, "rt5c", &chain_c, &error), "RT5 造本地链 C",
          error);
    backupproject::net::RemoteSnapshotInfo full_c;
    backupproject::net::RemoteSnapshotInfo delta_c;
    Check(UploadTrio(fixture, chain_c.repository_directory, chain_c.full_name,
                     "chain-c-root", 0, std::string(), lineage_a, &full_c,
                     &error),
          "RT5 上传链 C 的根", error);
    Check(UploadTrio(fixture, chain_c.repository_directory, chain_c.delta_name,
                     "chain-c-delta", 1, full_c.snapshot_id, lineage_a,
                     &delta_c, &error),
          "RT5 上传链 C 的 delta", error);
    backupproject::net::RemoteCacheLayout cache;
    Check(PrepareSharedCache('a', "crosscheck-c", &cache, &error),
          "RT5 建方向二的缓存", error);
    // 缓存里放两套材料：链 C 自己的（正确），以及链 B 的根（"别的快照"）。
    for (const std::string& member :
         {chain_c.full_name, chain_c.full_name + ".manifest",
          chain_c.full_name + ".identity", chain_c.delta_name,
          chain_c.delta_name + ".manifest", chain_c.delta_name + ".identity"}) {
      Check(CopyFileBytes(chain_c.repository_directory + "/" + member,
                          cache.cache_directory + "/" + member),
            "RT5 把链 C 的材料放进缓存 " + member);
    }
    for (const std::string& member :
         {chain_b.full_name, chain_b.full_name + ".manifest",
          chain_b.full_name + ".identity"}) {
      Check(CopyFileBytes(chain_b.repository_directory + "/" + member,
                          cache.cache_directory + "/" + member),
            "RT5 把链 B 的根也放进缓存 " + member);
    }
    // 索引把"链 C 根的服务端 id"映射到链 B 的根：两份材料都验证得过去，
    // 但 delta 信封声明的父是链 C 的根。
    Check(WriteFile(cache.cache_directory + "/.remote-index.tsv",
                    full_c.snapshot_id + "\t" + chain_b.full_name + "\n" +
                        delta_c.snapshot_id + "\t" + chain_c.delta_name + "\n"),
          "RT5 写一份父 id 指向别的快照的索引");
    const std::string dest = root + "/rt5-dest-c";
    MakeDirectoryTree(dest);
    backupproject::net::RemoteRestoreOutcome outcome;
    std::string restore_error;
    const bool restored = backupproject::net::RunRemoteRestore(
        &fixture->client, cache, delta_c.snapshot_id, dest,
        backupproject::RestoreOptions(), &outcome, &restore_error);
    Check(!restored, "RT5 缓存里的父材料被换成别的快照 -> 恢复必须失败",
          restore_error);
    Check(restore_error.find(chain_b.full_name) != std::string::npos,
          "RT5 拒绝理由点名了被换上去的那份材料名", restore_error);
    Check(!FileExists(dest + "/a.txt"), "RT5 被拒绝时目标目录没有产出");
    Note(
        "RT5 方向二端到端：元数据正确、缓存映射错误时，"
        "EnsureChainMaterial 的交叉校验挡下了它；原文：" +
        restore_error);
  }

  EndCase("RT5", checks_before, failures_before);
}

// ============================================================================
// RT6-RT8. SQLite 迁移：半途失败 / 大数据量 / 并发
// ============================================================================

std::vector<std::string> QueryRows(const std::string& path,
                                   const std::string& sql) {
  std::vector<std::string> rows;
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return rows;
  }
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) ==
      SQLITE_OK) {
    const int columns = sqlite3_column_count(statement);
    while (sqlite3_step(statement) == SQLITE_ROW) {
      std::string row;
      for (int index = 0; index < columns; ++index) {
        if (index != 0) {
          row += '\x1f';
        }
        if (sqlite3_column_type(statement, index) == SQLITE_NULL) {
          row += "<null>";
          continue;
        }
        const unsigned char* text = sqlite3_column_text(statement, index);
        const int bytes = sqlite3_column_bytes(statement, index);
        if (text != nullptr && bytes > 0) {
          row.append(reinterpret_cast<const char*>(text),
                     static_cast<std::size_t>(bytes));
        } else {
          row += "<empty>";
        }
      }
      rows.push_back(row);
    }
  }
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return rows;
}

std::vector<std::string> SnapshotColumnNames(const std::string& path) {
  std::vector<std::string> names;
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return names;
  }
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database, "PRAGMA table_info(snapshots);", -1,
                         &statement, nullptr) == SQLITE_OK) {
    while (sqlite3_step(statement) == SQLITE_ROW) {
      const unsigned char* text = sqlite3_column_text(statement, 1);
      if (text != nullptr) {
        names.push_back(reinterpret_cast<const char*>(text));
      }
    }
  }
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return names;
}

// 整个库的"可观察状态"：版本号 + 每张表的列名 + 每张表的全部行（按 rowid）。
// 迁移的回滚必须让这个字符串逐字符不变。
std::string DumpDatabaseState(const std::string& path) {
  std::string dump =
      "user_version=" + std::to_string(ReadUserVersion(path)) + "\n";
  for (const char* table : {"users", "snapshots", "deleted_users"}) {
    dump += std::string("table ") + table + " columns:";
    for (const std::string& column :
         QueryRows(path, std::string("PRAGMA table_info(") + table + ");")) {
      dump += " " + column;
    }
    dump += "\n";
    for (const std::string& row :
         QueryRows(path, std::string("SELECT * FROM ") + table +
                             " ORDER BY rowid;")) {
      dump += "  " + row + "\n";
    }
  }
  return dump;
}

// PR #20 的七个旧列（顺序固定）。迁移"只加列、不动旧值"就靠它比对。
const char* const kLegacySnapshotColumns =
    "id, user_id, display_name, size_bytes, sha256, created_at, storage_name";

std::vector<std::string> LegacySnapshotRows(const std::string& path) {
  return QueryRows(path, std::string("SELECT ") + kLegacySnapshotColumns +
                             " FROM snapshots ORDER BY id;");
}

std::vector<std::string> LegacyUserRows(const std::string& path) {
  return QueryRows(
      path,
      "SELECT id, username, password_salt, password_hash, password_iterations,"
      " created_at FROM users ORDER BY id;");
}

std::string SqlQuote(const std::string& text) {
  std::string out = "'";
  for (const char character : text) {
    if (character == '\'') {
      out += "''";
    } else {
      out += character;
    }
  }
  out += "'";
  return out;
}

std::string HexFromState(std::uint64_t* state, int bytes) {
  std::string out;
  const char* digits = "0123456789abcdef";
  for (int index = 0; index < bytes; ++index) {
    const std::uint64_t value = NextRandom(state);
    out.push_back(digits[value & 0xF]);
    out.push_back(digits[(value >> 4) & 0xF]);
  }
  return out;
}

// 造一个 v1 库：users 个用户、snapshots 条 legacy 快照，全部来自固定种子。
bool CreateLegacyDatabaseLarge(const std::string& path, int user_count,
                               int snapshot_count, std::uint64_t seed) {
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(path.c_str(), &database,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return false;
  }
  const char* schema[] = {
      "PRAGMA journal_mode=WAL;",
      "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT UNIQUE NOT "
      "NULL,"
      " password_salt BLOB NOT NULL, password_hash BLOB NOT NULL,"
      " password_iterations INTEGER NOT NULL, created_at INTEGER NOT NULL);",
      "CREATE TABLE snapshots (id TEXT PRIMARY KEY, user_id INTEGER NOT NULL,"
      " display_name TEXT NOT NULL, size_bytes INTEGER NOT NULL,"
      " sha256 TEXT NOT NULL, created_at INTEGER NOT NULL,"
      " storage_name TEXT NOT NULL, FOREIGN KEY(user_id) REFERENCES "
      "users(id));",
      "CREATE TABLE deleted_users (id INTEGER PRIMARY KEY, deleted_at INTEGER "
      "NOT NULL);",
      "CREATE INDEX snapshots_by_user ON snapshots(user_id, created_at, id);",
  };
  bool ok = true;
  for (const char* statement : schema) {
    if (!ExecSql(database, statement)) {
      ok = false;
    }
  }
  std::uint64_t state = seed;
  for (int index = 1; ok && index <= user_count; ++index) {
    const std::string username =
        "legacy-user-" + std::to_string(index) + "-" + HexFromState(&state, 4);
    const std::string salt = HexFromState(&state, 8);
    const std::string hash = HexFromState(&state, 16);
    const std::uint64_t iterations = 1000 + NextRandom(&state) % 5000;
    const std::uint64_t created_at = 100000 + NextRandom(&state) % 1000000;
    const std::string sql =
        "INSERT INTO users VALUES (" + std::to_string(index) + ", " +
        SqlQuote(username) + ", X'" + salt + "', X'" + hash + "', " +
        std::to_string(iterations) + ", " + std::to_string(created_at) + ");";
    if (!ExecSql(database, sql.c_str())) {
      ok = false;
    }
  }
  for (int index = 0; ok && index < snapshot_count; ++index) {
    const std::string snapshot_id = HexFromState(&state, 16);
    const std::uint64_t user_id =
        1 + NextRandom(&state) % static_cast<std::uint64_t>(user_count);
    std::string display = "legacy snapshot " + std::to_string(index) + " " +
                          HexFromState(&state, 4);
    if (index % 17 == 0) {
      display += " O'Brien \\ backslash \"quoted\"";
    }
    if (index % 23 == 0) {
      display += " 中文快照-确定性";
    }
    if (index % 29 == 0) {
      display += "\tTABBED";
    }
    const std::uint64_t size_bytes = 1 + NextRandom(&state) % 5000000;
    const std::string sha256 = HexFromState(&state, 32);
    const std::uint64_t created_at = 1000 + NextRandom(&state) % 1000000;
    const std::string sql =
        "INSERT INTO snapshots VALUES (" + SqlQuote(snapshot_id) + ", " +
        std::to_string(user_id) + ", " + SqlQuote(display) + ", " +
        std::to_string(size_bytes) + ", " + SqlQuote(sha256) + ", " +
        std::to_string(created_at) + ", " + SqlQuote(snapshot_id + ".bak") +
        ");";
    if (!ExecSql(database, sql.c_str())) {
      ok = false;
    }
  }
  if (ok && !ExecSql(database, "PRAGMA user_version=1;")) {
    ok = false;
  }
  sqlite3_close(database);
  return ok;
}

void TestMigrationHalfwayRollback() {
  std::printf("[chain] RT6 迁移做到一半就失败 -> 整体回滚\n");
  const int checks_before = g_checks;
  const int failures_before = g_failures;

  const std::string root = WorkRoot() + "/rt6-halfway";
  RemoveTree(root);
  Check(MakeDirectoryTree(root), "RT6 建测试目录");

  for (const char* column_name :
       {"snapshot_kind", "parent_id", "generation", "lineage"}) {
    const std::string column = column_name;
    const std::string path = root + "/half-" + column + ".sqlite3";
    Check(CreateLegacyDatabaseLarge(path, 3, 12, 0xA5A5ull),
          "RT6 造 v1 库（" + column + "）");
    const std::string before_state = DumpDatabaseState(path);
    std::string before_bytes;
    ReadFile(path, &before_bytes);
    RemoteMetadataStore store;
    store.FailMigrationAfterColumnForTesting(column);
    std::string error;
    Check(!store.Open(path, &error),
          "RT6 注入在补完 " + column + " 之后失败 -> Open 返回 false", error);
    Check(error.find("injected migration failure") != std::string::npos &&
              error.find(column) != std::string::npos,
          "RT6 失败确实来自注入点本身（不是别的错误）", error);
    Check(!store.IsOpen(), "RT6 失败之后 store 不是打开状态");
    Check(ReadUserVersion(path) == 1, "RT6 user_version 仍然是 1",
          std::to_string(ReadUserVersion(path)));
    const std::vector<std::string> columns = SnapshotColumnNames(path);
    bool chain_column_present = false;
    for (const std::string& name : columns) {
      if (name == "snapshot_kind" || name == "parent_id" ||
          name == "generation" || name == "lineage") {
        chain_column_present = true;
      }
    }
    Check(!chain_column_present,
          "RT6 PRAGMA table_info(snapshots) 里没有那四个新列");
    Check(columns.size() == 7, "RT6 snapshots 仍然是 7 列",
          std::to_string(columns.size()) + " 列");
    Check(DumpDatabaseState(path) == before_state,
          "RT6 行与列逐值不变（迁移整体 ROLLBACK）");
    std::string after_bytes;
    ReadFile(path, &after_bytes);
    Note("RT6 " + column + "：文件字节" +
         (before_bytes == after_bytes ? "也完全没变"
                                      : "有变化（WAL/页头），值已逐项比对"));

    RemoteMetadataStore retry;
    std::string retry_error;
    Check(retry.Open(path, &retry_error),
          "RT6 回滚之后这个库还能正常迁移（重试成功）", retry_error);
    Check(ReadUserVersion(path) == 2, "RT6 重试之后版本是 2");
    Check(SnapshotColumnNames(path).size() == 11, "RT6 重试之后 11 列");
    retry.Close();
  }

  RemoveTree(root);
  EndCase("RT6", checks_before, failures_before);
}

void TestLargeLegacyMigration() {
  std::printf("[chain] RT7 大数据量 v1 -> v2 迁移\n");
  const int checks_before = g_checks;
  const int failures_before = g_failures;

  const int users = 24;
  const int snapshots = 140;
  const std::string root = WorkRoot() + "/rt7-large";
  RemoveTree(root);
  Check(MakeDirectoryTree(root), "RT7 建测试目录");
  const std::string path = root + "/large.sqlite3";
  Check(
      CreateLegacyDatabaseLarge(path, users, snapshots, 0x1234567890ABCDEFull),
      "RT7 造 v1 库（" + std::to_string(users) + " 用户 / " +
          std::to_string(snapshots) + " 快照）");
  Check(ReadUserVersion(path) == 1, "RT7 迁移前版本是 1");
  Check(static_cast<int>(LegacyUserRows(path).size()) == users,
        "RT7 legacy 用户数正确", std::to_string(LegacyUserRows(path).size()));
  const std::vector<std::string> before_snapshots = LegacySnapshotRows(path);
  const std::vector<std::string> before_users = LegacyUserRows(path);
  Check(static_cast<int>(before_snapshots.size()) == snapshots,
        "RT7 legacy 快照数正确", std::to_string(before_snapshots.size()));

  RemoteMetadataStore store;
  std::string error;
  Check(store.Open(path, &error), "RT7 打开并迁移", error);
  Check(ReadUserVersion(path) == 2, "RT7 PRAGMA user_version == 2");
  Check(SnapshotColumnNames(path).size() == 11, "RT7 迁移后 11 列");
  Check(LegacySnapshotRows(path) == before_snapshots,
        "RT7 七个旧列逐行逐值完全不变");
  Check(LegacyUserRows(path) == before_users, "RT7 users 表逐行逐值不变");

  const std::vector<std::string> new_columns = QueryRows(
      path,
      "SELECT snapshot_kind || '|' || parent_id || '|' || generation || '|' ||"
      " lineage FROM snapshots ORDER BY id;");
  Check(static_cast<int>(new_columns.size()) == snapshots,
        "RT7 新列也能逐行读出来");
  int defaults = 0;
  for (const std::string& row : new_columns) {
    if (row == "0||0|") {
      defaults += 1;
    }
  }
  Check(
      defaults == snapshots,
      "RT7 新列全部是默认值（kind 0 / parent '' / generation 0 / lineage ''）",
      std::to_string(defaults) + "/" + std::to_string(snapshots));

  std::uint64_t counted = 0;
  Check(store.CountSnapshots(1, &counted, &error) == StoreResult::kOk,
        "RT7 存储层能按用户统计", error);
  std::vector<backupproject::net::RemoteSnapshotRecord> records;
  Check(store.ListSnapshots(1, &records, &error) == StoreResult::kOk,
        "RT7 存储层能列出用户 1 的快照", error);
  int legacy_full = 0;
  for (const backupproject::net::RemoteSnapshotRecord& record : records) {
    if (record.snapshot_kind == 0 && record.parent_id.empty() &&
        record.generation == 0 && record.lineage.empty()) {
      legacy_full += 1;
    }
  }
  Check(legacy_full == static_cast<int>(records.size()),
        "RT7 读出来的 legacy 行都是 standalone full");
  store.Close();

  RemoveTree(root);
  EndCase("RT7", checks_before, failures_before);
}

void TestConcurrentMigration() {
  std::printf("[chain] RT8 并发迁移\n");
  const int checks_before = g_checks;
  const int failures_before = g_failures;

  const std::string root = WorkRoot() + "/rt8-concurrent";
  RemoveTree(root);
  Check(MakeDirectoryTree(root), "RT8 建测试目录");

  for (const int threads : {2, 4}) {
    const std::string path =
        root + "/race-" + std::to_string(threads) + ".sqlite3";
    Check(CreateLegacyDatabaseLarge(path, 20, 120, 0xFEEDFACEull),
          "RT8 造 v1 库（" + std::to_string(threads) + " 线程）");
    const std::vector<std::string> before_snapshots = LegacySnapshotRows(path);
    const std::vector<std::string> before_users = LegacyUserRows(path);

    std::atomic<int> ready(0);
    std::atomic<bool> go(false);
    std::atomic<int> successes(0);
    std::atomic<int> failures(0);
    std::vector<std::string> errors(static_cast<std::size_t>(threads));
    std::vector<std::thread> workers;
    for (int index = 0; index < threads; ++index) {
      workers.emplace_back([&, index] {
        RemoteMetadataStore store;
        std::string local_error;
        ready.fetch_add(1);
        while (!go.load()) {
          std::this_thread::yield();  // 起始屏障：不用 sleep 猜时间
        }
        if (store.Open(path, &local_error)) {
          successes.fetch_add(1);
          std::uint64_t counted = 0;
          store.CountSnapshots(1, &counted, &local_error);
        } else {
          failures.fetch_add(1);
          errors[static_cast<std::size_t>(index)] = local_error;
        }
        store.Close();
      });
    }
    while (ready.load() < threads) {
      std::this_thread::yield();
    }
    go.store(true);
    for (std::thread& worker : workers) {
      worker.join();
    }

    Check(successes.load() + failures.load() == threads,
          "RT8 所有线程都给出了明确结果",
          std::to_string(successes.load()) + " 成功 / " +
              std::to_string(failures.load()) + " 失败");
    Check(ReadUserVersion(path) == 2, "RT8 并发迁移之后版本是 2",
          std::to_string(ReadUserVersion(path)));
    const std::vector<std::string> columns = SnapshotColumnNames(path);
    bool complete = columns.size() == 11;
    for (const char* required :
         {"snapshot_kind", "parent_id", "generation", "lineage"}) {
      bool present = false;
      for (const std::string& name : columns) {
        if (name == required) {
          present = true;
        }
      }
      if (!present) {
        complete = false;
      }
    }
    Check(complete, "RT8 schema 是完整的 v2（没有半 schema）",
          std::to_string(columns.size()) + " 列");
    Check(LegacySnapshotRows(path) == before_snapshots,
          "RT8 数据完整（七个旧列逐行逐值不变）");
    Check(LegacyUserRows(path) == before_users, "RT8 users 表完整");
    if (failures.load() > 0) {
      RemoteMetadataStore retry;
      std::string retry_error;
      Check(retry.Open(path, &retry_error), "RT8 失败的线程重试必须成功",
            retry_error);
      retry.Close();
    }
    std::string observed = "RT8 观测：" + std::to_string(threads) +
                           " 个线程同时对同一个 v1 库调用 Open() -> 成功 " +
                           std::to_string(successes.load()) + " / 失败 " +
                           std::to_string(failures.load());
    for (const std::string& message : errors) {
      if (!message.empty()) {
        observed += "；失败原因：" + message;
      }
    }
    Note(observed);
  }

  RemoveTree(root);
  EndCase("RT8", checks_before, failures_before);
}

// ---- RT9 用的原始协议连接：把两个上传的时序摆成「都 BEGIN 完，再各自
// END」----
//
// 高层 RemoteArchiveClient::UploadSnapshotFile 是 BEGIN+CHUNK+END 一体的，
// 摆不出这个交错；这里直接走 remote_test_support.h 的收发帧（与
// tests/unit/remote_server_test.cpp 用的是同一套 BPSEC1 客户端握手）。

struct RawConnection {
  int fd = -1;
  backupproject::net::SecureChannel channel;

  bool Connect(std::uint16_t port, const std::string& pin,
               std::string* error_message) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return false;
    }
    timeval timeout;
    timeout.tv_sec = 15;
    timeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
      ::close(fd);
      fd = -1;
      return false;
    }
    if (!remote_test_support::HandshakeTestClient(fd, pin, &channel,
                                                  error_message)) {
      ::close(fd);
      fd = -1;
      return false;
    }
    return true;
  }

  bool Request(std::uint16_t opcode, const std::string& payload,
               std::uint64_t request_id,
               backupproject::net::FrameHeader* header, std::string* response,
               std::string* error_message) {
    if (!remote_test_support::SendTestFrame(&channel, fd, opcode, 0, request_id,
                                            payload, error_message)) {
      return false;
    }
    return remote_test_support::ReceiveTestFrame(&channel, fd, header, response,
                                                 error_message) ==
           backupproject::net::FrameReadStatus::kOk;
  }

  void Close() {
    if (fd >= 0) {
      ::shutdown(fd, SHUT_RDWR);
      ::close(fd);
      fd = -1;
    }
  }
};

bool RawLogin(RawConnection* connection, const std::string& username,
              const std::string& password, std::string* error_message) {
  backupproject::net::PayloadBuilder builder;
  if (!builder.AppendString(username, backupproject::net::kMaxUsernameBytes,
                            error_message) ||
      !builder.AppendString(password, backupproject::net::kMaxPasswordBytes,
                            error_message)) {
    return false;
  }
  backupproject::net::FrameHeader header;
  std::string response;
  if (!connection->Request(
          static_cast<std::uint16_t>(backupproject::net::Opcode::kLogin),
          builder.data(), 1, &header, &response, error_message)) {
    return false;
  }
  if (header.status !=
      static_cast<std::uint32_t>(backupproject::net::Status::kOk)) {
    if (error_message != nullptr) {
      *error_message = std::string("login status ") +
                       backupproject::net::StatusName(header.status);
    }
    return false;
  }
  return true;
}

// 递归数目录下后缀为 suffix 的普通文件（服务端发布的 blob 是 <id>.bak）。
int CountFilesWithSuffixUnder(const std::string& directory,
                              const std::string& suffix) {
  int count = 0;
  for (const std::string& name : DirectoryEntries(directory)) {
    const std::string path = directory + "/" + name;
    struct stat info;
    if (::lstat(path.c_str(), &info) != 0) {
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      count += CountFilesWithSuffixUnder(path, suffix);
      continue;
    }
    if (path.size() >= suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
      count += 1;
    }
  }
  return count;
}

// ============================================================================
// RT9. 两个连接抢同一个父：恰好一个成功，另一个拿到链冲突
// ============================================================================
//
// 服务端在 UPLOAD_BEGIN 只校验「父存在、同一个用户、lineage 相同、代数=父+1」，
// 而「父最多一个活着的孩子」是在 UPLOAD_END 落库那一刻、与 INSERT 同一个
// 临界区里校验的（RemoteMetadataStore::InsertSnapshot）。所以两个连接完全
// 可以都通过 UPLOAD_BEGIN，然后抢 UPLOAD_END —— 这里就把这个交错摆出来：
// 先让 A/B 都 BEGIN 完，再各自送数据与 END。

void TestConcurrentSameParentUpload() {
  std::printf("[chain] RT9 双连接同父上传竞态\n");
  const int checks_before = g_checks;
  const int failures_before = g_failures;

  RedTeamServer* fixture = SharedServer();
  Check(fixture != nullptr, "RT9 共享服务端可用");
  if (fixture == nullptr) {
    EndCase("RT9", checks_before, failures_before);
    return;
  }
  const std::string root = fixture->base;
  std::string error;

  // 链根：一份真实完整快照，lineage 非空（增量必须与父同 lineage）。
  LocalChain chain;
  Check(BuildLocalChain(root, "rt9", &chain, &error), "RT9 造本地链", error);
  const std::string lineage = Hex64('9');
  backupproject::net::RemoteSnapshotInfo root_info;
  Check(UploadTrio(fixture, chain.repository_directory, chain.full_name,
                   "rt9-root", 0, std::string(), lineage, &root_info, &error),
        "RT9 上传链根", error);
  Check(root_info.generation == 0 && root_info.parent_snapshot_id.empty(),
        "RT9 链根是 generation 0 的完整快照");

  // 上传素材：同一个 delta 材料包，两个连接各传一次（等价于同一次上传被
  // 两个连接同时发起）。
  const std::string bundle_path = root + "/rt9-child.bundle";
  backupproject::net::SnapshotBundleInfo bundle_info;
  Check(backupproject::net::BuildSnapshotBundle(chain.repository_directory,
                                                chain.delta_name, bundle_path,
                                                &bundle_info, &error),
        "RT9 打出 delta 材料包", error);
  std::string payload;
  Check(ReadFile(bundle_path, &payload) && !payload.empty(),
        "RT9 读回材料包字节");
  const std::string digest = Sha256HexOf(payload);

  const std::string users_root = root + "/srv/data/users";
  const int blobs_before = CountFilesWithSuffixUnder(users_root, ".bak");

  RawConnection connection_a;
  RawConnection connection_b;
  Check(
      connection_a.Connect(fixture->server.bound_port(), fixture->pin, &error),
      "RT9 连接 A 建立（真 TCP + BPSEC1 握手）", error);
  Check(
      connection_b.Connect(fixture->server.bound_port(), fixture->pin, &error),
      "RT9 连接 B 建立（真 TCP + BPSEC1 握手）", error);
  Check(RawLogin(&connection_a, fixture->username, fixture->password, &error),
        "RT9 A 登录", error);
  Check(RawLogin(&connection_b, fixture->username, fixture->password, &error),
        "RT9 B 登录", error);

  std::string builder_error;
  auto upload_begin_payload = [&]() {
    backupproject::net::PayloadBuilder builder;
    builder.AppendString("rt9-child", backupproject::net::kMaxDisplayNameBytes,
                         &builder_error);
    builder.AppendU64(payload.size());
    builder.AppendString(digest, digest.size(), &builder_error);
    builder.AppendU16(1);  // kIncremental
    builder.AppendString(root_info.snapshot_id,
                         backupproject::net::kMaxSnapshotIdBytes,
                         &builder_error);
    builder.AppendString(lineage, backupproject::net::kMaxLineageBytes,
                         &builder_error);
    return builder.data();
  };
  const std::uint16_t op_begin =
      static_cast<std::uint16_t>(backupproject::net::Opcode::kUploadBegin);
  const std::uint16_t op_chunk =
      static_cast<std::uint16_t>(backupproject::net::Opcode::kUploadChunk);
  const std::uint16_t op_end =
      static_cast<std::uint16_t>(backupproject::net::Opcode::kUploadEnd);
  const std::uint32_t status_ok =
      static_cast<std::uint32_t>(backupproject::net::Status::kOk);
  const std::uint32_t status_chain_conflict =
      static_cast<std::uint32_t>(backupproject::net::Status::kChainConflict);

  // 两个连接都通过 UPLOAD_BEGIN（同一个父、同一个 lineage）。
  backupproject::net::FrameHeader header_a;
  backupproject::net::FrameHeader header_b;
  std::string response_a;
  std::string response_b;
  const bool begin_a = connection_a.Request(op_begin, upload_begin_payload(), 2,
                                            &header_a, &response_a, &error) &&
                       header_a.status == status_ok;
  Check(begin_a, "RT9 A 通过 UPLOAD_BEGIN（同一个父）",
        std::string(backupproject::net::StatusName(header_a.status)) + " " +
            error);
  const bool begin_b = connection_b.Request(op_begin, upload_begin_payload(), 2,
                                            &header_b, &response_b, &error) &&
                       header_b.status == status_ok;
  Check(begin_b, "RT9 B 也通过 UPLOAD_BEGIN（同一个父）",
        std::string(backupproject::net::StatusName(header_b.status)) + " " +
            error);

  // A 先送完数据并 END：必须成功。
  std::string chunk_error;
  const bool chunk_a = connection_a.Request(op_chunk, payload, 3, &header_a,
                                            &response_a, &chunk_error) &&
                       header_a.status == status_ok;
  Check(chunk_a, "RT9 A 送数据块", chunk_error);
  const bool end_a = connection_a.Request(op_end, std::string(), 4, &header_a,
                                          &response_a, &error) &&
                     header_a.status == status_ok;
  Check(end_a, "RT9 A 的 UPLOAD_END 成功（它先到）",
        std::string(backupproject::net::StatusName(header_a.status)) + " " +
            error);

  // B 再送数据并 END：必须拿到链冲突（父已经有活着的孩子）。
  const bool chunk_b = connection_b.Request(op_chunk, payload, 3, &header_b,
                                            &response_b, &chunk_error) &&
                       header_b.status == status_ok;
  Check(chunk_b, "RT9 B 送数据块", chunk_error);
  std::string end_b_error;
  const bool end_b_frame = connection_b.Request(
      op_end, std::string(), 4, &header_b, &response_b, &end_b_error);
  Check(end_b_frame, "RT9 B 的 UPLOAD_END 有响应（不是断连）", end_b_error);
  Check(header_b.status == status_chain_conflict,
        "RT9 B 拿到链冲突 status=13（父已经有孩子）",
        std::string(backupproject::net::StatusName(header_b.status)) + " (" +
            std::to_string(header_b.status) + ")");
  Check(begin_a && begin_b && end_a && end_b_frame &&
            header_b.status == status_chain_conflict,
        "RT9 判别：恰好一个上传成功、另一个被链冲突挡下");

  connection_a.Close();
  connection_b.Close();

  // 服务端只多出 1 行元数据 / 1 个 blob。
  std::vector<backupproject::net::RemoteSnapshotInfo> snapshots;
  Check(fixture->client.List(&snapshots, &error), "RT9 竞态之后仍能 LIST",
        error);
  int children = 0;
  bool winner_found = false;
  for (const backupproject::net::RemoteSnapshotInfo& snapshot : snapshots) {
    if (snapshot.parent_snapshot_id == root_info.snapshot_id) {
      children += 1;
      if (snapshot.snapshot_kind == 1 && snapshot.generation == 1) {
        winner_found = true;
      }
    }
  }
  Check(children == 1, "RT9 服务端只登记了 1 个孩子",
        std::to_string(children) + " 个");
  Check(winner_found, "RT9 那个孩子是 kind=1 / generation=1 的增量");
  const int blobs_after = CountFilesWithSuffixUnder(users_root, ".bak");
  Check(blobs_after == blobs_before + 1, "RT9 服务端只多出 1 个 blob",
        std::to_string(blobs_before) + " -> " + std::to_string(blobs_after));
  int tmps = 0;
  for (const std::string& user_name : DirectoryEntries(users_root)) {
    tmps += CountFilesWithSuffixUnder(users_root + "/" + user_name + "/tmp",
                                      ".part");
  }
  Check(tmps == 0, "RT9 失败那一路的上传临时文件被清理掉了",
        std::to_string(tmps) + " 个 .part");

  // 服务端仍然健康：PING 与新连接 LIST 都要正常。
  std::string software;
  std::uint16_t protocol_version = 0;
  std::uint64_t server_time = 0;
  Check(
      fixture->client.Ping(&software, &protocol_version, &server_time, &error),
      "RT9 竞态之后 PING 正常", error);
  std::vector<backupproject::net::RemoteSnapshotInfo> after;
  Check(
      fixture->client.List(&after, &error) && after.size() == snapshots.size(),
      "RT9 竞态之后 LIST 稳定", error);

  Note(
      "RT9 观测：两个连接都通过 UPLOAD_BEGIN（同一个父），A 的 UPLOAD_END "
      "成功，"
      "B 的 UPLOAD_END 得到 " +
      std::string(backupproject::net::StatusName(header_b.status)) +
      "（链冲突），服务端只多 1 行/1 blob，临时文件已清理。");

  EndCase("RT9", checks_before, failures_before);
}
}  // namespace

int main() {
  std::printf("远端增量链单元测试\n");
  TestSnapshotBundle();
  TestChainResolution();
  TestMigrationAndChainMetadata();
  TestBundleFuzz();
  TestUnknownSchemaVersion();
  TestBundleMaliciousConstructions();
  TestExtractionLocalAttacks();
  TestRemoteIndexAttacks();
  TestCacheIsolation();
  TestMetadataArchiveCrossCheck();
  TestMigrationHalfwayRollback();
  TestLargeLegacyMigration();
  TestConcurrentMigration();
  TestConcurrentSameParentUpload();
  ShutdownSharedServer();
  const int passed = g_checks - g_failures;
  std::printf("remote-chain-test: %d/%d checks passed\n", passed, g_checks);
  return g_failures == 0 ? 0 : 1;
}

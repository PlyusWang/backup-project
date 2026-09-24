// tests/unit/config_manager_test.cpp

#include "config_manager.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(__unix__)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace bp = backupproject;

namespace {

// 用例级计数器和断言级计数器刻意分开：
//   g_passed / g_failed / g_skipped 数的是**用例**，三者之和必须正好等于跑过
//   的用例数；g_check_failures 数的是**断言**，一个用例可以失败很多次。
// 早期版本只有一个 g_failures，既当断言数又当用例数用，于是汇总里的 "failed"
// 其实是失败断言数，passed + failed 根本对不上用例总数。
int g_checks = 0;
int g_check_failures = 0;
int g_passed = 0;
int g_failed = 0;
int g_skipped = 0;
// Run() 实际调用过的用例数，用来校验三档统计之和没有重复计数。
int g_ran = 0;

void Check(bool condition, const char* expression, const char* test_name) {
  ++g_checks;
  if (!condition) {
    ++g_check_failures;
    std::fprintf(stderr, "    CHECK FAILED: %s (%s)\n", expression, test_name);
  }
}

#define CHECK(test_name, expression) Check((expression), #expression, test_name)

void WriteFile(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

void MissingConfig(const fs::path& root) {
  const char* name = "MissingConfig";
  bp::AppConfig config;
  config.backup_repository_path = "stale value";
  std::string error = "stale error";
  bp::ConfigManager manager((root / "missing.json").string());
  CHECK(name, manager.Load(&config, &error) == bp::ConfigLoadStatus::kMissing);
  CHECK(name, config.backup_repository_path.empty());
  CHECK(name, error.empty());
  CHECK(name, !fs::exists(root / "missing.json"));
}

void RoundTripSpecialPaths(const fs::path& root) {
  const char* name = "RoundTripSpecialPaths";
  const fs::path config_path = root / "missing-parent" / "config.json";
  const std::string repository = "/home/用户/My Backups/#a%b/quote\"slash\\";
  bp::ConfigManager manager(config_path.string());
  bp::AppConfig saved{repository};
  std::string error;
  CHECK(name, manager.Save(saved, &error));
  CHECK(name, error.empty());
  CHECK(name, fs::is_regular_file(config_path));
  CHECK(name, !fs::exists(config_path.string() + ".tmp"));

  bp::AppConfig loaded;
  bp::ConfigManager reloaded(config_path.string());
  CHECK(name, reloaded.Load(&loaded, &error) == bp::ConfigLoadStatus::kLoaded);
  CHECK(name, loaded.backup_repository_path == repository);
}

void EmptyRepositoryIsValid(const fs::path& root) {
  const char* name = "EmptyRepositoryIsValid";
  const fs::path config_path = root / "empty.json";
  bp::ConfigManager manager(config_path.string());
  std::string error;
  CHECK(name, manager.Save(bp::AppConfig{}, &error));
  bp::AppConfig loaded{"not empty"};
  CHECK(name, manager.Load(&loaded, &error) == bp::ConfigLoadStatus::kLoaded);
  CHECK(name, loaded.backup_repository_path.empty());
}

void RepeatedSaveReplacesConfig(const fs::path& root) {
  const char* name = "RepeatedSaveReplacesConfig";
  const fs::path config_path = root / "repeat.json";
  bp::ConfigManager manager(config_path.string());
  std::string error;
  CHECK(name, manager.Save(bp::AppConfig{"/srv/first"}, &error));
  CHECK(name, manager.Save(bp::AppConfig{"/srv/second"}, &error));
  CHECK(name, !fs::exists(config_path.string() + ".tmp"));
  bp::AppConfig loaded;
  CHECK(name, manager.Load(&loaded, &error) == bp::ConfigLoadStatus::kLoaded);
  CHECK(name, loaded.backup_repository_path == "/srv/second");
}

void RejectMalformedAndTruncatedJson(const fs::path& root) {
  const char* name = "RejectMalformedAndTruncatedJson";
  const fs::path malformed = root / "malformed.json";
  WriteFile(malformed, "{ not JSON }");
  bp::AppConfig config{"stale"};
  std::string error;
  bp::ConfigManager malformed_manager(malformed.string());
  CHECK(name, malformed_manager.Load(&config, &error) == bp::ConfigLoadStatus::kError);
  CHECK(name, error.find("Invalid config JSON") != std::string::npos);
  CHECK(name, config.backup_repository_path.empty());

  const fs::path truncated = root / "truncated.json";
  WriteFile(truncated,
            "{\n  \"version\": 1,\n  \"backup_repository_path\": \"/srv");
  bp::ConfigManager truncated_manager(truncated.string());
  CHECK(name, truncated_manager.Load(&config, &error) == bp::ConfigLoadStatus::kError);
  CHECK(name, error.find("unterminated JSON string") != std::string::npos);
}

void RejectSchemaErrors(const fs::path& root) {
  const char* name = "RejectSchemaErrors";
  struct Case {
    const char* filename;
    const char* contents;
  };
  const Case cases[] = {
      {"missing-version.json", "{\"backup_repository_path\": \"/srv\"}"},
      {"wrong-version.json",
       "{\"version\": 2, \"backup_repository_path\": \"/srv\"}"},
      {"version-type.json",
       "{\"version\": \"1\", \"backup_repository_path\": \"/srv\"}"},
      {"missing-path.json", "{\"version\": 1}"},
      {"path-type.json", "{\"version\": 1, \"backup_repository_path\": 7}"},
  };
  for (const Case& item : cases) {
    const fs::path path = root / item.filename;
    WriteFile(path, item.contents);
    bp::AppConfig config;
    std::string error;
    bp::ConfigManager manager(path.string());
    CHECK(name, manager.Load(&config, &error) == bp::ConfigLoadStatus::kError);
    CHECK(name, !error.empty());
  }
}

void SaveFailureKeepsExistingFile(const fs::path& root) {
  const char* name = "SaveFailureKeepsExistingFile";
  const fs::path parent_file = root / "not-a-directory";
  WriteFile(parent_file, "keep this file");
  bp::ConfigManager manager((parent_file / "config.json").string());
  std::string error;
  CHECK(name, !manager.Save(bp::AppConfig{"/srv/repository"}, &error));
  CHECK(name, !error.empty());
  CHECK(name, ReadFile(parent_file) == "keep this file");
}

void NullOutputIsAnError(const fs::path& root) {
  const char* name = "NullOutputIsAnError";
  bp::ConfigManager manager((root / "config.json").string());
  std::string error;
  CHECK(name, manager.Load(nullptr, &error) == bp::ConfigLoadStatus::kError);
  CHECK(name, error.find("must not be null") != std::string::npos);
}

void UnreadableConfigWhenSupported(const fs::path& root) {
#if defined(__unix__)
  const char* name = "UnreadableConfigWhenSupported";
  const fs::path path = root / "unreadable.json";
  WriteFile(path, "{\"version\": 1, \"backup_repository_path\": \"/srv\"}");
  chmod(path.c_str(), 0000);
  bp::AppConfig config;
  std::string error;
  bp::ConfigManager manager(path.string());
  const bp::ConfigLoadStatus status = manager.Load(&config, &error);
  chmod(path.c_str(), 0600);
  if (status == bp::ConfigLoadStatus::kLoaded) {
    ++g_skipped;  // Root may still read a 0000 file.
    std::printf("[ SKIPPED ] %s (process can bypass permissions)\n", name);
    return;
  }
  CHECK(name, status == bp::ConfigLoadStatus::kError);
  CHECK(name, !error.empty());
#else
  (void)root;
  ++g_skipped;
  std::printf("[ SKIPPED ] UnreadableConfigWhenSupported (non-POSIX platform)\n");
#endif
}

void DuplicateVersionRejectedWithDiagnostic(const fs::path& root) {
  const char* name = "DuplicateVersionRejectedWithDiagnostic";
  const fs::path path = root / "duplicate-version.json";
  WriteFile(path,
            "{\"version\": 1, \"version\": 1, "
            "\"backup_repository_path\": \"/srv\"}");
  bp::AppConfig config;
  std::string error;
  bp::ConfigManager manager(path.string());
  CHECK(name, manager.Load(&config, &error) == bp::ConfigLoadStatus::kError);
  CHECK(name, !error.empty());
  CHECK(name, error.find("duplicate") != std::string::npos);
  CHECK(name, error.find("version") != std::string::npos);
}

void DuplicateRepositoryPathRejectedWithDiagnostic(const fs::path& root) {
  const char* name = "DuplicateRepositoryPathRejectedWithDiagnostic";
  const fs::path path = root / "duplicate-path.json";
  WriteFile(path,
            "{\"version\": 1, \"backup_repository_path\": \"/srv\", "
            "\"backup_repository_path\": \"/other\"}");
  bp::AppConfig config{"stale"};
  std::string error;
  bp::ConfigManager manager(path.string());
  CHECK(name, manager.Load(&config, &error) == bp::ConfigLoadStatus::kError);
  CHECK(name, !error.empty());
  CHECK(name, error.find("duplicate") != std::string::npos);
  CHECK(name, error.find("backup_repository_path") != std::string::npos);
  CHECK(name, config.backup_repository_path.empty());
}

void EmptyConfigPathIsError(const fs::path& root) {
  const char* name = "EmptyConfigPathIsError";
  bp::AppConfig config{"stale"};
  std::string error = "stale error";
  bp::ConfigManager manager{std::string()};
  CHECK(name, manager.config_file_path().empty());
  CHECK(name, manager.Load(&config, &error) == bp::ConfigLoadStatus::kError);
  CHECK(name, !error.empty());
  CHECK(name, config.backup_repository_path.empty());

  // 真正"路径非空、文件不存在"仍然是 kMissing：这两件事不能混。
  bp::ConfigManager absent((root / "definitely-absent.json").string());
  bp::AppConfig absent_config;
  std::string absent_error;
  CHECK(name, absent.Load(&absent_config, &absent_error) ==
                  bp::ConfigLoadStatus::kMissing);
  CHECK(name, absent_error.empty());
}

void AcceptsReorderedKeysAndWhitespace(const fs::path& root) {
  const char* name = "AcceptsReorderedKeysAndWhitespace";
  const fs::path path = root / "reordered.json";
  WriteFile(path,
            "{\n"
            "  \"backup_repository_path\": \"/x\",\n"
            "  \"version\": 1\n"
            "}\n");
  bp::AppConfig config;
  std::string error;
  bp::ConfigManager manager(path.string());
  CHECK(name, manager.Load(&config, &error) == bp::ConfigLoadStatus::kLoaded);
  CHECK(name, error.empty());
  CHECK(name, config.backup_repository_path == "/x");

  // 制表符、键顺序、字段间的空白都不影响固定 schema 的解析。
  const fs::path compact = root / "reordered-compact.json";
  WriteFile(compact,
            "{\t\"version\"\t:\t1\t,\t\"backup_repository_path\":\"/y\"}");
  bp::AppConfig compact_config;
  bp::ConfigManager compact_manager(compact.string());
  CHECK(name, compact_manager.Load(&compact_config, &error) ==
                  bp::ConfigLoadStatus::kLoaded);
  CHECK(name, compact_config.backup_repository_path == "/y");
}

void RejectsUnknownField(const fs::path& root) {
  const char* name = "RejectsUnknownField";
  const fs::path path = root / "unknown-field.json";
  WriteFile(path,
            "{\n"
            "  \"version\": 1,\n"
            "  \"backup_repository_path\": \"/x\",\n"
            "  \"unknown\": true\n"
            "}\n");
  bp::AppConfig config{"stale"};
  std::string error;
  bp::ConfigManager manager(path.string());
  CHECK(name, manager.Load(&config, &error) == bp::ConfigLoadStatus::kError);
  CHECK(name, !error.empty());
  CHECK(name, error.find("unknown config field") != std::string::npos);
}

// 每个用例只允许落进 passed / failed / skipped 三档中的一档。
// 之前 skipped 的用例在 Run 里又被计了一次 passed，于是
// passed + failed + skipped 会大于实际用例数——统计是错的。
void Run(const char* name, void (*test)(const fs::path&), const fs::path& root) {
  ++g_ran;
  const int failures_before = g_check_failures;
  const int skipped_before = g_skipped;
  test(root);
  if (g_check_failures != failures_before) {
    ++g_failed;
    std::printf("[  FAILED  ] %s\n", name);
    return;
  }
  if (g_skipped != skipped_before) {
    // 环境不满足：只算 skip，不再打 PASSED，也不计入 passed。
    return;
  }
  ++g_passed;
  std::printf("[  PASSED  ] %s\n", name);
}

}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() /
                        ("backup-project-config-manager-" +
                         std::to_string(static_cast<unsigned long long>(
                             std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count())));
  std::error_code error;
  fs::create_directories(root, error);
  if (error) {
    std::fprintf(stderr, "unable to create test directory: %s\n",
                 error.message().c_str());
    return 2;
  }

  Run("MissingConfig", MissingConfig, root);
  Run("RoundTripSpecialPaths", RoundTripSpecialPaths, root);
  Run("EmptyRepositoryIsValid", EmptyRepositoryIsValid, root);
  Run("RepeatedSaveReplacesConfig", RepeatedSaveReplacesConfig, root);
  Run("RejectMalformedAndTruncatedJson", RejectMalformedAndTruncatedJson,
      root);
  Run("RejectSchemaErrors", RejectSchemaErrors, root);
  Run("SaveFailureKeepsExistingFile", SaveFailureKeepsExistingFile, root);
  Run("NullOutputIsAnError", NullOutputIsAnError, root);
  Run("UnreadableConfigWhenSupported", UnreadableConfigWhenSupported, root);
  Run("DuplicateVersionRejectedWithDiagnostic",
      DuplicateVersionRejectedWithDiagnostic, root);
  Run("DuplicateRepositoryPathRejectedWithDiagnostic",
      DuplicateRepositoryPathRejectedWithDiagnostic, root);
  Run("EmptyConfigPathIsError", EmptyConfigPathIsError, root);
  Run("AcceptsReorderedKeysAndWhitespace", AcceptsReorderedKeysAndWhitespace,
      root);
  Run("RejectsUnknownField", RejectsUnknownField, root);

  fs::remove_all(root, error);

  // 自检：一个用例只能落进一档，三档之和必须正好等于跑过的用例数。
  // 这条判断把"统计错误"变成会失败的测试，而不是一个看不出来的数字。
  const int total = g_passed + g_failed + g_skipped;
  std::printf(
      "[config-manager] tests=%d passed=%d failed=%d skipped=%d checks=%d\n",
      total, g_passed, g_failed, g_skipped, g_checks);
  if (g_check_failures != 0) {
    std::fprintf(stderr, "[config-manager] %d CHECK failure(s) in %d case(s)\n",
                 g_check_failures, g_failed);
  }
  if (total != g_ran) {
    std::fprintf(stderr,
                 "[config-manager] harness accounting error: %d cases ran but "
                 "the tally adds up to %d\n",
                 g_ran, total);
    return 2;
  }
  return (g_failed == 0 && g_check_failures == 0) ? 0 : 1;
}

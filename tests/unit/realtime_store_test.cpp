// realtime_store_test.cpp
//
// PR #19：RealtimeConfig / RealtimeStore 的专项测试。
//
// 要钉住的：
//   * 支持矩阵六格全 YES，且 Realtime 不引入第二张表；
//   * 自动触发（Scheduled / Realtime）一律不接受加密；
//   * 解析严格：未知 key / 缺 key / 重复 key / 类型不符 / 超界 / NUL 全部拒绝，
//     而且**不把坏文件恢复成默认值**；
//   * 保存是原子的、写出去的读得回来；
//   * source / repository 重叠必须在组件级判断（/home/a 与 /home/abc 不误判）；
//   * job identity 对配置变化敏感。

#include "realtime_store.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "backup_mode.h"
#include "source_digest.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

std::string ConfigText(const std::string& source_path, bool enabled = false) {
  return std::string("{\n") + "    \"version\": 1,\n" +
         "    \"enabled\": " + (enabled ? "true" : "false") + ",\n" +
         "    \"trigger\": \"realtime\",\n" + "    \"source_path\": \"" +
         source_path + "\",\n" + "    \"debounce_ms\": 500,\n" +
         "    \"max_wait_ms\": 5000,\n" + "    \"retain_count\": 12,\n" +
         "    \"strategy\": \"full\",\n" + "    \"pack\": \"mypack\",\n" +
         "    \"compression\": \"none\",\n" +
         "    \"encryption\": \"none\",\n" + "    \"include_rules\": [],\n" +
         "    \"exclude_rules\": []\n" + "}\n";
}

}  // namespace

int main() {
  // ---- R1：支持矩阵 ----
  test_support::Section("INC-RT19 1. Trigger x Strategy 六格与选项矩阵");
  {
    struct ModeCase {
      bp::BackupTrigger trigger;
      bp::BackupStrategy strategy;
      bool supported;
      const char* label;
    };
    const std::vector<ModeCase> modes = {
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kFull, true,
         "Manual + Full"},
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kIncremental, true,
         "Manual + Incremental"},
        {bp::BackupTrigger::kScheduled, bp::BackupStrategy::kFull, true,
         "Scheduled + Full"},
        {bp::BackupTrigger::kScheduled, bp::BackupStrategy::kIncremental, true,
         "Scheduled + Incremental"},
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kFull, true,
         "Realtime + Full"},
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kIncremental, true,
         "Realtime + Incremental"},
    };
    for (const ModeCase& item : modes) {
      test_support::Check(bp::IsSupportedBackupMode(
                              item.trigger, item.strategy) == item.supported,
                          std::string("INC-RT19 T1 支持矩阵: ") + item.label);
    }

    struct OptionCase {
      bp::BackupTrigger trigger;
      bp::BackupStrategy strategy;
      bp::PackMethod pack;
      bp::EncryptionMethod encryption;
      bool supported;
      const char* label;
    };
    const std::vector<OptionCase> options = {
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kFull,
         bp::PackMethod::kMyPack, bp::EncryptionMethod::kNone, true,
         "Realtime + Full + mypack + none"},
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kFull,
         bp::PackMethod::kUstar, bp::EncryptionMethod::kNone, true,
         "Realtime + Full + ustar（Full 仍支持 USTAR）"},
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kMyPack, bp::EncryptionMethod::kNone, true,
         "Realtime + Incremental + mypack + none"},
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kFull,
         bp::PackMethod::kMyPack, bp::EncryptionMethod::kAes256CtrHmacSha256,
         false, "Realtime + Full + aes 被拒绝"},
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kMyPack, bp::EncryptionMethod::kAes256CtrHmacSha256,
         false, "Realtime + Incremental + aes 被拒绝"},
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kUstar, bp::EncryptionMethod::kNone, false,
         "Realtime + Incremental + ustar 被拒绝"},
    };
    for (const OptionCase& item : options) {
      bp::BackupOptionCombination combination;
      combination.trigger = item.trigger;
      combination.strategy = item.strategy;
      combination.pack_method = item.pack;
      combination.encryption_method = item.encryption;
      const bool supported =
          bp::IsSupportedBackupOptionCombination(combination);
      test_support::Check(supported == item.supported,
                          std::string("INC-RT19 T1 ") + item.label);
      if (!supported) {
        test_support::Check(
            !bp::UnsupportedBackupOptionCombinationReason(combination).empty(),
            std::string("INC-RT19 T1 拒绝理由非空: ") + item.label);
      }
    }
    bp::BackupOptionCombination encrypted;
    encrypted.trigger = bp::BackupTrigger::kRealtime;
    encrypted.strategy = bp::BackupStrategy::kFull;
    encrypted.encryption_method = bp::EncryptionMethod::kDesCbcHmacSha256;
    test_support::Check(
        bp::UnsupportedBackupOptionCombinationReason(encrypted).find(
            "不启用加密") != std::string::npos,
        "INC-RT19 T1 实时加密的拒绝理由说明不保存密码");
  }

  // ---- R2：默认值 / 往返 ----
  test_support::Section("INC-RT19 2. 默认值与原子往返");
  {
    const std::string work = test_support::FreshDir("realtime-store");
    const std::string source = work + "/source";
    test_support::Mkdir(source, 0755);
    const std::string path = work + "/realtime.json";
    bp::RealtimeStore store(path);

    bp::RealtimeConfig missing;
    std::string error;
    test_support::Check(
        store.Load(&missing, &error) == bp::RealtimeLoadStatus::kMissing,
        "INC-RT19 T2 文件不存在 -> kMissing（等价默认配置）", error);

    bp::RealtimeConfig config;
    config.source_path = source;
    config.debounce_ms = 750;
    config.max_wait_ms = 9000;
    config.retain_count = 5;
    config.strategy = bp::BackupStrategy::kIncremental;
    config.compression_method = bp::CompressionMethod::kLzssHuffman;
    config.include_rules = {"ext:txt", "name:*.md"};
    config.exclude_rules = {"name:skip*"};
    error.clear();
    test_support::Check(store.Save(config, &error), "INC-RT19 T2 保存成功",
                        error);
    test_support::Check(!test_support::Exists(path + ".tmp"),
                        "INC-RT19 T2 不留 temp 文件");

    bp::RealtimeConfig loaded;
    error.clear();
    test_support::Check(
        store.Load(&loaded, &error) == bp::RealtimeLoadStatus::kLoaded,
        "INC-RT19 T2 读回成功", error);
    test_support::Check(
        loaded.source_path == source && loaded.debounce_ms == 750 &&
            loaded.max_wait_ms == 9000 && loaded.retain_count == 5 &&
            loaded.strategy == bp::BackupStrategy::kIncremental &&
            loaded.compression_method == bp::CompressionMethod::kLzssHuffman &&
            loaded.include_rules == config.include_rules &&
            loaded.exclude_rules == config.exclude_rules &&
            loaded.trigger == bp::BackupTrigger::kRealtime,
        "INC-RT19 T2 字段逐个往返一致");

    // 默认值就是文档里冻结的那一组。
    const bp::RealtimeConfig defaults;
    test_support::Check(
        defaults.version == 1 && !defaults.enabled &&
            defaults.trigger == bp::BackupTrigger::kRealtime &&
            defaults.source_path.empty() && defaults.debounce_ms == 500 &&
            defaults.max_wait_ms == 5000 && defaults.retain_count == 12 &&
            defaults.strategy == bp::BackupStrategy::kFull &&
            defaults.pack_method == bp::PackMethod::kMyPack &&
            defaults.compression_method == bp::CompressionMethod::kNone &&
            defaults.encryption_method == bp::EncryptionMethod::kNone,
        "INC-RT19 T2 默认值与规格一致");
  }

  // ---- R3：严格解析 ----
  test_support::Section("INC-RT19 3. 严格解析：坏文件绝不恢复成默认值");
  {
    const std::string work = test_support::FreshDir("realtime-store-strict");
    const std::string source = work + "/source";
    test_support::Mkdir(source, 0755);
    const std::string base = ConfigText(source);

    struct BadCase {
      std::string label;
      std::string text;
    };
    std::vector<BadCase> cases;
    cases.push_back({"未知 key", base.substr(0, base.rfind("}")) +
                                     "    ,\"unknown_key\": 1\n}\n"});
    cases.push_back(
        {"缺 key（删掉 encryption 行）",
         base.substr(0, base.find("    \"encryption\"")) +
             base.substr(base.find("\n", base.find("    \"encryption\"")) +
                         1)});
    cases.push_back({"重复 key", base.substr(0, base.rfind("}")) +
                                     "    ,\"debounce_ms\": 600\n}\n"});
    cases.push_back(
        {"类型不符（debounce 是字符串）",
         base.substr(0, base.find("\"debounce_ms\"")) +
             "\"debounce_ms\": \"500\",\n" +
             base.substr(base.find("\n", base.find("\"debounce_ms\"")) + 1)});
    cases.push_back({"debounce 太小",
                     base.substr(0, base.find("\"debounce_ms\": 500")) +
                         "\"debounce_ms\": 99" +
                         base.substr(base.find("\"debounce_ms\": 500") + 19)});
    cases.push_back({"debounce 太大",
                     base.substr(0, base.find("\"debounce_ms\": 500")) +
                         "\"debounce_ms\": 60001" +
                         base.substr(base.find("\"debounce_ms\": 500") + 19)});
    cases.push_back({"max_wait 小于 debounce",
                     base.substr(0, base.find("\"max_wait_ms\": 5000")) +
                         "\"max_wait_ms\": 200" +
                         base.substr(base.find("\"max_wait_ms\": 5000") + 19)});
    cases.push_back({"retain 太大",
                     base.substr(0, base.find("\"retain_count\": 12")) +
                         "\"retain_count\": 1001" +
                         base.substr(base.find("\"retain_count\": 12") + 19)});
    cases.push_back(
        {"trigger 不是 realtime",
         base.substr(0, base.find("\"trigger\": \"realtime\"")) +
             "\"trigger\": \"scheduled\"" +
             base.substr(base.find("\"trigger\": \"realtime\"") + 22)});
    cases.push_back(
        {"strategy 未知",
         base.substr(0, base.find("\"strategy\": \"full\"")) +
             "\"strategy\": \"bogus\"" +
             base.substr(base.find("\"strategy\": \"full\"") + 19)});
    cases.push_back({"非 JSON", "not json at all\n"});
    cases.push_back({"截断", base.substr(0, base.size() / 2)});

    for (const BadCase& item : cases) {
      const std::string path = work + "/bad.json";
      test_support::Check(test_support::WriteFile(path, item.text, 0644),
                          "INC-RT19 T3 写入坏样本: " + item.label);
      bp::RealtimeStore store(path);
      bp::RealtimeConfig config;
      std::string error;
      const bp::RealtimeLoadStatus status = store.Load(&config, &error);
      test_support::Check(status == bp::RealtimeLoadStatus::kError,
                          "INC-RT19 T3 判别：拒绝 " + item.label,
                          std::to_string(static_cast<int>(status)));
      test_support::Check(!error.empty(),
                          "INC-RT19 T3 拒绝时给出原因: " + item.label);
    }

    // 边界值必须被接受。
    for (const std::uint32_t value : {100u, 60000u}) {
      bp::RealtimeConfig config;
      config.source_path = source;
      config.debounce_ms = value;
      config.max_wait_ms = 300000;
      std::string error;
      test_support::Check(
          bp::ValidateRealtimeConfig(config, &error),
          "INC-RT19 T3 debounce 边界被接受: " + std::to_string(value), error);
    }
    bp::RealtimeConfig edge;
    edge.source_path = source;
    edge.retain_count = 1;
    std::string error;
    test_support::Check(bp::ValidateRealtimeConfig(edge, &error),
                        "INC-RT19 T3 retain=1 被接受", error);
    edge.retain_count = 1000;
    error.clear();
    test_support::Check(bp::ValidateRealtimeConfig(edge, &error),
                        "INC-RT19 T3 retain=1000 被接受", error);
  }

  // ---- R4：路径重叠 ----
  test_support::Section("INC-RT19 4. source / repository 重叠边界");
  {
    const std::string work = test_support::FreshDir("realtime-overlap");
    const std::string source = work + "/a";
    const std::string sibling = work + "/abc";
    const std::string repo_inside = source + "/repo";
    const std::string repo_sibling = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(sibling, 0755);
    test_support::Mkdir(repo_inside, 0755);
    test_support::Mkdir(repo_sibling, 0755);

    std::string detail;
    test_support::Check(bp::ClassifyPathOverlap(source, source, &detail) ==
                            bp::RealtimePathOverlap::kEqual,
                        "INC-RT19 T4 source == repository 被识别", detail);
    test_support::Check(bp::ClassifyPathOverlap(source, repo_inside, &detail) ==
                            bp::RealtimePathOverlap::kRepositoryInsideSource,
                        "INC-RT19 T4 repository 在 source 内被识别", detail);
    test_support::Check(bp::ClassifyPathOverlap(repo_inside, source, &detail) ==
                            bp::RealtimePathOverlap::kSourceInsideRepository,
                        "INC-RT19 T4 source 在 repository 内被识别", detail);
    test_support::Check(
        bp::ClassifyPathOverlap(source, repo_sibling, &detail) ==
            bp::RealtimePathOverlap::kNone,
        "INC-RT19 T4 兄弟目录不算重叠", detail);
    // /home/a 与 /home/abc：字符串前缀相同但不是祖孙关系。
    test_support::Check(bp::ClassifyPathOverlap(source, sibling, &detail) ==
                            bp::RealtimePathOverlap::kNone,
                        "INC-RT19 T4 判别：字符串前缀相同的兄弟目录不误判",
                        detail);

    // enable 校验：重叠、软链接 root、不存在、不是目录。
    std::string error;
    std::string identity;
    bp::RealtimeConfig config;
    config.source_path = source;
    error.clear();
    test_support::Check(
        !bp::ValidateRealtimeForEnable(config, repo_inside, &error, &identity),
        "INC-RT19 T4 enable 拒绝 repository 在 source 内", error);
    error.clear();
    test_support::Check(
        bp::ValidateRealtimeForEnable(config, repo_sibling, &error, &identity),
        "INC-RT19 T4 enable 接受不重叠的仓库", error);
    test_support::Check(!identity.empty(),
                        "INC-RT19 T4 enable 返回仓库 identity");

    const std::string link = work + "/link";
    test_support::Check(test_support::CreateSymlink(source, link),
                        "INC-RT19 T4 造 source 软链接");
    bp::RealtimeConfig linked;
    linked.source_path = link;
    error.clear();
    test_support::Check(
        !bp::ValidateRealtimeForEnable(linked, repo_sibling, &error, &identity),
        "INC-RT19 T4 enable 拒绝软链接 source", error);

    bp::RealtimeConfig missing;
    missing.source_path = work + "/does-not-exist";
    error.clear();
    test_support::Check(!bp::ValidateRealtimeForEnable(missing, repo_sibling,
                                                       &error, &identity),
                        "INC-RT19 T4 enable 拒绝不存在的 source", error);

    bp::RealtimeConfig file_source;
    test_support::WriteFile(work + "/file.txt", "x", 0644);
    file_source.source_path = work + "/file.txt";
    error.clear();
    test_support::Check(!bp::ValidateRealtimeForEnable(
                            file_source, repo_sibling, &error, &identity),
                        "INC-RT19 T4 enable 拒绝「不是目录」的 source", error);
  }

  // ---- R5：job identity ----
  test_support::Section("INC-RT19 5. job identity 对配置变化敏感");
  {
    bp::RealtimeConfig config;
    config.source_path = "/src";
    config.include_rules = {"ext:txt"};
    const std::string base =
        bp::RealtimeJobIdentityDigest(config, "/repo", "/src");
    test_support::Check(bp::IsContentDigest(base),
                        "INC-RT19 T5 job identity 是合法摘要");
    test_support::Check(
        bp::RealtimeJobIdentityDigest(config, "/repo", "/src") == base,
        "INC-RT19 T5 同一配置两次摘要相同");

    bp::RealtimeConfig other_strategy = config;
    other_strategy.strategy = bp::BackupStrategy::kIncremental;
    test_support::Check(
        bp::RealtimeJobIdentityDigest(other_strategy, "/repo", "/src") != base,
        "INC-RT19 T5 策略变化 -> 摘要变化");
    bp::RealtimeConfig other_pack = config;
    other_pack.pack_method = bp::PackMethod::kUstar;
    test_support::Check(
        bp::RealtimeJobIdentityDigest(other_pack, "/repo", "/src") != base,
        "INC-RT19 T5 pack 变化 -> 摘要变化");
    bp::RealtimeConfig other_rules = config;
    other_rules.exclude_rules = {"name:tmp*"};
    test_support::Check(
        bp::RealtimeJobIdentityDigest(other_rules, "/repo", "/src") != base,
        "INC-RT19 T5 规则变化 -> 摘要变化");
    test_support::Check(
        bp::RealtimeJobIdentityDigest(config, "/other-repo", "/src") != base,
        "INC-RT19 T5 仓库变化 -> 摘要变化");
    test_support::Check(
        bp::RealtimeJobIdentityDigest(config, "/repo", "/other-src") != base,
        "INC-RT19 T5 源变化 -> 摘要变化");
  }

  // ---- R6：规则编译（D）与 JSON 往返（E） ----
  test_support::Section("INC-RT19 6. 规则真的编译 + 写出去的读得回来");
  {
    const std::string work = test_support::FreshDir("realtime-store-closure");
    const std::string source = work + "/source";
    test_support::Mkdir(source, 0755);
    const std::string path = work + "/realtime.json";
    bp::RealtimeStore store(path);
    std::string error;

    // D：非法 DSL 必须在保存 / 启用之前就被拒绝，而不是等第一次触发。
    bp::RealtimeConfig invalid;
    invalid.source_path = source;
    invalid.include_rules = {"nonsense:xx"};
    error.clear();
    test_support::Check(
        !bp::ValidateRealtimeConfig(invalid, &error),
        "INC-RT19 T6 判别：非法规则在 ValidateRealtimeConfig 就被拒绝", error);
    test_support::Check(error.find("Invalid filter rule") != std::string::npos,
                        "INC-RT19 T6 拒绝理由来自共享 Filter", error);
    error.clear();
    test_support::Check(!store.Save(invalid, &error),
                        "INC-RT19 T6 判别：Save 也拒绝（同一份校验）", error);
    test_support::Check(!test_support::Exists(path),
                        "INC-RT19 T6 判别：被拒绝的配置一个字节都没落盘");

    // 同一个错误也要能从共享 helper 直接拿到（CLI / GUI 用的是它）。
    bp::Filter filter;
    error.clear();
    test_support::Check(
        !bp::BuildRealtimeFilter(invalid, &filter, &error) &&
            error.find("Invalid filter rule") != std::string::npos,
        "INC-RT19 T6 判别：BuildRealtimeFilter 与校验同一处裁决", error);

    // 合法规则照样通过（包含一个带 tab 的合法 name 规则：E 的往返素材）。
    bp::RealtimeConfig valid;
    valid.source_path = source;
    valid.include_rules = {"ext:txt"};
    valid.exclude_rules = {"name:skip*"};
    error.clear();
    test_support::Check(bp::ValidateRealtimeConfig(valid, &error),
                        "INC-RT19 T6 合法规则通过校验", error);

    // E：控制字符的 Save -> Load 往返。
    //
    // source_path 与规则都是"业务层允许、但 JSON 必须转义"的字符串：
    // tab / CR / BS / FF / 0x01。旧实现会写出 parser 明确拒绝的 raw C0，
    // 于是写成功能、读回失败。
    struct Case {
      const char* label;
      std::string value;
    };
    const std::vector<Case> cases = {
        {"tab(0x09)", std::string("tab\there")},
        {"CR(0x0d)", std::string("cr\rhere")},
        {"BS(0x08)", std::string("bs\bhere")},
        {"FF(0x0c)", std::string("ff\fhere")},
        {"SOH(0x01)", std::string("soh\x01here")},
        {"混合", std::string("mix\t\r\b\f\x01end")},
    };
    for (const Case& item : cases) {
      bp::RealtimeConfig config;
      config.source_path = source + "/" + item.value;
      config.include_rules = {"name:" + item.value};
      config.exclude_rules = {"ext:txt"};
      error.clear();
      if (!bp::ValidateRealtimeConfig(config, &error)) {
        test_support::Check(
            false,
            std::string("INC-RT19 T6 ") + item.label + " 是业务层接受的配置",
            error);
        continue;
      }
      error.clear();
      if (!store.Save(config, &error)) {
        test_support::Check(
            false, std::string("INC-RT19 T6 ") + item.label + " 保存", error);
        continue;
      }
      // 写出来的文件里不允许有 raw C0（parser 明确拒绝的那类字节）。
      std::string text;
      test_support::Check(test_support::ReadFile(path, &text),
                          std::string("INC-RT19 T6 ") + item.label + " 读文件");
      bool raw_control = false;
      for (const char character : text) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x20 && byte != '\n') raw_control = true;
      }
      test_support::Check(!raw_control, std::string("INC-RT19 T6 判别：") +
                                            item.label +
                                            " 写出来的 JSON 没有 raw C0 字节");
      bp::RealtimeConfig loaded;
      error.clear();
      const bp::RealtimeLoadStatus status = store.Load(&loaded, &error);
      test_support::Check(status == bp::RealtimeLoadStatus::kLoaded &&
                              loaded.source_path == config.source_path &&
                              loaded.include_rules == config.include_rules &&
                              loaded.exclude_rules == config.exclude_rules,
                          std::string("INC-RT19 T6 判别：") + item.label +
                              " Save -> Load 往返一致",
                          error);
    }
  }

  return test_support::Finish("realtime_store_test");
}

// tests/unit/server_profile_test.cpp
//
// ServerProfile 与 .bpserver 的测试（PR #23）。
//
// 这一层的价值全在"模式与字段必须自洽"：证书模式缺 server_id、pin 模式缺
// 指纹、两种模式同时配置 —— 每一条都必须在解析时就失败，而不是等到连接时
// 用一句"握手失败"收场。

#include <cstdio>
#include <string>

#include "remote_backup_client.h"
#include "server_profile.h"

namespace {

using backupproject::net::FormatBpserverProfile;
using backupproject::net::kIdentityCertificate;
using backupproject::net::kIdentityPin;
using backupproject::net::kIdentitySsh;
using backupproject::net::OfficialCloudProfile;
using backupproject::net::ParseBpserverProfile;
using backupproject::net::ServerProfile;
using backupproject::net::ToRemoteEndpoint;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const std::string& label, const std::string& detail = "") {
  if (ok) {
    ++g_passed;
    std::printf("[profile]   ok   %s\n", label.c_str());
    return;
  }
  ++g_failed;
  std::printf("[profile]   FAIL %s（%s）\n", label.c_str(), detail.c_str());
}

const char* kPin = "sha256:f491f0be36bc715d064d96766eaf96539b39737515deaff9390b20e0cb6c9b60";

}  // namespace

int main() {
  {
    const ServerProfile& official = OfficialCloudProfile();
    Check(official.display_name == "Backup Project Cloud",
          "P01 display_name = Backup Project Cloud", official.display_name);
    Check(official.host == "8.130.9.200", "P02 host 来自 ssh -G 的 HostName",
          official.host);
    Check(official.port == 18765, "P03 port = 18765");
    Check(official.identity == kIdentityCertificate,
          "P04 identity = certificate（签名身份）", official.identity);
    Check(official.expected_server_id == "backup-project-cloud-production",
          "P05 expected_server_id 正确", official.expected_server_id);
    Check(official.trusted_roots_file.empty(),
          "P06 trusted_roots 为空 = 用编译进二进制的官方根");
    Check(official.server_key_pin.empty() && official.ssh_target.empty(),
          "P07 官方云端不需要用户填任何指纹 / SSH 目标");
    Check(official.official_cloud, "P08 标记为官方云端");

    backupproject::net::RemoteEndpoint endpoint;
    std::string error;
    Check(ToRemoteEndpoint(official, &endpoint, &error) &&
              endpoint.identity_mode == "certificate" &&
              endpoint.trusted_roots_file.empty() &&
              endpoint.expected_server_id == official.expected_server_id,
          "P09 官方云端 -> 证书模式端点（内置根）", error);
  }

  {
    ServerProfile certificate;
    certificate.display_name = "my-server";
    certificate.host = "192.168.1.10";
    certificate.port = 18765;
    certificate.identity = kIdentityCertificate;
    certificate.expected_server_id = "my-server";
    certificate.trusted_roots_file = "/etc/backup-project/my-root.pub";

    ServerProfile pin = certificate;
    pin.identity = kIdentityPin;
    pin.expected_server_id.clear();
    pin.trusted_roots_file.clear();
    pin.server_key_pin = kPin;

    ServerProfile ssh = pin;
    ssh.identity = kIdentitySsh;
    ssh.ssh_target = "user@192.168.1.10";

    const ServerProfile all[3] = {certificate, pin, ssh};
    for (const ServerProfile& original : all) {
      ServerProfile parsed;
      std::string error;
      const std::string text = FormatBpserverProfile(original);
      const bool ok = ParseBpserverProfile(text, &parsed, &error);
      Check(ok && parsed.display_name == original.display_name &&
                parsed.host == original.host && parsed.port == original.port &&
                parsed.identity == original.identity &&
                parsed.expected_server_id == original.expected_server_id &&
                parsed.trusted_roots_file == original.trusted_roots_file &&
                parsed.server_key_pin == original.server_key_pin &&
                parsed.ssh_target == original.ssh_target,
            "P10 .bpserver 往返逐字段一致（" + original.identity + "）", error);
    }
  }

  {
    struct Case {
      const char* label;
      const char* text;
    };
    const Case cases[] = {
        {"P11 未知键必须报错",
         "display_name = x\nhost = h\nport = 18765\nidentity = certificate\n"
         "expected_server_id = s\nrubbish = 1\n"},
        {"P12 port 不合法必须报错",
         "display_name = x\nhost = h\nport = 70000\nidentity = certificate\n"
         "expected_server_id = s\n"},
        {"P13 证书模式缺 server_id 必须报错",
         "display_name = x\nhost = h\nport = 18765\nidentity = certificate\n"},
        {"P14 指纹模式缺 pin 必须报错",
         "display_name = x\nhost = h\nport = 18765\nidentity = pin\n"},
        {"P15 两种身份字段混配必须报错",
         "display_name = x\nhost = h\nport = 18765\nidentity = certificate\n"
         "expected_server_id = s\nserver_key_pin = sha256:00\n"},
        {"P16 ssh 模式缺 ssh_target 必须报错",
         "display_name = x\nhost = h\nport = 18765\nidentity = ssh\n"
         "server_key_pin = sha256:00\n"},
        {"P17 未知身份方式必须报错",
         "display_name = x\nhost = h\nport = 18765\nidentity = magic\n"},
        {"P18 不是 key = value 必须报错", "display_name x\nhost = h\n"},
        {"P19 缺 display_name 必须报错",
         "host = h\nport = 18765\nidentity = certificate\nexpected_server_id = s\n"},
    };
    for (const Case& item : cases) {
      ServerProfile parsed;
      std::string error;
      Check(!ParseBpserverProfile(item.text, &parsed, &error), item.label,
            "竟然通过了");
    }
  }

  {
    ServerProfile pin;
    pin.display_name = "pin-mode";
    pin.host = "10.0.0.5";
    pin.identity = kIdentityPin;
    pin.server_key_pin = kPin;
    backupproject::net::RemoteEndpoint endpoint;
    std::string error;
    Check(ToRemoteEndpoint(pin, &endpoint, &error) &&
              endpoint.identity_mode == "pin" &&
              endpoint.server_key_pin == kPin &&
              endpoint.expected_server_id.empty(),
          "P20 pin 模式 -> pin 端点", error);

    ServerProfile ssh = pin;
    ssh.identity = kIdentitySsh;
    ssh.ssh_target = "user@10.0.0.5";
    Check(ToRemoteEndpoint(ssh, &endpoint, &error) &&
              endpoint.identity_mode == "pin" &&
              endpoint.server_key_pin == kPin,
          "P21 ssh 模式在协议层仍是 pin（隧道由调用方建立）", error);
  }

  std::printf("[profile] passed=%d failed=%d\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// src/network/server_profile.cpp
#include "server_profile.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

#include "x25519.h"

namespace backupproject {
namespace net {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

std::string Trim(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                         text[begin] == '\r' || text[begin] == '\n')) {
    ++begin;
  }
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\r' || text[end - 1] == '\n')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool ParsePort(const std::string& text, std::uint16_t* out) {
  if (text.empty() || text.size() > 5) {
    return false;
  }
  std::uint32_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::uint32_t>(character - '0');
  }
  if (value == 0 || value > 65535) {
    return false;
  }
  *out = static_cast<std::uint16_t>(value);
  return true;
}

// 每种身份方式各自需要的字段。**模式与字段必须自洽**：证书模式缺 server_id、
// pin 模式缺指纹，都要在这里就报错，而不是等到连接时才以一句"握手失败"收场。
bool Validate(const ServerProfile& profile, std::string* error_message) {
  if (profile.display_name.empty()) {
    SetError(error_message, "profile 缺少 display_name");
    return false;
  }
  if (profile.host.empty()) {
    SetError(error_message, "profile 缺少 host");
    return false;
  }
  if (profile.port == 0) {
    SetError(error_message, "profile 的 port 不能是 0");
    return false;
  }
  if (profile.identity == kIdentityCertificate) {
    if (profile.expected_server_id.empty()) {
      SetError(
          error_message,
          "签名身份（certificate）必须给出 expected_server_id："
          "证书里必须出现这个名字，否则同一把根签发的另一台服务器也能冒充");
      return false;
    }
    if (!profile.server_key_pin.empty()) {
      SetError(error_message,
               "签名身份下不应该有 server_key_pin：证书与 pin 是两条互斥的路，"
               "同时配置会让人看不出到底在按哪条走");
      return false;
    }
  } else if (profile.identity == kIdentityPin ||
             profile.identity == kIdentitySsh) {
    if (profile.server_key_pin.empty()) {
      SetError(error_message, "指纹身份（pin / ssh）必须给出 server_key_pin");
      return false;
    }
    if (!profile.expected_server_id.empty() ||
        !profile.trusted_roots_file.empty()) {
      SetError(error_message,
               "指纹身份下不应该有 expected_server_id / trusted_roots："
               "它们是证书模式的字段");
      return false;
    }
  } else {
    SetError(error_message,
             "identity 只能是 certificate / pin / ssh，收到的是 " +
                 profile.identity);
    return false;
  }
  if (profile.identity == kIdentitySsh && profile.ssh_target.empty()) {
    SetError(error_message, "ssh 身份必须给出 ssh_target（user@host）");
    return false;
  }
  return true;
}

}  // namespace

const ServerProfile& OfficialCloudProfile() {
  static const ServerProfile profile = [] {
    ServerProfile value;
    value.display_name = kOfficialCloudDisplayName;
    value.host = kOfficialCloudHost;
    value.port = kOfficialCloudPort;
    value.identity = kIdentityCertificate;
    value.expected_server_id = kOfficialCloudServerId;
    // trusted_roots_file 留空：用编译进二进制的官方根。官方云端**不接受**
    // 任何本地根文件，否则"在旁边放一个根"就能改信任。
    value.trusted_roots_file.clear();
    value.official_cloud = true;
    return value;
  }();
  return profile;
}

bool ParseBpserverProfile(const std::string& text, ServerProfile* out,
                          std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  ServerProfile profile;
  std::istringstream stream(text);
  std::string line;
  int line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    line = Trim(line);
    if (line.empty()) {
      continue;
    }
    const std::size_t equal = line.find('=');
    if (equal == std::string::npos) {
      SetError(error_message, "第 " + std::to_string(line_number) +
                                  " 行不是 key = value 形式");
      return false;
    }
    const std::string key = Trim(line.substr(0, equal));
    const std::string value = Trim(line.substr(equal + 1));
    if (key == "display_name") {
      profile.display_name = value;
    } else if (key == "host") {
      profile.host = value;
    } else if (key == "port") {
      if (!ParsePort(value, &profile.port)) {
        SetError(error_message, "第 " + std::to_string(line_number) +
                                    " 行的 port 不合法：" + value);
        return false;
      }
    } else if (key == "identity") {
      profile.identity = value;
    } else if (key == "expected_server_id") {
      profile.expected_server_id = value;
    } else if (key == "trusted_roots") {
      profile.trusted_roots_file = value;
    } else if (key == "server_key_pin") {
      profile.server_key_pin = value;
    } else if (key == "ssh_target") {
      profile.ssh_target = value;
    } else {
      // 未知键一律报错：拼错的键名如果被忽略，用户会以为配置生效了。
      SetError(error_message,
               "第 " + std::to_string(line_number) + " 行是未知的键：" + key);
      return false;
    }
  }
  if (!Validate(profile, error_message)) {
    return false;
  }
  *out = profile;
  return true;
}

bool LoadBpserverProfileFile(const std::string& path, ServerProfile* out,
                             std::string* error_message) {
  std::ifstream input(path);
  if (!input) {
    SetError(error_message, "打不开 .bpserver 文件：" + path);
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return ParseBpserverProfile(buffer.str(), out, error_message);
}

std::string FormatBpserverProfile(const ServerProfile& profile) {
  std::ostringstream out;
  out << "# backup-project 服务器描述文件（.bpserver）\n";
  out << "display_name = " << profile.display_name << "\n";
  out << "host = " << profile.host << "\n";
  out << "port = " << profile.port << "\n";
  out << "identity = " << profile.identity << "\n";
  if (!profile.expected_server_id.empty()) {
    out << "expected_server_id = " << profile.expected_server_id << "\n";
  }
  if (!profile.trusted_roots_file.empty()) {
    out << "trusted_roots = " << profile.trusted_roots_file << "\n";
  }
  if (!profile.server_key_pin.empty()) {
    out << "server_key_pin = " << profile.server_key_pin << "\n";
  }
  if (!profile.ssh_target.empty()) {
    out << "ssh_target = " << profile.ssh_target << "\n";
  }
  return out.str();
}

bool ToRemoteEndpoint(const ServerProfile& profile, RemoteEndpoint* out,
                      std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  if (!Validate(profile, error_message)) {
    return false;
  }
  RemoteEndpoint endpoint;
  endpoint.host = profile.host;
  endpoint.port = profile.port;
  if (profile.identity == kIdentityCertificate) {
    endpoint.identity_mode = "certificate";
    endpoint.expected_server_id = profile.expected_server_id;
    endpoint.trusted_roots_file = profile.trusted_roots_file;
  } else {
    // pin 与 ssh 在协议层是同一条路（BPSEC1 + 人工指纹）；区别只在传输层：
    // ssh 方式的 host/port 由调用方改成隧道本地端口。
    endpoint.identity_mode = "pin";
    endpoint.server_key_pin = profile.server_key_pin;
  }
  *out = endpoint;
  return true;
}

}  // namespace net
}  // namespace backupproject

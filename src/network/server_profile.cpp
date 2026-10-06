// src/network/server_profile.cpp
// 本文件把"要连哪台服务器、怎么确认它是它"翻译成网络层能用的 RemoteEndpoint，
// 并负责 .bpserver 文本格式的读写。
//
// 唯一性：三种身份方式（certificate / pin / ssh）到协议字段的映射**只有**
// ToRemoteEndpoint 一处。界面、CLI、调度器都从这里取端点 —— 任何"顺手在
// 调用方拼一个 endpoint"的做法都会重新引入半配置状态。
//
// 信任边界：.bpserver 是用户可写的普通文件，一律按不可信输入解析。未知键、
// 非法 port、模式与字段不自洽，全部在解析阶段报错，绝不"忽略掉继续跑"。
//
// 失败语义：所有入口返回 bool 并把英文诊断写进 error_message（允许为
// nullptr）；失败时 *out 不被修改，调用方拿不到半成品。
#include "server_profile.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

#include "x25519.h"

namespace backupproject {
namespace net {
namespace {

// error_message 允许为空；所有失败路径都经过这里，保证诊断不会丢。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 只去首尾空白（含 \r，也就是 CR，让 CRLF 文件也能读）：不折叠大小写、
// 也不删中间空白，因为值本身可能是带空格的路径。
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

// 端口必须是纯十进制、无符号、落在 1..65535：0 会被内核解释成"随机端口"，
// 对一个要写进配置文件的描述来说那是错的。先卡长度上限，避免溢出。
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
// 模式自洽性是这一层唯一的不变量，两个方向都要查：
//   * 该有的字段必须有 —— certificate 缺 expected_server_id，等于放弃
//     "证书里必须是这台服务器"的校验，同一把根签发的另一台就能冒充；
//   * 不该有的字段必须没有 —— 证书模式下同时配 pin，没人说得清在按哪条走。
// 返回 false 时调用方不得继续，这里不存在"降级到能用为止"的分支。
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

// 官方云端 profile 在进程内只构造一次，之后只读返回引用（函数内 static 的
// 初始化自 C++11 起是线程安全的）。调用方拿到的是 const 引用，想改也改不了
// —— 这正是"官方配置不允许被运行时改写"的实现方式。
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

// 解析 .bpserver 文本。逐行处理，遇到第一个错误立刻返回，不做"尽力解析"：
//   * '#' 起注释，先截注释再 Trim，所以行尾注释也能吃掉；
//   * 空行跳过，非空行必须是 key = value；
//   * 未知键直接报错 —— 拼错的键如果被忽略，用户会以为配置生效了；
//   * 没有重复键检测，后出现的同名键覆盖先出现的（文件是用户手写的，
//     这里保持"后写为准"的直觉行为）。
// 语法全过之后统一走 Validate()：能解析不等于配置自洽。成功时只在最后
// 一次性写 *out，中途失败不会留下半成品 profile。
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

// 文件路径只是"读全文再转发"：打不开文件与内容不合法是两种不同的诊断。
// 本函数不检查权限位、不检查扩展名，也不做路径规范化。
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

// 与 ParseBpserverProfile 严格互逆：空字段一律不输出，于是"读进来再写出去"
// 不会凭空多出 identity 用不到的键（那些键会被 Validate 判为不自洽）。
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

// 唯一的翻译点。pin 与 ssh 在协议层是同一条路（BPSEC1 + 人工指纹），区别只
// 在传输层，所以 ssh 方式只填端点与指纹，host/port 由调用方在隧道建好后改成
// 隧道本地端口。这里再 Validate 一次：调用方可能绕过 Parse 直接填一个
// ServerProfile，端点工厂必须自己成立，不能假设上游检查过了。
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

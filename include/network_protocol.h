// include/network_protocol.h
//
// PR #20：BPNET1 —— 远程备份的线上协议。
//
// 这一层只负责"把结构化消息变成字节、再从字节变回来"，不碰备份语义：
// 它不知道什么是归档、什么是增量链，只知道帧、字段和错误码。
//
// 三条硬规则（每一条都有对应的单元测试）：
//
//   1. 显式大端。所有整数手工逐字节拼接，绝不 write(fd, &struct, sizeof)。
//      结构体布局受 padding / 对齐 / 主机字节序影响，换个编译器就变。
//   2. 有界。每个**客户端给出的**长度（用户名、显示名、token、payload、
//      文件总大小）都先过上限检查再谈分配。协议里不存在
//      "payload_length = 0xFFFFFFFFFFFFFFFF -> resize()" 这条路径。
//   3. 帧边界明确。先在内存里凑齐 32 字节帧头，再按声明的长度收 payload；
//      读满才算一帧，绝不靠 recv() 的偶然切分来猜消息边界。
//
// 帧头固定 32 字节，全部大端：
//
//   偏移  长度  字段
//   0     4     magic "BPN1"
//   4     2     version（当前 1）
//   6     2     opcode
//   8     2     flags（保留，当前必须为 0）
//   10    2     reserved（必须为 0）
//   12    4     status（响应里的错误码，请求里为 0）
//   16    8     request_id（客户端生成，响应原样回填）
//   24    8     payload_length（<= kMaxPayloadBytes）
//
// 控制帧与数据块的 payload 上限是同一个 1 MiB：数据块每块也 <= 1 MiB，
// 所以"一个帧的 payload 不超过 1 MiB"这一条就能同时约束两者。

#ifndef BACKUP_PROJECT_INCLUDE_NETWORK_PROTOCOL_H_
#define BACKUP_PROJECT_INCLUDE_NETWORK_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace backupproject {
namespace net {

// ---- 协议常量 ----

// "BPN1"：0x42 0x50 0x4E 0x31。
inline constexpr std::uint32_t kProtocolMagic = 0x42504E31u;
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderSize = 32;
// 单个帧 payload 的上限（控制帧与文件块共用）。
inline constexpr std::uint64_t kMaxPayloadBytes = 1024ull * 1024ull;

// 字段上限。客户端给的每个长度都要先过这里，再谈分配。
inline constexpr std::size_t kMinUsernameBytes = 3;
inline constexpr std::size_t kMaxUsernameBytes = 64;
inline constexpr std::size_t kMaxPasswordBytes = 256;
inline constexpr std::size_t kMaxDisplayNameBytes = 255;
inline constexpr std::size_t kMaxSnapshotIdBytes = 64;
inline constexpr std::size_t kMaxTokenBytes = 512;
inline constexpr std::size_t kSha256HexBytes = 64;
// 一个 LIST 响应里最多多少条（防止一帧被撑爆）。
inline constexpr std::uint32_t kMaxListEntries = 4096;

// 服务端默认的单次上传上限，可用 --max-upload-bytes 调整。
// 一个文件块的大小。客户端用它切文件，服务端用它读磁盘；两边都远小于
// 1 MiB 的帧上限，因此"整份归档进内存"这条路径不存在。
inline constexpr std::size_t kTransferChunkBytes = 256u * 1024u;

inline constexpr std::uint64_t kDefaultMaxUploadBytes =
    8ull * 1024ull * 1024ull * 1024ull;

// ---- 操作码 ----

enum class Opcode : std::uint16_t {
  kPing = 1,
  kRegister = 2,
  kLogin = 3,
  kLogout = 4,
  // 恢复会话：在一**条新的** TCP 连接上用 token 重新认证。
  //
  // 为什么需要它：token 是 12 小时有效的无状态签名凭据，而 TCP 连接的生命周期
  // 短得多（服务端会在 io_timeout 之后主动关掉空闲连接，隧道重启也会断）。
  // 没有这个操作码，一次空闲超时就会把用户"踢下线"——而 token 其实还好好的。
  // 有了它：连接断了就重连再用 token 认证，用户无感；token 真的无效（过期、
  // 账户已注销）时服务端明确回 UNAUTHORIZED。
  //
  // 载荷：token（u16 前缀字符串）。目标账户来自 token 自己的签名内容，
  // 客户端无法指定"恢复成谁"。
  kResume = 5,
  kList = 10,
  kUploadBegin = 20,
  kUploadChunk = 21,
  kUploadEnd = 22,
  kDownloadBegin = 30,
  kDownloadChunk = 31,
  kDownloadEnd = 32,
  kDelete = 40,
  // 注销账户：删除调用方**自己的**账户以及它的全部云端数据。
  //
  // 它不是 LOGOUT：LOGOUT 只结束这条连接上的会话，账户与数据都还在。
  // 载荷是当前口令（u16 前缀字符串），服务端重新校验一次——一个过期或被
  // 偷到的 token 不足以删掉一个账户。请求里**没有**用户名：目标永远是
  // token 自己所属的那个 user id，客户端无法指定删别人。
  kDeleteAccount = 41,
  // 请求帧本身非法（版本不对、操作码不认识）时的通用错误响应。
  kError = 0xFFFE,
};

// ---- 统一错误码 ----
//
// 服务端只回这些码，绝不把 errno 或路径原样发给客户端：errno 与绝对路径
// 本身就能泄漏服务端的目录结构。内部原因只写服务端日志。

enum class Status : std::uint32_t {
  kOk = 0,
  kInvalidRequest = 1,
  kUnauthorized = 2,
  kForbidden = 3,
  kNotFound = 4,
  kAlreadyExists = 5,
  kInvalidState = 6,
  kTooLarge = 7,
  kIntegrityMismatch = 8,
  kInternalError = 9,
  kUnsupportedVersion = 10,
  kMalformedFrame = 11,
  // 该操作码在当前构建里还没有实现。PR20 是分阶段提交的：第一个 commit
  // 只有协议、帧循环与 PING，之后的 commit 才把其余操作码一个个接上。
  // 在那之前服务端如实回答"不支持"，不假装成功。
  kUnsupported = 12,
};

// ---- 帧头 ----

struct FrameHeader {
  std::uint16_t version = kProtocolVersion;
  std::uint16_t opcode = 0;
  std::uint16_t flags = 0;
  std::uint16_t reserved = 0;
  std::uint32_t status = 0;
  std::uint64_t request_id = 0;
  std::uint64_t payload_length = 0;
};

// 手工拼 32 字节帧头（大端）。绝不 reinterpret_cast 结构体。
std::string EncodeFrameHeader(const FrameHeader& header);

// 解 32 字节帧头。长度不足、magic 不对、version 不认识、flags/reserved
// 非 0、payload 超上限，都返回 false 并把原因写进 error_message。
bool DecodeFrameHeader(const unsigned char* data, std::size_t size,
                       FrameHeader* header, std::string* error_message);

// 尽力解析帧头字段（不做任何校验）。帧头非法时错误响应仍然需要回填
// request_id 与 opcode，否则客户端对不上是哪条请求失败了。
void DecodeFrameHeaderFields(const unsigned char* data, std::size_t size,
                             FrameHeader* header);

bool IsKnownOpcode(std::uint16_t opcode);
// 人类可读的名字，日志与测试断言共用；不认识时返回 "unknown"。
const char* OpcodeName(std::uint16_t opcode);
const char* StatusName(std::uint32_t status);
// 该操作码是否允许空 payload（PING / LOGOUT / UPLOAD_END 等）。
bool OpcodeAllowsEmptyPayload(std::uint16_t opcode);

// ---- payload 构造 ----

class PayloadBuilder {
 public:
  void AppendU8(std::uint8_t value);
  void AppendU16(std::uint16_t value);
  void AppendU32(std::uint32_t value);
  void AppendU64(std::uint64_t value);
  // u16 长度前缀 + 原始字节。超过 max_bytes 时不写入并返回 false。
  bool AppendString(const std::string& value, std::size_t max_bytes,
                    std::string* error_message);
  void AppendBytes(const void* data, std::size_t size);

  const std::string& data() const { return data_; }
  std::string* mutable_data() { return &data_; }
  std::size_t size() const { return data_.size(); }

 private:
  std::string data_;
};

// ---- payload 解析 ----
//
// 读侧一律"先看还剩多少，再取"。任何越界读取都返回 false 并把游标留在原地，
// 因此解析失败之后对象仍然可以安全析构，不会读越界内存。

class PayloadReader {
 public:
  explicit PayloadReader(const std::string& payload);

  bool ReadU8(std::uint8_t* out);
  bool ReadU16(std::uint16_t* out);
  bool ReadU32(std::uint32_t* out);
  bool ReadU64(std::uint64_t* out);
  // 读 u16 长度前缀的字符串。长度超过 max_bytes 时失败（**不分配**）。
  bool ReadString(std::size_t max_bytes, std::string* out);
  bool ReadBytes(std::size_t count, std::string* out);

  bool AtEnd() const { return cursor_ == payload_.size(); }
  std::size_t remaining() const { return payload_.size() - cursor_; }
  const std::string& error_message() const { return error_message_; }

 private:
  bool Fail(const std::string& reason);
  bool Require(std::size_t count);

  const std::string& payload_;
  std::size_t cursor_ = 0;
  std::string error_message_;
};

// ---- 定长收发 ----
//
// 处理 EINTR 与短读写。closed_by_peer 只在"一个字节都没读到就 EOF"时为 true；
// 读到一半才断开是错误，不是正常关闭。

bool SendAll(int fd, const void* data, std::size_t size,
             std::string* error_message);
bool ReceiveAll(int fd, void* data, std::size_t size, bool* closed_by_peer,
                std::string* error_message);

// 发一帧（帧头 + payload）。payload 超上限时直接失败，不发送半个帧。
bool SendFrame(int fd, std::uint16_t opcode, std::uint32_t status,
               std::uint64_t request_id, const std::string& payload,
               std::string* error_message);

// 读一帧的结果分类。调用方据此决定"回错误帧后继续"还是"断开连接"。
enum class FrameReadStatus {
  kOk,
  // 对端干净地关闭了连接（还没开始读帧头）。
  kClosed,
  // 帧头合法但内容不被接受（版本、操作码、flags）：流位置仍然完好，
  // 可以回一个错误帧后继续服务。
  kInvalidFrame,
  // magic 不对或 payload 长度超限：流已经失去同步（或根本无法安全丢弃），
  // 只能断开连接。
  kCorruptStream,
  kIoError,
};

FrameReadStatus ReceiveFrame(int fd, FrameHeader* header, std::string* payload,
                             std::string* error_message);

// ---- 字段校验 ----

// 用户名：3..64 字节，只允许 [A-Za-z0-9_.-]，且不能是 "." 或 ".."。
// 用户名永远不会被拼进文件系统路径（磁盘上一律用数字 user id），
// 这条校验是为了让日志、SQL 与显示都只面对一个很小的字符集。
bool IsValidUsername(const std::string& username, std::string* error_message);
// 密码：1..256 字节，不允许内嵌 NUL。空密码在这里就被拒绝。
bool IsValidPassword(const std::string& password, std::string* error_message);
// 显示名：1..255 字节，不允许 NUL 与控制字符。
// 允许含 '/' 与 '..'——它**只**进 SQLite 的 metadata，永远不参与路径拼接。
bool IsValidDisplayName(const std::string& name, std::string* error_message);
// snapshot id：32 个小写十六进制字符（16 字节随机数）。
bool IsValidSnapshotId(const std::string& snapshot_id,
                       std::string* error_message);
bool IsValidSha256Hex(const std::string& hex, std::string* error_message);

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_NETWORK_PROTOCOL_H_

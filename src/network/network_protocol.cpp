// src/network/network_protocol.cpp
//
// BPNET1 的编解码实现。见 include/network_protocol.h 里的协议说明。
//
// 这个文件里没有任何"按主机字节序直接读结构体"的捷径：所有多字节整数都是
// 手工移位拼出来的，因此把小端机器上的字节流喂给大端机器解析也不会读错。

// 职责：BPNET1 帧的编解码与字段校验。输入全部来自**不可信网络**，
// 每个字段都在分配内存或写进结构体之前先做边界与取值检查；失败返回 false
// 或错误分类，不抛异常，也不把半截结果留给调用方。
//
// 边界：本层不持有 socket、不 connect/accept/close、
// 不做认证，也不理解备份语义（归档、增量链、快照目录都在这一层之外）。
// 超时策略、重试与连接生命周期归调用方。
//
// 数据流：发送侧 PayloadBuilder 追加字段 -> SendFrame
// 拼 32 字节大端帧头 -> SendAll 循环写满；接收侧
// ReceiveAll收满帧头 -> DecodeFrameHeader 校验 ->
// 按 payload_length 收满 payload ->
// PayloadReader 顺序取字段。
//
// 不变量：帧头字段顺序即线上布局；payload 长度一定在分配**之前**过完
// kMaxPayloadBytes；PayloadReader 的
// cursor_ 永不超过 payload_.size()。
//
// 失败语义：帧只发出去一半就失去帧同步，调用方必须断开连接，不能靠重发补救。
#include "network_protocol.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

namespace backupproject {
namespace net {
namespace {

// ---- 大端原语 ----
//
// byte_order.h 里那套是归档格式用的 little-endian；线上协议固定大端，
// 两者不能混用，所以这里单独实现，绝不共享。

// 下面六个原语是本文件仅有的字节序出入口：只做位运算与逐字节存取，
// 不依赖主机对齐、不做 reinterpret_cast。
//
// 它们不做边界检查——调用方负责保证指针处至少有 2/4/8 字节可读。
void AppendU16BE(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xFFu));
  out->push_back(static_cast<char>(value & 0xFFu));
}

void AppendU32BE(std::string* out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void AppendU64BE(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

std::uint16_t LoadU16BE(const unsigned char* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(data[0]) << 8) |
                                    static_cast<std::uint32_t>(data[1]));
}

std::uint32_t LoadU32BE(const unsigned char* data) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value = (value << 8) | static_cast<std::uint32_t>(data[index]);
  }
  return value;
}

std::uint64_t LoadU64BE(const unsigned char* data) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<std::uint64_t>(data[index]);
  }
  return value;
}

// 只接受**小写**十六进制。快照 id、sha256、lineage
// 都是按字节相等比较的键（进 SQLite、进日志、跨端比对），
// 大小写混用会让同一个值出现两种表示；在入口拒绝大写比在每个比较点做 case
// fold 更难写错。
bool IsLowerHex(const std::string& value, std::size_t expected) {
  if (value.size() != expected) {
    return false;
  }
  for (const char character : value) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ---- 帧头 ----

// 写出的八个字段顺序 == 帧头线上布局，改顺序等于改协议：对端按固定偏移读，
// 读到的就是错的值。新增字段只能追加到 32 字节之后并提升 version，
// 同时要改头文件里的偏移表。
std::string EncodeFrameHeader(const FrameHeader& header) {
  std::string out;
  out.reserve(kFrameHeaderSize);
  AppendU32BE(&out, kProtocolMagic);
  AppendU16BE(&out, header.version);
  AppendU16BE(&out, header.opcode);
  AppendU16BE(&out, header.flags);
  AppendU16BE(&out, header.reserved);
  AppendU32BE(&out, header.status);
  AppendU64BE(&out, header.request_id);
  AppendU64BE(&out, header.payload_length);
  return out;
}

// 校验顺序是有意安排的：空指针 -> 帧头长度 -> magic -> version
// -> flags -> reserved -> payload 上限。magic
// 不对时后面的字节没有意义，先拒绝可以避免拿垃圾去比版本。
//
// 后置条件：成功时 *header 是完整结果，失败时 *header
// 一个字节都不写（要么全写、要么不写）。error_message
// 只给人看日志，调用方要分支请用 ReceiveFrame 的
// FrameReadStatus。
//
// payload 上限检查在**任何分配之前**，这里是挡住“payload 有
// 2^64-1 字节”的地方。
bool DecodeFrameHeader(const unsigned char* data, std::size_t size,
                       FrameHeader* header, std::string* error_message) {
  if (data == nullptr || header == nullptr) {
    if (error_message != nullptr) {
      *error_message = "frame header buffer is null";
    }
    return false;
  }
  if (size < kFrameHeaderSize) {
    if (error_message != nullptr) {
      *error_message = "frame header is shorter than 32 bytes";
    }
    return false;
  }
  if (LoadU32BE(data) != kProtocolMagic) {
    if (error_message != nullptr) {
      *error_message = "frame magic is not BPN1";
    }
    return false;
  }
  FrameHeader parsed;
  parsed.version = LoadU16BE(data + 4);
  parsed.opcode = LoadU16BE(data + 6);
  parsed.flags = LoadU16BE(data + 8);
  parsed.reserved = LoadU16BE(data + 10);
  parsed.status = LoadU32BE(data + 12);
  parsed.request_id = LoadU64BE(data + 16);
  parsed.payload_length = LoadU64BE(data + 24);

  if (parsed.version != kProtocolVersion) {
    if (error_message != nullptr) {
      *error_message = "unsupported protocol version";
    }
    return false;
  }
  if (parsed.flags != 0) {
    if (error_message != nullptr) {
      *error_message = "frame flags must be zero";
    }
    return false;
  }
  if (parsed.reserved != 0) {
    if (error_message != nullptr) {
      *error_message = "frame reserved field must be zero";
    }
    return false;
  }
  // 长度检查在**任何分配之前**：这里就是"客户端说 payload 有 2^64-1 字节"
  // 被挡住的地方。
  if (parsed.payload_length > kMaxPayloadBytes) {
    if (error_message != nullptr) {
      *error_message = "frame payload length exceeds the 1 MiB limit";
    }
    return false;
  }
  *header = parsed;
  return true;
}

// 与 DecodeFrameHeader 的区别：这里不做任何校验，
// magic/version/flags 照读。
// 存在的理由是错误路径——收到不认识的帧时仍要回一个带 request_id
// 的错误帧，否则客户端对不上是哪条请求失败了。长度不足直接返回，不碰 out。
void DecodeFrameHeaderFields(const unsigned char* data, std::size_t size,
                             FrameHeader* header) {
  if (data == nullptr || header == nullptr || size < kFrameHeaderSize) {
    return;
  }
  header->version = LoadU16BE(data + 4);
  header->opcode = LoadU16BE(data + 6);
  header->flags = LoadU16BE(data + 8);
  header->reserved = LoadU16BE(data + 10);
  header->status = LoadU32BE(data + 12);
  header->request_id = LoadU64BE(data + 16);
  header->payload_length = LoadU64BE(data + 24);
}

// ---- 名字表 ----

bool IsKnownSnapshotKind(std::uint16_t kind) {
  return kind == static_cast<std::uint16_t>(SnapshotKind::kFull) ||
         kind == static_cast<std::uint16_t>(SnapshotKind::kIncremental);
}

bool IsKnownOpcode(std::uint16_t opcode) {
  switch (static_cast<Opcode>(opcode)) {
    case Opcode::kPing:
    case Opcode::kRegister:
    case Opcode::kLogin:
    case Opcode::kLogout:
    case Opcode::kResume:
    case Opcode::kList:
    case Opcode::kUploadBegin:
    case Opcode::kUploadChunk:
    case Opcode::kUploadEnd:
    case Opcode::kDownloadBegin:
    case Opcode::kDownloadChunk:
    case Opcode::kDownloadEnd:
    case Opcode::kDelete:
    case Opcode::kDeleteAccount:
    case Opcode::kError:
      return true;
  }
  return false;
}

const char* OpcodeName(std::uint16_t opcode) {
  switch (static_cast<Opcode>(opcode)) {
    case Opcode::kPing:
      return "PING";
    case Opcode::kRegister:
      return "REGISTER";
    case Opcode::kLogin:
      return "LOGIN";
    case Opcode::kLogout:
      return "LOGOUT";
    case Opcode::kResume:
      return "RESUME";
    case Opcode::kList:
      return "LIST";
    case Opcode::kUploadBegin:
      return "UPLOAD_BEGIN";
    case Opcode::kUploadChunk:
      return "UPLOAD_CHUNK";
    case Opcode::kUploadEnd:
      return "UPLOAD_END";
    case Opcode::kDownloadBegin:
      return "DOWNLOAD_BEGIN";
    case Opcode::kDownloadChunk:
      return "DOWNLOAD_CHUNK";
    case Opcode::kDownloadEnd:
      return "DOWNLOAD_END";
    case Opcode::kDelete:
      return "DELETE";
    case Opcode::kDeleteAccount:
      return "DELETE_ACCOUNT";
    case Opcode::kError:
      return "ERROR";
  }
  return "UNKNOWN";
}

const char* StatusName(std::uint32_t status) {
  switch (static_cast<Status>(status)) {
    case Status::kOk:
      return "OK";
    case Status::kInvalidRequest:
      return "INVALID_REQUEST";
    case Status::kUnauthorized:
      return "UNAUTHORIZED";
    case Status::kForbidden:
      return "FORBIDDEN";
    case Status::kNotFound:
      return "NOT_FOUND";
    case Status::kAlreadyExists:
      return "ALREADY_EXISTS";
    case Status::kInvalidState:
      return "INVALID_STATE";
    case Status::kTooLarge:
      return "TOO_LARGE";
    case Status::kIntegrityMismatch:
      return "INTEGRITY_MISMATCH";
    case Status::kInternalError:
      return "INTERNAL_ERROR";
    case Status::kUnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case Status::kMalformedFrame:
      return "MALFORMED_FRAME";
    case Status::kUnsupported:
      return "UNSUPPORTED";
    case Status::kChainConflict:
      return "CHAIN_CONFLICT";
  }
  return "UNKNOWN_STATUS";
}

// 语义是“该操作码允许空 payload”，不是“必须为空”：
// UPLOAD_END 收尾时没有 body，DOWNLOAD_CHUNK 用空
// payload 表示流结束。列表之外的操怍码收到空 payload 一律拒绝。
bool OpcodeAllowsEmptyPayload(std::uint16_t opcode) {
  switch (static_cast<Opcode>(opcode)) {
    case Opcode::kPing:
    case Opcode::kLogout:
    case Opcode::kList:
    case Opcode::kUploadEnd:
    case Opcode::kDownloadChunk:
    case Opcode::kDownloadEnd:
      return true;
    default:
      return false;
  }
}

// ---- payload 构造 ----

void PayloadBuilder::AppendU8(std::uint8_t value) {
  data_.push_back(static_cast<char>(value & 0xFFu));
}

void PayloadBuilder::AppendU16(std::uint16_t value) {
  AppendU16BE(&data_, value);
}

void PayloadBuilder::AppendU32(std::uint32_t value) {
  AppendU32BE(&data_, value);
}

void PayloadBuilder::AppendU64(std::uint64_t value) {
  AppendU64BE(&data_, value);
}

// 编码为 u16 长度前缀 + 原始字节，不补 NUL、不做编码转换。
// 两个上限都要查：max_bytes 是调用方给的字段级上限，0xFFFF 是
// u16 前缀本身的表示能力——漏掉后者会静默截断长度，
// 接收侧于是按错长度切分整个 payload。超限时不写入任何字节。
bool PayloadBuilder::AppendString(const std::string& value,
                                  std::size_t max_bytes,
                                  std::string* error_message) {
  if (value.size() > max_bytes || value.size() > 0xFFFFu) {
    if (error_message != nullptr) {
      *error_message = "string field exceeds its maximum length";
    }
    return false;
  }
  AppendU16BE(&data_, static_cast<std::uint16_t>(value.size()));
  data_.append(value);
  return true;
}

void PayloadBuilder::AppendBytes(const void* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  data_.append(static_cast<const char*>(data), size);
}

// ---- payload 解析 ----

PayloadReader::PayloadReader(const std::string& payload) : payload_(payload) {}

// 只记录**第一条**错误：后续失败往往只是同一次越界的连锁反应，
// 覆盖成最后一条会把真正的根因丢掉。返回值恒为 false，
// 是为了让调用点统一写成 return Fail(...)。
bool PayloadReader::Fail(const std::string& reason) {
  if (error_message_.empty()) {
    error_message_ = reason;
  }
  return false;
}

// 不变量：cursor_ <= payload_.size()（每次读取都先
// Require 再推进游标），所以这里的减法不会下溢。count
// 可能来自帧内的长度前缀，是攻击者可控的值，必须在这里挡住声明长度超过剩余
// payload 的情况。
bool PayloadReader::Require(std::size_t count) {
  if (count > payload_.size() - cursor_) {
    return Fail("payload ended before the declared field was complete");
  }
  return true;
}

// 四个定长读取函数的契约一致：失败时返回 false，**不修改 *out、
// 不移动游标**，所以解析中途出错后对象仍可安全析构。out
// 为空指针算调用方的编程错误，走同一条失败路径而不是崩溃。
bool PayloadReader::ReadU8(std::uint8_t* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(1)) {
    return false;
  }
  *out = static_cast<std::uint8_t>(payload_[cursor_]);
  cursor_ += 1;
  return true;
}

bool PayloadReader::ReadU16(std::uint16_t* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(2)) {
    return false;
  }
  *out = LoadU16BE(reinterpret_cast<const unsigned char*>(payload_.data()) +
                   cursor_);
  cursor_ += 2;
  return true;
}

bool PayloadReader::ReadU32(std::uint32_t* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(4)) {
    return false;
  }
  *out = LoadU32BE(reinterpret_cast<const unsigned char*>(payload_.data()) +
                   cursor_);
  cursor_ += 4;
  return true;
}

bool PayloadReader::ReadU64(std::uint64_t* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(8)) {
    return false;
  }
  *out = LoadU64BE(reinterpret_cast<const unsigned char*>(payload_.data()) +
                   cursor_);
  cursor_ += 8;
  return true;
}

// 顺序不能换：先读长度前缀，再比 max_bytes，最后才 Require +
// assign。若先分配再检查，一个 length = 65535
// 的字段就足以让服务端按客户端报出的尺寸申请内存。
//
// assign(payload_, cursor_, length) 只拷
// length 字节，不做 NUL 截断，字符串里的 '\0' 原样保留。
bool PayloadReader::ReadString(std::size_t max_bytes, std::string* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  std::uint16_t length = 0;
  if (!ReadU16(&length)) {
    return false;
  }
  // 先比上限再分配：length 是客户端给的，不能直接 resize。
  if (length > max_bytes) {
    return Fail("string field exceeds its maximum length");
  }
  if (!Require(length)) {
    return false;
  }
  out->assign(payload_, cursor_, length);
  cursor_ += length;
  return true;
}

bool PayloadReader::ReadBytes(std::size_t count, std::string* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(count)) {
    return false;
  }
  out->assign(payload_, cursor_, count);
  cursor_ += count;
  return true;
}

// ---- 定长收发 ----

// 循环写满：send 可能只写出去一部分（发送缓冲区满），
// 返回值是本次写了多少而不是是否成功。EINTR 直接重试，written ==
// 0 视为异常立即失败，不自旋。
//
// MSG_NOSIGNAL：对端提前关闭时 send 会触发 SIGPIPE，
// 默认动作是杀掉整个进程；服务端要把这当成一次普通的发送失败去记日志、关连接。
//
// 失败时只保证最多写出去一部分，帧可能停在半个帧头上，字节流从此失去同步（见
// ReceiveFrame 的 kCorruptStream）。
bool SendAll(int fd, const void* data, std::size_t size,
             std::string* error_message) {
  const char* cursor = static_cast<const char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    const ssize_t written = ::send(fd, cursor, remaining, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = std::string("send failed: ") + std::strerror(errno);
      }
      return false;
    }
    if (written == 0) {
      if (error_message != nullptr) {
        *error_message = "send returned zero before the buffer was flushed";
      }
      return false;
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

std::int64_t MonotonicMillis() {
  struct timespec now;
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000 +
         static_cast<std::int64_t>(now.tv_nsec / 1000000);
}

// 收满 size 字节才算成功。deadline_ms
// 是**整体**预算（CLOCK_MONOTONIC 时间点，故不受校时回拨影响），
// 0 = 不设限。
//
//   * 每次 recv 前重算剩余预算，并用 poll 把单次等待压进预算之内，
//   这样每个超时周期只挤 1 字节的慢速对端也无法把连接拖成永久挂起；
//
//   * closed_by_peer 只在一个字节都没收到就 EOF 时为
//   true，读到一半断开是错误；
//
//   * 失败时已写进 data 的字节数不确定，调用方不得假定缓冲区内容可用。
bool ReceiveAll(int fd, void* data, std::size_t size, bool* closed_by_peer,
                std::string* error_message, std::int64_t deadline_ms) {
  if (closed_by_peer != nullptr) {
    *closed_by_peer = false;
  }
  char* cursor = static_cast<char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    // 每次 recv 之前查一次整体预算：慢速滴水的对端（每个超时周期挤 1 个字节）
    // 单靠 SO_RCVTIMEO 是拦不住的。
    if (deadline_ms != 0) {
      const std::int64_t now = MonotonicMillis();
      if (now >= deadline_ms) {
        if (error_message != nullptr) {
          *error_message = "receive deadline exceeded";
        }
        return false;
      }
      // 还要把"这一次 recv 最多能等多久"压到剩余预算之内。
      // 只查上面的预算是不够的：recv 一阻塞就再也回不到这里，而 SO_RCVTIMEO
      // 是**调用方**设置的 —— socketpair、忘了设超时的调用方、以及任何没有读
      // 超时的 fd 都会让 recv 永久阻塞，整体握手预算就形同虚设。
      // 一个真实的失败场景：客户端在等一条对端根本不会发的 ServerHello，
      // 进程 0% CPU 卡死。
      struct pollfd waiter;
      waiter.fd = fd;
      waiter.events = POLLIN;
      waiter.revents = 0;
      const std::int64_t left = deadline_ms - now;
      const int wait_ms = static_cast<int>(left > 3600000 ? 3600000 : left);
      const int poll_result = ::poll(&waiter, 1, wait_ms);
      if (poll_result == 0) {
        if (error_message != nullptr) {
          *error_message = "receive deadline exceeded";
        }
        return false;
      }
      if (poll_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (error_message != nullptr) {
          *error_message = std::string("poll failed: ") + std::strerror(errno);
        }
        return false;
      }
    }
    const ssize_t got = ::recv(fd, cursor, remaining, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = std::string("recv failed: ") + std::strerror(errno);
      }
      return false;
    }
    if (got == 0) {
      // 一个字节都没读到就 EOF 是"对端正常关闭"；读到一半断开是错误。
      if (remaining == size) {
        if (closed_by_peer != nullptr) {
          *closed_by_peer = true;
        }
        if (error_message != nullptr) {
          *error_message = "peer closed the connection";
        }
        return false;
      }
      if (error_message != nullptr) {
        *error_message = "peer closed the connection in the middle of a frame";
      }
      return false;
    }
    cursor += got;
    remaining -= static_cast<std::size_t>(got);
  }
  return true;
}

// 一帧 = 32 字节帧头 + 可选 payload，分两次 send
// 而不是先拼成一个大 buffer：payload 最大 1 MiB，
// 再拷一份纯属浪费；代价是失败可能停在帧头与 payload 之间，所以返回
// false 之后调用方**必须**断开连接。
//
// version/flags/reserved 由这里统一填死，调用方只能给
// opcode/status/request_id/payload，
// 发不出本进程不认识的帧。
bool SendFrame(int fd, std::uint16_t opcode, std::uint32_t status,
               std::uint64_t request_id, const std::string& payload,
               std::string* error_message) {
  if (payload.size() > kMaxPayloadBytes) {
    if (error_message != nullptr) {
      *error_message = "refusing to send a payload larger than 1 MiB";
    }
    return false;
  }
  FrameHeader header;
  header.version = kProtocolVersion;
  header.opcode = opcode;
  header.flags = 0;
  header.reserved = 0;
  header.status = status;
  header.request_id = request_id;
  header.payload_length = payload.size();
  const std::string encoded = EncodeFrameHeader(header);
  if (!SendAll(fd, encoded.data(), encoded.size(), error_message)) {
    return false;
  }
  if (!payload.empty() &&
      !SendAll(fd, payload.data(), payload.size(), error_message)) {
    return false;
  }
  return true;
}

// 一次调用只消费**一帧**，绝不靠 recv 的偶然切分去猜消息边界。
// 返回值的分类直接决定调用方的动作：
//
//   kOk            帧完整可用；
//
//   kClosed        对端在帧边界处正常关闭，按连接结束处理；
//
//   kInvalidFrame  帧头可解析但不被接受，流位置仍完好，
//   可回错误帧后继续服务；
//
//   kCorruptStream magic 不对或 payload 超限，
//   流已失同步，只能断开；
//
//   kIoError       读写失败或超时，同样按断开处理。
//
// payload 的 resize 在 DecodeFrameHeader
// 通过**之后**，此时长度已过 1 MiB 上限，
// 这是本文件唯一一处按对端声明的长度分配内存的地方。
FrameReadStatus ReceiveFrame(int fd, FrameHeader* header, std::string* payload,
                             std::string* error_message) {
  if (header == nullptr || payload == nullptr) {
    if (error_message != nullptr) {
      *error_message = "frame output pointers are null";
    }
    return FrameReadStatus::kIoError;
  }
  unsigned char raw[kFrameHeaderSize];
  bool closed = false;
  std::string io_error;
  if (!ReceiveAll(fd, raw, kFrameHeaderSize, &closed, &io_error)) {
    if (error_message != nullptr) {
      *error_message = io_error;
    }
    return closed ? FrameReadStatus::kClosed : FrameReadStatus::kIoError;
  }

  // 先把字段尽力解出来（不校验），这样错误响应也能回填 request_id。
  DecodeFrameHeaderFields(raw, kFrameHeaderSize, header);
  FrameHeader validated;
  std::string header_error;
  if (!DecodeFrameHeader(raw, kFrameHeaderSize, &validated, &header_error)) {
    if (error_message != nullptr) {
      *error_message = header_error;
    }
    // 分两类：magic 不对或长度超限时，流里已经找不到下一个帧的边界，
    // 只能断开；版本/操作码/flags 的问题可以回一个错误帧继续。
    const bool magic_bad = LoadU32BE(raw) != kProtocolMagic;
    const bool length_bad = LoadU64BE(raw + 24) > kMaxPayloadBytes;
    return (magic_bad || length_bad) ? FrameReadStatus::kCorruptStream
                                     : FrameReadStatus::kInvalidFrame;
  }
  *header = validated;

  payload->clear();
  if (header->payload_length > 0) {
    // 长度已经在上面的 DecodeFrameHeader 里过了 1 MiB 上限，
    // 这里才允许按它分配。
    payload->resize(static_cast<std::size_t>(header->payload_length));
    bool payload_closed = false;
    std::string payload_error;
    if (!ReceiveAll(fd, &(*payload)[0], payload->size(), &payload_closed,
                    &payload_error)) {
      if (error_message != nullptr) {
        *error_message = payload_error;
      }
      return FrameReadStatus::kIoError;
    }
  }
  return FrameReadStatus::kOk;
}

// ---- 字段校验 ----

// 用户名校验的唯一实现，返回**分类**而不是 bool：界面按分类给提示，
// 服务端按分类记日志，两边看到的原因必须一致。目的是让用户名只落在
// [A-Za-z0-9_.-] 这个小字符集里，进日志、进 SQL、
// 进显示都不必再转义（它永远不参与路径拼接，磁盘上只有数字 user id）。
//
// error_message 对长度类错误复用同一条英文原因，
// 保持老调用方的输出不变。
UsernameValidation ValidateUsername(const std::string& username,
                                    std::string* error_message) {
  if (username.empty()) {
    if (error_message != nullptr) {
      *error_message = "username must be 3 to 64 bytes long";
    }
    return UsernameValidation::kEmpty;
  }
  if (username.size() < kMinUsernameBytes) {
    if (error_message != nullptr) {
      *error_message = "username must be 3 to 64 bytes long";
    }
    return UsernameValidation::kTooShort;
  }
  if (username.size() > kMaxUsernameBytes) {
    if (error_message != nullptr) {
      *error_message = "username must be 3 to 64 bytes long";
    }
    return UsernameValidation::kTooLong;
  }
  if (username == "." || username == "..") {
    if (error_message != nullptr) {
      *error_message = "username must not be a dot path";
    }
    // 长度下限是 3，所以这两个值其实到不了这里；分类上归入"字符不合法"。
    return UsernameValidation::kInvalidCharacter;
  }
  for (const char character : username) {
    const bool digit = character >= '0' && character <= '9';
    const bool upper = character >= 'A' && character <= 'Z';
    const bool lower = character >= 'a' && character <= 'z';
    const bool symbol =
        character == '_' || character == '.' || character == '-';
    if (!digit && !upper && !lower && !symbol) {
      if (error_message != nullptr) {
        *error_message =
            "username may only contain letters, digits, dot, dash and "
            "underscore";
      }
      return UsernameValidation::kInvalidCharacter;
    }
  }
  return UsernameValidation::kOk;
}

bool IsValidUsername(const std::string& username, std::string* error_message) {
  return ValidateUsername(username, error_message) == UsernameValidation::kOk;
}

// 密码只查长度与内嵌 NUL，不查字符集：它将被当作原始字节喂给口令散列，
// 任何规范化都会改变用户的密码。NUL 单独拒绝，是因为这串字节还要经过 C
// 字符串接口与日志格式化，内嵌 NUL
// 会让不同实现看到不同长度的密码（经典的口令截断问题）。
bool IsValidPassword(const std::string& password, std::string* error_message) {
  if (password.empty() || password.size() > kMaxPasswordBytes) {
    if (error_message != nullptr) {
      *error_message = "password must be 1 to 256 bytes long";
    }
    return false;
  }
  if (password.find('\0') != std::string::npos) {
    if (error_message != nullptr) {
      *error_message = "password must not contain a NUL byte";
    }
    return false;
  }
  return true;
}

// 显示名允许 UTF-8（按字节判控制字符时，多字节序列的高位字节都 >=
// 0x80，不会被误判）。拒绝 0x00-0x1F 与 0x7F 是因为它要进
// SQLite metadata、日志与终端：控制字符能搅乱日志行，ESC
// 序列还能操纵终端。它永不参与路径拼接，所以 '/' 与 .. 合法。
bool IsValidDisplayName(const std::string& name, std::string* error_message) {
  if (name.empty() || name.size() > kMaxDisplayNameBytes) {
    if (error_message != nullptr) {
      *error_message = "display name must be 1 to 255 bytes long";
    }
    return false;
  }
  for (const char character : name) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7F) {
      if (error_message != nullptr) {
        *error_message = "display name must not contain control characters";
      }
      return false;
    }
  }
  return true;
}

bool IsValidSnapshotId(const std::string& snapshot_id,
                       std::string* error_message) {
  if (!IsLowerHex(snapshot_id, 32)) {
    if (error_message != nullptr) {
      *error_message = "snapshot id must be 32 lowercase hex characters";
    }
    return false;
  }
  return true;
}

bool IsValidSha256Hex(const std::string& hex, std::string* error_message) {
  if (!IsLowerHex(hex, kSha256HexBytes)) {
    if (error_message != nullptr) {
      *error_message = "sha256 must be 64 lowercase hex characters";
    }
    return false;
  }
  return true;
}

}  // namespace net
}  // namespace backupproject

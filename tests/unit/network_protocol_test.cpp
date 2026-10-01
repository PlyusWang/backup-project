// tests/unit/network_protocol_test.cpp
//
// PR #20：BPNET1 帧编解码的专项测试。
//
// 重点不是 happy path，而是**解析器不能越界、不能按客户端说的长度分配**：
// 截断的帧头、截断的 payload、假 magic、错版本、未知操作码、超大长度、
// 超长字符串、内嵌 NUL、一帧之后接垃圾、同一个流里的多帧，逐条覆盖。

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "network_protocol.h"
#include "test_support.h"

namespace bp = backupproject;
namespace net = backupproject::net;

namespace {

std::string ToHex(const std::string& data) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  for (const char character : data) {
    const unsigned char byte = static_cast<unsigned char>(character);
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0F]);
  }
  return out;
}

net::FrameHeader SampleHeader() {
  net::FrameHeader header;
  header.version = net::kProtocolVersion;
  header.opcode = static_cast<std::uint16_t>(net::Opcode::kList);
  header.flags = 0;
  header.reserved = 0;
  header.status = 0x01020304u;
  header.request_id = 0x0102030405060708ull;
  header.payload_length = 0x11;
  return header;
}

}  // namespace

int main() {
  test_support::Section("BPNET 1. 帧头编码：大端、定长、字节精确");
  {
    const net::FrameHeader header = SampleHeader();
    const std::string encoded = net::EncodeFrameHeader(header);
    test_support::Check(encoded.size() == net::kFrameHeaderSize,
                        "BPNET T1 帧头正好 32 字节");
    test_support::Check(ToHex(encoded) ==
                            "42504e31"      // magic "BPN1"
                            "0001"          // version
                            "000a"          // opcode LIST
                            "0000"          // flags
                            "0000"          // reserved
                            "01020304"      // status
                            "0102030405060708"  // request_id
                            "0000000000000011",  // payload_length
                        "BPNET T1 判别：逐字节大端布局与字段顺序",
                        ToHex(encoded));
  }

  test_support::Section("BPNET 2. 帧头解码：合法帧往返一致");
  {
    const net::FrameHeader header = SampleHeader();
    const std::string encoded = net::EncodeFrameHeader(header);
    net::FrameHeader decoded;
    std::string error;
    const bool ok = net::DecodeFrameHeader(
        reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size(),
        &decoded, &error);
    test_support::Check(ok, "BPNET T2 合法帧头被接受", error);
    test_support::Check(decoded.version == header.version &&
                            decoded.opcode == header.opcode &&
                            decoded.status == header.status &&
                            decoded.request_id == header.request_id &&
                            decoded.payload_length == header.payload_length,
                        "BPNET T2 判别：所有字段往返一致");
  }

  test_support::Section("BPNET 3. 帧头解码：非法输入逐条拒绝");
  {
    const std::string good = net::EncodeFrameHeader(SampleHeader());
    net::FrameHeader out;
    std::string error;

    test_support::Check(!net::DecodeFrameHeader(
                            reinterpret_cast<const unsigned char*>(good.data()),
                            31, &out, &error),
                        "BPNET T3 31 字节（截断 1 字节）被拒绝", error);

    std::string bad_magic = good;
    bad_magic[0] = 'X';
    test_support::Check(!net::DecodeFrameHeader(
                            reinterpret_cast<const unsigned char*>(
                                bad_magic.data()),
                            bad_magic.size(), &out, &error),
                        "BPNET T3 假 magic 被拒绝", error);

    std::string bad_version = good;
    bad_version[5] = 2;
    error.clear();
    test_support::Check(!net::DecodeFrameHeader(
                            reinterpret_cast<const unsigned char*>(
                                bad_version.data()),
                            bad_version.size(), &out, &error),
                        "BPNET T3 未知版本被拒绝", error);

    std::string bad_flags = good;
    bad_flags[9] = 1;
    error.clear();
    test_support::Check(!net::DecodeFrameHeader(
                            reinterpret_cast<const unsigned char*>(
                                bad_flags.data()),
                            bad_flags.size(), &out, &error),
                        "BPNET T3 flags 非 0 被拒绝", error);

    std::string bad_reserved = good;
    bad_reserved[11] = 7;
    error.clear();
    test_support::Check(!net::DecodeFrameHeader(
                            reinterpret_cast<const unsigned char*>(
                                bad_reserved.data()),
                            bad_reserved.size(), &out, &error),
                        "BPNET T3 reserved 非 0 被拒绝", error);

    // payload_length = 0xFFFFFFFFFFFFFFFF：必须在这里被挡住，
    // 绝不能走到"按这个长度分配"。
    std::string huge = good;
    for (int index = 24; index < 32; ++index) {
      huge[index] = static_cast<char>(0xFF);
    }
    error.clear();
    test_support::Check(!net::DecodeFrameHeader(
                            reinterpret_cast<const unsigned char*>(huge.data()),
                            huge.size(), &out, &error),
                        "BPNET T3 超大 payload 长度被拒绝", error);

    // 失败时不得改动调用方传进来的 header。
    net::FrameHeader untouched;
    untouched.request_id = 0xABCDEF;
    net::FrameHeader before = untouched;
    error.clear();
    net::DecodeFrameHeader(reinterpret_cast<const unsigned char*>(huge.data()),
                           huge.size(), &untouched, &error);
    test_support::Check(untouched.request_id == before.request_id,
                        "BPNET T3 解码失败时不改动输出结构体");
  }

  test_support::Section("BPNET 4. 尽力解析：错误响应仍然能回填字段");
  {
    std::string bad_version = net::EncodeFrameHeader(SampleHeader());
    bad_version[5] = 9;
    net::FrameHeader filled;
    net::DecodeFrameHeaderFields(
        reinterpret_cast<const unsigned char*>(bad_version.data()),
        bad_version.size(), &filled);
    test_support::Check(filled.request_id == 0x0102030405060708ull &&
                            filled.opcode ==
                                static_cast<std::uint16_t>(net::Opcode::kList),
                        "BPNET T4 判别：帧头非法但 request_id / opcode 可回填");
    net::FrameHeader partial;
    net::DecodeFrameHeaderFields(
        reinterpret_cast<const unsigned char*>(bad_version.data()), 10,
        &partial);
    test_support::Check(partial.request_id == 0,
                        "BPNET T4 不足 32 字节时不读越界");
  }

  test_support::Section("BPNET 5. payload 构造与解析：整数往返");
  {
    net::PayloadBuilder builder;
    builder.AppendU8(0xAB);
    builder.AppendU16(0xABCD);
    builder.AppendU32(0xDEADBEEF);
    builder.AppendU64(0x0123456789ABCDEFull);
    net::PayloadReader reader(builder.data());
    std::uint8_t a = 0;
    std::uint16_t b = 0;
    std::uint32_t c = 0;
    std::uint64_t d = 0;
    test_support::Check(reader.ReadU8(&a) && a == 0xAB, "BPNET T5 u8 往返");
    test_support::Check(reader.ReadU16(&b) && b == 0xABCD, "BPNET T5 u16 往返");
    test_support::Check(reader.ReadU32(&c) && c == 0xDEADBEEF,
                        "BPNET T5 u32 往返");
    test_support::Check(reader.ReadU64(&d) && d == 0x0123456789ABCDEFull,
                        "BPNET T5 u64 往返");
    test_support::Check(reader.AtEnd(), "BPNET T5 读完之后 AtEnd");
  }

  test_support::Section("BPNET 6. payload 解析：截断与越界一律失败");
  {
    net::PayloadBuilder builder;
    builder.AppendU32(0x11223344);
    const std::string payload = builder.data();

    // 注意：切片必须先落成一个具名对象。直接传 substr() 的临时值会让
    // PayloadReader 持有悬空引用——这正是 ASan 会在测试里抓到的东西。
    const std::string truncated_payload = payload.substr(0, 3);
    net::PayloadReader truncated(truncated_payload);
    std::uint32_t value = 0;
    test_support::Check(!truncated.ReadU32(&value),
                        "BPNET T6 3 字节读 u32 失败");
    test_support::Check(!truncated.error_message().empty(),
                        "BPNET T6 失败带原因");
    test_support::Check(!truncated.AtEnd(),
                        "BPNET T6 失败后游标不前进（数据仍在）");

    // u16 长度前缀说 60000 字节，实际只有 4 字节：先判上限再判剩余，
    // 两条都不能分配 60000 字节。
    std::string lying;
    lying.push_back(static_cast<char>(0xEA));
    lying.push_back(static_cast<char>(0x60));
    lying += "abcd";
    net::PayloadReader string_reader(lying);
    std::string text;
    test_support::Check(!string_reader.ReadString(net::kMaxUsernameBytes, &text),
                        "BPNET T6 超长字符串字段被拒绝");
    test_support::Check(text.empty(), "BPNET T6 判别：被拒绝时没有写入输出");

    net::PayloadReader short_reader(payload);
    test_support::Check(short_reader.ReadBytes(5, &text) == false,
                        "BPNET T6 读超过剩余长度的字节数失败");
    test_support::Check(short_reader.ReadBytes(4, &text) && text.size() == 4,
                        "BPNET T6 失败没有破坏后续正常读取");
  }

  test_support::Section("BPNET 7. payload 构造：超限字段不写入");
  {
    net::PayloadBuilder builder;
    builder.AppendU8(1);
    const std::string long_name(300, 'x');
    std::string error;
    test_support::Check(
        !builder.AppendString(long_name, net::kMaxDisplayNameBytes, &error),
        "BPNET T7 超长字符串被拒绝");
    test_support::Check(builder.size() == 1,
                        "BPNET T7 判别：被拒绝时缓冲区没有半截写入");
    test_support::Check(!error.empty(), "BPNET T7 拒绝时给出原因");

    net::PayloadBuilder ok;
    std::string ok_error;
    test_support::Check(ok.AppendString("name", 255, &ok_error),
                        "BPNET T7 合法字符串被接受", ok_error);
    net::PayloadReader reader(ok.data());
    std::string round;
    test_support::Check(reader.ReadString(255, &round) && round == "name",
                        "BPNET T7 字符串往返一致");
  }

  test_support::Section("BPNET 8. 内嵌 NUL 与二进制显示名逐字节保留");
  {
    std::string name = "a";
    name.push_back('\0');
    name += "b";
    net::PayloadBuilder builder;
    std::string error;
    test_support::Check(builder.AppendString(name, 255, &error),
                        "BPNET T8 含 NUL 的字符串能编码（校验在协议之外）",
                        error);
    net::PayloadReader reader(builder.data());
    std::string round;
    test_support::Check(reader.ReadString(255, &round) && round == name,
                        "BPNET T8 判别：NUL 之后的内容没有被截断");
    test_support::Check(!net::IsValidDisplayName(name, &error),
                        "BPNET T8 显示名校验拒绝内嵌 NUL");
  }

  test_support::Section("BPNET 9. 定长收发：socketpair 上的真实帧");
  {
    int pair[2] = {-1, -1};
    test_support::Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
                        "BPNET T9 socketpair 建立");
    std::string error;
    const std::string payload = "hello-bpnet";
    test_support::Check(net::SendFrame(pair[0],
                                       static_cast<std::uint16_t>(
                                           net::Opcode::kUploadBegin),
                                       0, 42, payload, &error),
                        "BPNET T9 发送帧", error);
    net::FrameHeader header;
    std::string received;
    const net::FrameReadStatus status =
        net::ReceiveFrame(pair[1], &header, &received, &error);
    test_support::Check(status == net::FrameReadStatus::kOk,
                        "BPNET T9 接收帧成功", error);
    test_support::Check(header.opcode ==
                                static_cast<std::uint16_t>(
                                    net::Opcode::kUploadBegin) &&
                            header.request_id == 42 && received == payload,
                        "BPNET T9 判别：opcode / request_id / payload 一致");
    ::close(pair[0]);
    ::close(pair[1]);
  }

  test_support::Section("BPNET 10. 定长收发：截断的帧头与截断的 payload");
  {
    int pair[2] = {-1, -1};
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    const std::string half_header(10, 'x');
    ::send(pair[0], half_header.data(), half_header.size(), 0);
    ::close(pair[0]);
    net::FrameHeader header;
    std::string payload;
    std::string error;
    const net::FrameReadStatus status =
        net::ReceiveFrame(pair[1], &header, &payload, &error);
    test_support::Check(status == net::FrameReadStatus::kIoError,
                        "BPNET T10 截断的帧头算 I/O 错误而不是正常关闭", error);
    ::close(pair[1]);

    int second[2] = {-1, -1};
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, second);
    net::FrameHeader full = SampleHeader();
    full.opcode = static_cast<std::uint16_t>(net::Opcode::kUploadChunk);
    full.payload_length = 100;
    const std::string encoded = net::EncodeFrameHeader(full);
    ::send(second[0], encoded.data(), encoded.size(), 0);
    const std::string partial_payload(37, 'y');
    ::send(second[0], partial_payload.data(), partial_payload.size(), 0);
    ::close(second[0]);
    error.clear();
    const net::FrameReadStatus second_status =
        net::ReceiveFrame(second[1], &header, &payload, &error);
    test_support::Check(second_status == net::FrameReadStatus::kIoError,
                        "BPNET T10 payload 中途断开算 I/O 错误", error);
    test_support::Check(payload.empty() || payload.size() == 100,
                        "BPNET T10 判别：不会交出半截 payload 当成功");
    ::close(second[1]);
  }

  test_support::Section("BPNET 11. 定长收发：magic 损坏必须断开流");
  {
    int pair[2] = {-1, -1};
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    std::string encoded = net::EncodeFrameHeader(SampleHeader());
    encoded[1] = 'Z';
    ::send(pair[0], encoded.data(), encoded.size(), 0);
    net::FrameHeader header;
    std::string payload;
    std::string error;
    const net::FrameReadStatus status =
        net::ReceiveFrame(pair[1], &header, &payload, &error);
    test_support::Check(status == net::FrameReadStatus::kCorruptStream,
                        "BPNET T11 假 magic 归类为流损坏", error);

    // 超大长度同理：无法安全丢弃，只能断开。
    std::string huge = net::EncodeFrameHeader(SampleHeader());
    for (int index = 24; index < 32; ++index) {
      huge[index] = static_cast<char>(0xFF);
    }
    ::send(pair[0], huge.data(), huge.size(), 0);
    error.clear();
    const net::FrameReadStatus huge_status =
        net::ReceiveFrame(pair[1], &header, &payload, &error);
    test_support::Check(huge_status == net::FrameReadStatus::kCorruptStream,
                        "BPNET T11 超大长度归类为流损坏", error);
    test_support::Check(payload.empty(),
                        "BPNET T11 判别：没有按 2^64-1 分配过任何缓冲");
    ::close(pair[0]);
    ::close(pair[1]);
  }

  test_support::Section("BPNET 12. 定长收发：非法但可恢复的帧头");
  {
    int pair[2] = {-1, -1};
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    net::FrameHeader version_two = SampleHeader();
    version_two.version = 2;
    version_two.payload_length = 3;
    const std::string encoded = net::EncodeFrameHeader(version_two) + "abc";
    ::send(pair[0], encoded.data(), encoded.size(), 0);
    net::FrameHeader header;
    std::string payload;
    std::string error;
    const net::FrameReadStatus status =
        net::ReceiveFrame(pair[1], &header, &payload, &error);
    test_support::Check(status == net::FrameReadStatus::kInvalidFrame,
                        "BPNET T12 错版本归类为可恢复的非法帧", error);
    test_support::Check(header.version == 2 &&
                            header.request_id ==
                                version_two.request_id,
                        "BPNET T12 判别：非法帧仍然回填了字段");
    ::close(pair[0]);
    ::close(pair[1]);
  }

  test_support::Section("BPNET 13. 同一个流里的多帧与帧后垃圾");
  {
    int pair[2] = {-1, -1};
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    std::string stream;
    for (std::uint64_t index = 1; index <= 3; ++index) {
      stream += net::EncodeFrameHeader([&] {
        net::FrameHeader header = SampleHeader();
        header.opcode = static_cast<std::uint16_t>(net::Opcode::kPing);
        header.request_id = index;
        header.payload_length = index;
        return header;
      }());
      stream.append(static_cast<std::size_t>(index), 'p');
    }
    ::send(pair[0], stream.data(), stream.size(), 0);

    bool all_ok = true;
    for (std::uint64_t index = 1; index <= 3; ++index) {
      net::FrameHeader header;
      std::string payload;
      std::string error;
      if (net::ReceiveFrame(pair[1], &header, &payload, &error) !=
              net::FrameReadStatus::kOk ||
          header.request_id != index ||
          payload.size() != static_cast<std::size_t>(index)) {
        all_ok = false;
      }
    }
    test_support::Check(all_ok, "BPNET T13 判别：连续三帧逐帧边界正确");

    // 帧后垃圾要凑够一个帧头长度：只发 11 字节的话读侧会一直等满 32 字节
    // （对端并没有关闭），那是在测"阻塞"而不是在测"帧同步"。
    const std::string garbage(48, 'Z');
    ::send(pair[0], garbage.data(), garbage.size(), 0);
    net::FrameHeader header;
    std::string payload;
    std::string error;
    test_support::Check(net::ReceiveFrame(pair[1], &header, &payload, &error) ==
                            net::FrameReadStatus::kCorruptStream,
                        "BPNET T13 帧后垃圾被识别为流损坏");
    ::close(pair[0]);
    ::close(pair[1]);
  }

  test_support::Section("BPNET 14. 干净关闭与正常关闭的区别");
  {
    int pair[2] = {-1, -1};
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    ::close(pair[0]);
    net::FrameHeader header;
    std::string payload;
    std::string error;
    test_support::Check(net::ReceiveFrame(pair[1], &header, &payload, &error) ==
                            net::FrameReadStatus::kClosed,
                        "BPNET T14 对端未发任何字节就关闭 = kClosed");
    ::close(pair[1]);
  }

  test_support::Section("BPNET 15. 操作码与状态码表");
  {
    test_support::Check(net::IsKnownOpcode(
                            static_cast<std::uint16_t>(net::Opcode::kPing)) &&
                            net::IsKnownOpcode(static_cast<std::uint16_t>(
                                net::Opcode::kUploadEnd)) &&
                            !net::IsKnownOpcode(9999),
                        "BPNET T15 操作码识别表");
    test_support::Check(std::string(net::OpcodeName(9999)) == "UNKNOWN" &&
                            std::string(net::StatusName(
                                static_cast<std::uint32_t>(
                                    net::Status::kIntegrityMismatch))) ==
                                "INTEGRITY_MISMATCH",
                        "BPNET T15 名字表");
    test_support::Check(net::OpcodeAllowsEmptyPayload(
                            static_cast<std::uint16_t>(net::Opcode::kPing)) &&
                            !net::OpcodeAllowsEmptyPayload(
                                static_cast<std::uint16_t>(
                                    net::Opcode::kLogin)),
                        "BPNET T15 空 payload 许可表");
    // PR #20 closure：注销账户是**用户**操作，它出现在协议里；管理员能力不在
    // 协议里。这两件事必须能被区分开：
    //   * DELETE_ACCOUNT 是已知操作码，而且必须带口令载荷（不允许空载荷）；
    //   * 0x0100..0x01FF 这一整段（最容易被后人拿来做 ADMIN_* 的地方）一个
    //     已知操作码都没有——管理工具是 ECS 本机程序，不通过 BPNET1 暴露，
    //     也不应该有人在协议里给它留门。
    test_support::Check(
        net::IsKnownOpcode(
            static_cast<std::uint16_t>(net::Opcode::kDeleteAccount)) &&
            std::string(net::OpcodeName(static_cast<std::uint16_t>(
                            net::Opcode::kDeleteAccount))) ==
                "DELETE_ACCOUNT" &&
            !net::OpcodeAllowsEmptyPayload(static_cast<std::uint16_t>(
                net::Opcode::kDeleteAccount)),
        "BPNET T15 注销账户是已知操作码，且必须带口令载荷");
    bool admin_range_clean = true;
    for (std::uint32_t opcode = 0x0100; opcode <= 0x01FF; ++opcode) {
      if (net::IsKnownOpcode(static_cast<std::uint16_t>(opcode))) {
        admin_range_clean = false;
      }
    }
    test_support::Check(admin_range_clean,
                        "BPNET T15 0x0100..0x01FF 里没有任何已知操作码"
                        "（协议里没有管理员入口）");
  }

  test_support::Section("BPNET 16. 字段校验表");
  {
    std::string error;
    test_support::Check(net::IsValidUsername("night-01", &error),
                        "BPNET T16 合法用户名", error);
    test_support::Check(!net::IsValidUsername("ab", &error),
                        "BPNET T16 太短的用户名被拒绝");
    test_support::Check(!net::IsValidUsername(std::string(65, 'a'), &error),
                        "BPNET T16 太长的用户名被拒绝");
    test_support::Check(!net::IsValidUsername("..", &error),
                        "BPNET T16 \"..\" 被拒绝");
    test_support::Check(!net::IsValidUsername("a/b", &error),
                        "BPNET T16 含 '/' 的用户名被拒绝");
    test_support::Check(!net::IsValidUsername("a\\b", &error),
                        "BPNET T16 含反斜杠的用户名被拒绝");
    test_support::Check(!net::IsValidUsername("has space", &error),
                        "BPNET T16 含空格的用户名被拒绝");
    test_support::Check(
        !net::IsValidUsername(std::string("ab\0cd", 5), &error),
        "BPNET T16 含 NUL 的用户名被拒绝");

    test_support::Check(!net::IsValidPassword("", &error),
                        "BPNET T16 空密码被拒绝");
    test_support::Check(net::IsValidPassword("x", &error),
                        "BPNET T16 单字节密码合法（长度校验在别处）");
    test_support::Check(!net::IsValidPassword(std::string(257, 'x'), &error),
                        "BPNET T16 超长密码被拒绝");

    test_support::Check(net::IsValidDisplayName("../x", &error),
                        "BPNET T16 显示名允许 \"../x\"（只进 metadata）", error);
    test_support::Check(net::IsValidDisplayName("/absolute", &error),
                        "BPNET T16 显示名允许绝对路径样式");
    test_support::Check(net::IsValidDisplayName("a\\b", &error),
                        "BPNET T16 显示名允许反斜杠");
    test_support::Check(net::IsValidDisplayName("备份_2026.bak", &error),
                        "BPNET T16 显示名允许 UTF-8");
    test_support::Check(!net::IsValidDisplayName("", &error),
                        "BPNET T16 空显示名被拒绝");
    test_support::Check(!net::IsValidDisplayName(std::string(256, 'a'), &error),
                        "BPNET T16 超长显示名被拒绝");
    test_support::Check(!net::IsValidDisplayName("line\nbreak", &error),
                        "BPNET T16 含控制字符的显示名被拒绝");

    test_support::Check(net::IsValidSnapshotId(std::string(32, 'a'), &error),
                        "BPNET T16 合法 snapshot id", error);
    test_support::Check(!net::IsValidSnapshotId(std::string(31, 'a'), &error),
                        "BPNET T16 长度不对的 snapshot id 被拒绝");
    test_support::Check(!net::IsValidSnapshotId(std::string(32, 'A'), &error),
                        "BPNET T16 大写十六进制被拒绝（避免大小写歧义）");
    test_support::Check(net::IsValidSha256Hex(std::string(64, '0'), &error),
                        "BPNET T16 合法 sha256 文本", error);
    test_support::Check(!net::IsValidSha256Hex(std::string(63, '0'), &error),
                        "BPNET T16 长度不对的 sha256 被拒绝");
  }

  return test_support::Finish("network_protocol_test");
}

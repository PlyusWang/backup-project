// tests/unit/remote_test_support.h
//
// PR #21：远端相关单元测试共用的 BPSEC1 辅助。
//
// 服务端在说第一句 BPNET1 之前必须先完成 BPSEC1 握手：套接字上的头 72 个字节
// 会被当成 ClientHello 解析，任何"没有握手就直接喂帧"的测试都必然失败。
// 这个头文件把那几行固定下来，避免每个测试各写一遍——写错一遍，测的就不是
// 产品路径了。
//
// 全部是 inline 函数：只要包含这个头文件即可，测试脚本不用新增编译单元。

#ifndef BACKUP_PROJECT_TESTS_UNIT_REMOTE_TEST_SUPPORT_H_
#define BACKUP_PROJECT_TESTS_UNIT_REMOTE_TEST_SUPPORT_H_

#include <unistd.h>

#include <cstdio>
#include <string>

#include "remote_server.h"
#include "secure_transport.h"

namespace remote_test_support {

// 为一次测试生成服务端传输身份密钥文件，并准备好客户端 pin 文本。
//
//   key_file  身份私钥文件路径（放在测试自己的临时目录里）
//   identity  输出：服务端 identity（测试把它交给 fixture.config 的
//             transport_key_file_path 指向的文件即可）
//   pin_text  输出："sha256:<指纹>"，可直接填进 RemoteEndpoint::server_key_pin
//
// 允许重复调用：已存在的 key_file 会先删掉再重新生成（每次一套新密钥）。
inline bool PrepareTransportIdentity(
    const std::string& key_file,
    backupproject::net::TransportIdentity* identity, std::string* pin_text,
    std::string* error_message) {
  ::unlink(key_file.c_str());
  if (!backupproject::net::GenerateTransportIdentity(identity, error_message)) {
    return false;
  }
  if (!backupproject::net::SaveTransportIdentity(key_file, *identity,
                                                 /*overwrite=*/false,
                                                 error_message)) {
    return false;
  }
  *pin_text = "sha256:" +
              backupproject::crypto::X25519Fingerprint(identity->public_key);
  return true;
}

// 在一条已经连上的 fd 上完成客户端侧握手。
inline bool HandshakeTestClient(int fd, const std::string& pin_text,
                                backupproject::net::SecureChannel* channel,
                                std::string* error_message) {
  backupproject::net::ServerKeyPin pin;
  if (!backupproject::net::ParseServerKeyPin(pin_text, &pin, error_message)) {
    return false;
  }
  channel->Reset();
  return channel->HandshakeClient(fd, pin, error_message);
}

// 发一帧（走加密通道），签名与 network_protocol.h 的 SendFrame 一致。
inline bool SendTestFrame(backupproject::net::SecureChannel* channel, int fd,
                          std::uint16_t opcode, std::uint32_t status,
                          std::uint64_t request_id,
                          const std::string& payload,
                          std::string* error_message) {
  return channel->SendFrame(fd, opcode, status, request_id, payload,
                            error_message);
}

// 读一帧（走加密通道），签名与 network_protocol.h 的 ReceiveFrame 一致。
inline backupproject::net::FrameReadStatus ReceiveTestFrame(
    backupproject::net::SecureChannel* channel, int fd,
    backupproject::net::FrameHeader* header, std::string* payload,
    std::string* error_message) {
  return channel->ReceiveFrame(fd, header, payload, error_message);
}

}  // namespace remote_test_support

#endif  // BACKUP_PROJECT_TESTS_UNIT_REMOTE_TEST_SUPPORT_H_

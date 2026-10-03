// tests/review/identity_fifo_probe.cpp
//
// PR #21 独立审查轮（review-only 探针，不参与产品构建、也不进
// scripts/secure_transport_test.sh —— 因为它**故意会一直阻塞**）。
//
// 复现的问题：src/network/secure_transport.cpp 里 SaveTransportIdentity()
// 与 LoadTransportIdentity() 都是"先 open() 后 fstat() 检查普通文件"。
// 如果路径上被放了 FIFO，open() 本身就会阻塞（open(FIFO, O_WRONLY) 等读者、
// open(FIFO, O_RDONLY) 等写者），那句"必须是普通文件"的校验根本没机会执行。
//
// 用法（必须带 timeout，否则会一直挂着）：
//   g++ -std=c++17 -O1 -Iinclude -pthread
//       src/crypto/{sha256,hmac,pbkdf2,aes,random,x25519,hkdf}.cpp
//       src/network/{network_protocol,secure_transport}.cpp
//       tests/review/identity_fifo_probe.cpp -o /tmp/identity_fifo_probe
//   timeout --signal=KILL 3 /tmp/identity_fifo_probe
//
// 期望（修好之后）：立刻打印 SAVE_RETURNED ok=0 ... / LOAD_RETURNED ok=0 ...
// 实际（当前实现）：只打印 SAVE_BEGIN，然后被 timeout 杀掉。

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "secure_transport.h"

int main() {
  const std::string dir = "/tmp/identity-fifo-probe";
  ::mkdir(dir.c_str(), 0700);
  const std::string path = dir + "/transport.key";
  ::unlink(path.c_str());
  if (::mkfifo(path.c_str(), 0600) != 0) {
    std::printf("mkfifo 失败\n");
    return 2;
  }
  backupproject::net::TransportIdentity identity;
  std::string error;
  if (!backupproject::net::GenerateTransportIdentity(&identity, &error)) {
    return 2;
  }

  std::printf("SAVE_BEGIN\n");
  std::fflush(stdout);
  const bool saved =
      backupproject::net::SaveTransportIdentity(path, identity,
                                                /*overwrite=*/true, &error);
  std::printf("SAVE_RETURNED ok=%d error=%s\n", saved ? 1 : 0, error.c_str());
  std::fflush(stdout);

  backupproject::net::TransportIdentity loaded;
  std::printf("LOAD_BEGIN\n");
  std::fflush(stdout);
  const bool ok =
      backupproject::net::LoadTransportIdentity(path, &loaded, &error);
  std::printf("LOAD_RETURNED ok=%d error=%s\n", ok ? 1 : 0, error.c_str());
  return 0;
}

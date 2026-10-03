// tests/review/identity_tail_eintr.cpp
//
// PR #21 收尾审查发现的次要不一致（主 blocker 在
// tests/review/bundle_source_mutation.cpp）：LoadTransportIdentity() 读满 32
// 字节之后要多读 1 个字节来判断"文件是不是正好 32 字节"。旧代码是
//
//     const ssize_t tail = ::read(fd, &extra, 1);
//     if (tail < 0 && errno != EINTR) { ...错误... return false; }
//     if (tail == 1) { ...超过 32 字节... return false; }
//
// 也就是说 tail == -1 && errno == EINTR 既不是错误、也不等于 1，直接落到
// 两个分支之外 —— 一次信号打断被当成"没有多余字节"，exact-32 不变式被信号
// 放宽了：一个比 32 字节长的文件会被当成合法私钥收下。
//
// 本探针用 tests/review/read_eintr_interposer.cpp 在**链接期**覆盖 read()，
// 确定性地只在那一次 1 字节读取上注入一次 EINTR（不需要 LD_PRELOAD、不需要
// 真的发信号、不 sleep），然后要求：
//   1) 33 字节的密钥文件**必须仍然被拒绝**（旧代码会接受 —— 缺陷判据）；
//   2) 32 字节的密钥文件必须仍然被接受（重试之后读到 EOF，正常路径不受影响），
//      并且推导出的公钥与独立算出来的那一个一致；
//   3) 两个用例都断言"注入真的发生过"，否则用例是空的。
//
// 编译方式：与探针一起链接 tests/review/read_eintr_interposer.cpp
// （scripts/secure_transport_test.sh 里的 review-identity-tail-eintr 一档）。
//
// 退出码：0 = 全部通过。

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "crypto.h"
#include "secure_transport.h"

extern "C" void ReadEintrArm();
extern "C" int ReadEintrDisarm();

namespace {

int g_checks = 0;
int g_failures = 0;
const char* kName = "review-identity-tail-eintr";

void Check(bool ok, const std::string& name, const std::string& detail = "") {
  g_checks += 1;
  if (!ok) {
    g_failures += 1;
    std::printf("  FAIL %s%s\n", name.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
  }
}

void Note(const std::string& text) {
  std::printf("[identity-tail-eintr] %s\n", text.c_str());
}

bool WriteKeyFile(const std::string& path, const std::string& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t written =
        ::write(fd, bytes.data() + done, bytes.size() - done);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return false;
    }
    done += static_cast<std::size_t>(written);
  }
  if (::close(fd) != 0) {
    return false;
  }
  return ::chmod(path.c_str(), 0600) == 0;
}

}  // namespace

int main() {
  const std::string root = "/tmp/identity-tail-eintr-" +
                           std::to_string(static_cast<long>(::getpid()));
  if (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST) {
    std::printf("无法创建 %s\n", root.c_str());
    return 1;
  }
  std::printf("传输身份私钥尾部 read 的 EINTR 语义\n");

  std::string material;
  std::string error;
  if (!backupproject::crypto::RandomBytes(32, &material, &error) ||
      material.size() != 32) {
    Check(false, "生成 32 字节测试密钥", error);
    std::printf("%s: %d/%d checks passed\n", kName, g_checks - g_failures,
                g_checks);
    return 1;
  }
  const std::string key32 = root + "/key-32";
  const std::string key33 = root + "/key-33";
  Check(WriteKeyFile(key32, material), "写出正好 32 字节的私钥（0600）");
  Check(WriteKeyFile(key33, material + "X"), "写出 33 字节的私钥（0600）");

  // 独立算一遍公钥：用它验证"重试之后的 32 字节没有被弄坏"。
  std::string expected_public;
  std::string derive_error;
  const bool derived = backupproject::crypto::X25519PublicKeyFromPrivate(
      material, &expected_public, &derive_error);
  // 公钥是 32 字节原始 u 坐标（不是十六进制文本）。
  Check(derived && expected_public.size() == 32,
        "独立推导出期望公钥（32 字节）", derive_error);

  // ---- 用例 1：33 字节 + 尾部 EINTR 注入 -> 必须拒绝 ----
  backupproject::net::TransportIdentity identity;
  ReadEintrArm();
  const bool accepted33 =
      backupproject::net::LoadTransportIdentity(key33, &identity, &error);
  const int injected33 = ReadEintrDisarm();
  Check(injected33 == 1, "EINTR 注入确实落在尾部那一次 1 字节读取上",
        std::to_string(injected33) + " 次");
  Check(!accepted33, "33 字节的私钥被拒绝（尾部 read 被打断一次也不放行）",
        "竟然接受了");
  Check(error.find("超过 32 字节") != std::string::npos,
        "拒绝原因是长度超过 32 字节", error);
  Note("33 字节用例：注入 " + std::to_string(injected33) + " 次 EINTR，" +
       (accepted33 ? "被接受（缺陷）" : "被拒绝（正确）") + "：" + error);

  // ---- 用例 2：32 字节 + 尾部 EINTR 注入 -> 必须接受 ----
  error.clear();
  backupproject::net::TransportIdentity accepted;
  ReadEintrArm();
  const bool ok32 =
      backupproject::net::LoadTransportIdentity(key32, &accepted, &error);
  const int injected32 = ReadEintrDisarm();
  Check(injected32 == 1,
        "EINTR 注入确实落在尾部那一次 1 字节读取上（32 字节用例）",
        std::to_string(injected32) + " 次");
  Check(ok32, "32 字节的私钥仍然被接受（重试之后读到 EOF）", error);
  Check(ok32 && accepted.private_key == material, "私钥字节被完整读入");
  Check(ok32 && accepted.public_key == expected_public,
        "推导出的公钥与独立计算一致", accepted.public_key);
  Note("32 字节用例：注入 " + std::to_string(injected32) + " 次 EINTR，" +
       (ok32 ? "被接受（正确）" : "被拒绝（错误）") + "。");

  // ---- 用例 3：控制组（不注入）----
  error.clear();
  ReadEintrArm();
  ReadEintrDisarm();
  backupproject::net::TransportIdentity control;
  const bool rejected_without_injection =
      backupproject::net::LoadTransportIdentity(key33, &control, &error);
  Check(!rejected_without_injection, "控制组：33 字节的私钥本来就被拒绝",
        error);

  ::unlink(key32.c_str());
  ::unlink(key33.c_str());
  const std::string cleanup = "rm -rf '" + root + "'";
  if (std::system(cleanup.c_str()) != 0) {
    Note("清理 " + root + " 失败（不影响判定）");
  }

  std::printf("%s: %d/%d checks passed\n", kName, g_checks - g_failures,
              g_checks);
  return g_failures == 0 ? 0 : 1;
}

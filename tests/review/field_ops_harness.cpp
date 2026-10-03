// tests/review/field_ops_harness.cpp
//
// **REVIEW-ONLY 工具，不属于产品构建，也不被任何产品二进制链接。**
//
// 目的：让独立审查能够直接拿 x25519.cpp 内部的有限域原语（FeAdd / FeSub /
// FeMul / FeInvert / FeFromBytes / FeToBytes）与一个**独立实现的** big-int
// oracle 对拍。这些原语在匿名命名空间里，正常是拿不到的；测试用文件级
// #include 把同一个翻译单元包进来，因此没有为了测试而改动产品代码的接口。
//
// 协议（stdin 每行一条，stdout 每行一条结果）：
//   add  <hex32> <hex32>      两个域元素相加（mod p）
//   sub  <hex32> <hex32>      相减
//   mul  <hex32> <hex32>      相乘
//   sq   <hex32>              平方
//   inv  <hex32>              求逆（0 的逆按实现约定输出全 0）
//   x25519 <hex32> <hex32>    X25519(scalar, u)
//   所有 hex 都是 32 字节小端域元素 / 标量 / u 坐标的十六进制。
//   输出：<hex32> 或 ERR <原因>。

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/crypto/x25519.cpp"

namespace backupproject {
namespace crypto {
namespace review {

using backupproject::crypto::Fe;
using backupproject::crypto::FeAdd;
using backupproject::crypto::FeFromBytes;
using backupproject::crypto::FeInvert;
using backupproject::crypto::FeMul;
using backupproject::crypto::FeSub;
using backupproject::crypto::FeToBytes;

bool ParseHex32(const std::string& text, unsigned char out[32]) {
  if (text.size() != 64) {
    return false;
  }
  for (std::size_t i = 0; i < 32; ++i) {
    unsigned int byte = 0;
    if (std::sscanf(text.substr(2 * i, 2).c_str(), "%2x", &byte) != 1) {
      return false;
    }
    out[i] = static_cast<unsigned char>(byte);
  }
  return true;
}

std::string ToHex32(const unsigned char bytes[32]) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < 32; ++i) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0x0F]);
  }
  return out;
}

}  // namespace review
}  // namespace crypto
}  // namespace backupproject

int main() {
  using namespace backupproject::crypto::review;
  char line[512];
  while (std::fgets(line, sizeof(line), stdin) != nullptr) {
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.pop_back();
    }
    if (text.empty() || text[0] == '#') {
      continue;
    }
    std::size_t space = text.find(' ');
    const std::string op = text.substr(0, space);
    std::string rest = space == std::string::npos ? std::string()
                                                  : text.substr(space + 1);
    std::size_t second = rest.find(' ');
    const std::string first = second == std::string::npos
                                  ? rest
                                  : rest.substr(0, second);
    const std::string other = second == std::string::npos
                                  ? std::string()
                                  : rest.substr(second + 1);

    unsigned char a[32];
    unsigned char b[32];
    if (!ParseHex32(first, a)) {
      std::printf("ERR bad first operand\n");
      continue;
    }
    if (op == "x25519") {
      if (!ParseHex32(other, b)) {
        std::printf("ERR bad second operand\n");
        continue;
      }
      std::string out;
      std::string error;
      const std::string scalar(reinterpret_cast<const char*>(a), 32);
      const std::string u(reinterpret_cast<const char*>(b), 32);
      if (!backupproject::crypto::X25519(scalar, u, &out, &error)) {
        std::printf("ERR %s\n", error.c_str());
        continue;
      }
      std::printf("%s\n", ToHex32(reinterpret_cast<const unsigned char*>(out.data())).c_str());
      continue;
    }

    Fe x;
    FeFromBytes(&x, a);
    Fe y;
    bool needs_second = (op == "add" || op == "sub" || op == "mul");
    if (needs_second) {
      if (!ParseHex32(other, b)) {
        std::printf("ERR bad second operand\n");
        continue;
      }
      FeFromBytes(&y, b);
    }
    Fe result;
    if (op == "add") {
      FeAdd(x, y, &result);
    } else if (op == "sub") {
      FeSub(x, y, &result);
    } else if (op == "mul") {
      FeMul(x, y, &result);
    } else if (op == "sq") {
      FeMul(x, x, &result);
    } else if (op == "inv") {
      FeInvert(x, &result);
    } else {
      std::printf("ERR unknown op\n");
      continue;
    }
    unsigned char out[32];
    FeToBytes(result, out);
    std::printf("%s\n", ToHex32(out).c_str());
  }
  return 0;
}

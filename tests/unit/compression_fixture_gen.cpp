// compression_fixture_gen.cpp
//
// 一次性工具：用某一版压缩实现生成"冻结 fixture"，供后续改动做 wire-format
// 兼容性对照。
//
// 用法：compression_fixture_gen <输出目录>
//
// 它写出：
//   inputs/<name>.bin     原始输入（fixture
//   自带输入，测试不依赖任何输入生成器） frozen/<name>.huf1    该版本
//   HuffmanCompress 的完整输出 frozen/<name>.lzh1    该版本 LzssHuffmanCompress
//   的完整输出 MANIFEST.txt          每个文件的字节数与 SHA256
//
// 关键点：**输入也一起存下来**。如果测试自己重新生成输入，哪天输入的生成方式
// 变了，"压缩输出变了"就会被误判成格式不兼容。存原始字节之后，比较的两端都是
// 确定的。

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "compression.h"
#include "crypto.h"

namespace {

struct Fixture {
  const char* name;
  std::string data;
};

// 512 字节的确定性伪随机块（同一个 LCG，任何机器上结果相同）。
std::string PseudoRandomBlock(std::size_t size, std::uint64_t seed) {
  std::string block(size, '\0');
  std::uint64_t state = seed | 1;
  for (std::size_t index = 0; index < size; ++index) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    block[index] = static_cast<char>(state >> 33);
  }
  return block;
}

std::string Repeat(const std::string& unit, std::size_t total) {
  std::string out;
  out.reserve(total);
  while (out.size() < total) {
    out += unit;
  }
  out.resize(total);
  return out;
}

std::vector<Fixture> BuildFixtures() {
  std::vector<Fixture> fixtures;
  fixtures.push_back({"empty", std::string()});

  std::string single(100000, 'a');
  fixtures.push_back({"single-symbol", single});

  const std::string line =
      "the quick brown fox jumps over the lazy dog 0123456789\n";
  fixtures.push_back({"text", Repeat(line, 200000)});

  std::string all256;
  all256.reserve(65536);
  for (int round = 0; round < 256; ++round) {
    for (int value = 0; value < 256; ++value) {
      all256.push_back(static_cast<char>(value));
    }
  }
  fixtures.push_back({"all-256-symbols", all256});

  fixtures.push_back({"repetitive-binary",
                      Repeat(PseudoRandomBlock(512, 0x5EED1234u), 131072)});
  return fixtures;
}

bool WriteFileBytes(const std::string& path, const std::string& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return false;
  }
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t written =
        ::write(fd, bytes.data() + done, bytes.size() - done);
    if (written <= 0) {
      ::close(fd);
      return false;
    }
    done += static_cast<std::size_t>(written);
  }
  return ::close(fd) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: compression_fixture_gen <output-directory>\n");
    return 2;
  }
  const std::string root = argv[1];
  ::mkdir(root.c_str(), 0755);
  ::mkdir((root + "/inputs").c_str(), 0755);
  ::mkdir((root + "/frozen").c_str(), 0755);

  std::FILE* manifest = std::fopen((root + "/MANIFEST.txt").c_str(), "w");
  if (manifest == nullptr) {
    std::fprintf(stderr, "cannot write manifest\n");
    return 1;
  }
  std::fprintf(manifest,
               "# "
               "name\traw_bytes\traw_sha256\thuf1_bytes\thuf1_sha256\tlzh1_"
               "bytes\tlzh1_sha256\n");

  int failures = 0;
  for (const Fixture& fixture : BuildFixtures()) {
    const std::string base = root + "/";
    const std::string raw_path = base + "inputs/" + fixture.name + ".bin";
    const std::string huf_path = base + "frozen/" + fixture.name + ".huf1";
    const std::string lzh_path = base + "frozen/" + fixture.name + ".lzh1";

    std::string error;
    std::string huffman;
    std::string lzss;
    if (!backupproject::compression::HuffmanCompress(fixture.data, &huffman,
                                                     &error)) {
      std::fprintf(stderr, "huffman failed for %s: %s\n", fixture.name,
                   error.c_str());
      ++failures;
      continue;
    }
    if (!backupproject::compression::LzssHuffmanCompress(fixture.data, &lzss,
                                                         &error)) {
      std::fprintf(stderr, "lzh1 failed for %s: %s\n", fixture.name,
                   error.c_str());
      ++failures;
      continue;
    }
    if (!WriteFileBytes(raw_path, fixture.data) ||
        !WriteFileBytes(huf_path, huffman) || !WriteFileBytes(lzh_path, lzss)) {
      std::fprintf(stderr, "write failed for %s\n", fixture.name);
      ++failures;
      continue;
    }
    std::fprintf(manifest, "%s\t%zu\t%s\t%zu\t%s\t%zu\t%s\n", fixture.name,
                 fixture.data.size(),
                 backupproject::crypto::Sha256Hex(fixture.data).c_str(),
                 huffman.size(),
                 backupproject::crypto::Sha256Hex(huffman).c_str(), lzss.size(),
                 backupproject::crypto::Sha256Hex(lzss).c_str());
    std::printf("%-20s raw=%-8zu huf1=%-8zu lzh1=%-8zu\n", fixture.name,
                fixture.data.size(), huffman.size(), lzss.size());
  }
  std::fclose(manifest);
  if (failures != 0) {
    std::fprintf(stderr, "%d fixture(s) failed\n", failures);
    return 1;
  }
  std::printf("fixtures written to %s\n", root.c_str());
  return 0;
}

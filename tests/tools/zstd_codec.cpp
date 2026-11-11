// Independent codec fixture: never takes the product singleton lock.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

#include "compression.h"
#include "file_io.h"

namespace {
namespace c = backupproject::compression;
std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read fixture input");
  return {std::istreambuf_iterator<char>(in), {}};
}
bool Run(const std::string& mode, const std::string& input,
         const std::string& output, std::uint64_t expected,
         std::string* error) {
  backupproject::FileSink sink;
  auto parent = std::filesystem::path(output).parent_path();
  if (parent.empty()) parent = ".";
  if (!sink.OpenTemp(parent.string(), ".codec-", error)) return false;
  bool ok =
      mode == "encode"
          ? c::ZstdCompressStream(input, &sink, nullptr, nullptr, error)
          : c::ZstdDecompressStream(input, &sink, expected, nullptr, error);
  if (!ok || !sink.Close(error)) {
    sink.Abandon();
    return false;
  }
  ok = backupproject::PublishNoReplace(sink.path(), output, error);
  if (!ok) std::filesystem::remove(sink.path());
  return ok;
}
bool SelfTest() {
  std::string error;
  for (const auto& data :
       {std::string(), std::string("one"), std::string(200000, 'x'),
        std::string("\0\xff\x80", 3)}) {
    std::string packed, plain = "sentinel";
    if (!c::ZstdCompress(data, &packed, &error) ||
        !c::ZstdDecompress(packed, data.size(), &plain, &error) ||
        plain != data)
      return false;
    auto bad = packed;
    bad.back() ^= 1;
    plain = "sentinel";
    if (c::ZstdDecompress(bad, data.size(), &plain, &error) ||
        plain != "sentinel")
      return false;
    if (c::ZstdDecompress(packed, data.size() + 1, &plain, &error) ||
        plain != "sentinel")
      return false;
    // Memory aliasing must not invalidate the input until decoding succeeds.
    auto alias = packed;
    if (!c::ZstdDecompress(alias, data.size(), &alias, &error) || alias != data)
      return false;
    for (int legacy = 0; legacy < 2; ++legacy) {
      const bool encoded = legacy
                               ? c::LzssHuffmanCompress(data, &packed, &error)
                               : c::HuffmanCompress(data, &packed, &error);
      const bool decoded =
          legacy ? c::LzssHuffmanDecompress(packed, &plain, &error)
                 : c::HuffmanDecompress(packed, &plain, &error);
      if (!encoded || !decoded || plain != data) return false;
    }
  }
  std::cout << "memory roundtrip, aliasing, unchanged-on-failure, legacy "
               "codecs: PASS\n";
  return true;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "selftest")
      return SelfTest() ? 0 : 1;
    if (argc == 5 && std::string(argv[1]) == "batch") {
      std::ifstream manifest(argv[4]);
      std::string name;
      std::uint64_t expected;
      while (manifest >> name >> expected) {
        const auto input = std::filesystem::path(argv[2]) / name;
        const auto output = std::filesystem::path(argv[3]) / name;
        std::string error, memory = "sentinel";
        const bool ok =
            Run("decode", input.string(), output.string(), expected, &error);
        // Verify the public memory API agrees and preserves its old output on
        // error.
        const bool mem_ok =
            c::ZstdDecompress(Read(input), expected, &memory, &error);
        if (mem_ok != ok || (!ok && memory != "sentinel") ||
            (ok && memory != Read(output)))
          return 3;
        std::cout << name << '\t' << (ok ? "ACCEPT" : "REJECT") << '\t' << error
                  << '\n';
      }
      return manifest.eof() ? 0 : 2;
    }
    if (argc < 4 || argc > 5) return 2;
    const std::string mode = argv[1];
    if (mode != "encode" && mode != "decode") return 2;
    std::size_t used = 0;
    const std::uint64_t expected = argc == 5 ? std::stoull(argv[4], &used) : 0;
    if (mode == "decode" &&
        (argc != 5 || argv[4][0] == '-' || used != std::string(argv[4]).size()))
      return 2;
    std::string error;
    if (!Run(mode, argv[2], argv[3], expected, &error)) {
      std::cerr << error << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}

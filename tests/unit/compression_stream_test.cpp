// compression_stream_test.cpp
//
// 流式压缩编解码的针对性测试。
//
// 它守两件事：
//   1) **wire format 没变**。tests/fixtures/compression 里的输入与流是从
//      冻结 HEAD 用当时的实现生成并存下来的；字符串接口与流式接口都必须
//      产出逐字节相同的结果，两个解码器都必须能读回冻结流。
//   2) **流式接口真的是流式的**：整个文件到文件的过程用固定缓冲，
//      而且"头部声明的长度"与"调用方期望的长度"不一致时，
//      在做任何大块输出之前就失败。

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "compression.h"
#include "file_io.h"
#include "test_support.h"

namespace {

using backupproject::FileSink;
using backupproject::FileSource;
using backupproject::TempDirectoryGuard;
namespace compression = backupproject::compression;

std::string ReadAll(const std::string& path) {
  FileSource source;
  std::string error;
  if (!source.Open(path, &error)) {
    test_support::Check(false, "read " + path, error);
    return std::string();
  }
  std::string out(static_cast<std::size_t>(source.size()), '\0');
  if (!out.empty() && !source.ReadAt(0, &out[0], out.size(), &error)) {
    test_support::Check(false, "read " + path, error);
    return std::string();
  }
  return out;
}

bool WriteAll(const std::string& path, const std::string& bytes) {
  FileSink sink;
  std::string error;
  if (!sink.Open(path, &error)) {
    return false;
  }
  if (!sink.Write(bytes.data(), bytes.size(), &error) || !sink.Close(&error)) {
    sink.Abandon();
    return false;
  }
  return true;
}

// 流式压缩一个文件到另一个文件。
bool CompressFileTo(const std::string& input, const std::string& output,
                    const std::string& workspace, bool lzss,
                    std::uint64_t* produced, std::string* error) {
  FileSink sink;
  if (!sink.Open(output, error)) {
    return false;
  }
  std::uint64_t size = 0;
  const bool ok = lzss ? compression::LzssHuffmanCompressStream(
                             input, &sink, workspace, nullptr, &size, error)
                       : compression::HuffmanCompressStream(
                             input, &sink, nullptr, &size, error);
  if (!ok) {
    sink.Abandon();
    return false;
  }
  if (!sink.Close(error)) {
    sink.Abandon();
    return false;
  }
  *produced = size;
  return true;
}

bool DecompressFileTo(const std::string& input, const std::string& output,
                      const std::string& workspace, bool lzss,
                      std::uint64_t expected, std::uint64_t* written,
                      std::string* error) {
  FileSink sink;
  if (!sink.Open(output, error)) {
    return false;
  }
  std::uint64_t size = 0;
  const bool ok = lzss ? compression::LzssHuffmanDecompressStream(
                             input, &sink, workspace, expected, &size, error)
                       : compression::HuffmanDecompressStream(
                             input, 0, &sink, expected, &size, error);
  if (!ok) {
    sink.Abandon();
    return false;
  }
  if (!sink.Close(error)) {
    sink.Abandon();
    return false;
  }
  *written = size;
  return true;
}

// 逐块比较两个文件，避免为了比较把大文件读进内存。
bool FilesIdentical(const std::string& left, const std::string& right,
                    std::string* detail) {
  FileSource a;
  FileSource b;
  std::string error;
  if (!a.Open(left, &error) || !b.Open(right, &error)) {
    *detail = error;
    return false;
  }
  if (a.size() != b.size()) {
    *detail = "size differs: " + std::to_string(a.size()) + " vs " +
              std::to_string(b.size());
    return false;
  }
  std::vector<unsigned char> buffer_a(1 << 20);
  std::vector<unsigned char> buffer_b(1 << 20);
  std::uint64_t offset = 0;
  while (offset < a.size()) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer_a.size(), a.size() - offset));
    if (!a.ReadAt(offset, buffer_a.data(), want, &error) ||
        !b.ReadAt(offset, buffer_b.data(), want, &error)) {
      *detail = error;
      return false;
    }
    if (std::memcmp(buffer_a.data(), buffer_b.data(), want) != 0) {
      *detail = "content differs at offset " + std::to_string(offset);
      return false;
    }
    offset += want;
  }
  return true;
}

// ---- 1) 冻结 fixture 兼容性 -------------------------------------------------

void RunFrozenFixtures(const std::string& workdir,
                       const std::string& fixtures_root,
                       const std::string& workspace) {
  test_support::Section("frozen HUF1 / LZH1 fixtures (wire compatibility)");
  test_support::Check(test_support::Exists(fixtures_root + "/MANIFEST.txt"),
                      "fixture manifest exists");
  const char* names[] = {"empty", "single-symbol", "text", "all-256-symbols",
                         "repetitive-binary"};
  for (const char* name : names) {
    const std::string raw_path =
        fixtures_root + "/inputs/" + std::string(name) + ".bin";
    const std::string huf_path =
        fixtures_root + "/frozen/" + std::string(name) + ".huf1";
    const std::string lzh_path =
        fixtures_root + "/frozen/" + std::string(name) + ".lzh1";
    const std::string raw = ReadAll(raw_path);
    const std::string huf = ReadAll(huf_path);
    const std::string lzh = ReadAll(lzh_path);
    if (!test_support::Exists(raw_path)) {
      continue;
    }
    const std::string label = std::string(name);

    // 字符串接口：输出必须与冻结字节完全相同。
    std::string out;
    std::string error;
    const bool ok_h = compression::HuffmanCompress(raw, &out, &error);
    test_support::Check(
        ok_h && out == huf,
        "string HuffmanCompress == frozen bytes (" + label + ")", error);
    out.clear();
    error.clear();
    const bool ok_l = compression::LzssHuffmanCompress(raw, &out, &error);
    test_support::Check(
        ok_l && out == lzh,
        "string LzssHuffmanCompress == frozen bytes (" + label + ")", error);

    // 流式接口：同样必须逐字节相同（这条才是本轮真正要保住的东西）。
    const std::string input_file = workdir + "/" + label + ".raw";
    test_support::Check(WriteAll(input_file, raw),
                        "write fixture input " + label);
    const std::string huf_out = workdir + "/" + label + ".huf1";
    const std::string lzh_out = workdir + "/" + label + ".lzh1";
    std::uint64_t produced = 0;
    error.clear();
    const bool stream_h = CompressFileTo(input_file, huf_out, workspace,
                                         /*lzss=*/false, &produced, &error);
    test_support::Check(
        stream_h && produced == huf.size() &&
            FilesIdentical(huf_out, huf_path, &error),
        "streaming HuffmanCompressStream == frozen bytes (" + label + ")",
        error);
    error.clear();
    const bool stream_l = CompressFileTo(input_file, lzh_out, workspace,
                                         /*lzss=*/true, &produced, &error);
    test_support::Check(
        stream_l && produced == lzh.size() &&
            FilesIdentical(lzh_out, lzh_path, &error),
        "streaming LzssHuffmanCompressStream == frozen bytes (" + label + ")",
        error);

    // 两个解码器都要能读冻结流。
    std::string back;
    error.clear();
    test_support::Check(
        compression::HuffmanDecompress(huf, &back, &error) && back == raw,
        "string HuffmanDecompress reads frozen stream (" + label + ")", error);
    back.clear();
    error.clear();
    test_support::Check(
        compression::LzssHuffmanDecompress(lzh, &back, &error) && back == raw,
        "string LzssHuffmanDecompress reads frozen stream (" + label + ")",
        error);
    const std::string huf_back = workdir + "/" + label + ".huf1.out";
    const std::string lzh_back = workdir + "/" + label + ".lzh1.out";
    std::uint64_t written = 0;
    error.clear();
    const bool dec_h =
        DecompressFileTo(huf_path, huf_back, workspace,
                         /*lzss=*/false, raw.size(), &written, &error);
    test_support::Check(
        dec_h && written == raw.size() &&
            FilesIdentical(huf_back, raw_path, &error),
        "streaming HuffmanDecompressStream reads frozen (" + label + ")",
        error);
    error.clear();
    const bool dec_l =
        DecompressFileTo(lzh_path, lzh_back, workspace,
                         /*lzss=*/true, raw.size(), &written, &error);
    test_support::Check(
        dec_l && written == raw.size() &&
            FilesIdentical(lzh_back, raw_path, &error),
        "streaming LzssHuffmanDecompressStream reads frozen (" + label + ")",
        error);
  }
}

// ---- 2) 尺寸交叉校验（在大量解码之前失败）----------------------------------

void RunSizeChecks(const std::string& workdir, const std::string& workspace) {
  test_support::Section("size cross-checks happen before decoding");
  // 造一份像样的输入，这样"失败前 wrote 0 字节"才有说服力。
  std::string content;
  for (int index = 0; index < 40000; ++index) {
    content += "streaming size check line ";
    content += std::to_string(index % 53);
    content += "\n";
  }
  const std::string input_file = workdir + "/size.raw";
  test_support::Check(WriteAll(input_file, content), "write size-check input");
  const std::string huf_file = workdir + "/size.huf1";
  const std::string lzh_file = workdir + "/size.lzh1";
  std::uint64_t huf_bytes = 0;
  std::uint64_t lzh_bytes = 0;
  std::string error;
  test_support::Check(CompressFileTo(input_file, huf_file, workspace, false,
                                     &huf_bytes, &error) &&
                          huf_bytes > 0,
                      "huffman fixture for size checks", error);
  error.clear();
  test_support::Check(CompressFileTo(input_file, lzh_file, workspace, true,
                                     &lzh_bytes, &error) &&
                          lzh_bytes > 0,
                      "lzh1 fixture for size checks", error);

  // 头部信息读得到，而且与真实长度一致。
  compression::HuffmanStreamInfo huf_info;
  error.clear();
  test_support::Check(
      compression::HuffmanReadStreamInfo(huf_file, 0, &huf_info, &error) &&
          huf_info.original_size == content.size() &&
          huf_info.stream_bytes == huf_bytes,
      "HuffmanReadStreamInfo agrees with reality", error);
  compression::LzssHuffmanStreamInfo lzh_info;
  error.clear();
  test_support::Check(
      compression::LzssHuffmanReadStreamInfo(lzh_file, &lzh_info, &error) &&
          lzh_info.original_size == content.size() &&
          lzh_info.inner.original_size == lzh_info.token_stream_size,
      "LzssHuffmanReadStreamInfo agrees with reality", error);

  // 期望长度与实际不符：必须在写出任何字节之前失败。
  {
    FileSink sink;
    error.clear();
    const std::string out = workdir + "/size-wrong.out";
    const bool opened = sink.Open(out, &error);
    std::uint64_t written = 0;
    const bool ok = opened && compression::HuffmanDecompressStream(
                                  huf_file, 0, &sink, content.size() + 4096,
                                  &written, &error);
    test_support::Check(!ok, "huffman rejects a wrong expected size");
    test_support::Check(sink.bytes_written() == 0,
                        "huffman wrote nothing before the size check",
                        std::to_string(sink.bytes_written()));
    sink.Abandon();
  }
  {
    FileSink sink;
    error.clear();
    const std::string out = workdir + "/size-wrong2.out";
    const bool opened = sink.Open(out, &error);
    std::uint64_t written = 0;
    const bool ok = opened && compression::LzssHuffmanDecompressStream(
                                  lzh_file, &sink, workspace,
                                  content.size() + 4096, &written, &error);
    test_support::Check(!ok, "lzh1 rejects a wrong expected size");
    test_support::Check(sink.bytes_written() == 0,
                        "lzh1 wrote nothing before the size check",
                        std::to_string(sink.bytes_written()));
    sink.Abandon();
  }
  // 正常长度必须成功——否则上面两条"失败"证明不了任何东西。
  {
    const std::string out = workdir + "/size-ok.out";
    std::uint64_t written = 0;
    error.clear();
    const bool ok = DecompressFileTo(huf_file, out, workspace, false,
                                     content.size(), &written, &error) &&
                    FilesIdentical(out, input_file, &error);
    test_support::Check(ok, "huffman accepts the correct expected size", error);
  }
}

// ---- 3) 坏流：都不许写出半成品 ---------------------------------------------

void RunMalformedStreams(const std::string& workdir,
                         const std::string& workspace) {
  test_support::Section("malformed streams never leave partial output");
  std::string content;
  for (int index = 0; index < 20000; ++index) {
    content += "malformed stream payload ";
    content += std::to_string(index % 17);
    content += "\n";
  }
  const std::string input_file = workdir + "/bad.raw";
  test_support::Check(WriteAll(input_file, content), "write malformed input");
  const std::string huf_file = workdir + "/bad.huf1";
  const std::string lzh_file = workdir + "/bad.lzh1";
  std::uint64_t produced = 0;
  std::string error;
  CompressFileTo(input_file, huf_file, workspace, false, &produced, &error);
  error.clear();
  CompressFileTo(input_file, lzh_file, workspace, true, &produced, &error);

  const std::string huf = ReadAll(huf_file);
  const std::string lzh = ReadAll(lzh_file);

  struct Case {
    const char* label;
    std::string bytes;
    bool lzss;
  };
  std::vector<Case> cases;
  cases.push_back({"huf1 truncated header", huf.substr(0, 100), false});
  cases.push_back(
      {"huf1 truncated payload", huf.substr(0, huf.size() - 1), false});
  cases.push_back({"huf1 trailing byte", huf + "X", false});
  cases.push_back({"lzh1 truncated header", lzh.substr(0, 10), true});
  cases.push_back(
      {"lzh1 truncated payload", lzh.substr(0, lzh.size() - 1), true});
  cases.push_back({"lzh1 trailing byte", lzh + "X", true});
  // magic 破坏
  std::string broken = huf;
  broken[0] = 'X';
  cases.push_back({"huf1 bad magic", broken, false});
  broken = lzh;
  broken[0] = 'X';
  cases.push_back({"lzh1 bad magic", broken, true});
  // 内层 HUF1 的 original_size 被改小 -> 与外层 token_stream_size 不符
  broken = lzh;
  broken[20 + 4] = static_cast<char>(broken[20 + 4] ^ 0x01);
  cases.push_back({"lzh1 inner size mismatch", broken, true});

  for (const Case& item : cases) {
    const std::string path = workdir + "/bad-case.bin";
    // FileSink::Open 是 O_EXCL：每个 case 都要先把上一个 case 的文件清掉。
    test_support::RemoveTree(path);
    test_support::Check(WriteAll(path, item.bytes),
                        std::string("write case ") + item.label);
    const std::string out = workdir + "/bad-case.out";
    test_support::RemoveTree(out);
    FileSink sink;
    error.clear();
    if (!sink.Open(out, &error)) {
      test_support::Check(false, std::string("open out for ") + item.label,
                          error);
      continue;
    }
    std::uint64_t written = 0;
    const bool ok =
        item.lzss
            ? compression::LzssHuffmanDecompressStream(
                  path, &sink, workspace, content.size(), &written, &error)
            : compression::HuffmanDecompressStream(
                  path, 0, &sink, content.size(), &written, &error);
    test_support::Check(!ok, std::string("rejected: ") + item.label, error);
    sink.Abandon();
    test_support::Check(!test_support::Exists(out),
                        std::string("no leftover output: ") + item.label);
  }
}

// ---- 4) 确定性与较大的往返 --------------------------------------------------

void RunStreamingRoundTrip(const std::string& workdir,
                           const std::string& workspace) {
  test_support::Section("streaming round trip and determinism");
  // 8 MiB 混合内容：可压缩的文本段 + 不可压缩的伪随机段。
  std::string content;
  content.reserve(8u << 20);
  std::uint64_t state = 0x12345678ull;
  for (std::size_t index = 0; index < (8u << 20); ++index) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    if ((index / 65536) % 2 == 0) {
      content.push_back(static_cast<char>('a' + (index % 26)));
    } else {
      content.push_back(static_cast<char>(state >> 33));
    }
  }
  const std::string input_file = workdir + "/big.raw";
  test_support::Check(WriteAll(input_file, content),
                      "write 8 MiB streaming input");

  for (int pass = 0; pass < 2; ++pass) {
    const bool lzss = pass == 1;
    const std::string label = lzss ? "lzh1" : "huffman";
    const std::string packed = workdir + "/big-" + label + ".bin";
    const std::string second = workdir + "/big-" + label + "-2.bin";
    const std::string restored = workdir + "/big-" + label + ".out";
    std::uint64_t produced = 0;
    std::string error;
    const bool ok =
        CompressFileTo(input_file, packed, workspace, lzss, &produced, &error);
    test_support::Check(ok && produced > 0, label + " streaming compress",
                        error);
    if (!ok) {
      continue;
    }
    error.clear();
    std::uint64_t again = 0;
    const bool repeat =
        CompressFileTo(input_file, second, workspace, lzss, &again, &error);
    test_support::Check(
        repeat && again == produced && FilesIdentical(packed, second, &error),
        label + " streaming compression is deterministic", error);
    std::uint64_t written = 0;
    error.clear();
    const bool back = DecompressFileTo(packed, restored, workspace, lzss,
                                       content.size(), &written, &error);
    test_support::Check(back && written == content.size() &&
                            FilesIdentical(restored, input_file, &error),
                        label + " streaming round trip", error);
  }
}

// 手工拼一条 LZH1：magic + original_size + token_stream_size + 内层 HUF1。
// 用它来构造"编码器永远不会产出、但解码器必须拒绝"的流。
std::string MakeLzh1(std::uint64_t original_size,
                     std::uint64_t token_stream_size,
                     const std::string& inner) {
  std::string out;
  out.append("LZH1", 4);
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((original_size >> shift) & 0xFF));
  }
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((token_stream_size >> shift) & 0xFF));
  }
  out += inner;
  return out;
}

// ---- 5) 非规范 padding 必须被拒绝 -------------------------------------------

void RunNoncanonicalPadding(const std::string& workdir,
                            const std::string& fixtures_root,
                            const std::string& workspace) {
  test_support::Section("noncanonical padding bits are rejected");

  // --- HUF1：bit_count 不是 8 的倍数时，最后一个字节里没被覆盖的低位必须是 0。
  const char* names[] = {"empty", "single-symbol", "text", "all-256-symbols",
                         "repetitive-binary"};
  int huf_cases = 0;
  for (const char* name : names) {
    const std::string label = name;
    const std::string huf_path = fixtures_root + "/frozen/" + label + ".huf1";
    if (!test_support::Exists(huf_path)) {
      continue;
    }
    compression::HuffmanStreamInfo info;
    std::string error;
    if (!compression::HuffmanReadStreamInfo(huf_path, 0, &info, &error)) {
      continue;
    }
    if (info.bit_count % 8 == 0) {
      continue;  // 这条 fixture 是字节对齐的，没有 padding 位可改
    }
    const std::string pristine = ReadAll(huf_path);
    std::string clean;
    error.clear();
    test_support::Check(
        compression::HuffmanDecompress(pristine, &clean, &error),
        "pristine HUF1 still decodes (" + label + ")", error);

    std::string dirty = pristine;
    // 最低位一定是 padding 位（只在 bit_count % 8 != 0 时才会走到这里）。
    dirty.back() =
        static_cast<char>(static_cast<unsigned char>(dirty.back()) | 0x01);

    std::string out;
    error.clear();
    const bool memory_ok = compression::HuffmanDecompress(dirty, &out, &error);
    test_support::Check(
        !memory_ok && error.find("padding") != std::string::npos,
        "memory HuffmanDecompress rejects non-zero padding (" + label + ")",
        error);

    const std::string dirty_path =
        workdir + "/dirty-padding-" + label + ".huf1";
    const std::string dirty_out = workdir + "/dirty-padding-" + label + ".out";
    test_support::Check(WriteAll(dirty_path, dirty),
                        "write dirty HUF1 (" + label + ")");
    test_support::RemoveTree(dirty_out);
    FileSink sink;
    error.clear();
    std::uint64_t written = 0;
    const bool opened = sink.Open(dirty_out, &error);
    const bool stream_ok =
        opened && compression::HuffmanDecompressStream(
                      dirty_path, 0, &sink, clean.size(), &written, &error);
    test_support::Check(
        !stream_ok && error.find("padding") != std::string::npos,
        "stream HuffmanDecompressStream rejects non-zero padding (" + label +
            ")",
        error);
    sink.Abandon();
    ++huf_cases;
  }
  test_support::Check(huf_cases > 0,
                      "at least one frozen fixture exercises HUF1 padding bits",
                      std::to_string(huf_cases));

  // --- LZSS：最后一个 control byte 里没用到的低位必须是 0。
  const std::size_t token_counts[] = {1, 3, 7};
  for (const std::size_t tokens : token_counts) {
    const std::string label = std::to_string(tokens) + "-token-final-group";
    std::uint32_t control = 0;
    for (std::size_t index = 0; index < tokens; ++index) {
      control |= std::uint32_t{1} << (7 - index);
    }
    std::string token_stream;
    token_stream.push_back(static_cast<char>(control));
    for (std::size_t index = 0; index < tokens; ++index) {
      token_stream.push_back(static_cast<char>('A' + static_cast<int>(index)));
    }
    const std::uint64_t original_size = tokens;

    std::string clean;
    std::string error;
    test_support::Check(
        compression::LzssDecode(token_stream, original_size, &clean, &error) &&
            clean.size() == tokens,
        "pristine token stream decodes (" + label + ")", error);

    std::string dirty = token_stream;
    dirty[0] = static_cast<char>(static_cast<unsigned char>(dirty[0]) | 0x01);
    std::string out;
    error.clear();
    const bool memory_ok =
        compression::LzssDecode(dirty, original_size, &out, &error);
    test_support::Check(
        !memory_ok && error.find("control bits") != std::string::npos,
        "memory LzssDecode rejects dirty control bits (" + label + ")", error);

    // LZH1 路径：手工包一层外层头部 + 内层 HUF1。
    std::string inner;
    error.clear();
    test_support::Check(
        compression::HuffmanCompress(token_stream, &inner, &error),
        "inner HUF1 built (" + label + ")", error);
    const std::string lzh1 =
        MakeLzh1(original_size, token_stream.size(), inner);
    std::string pristine_back;
    error.clear();
    test_support::Check(
        compression::LzssHuffmanDecompress(lzh1, &pristine_back, &error) &&
            pristine_back == clean,
        "pristine LZH1 decodes (" + label + ")", error);

    std::string dirty_inner;
    error.clear();
    compression::HuffmanCompress(dirty, &dirty_inner, &error);
    const std::string dirty_lzh1 =
        MakeLzh1(original_size, dirty.size(), dirty_inner);
    std::string dirty_back;
    error.clear();
    const bool lzh_ok =
        compression::LzssHuffmanDecompress(dirty_lzh1, &dirty_back, &error);
    test_support::Check(
        !lzh_ok && error.find("control bits") != std::string::npos,
        "LZH1 rejects dirty control bits (" + label + ")", error);

    const std::string dirty_path =
        workdir + "/dirty-control-" + std::to_string(tokens) + ".lzh1";
    const std::string dirty_out =
        workdir + "/dirty-control-" + std::to_string(tokens) + ".out";
    test_support::Check(WriteAll(dirty_path, dirty_lzh1),
                        "write dirty LZH1 (" + label + ")");
    test_support::RemoveTree(dirty_out);
    FileSink sink;
    error.clear();
    std::uint64_t written = 0;
    const bool opened = sink.Open(dirty_out, &error);
    const bool stream_ok = opened && compression::LzssHuffmanDecompressStream(
                                         dirty_path, &sink, workspace,
                                         original_size, &written, &error);
    test_support::Check(
        !stream_ok && error.find("control bits") != std::string::npos,
        "stream LzssHuffmanDecompressStream rejects dirty control bits (" +
            label + ")",
        error);
    sink.Abandon();
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::printf("compression streaming test\n");
  const std::string workdir = test_support::FreshDir("compress-stream");
  const std::string fixtures_root =
      argc > 1 ? std::string(argv[1])
               : std::string("tests/fixtures/compression");

  TempDirectoryGuard workspace;
  std::string error;
  if (!workspace.Create(workdir, "work-", &error)) {
    std::printf("  FAIL  create workspace -- %s\n", error.c_str());
    return 1;
  }
  struct stat info;
  if (test_support::StatOf(workspace.path(), &info)) {
    test_support::Check((info.st_mode & 0777) == 0700,
                        "private workspace is 0700",
                        test_support::Octal(info.st_mode & 0777));
  }

  RunFrozenFixtures(workdir, fixtures_root, workspace.path());
  RunSizeChecks(workdir, workspace.path());
  RunMalformedStreams(workdir, workspace.path());
  RunStreamingRoundTrip(workdir, workspace.path());
  RunNoncanonicalPadding(workdir, fixtures_root, workspace.path());

  return test_support::Finish("compression-stream");
}

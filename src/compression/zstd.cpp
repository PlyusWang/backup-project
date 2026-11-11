// Handwritten RFC 8878 Zstandard subset. See docs/ZSTANDARD_FORMAT.md.
// Standard tools are validation oracles only; no third-party codec is linked.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#include "codec_io.h"
#include "compression.h"

namespace backupproject {
namespace compression {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kBlock = 65536;
constexpr std::uint64_t kWindowLimit = 8ULL * 1024 * 1024;
[[noreturn]] void Fail(const std::string& s) { throw std::runtime_error(s); }
unsigned Log2(std::uint64_t v) {
  if (!v) Fail("log2 zero");
  unsigned n = 0;
  while (v >>= 1) ++n;
  return n;
}
void Put(Bytes& b, std::uint64_t v, unsigned n) {
  for (unsigned i = 0; i < n; ++i)
    b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
struct Cursor {
  const Bytes& b;
  std::size_t p = 0;
  std::uint64_t Get(unsigned n) {
    if (n > b.size() - p) Fail("truncated field");
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= std::uint64_t(b[p++]) << (8 * i);
    return v;
  }
  Bytes Take(std::size_t n) {
    if (n > b.size() - p) Fail("truncated section");
    Bytes v(b.begin() + p, b.begin() + p + n);
    p += n;
    return v;
  }
  std::size_t Left() const { return b.size() - p; }
};
struct Bits {
  Bytes bytes;
  std::size_t count = 0;
  void Add(std::uint64_t value, unsigned n) {
    if (n > 32 || (n < 32 && value >= (1ULL << n))) Fail("bit writer overflow");
    for (unsigned i = 0; i < n; ++i) {
      if (count % 8 == 0) bytes.push_back(0);
      bytes[count / 8] |= ((value >> i) & 1U) << (count % 8);
      ++count;
    }
  }
  Bytes Finish() {
    Add(1, 1);
    return bytes;
  }
};
// Zstandard entropy streams are consumed backwards from an explicit stop bit.
struct Reverse {
  const Bytes& b;
  std::size_t bits;
  explicit Reverse(const Bytes& v) : b(v), bits(0) {
    if (b.empty() || b.back() == 0) Fail("missing bitstream stop bit");
    bits = (b.size() - 1) * 8 + Log2(b.back());
  }
  std::uint32_t Get(unsigned n) {
    if (n > 32 || n > bits) Fail("truncated reverse bits");
    std::uint32_t v = 0;
    const auto begin = bits - n;
    for (unsigned i = 0; i < n; ++i)
      v |= ((b[(begin + i) / 8] >> ((begin + i) % 8)) & 1U) << i;
    bits -= n;
    return v;
  }
};
struct FseEntry {
  unsigned symbol = 0, bits = 0, base = 0;
};
// RFC normalized counts define a deterministic symbol spread and decode state.
// Encoding walks that same table backwards instead of maintaining another
// model.
struct Fse {
  unsigned log;
  std::vector<FseEntry> d;
  explicit Fse(const std::vector<int>& norm, unsigned accuracy)
      : log(accuracy), d(1U << accuracy) {
    const unsigned size = 1U << log;
    unsigned total = 0;
    for (int n : norm) {
      if (n < -1) Fail("bad normalized count");
      total += n < 0 ? 1 : n;
    }
    if (total != size) Fail("bad normalized sum");
    unsigned high = size - 1;
    std::vector<unsigned> next(norm.size());
    for (unsigned s = 0; s < norm.size(); ++s) {
      if (norm[s] == -1) {
        d[high--].symbol = s;
        next[s] = 1;
      } else
        next[s] = norm[s];
    }
    unsigned p = 0;
    const unsigned step = (size >> 1) + (size >> 3) + 3;
    for (unsigned s = 0; s < norm.size(); ++s)
      for (int i = 0; i < norm[s]; ++i) {
        d[p].symbol = s;
        do {
          p = (p + step) & (size - 1);
        } while (p > high);
      }
    if (p != 0) Fail("incomplete FSE spread");
    for (auto& cell : d) {
      const unsigned n = next[cell.symbol]++;
      cell.bits = log - Log2(n);
      cell.base = (n << cell.bits) - size;
    }
  }
  unsigned First(unsigned symbol) const {
    for (unsigned i = 0; i < d.size(); ++i)
      if (d[i].symbol == symbol) return i;
    Fail("symbol absent from FSE table");
  }
  unsigned Previous(unsigned symbol, unsigned future, Bits& out) const {
    for (unsigned i = 0; i < d.size(); ++i) {
      const auto& c = d[i];
      if (c.symbol == symbol && future >= c.base &&
          future - c.base < (1U << c.bits)) {
        out.Add(future - c.base, c.bits);
        return i;
      }
    }
    Fail("no inverse FSE transition");
  }
  unsigned Update(unsigned state, Reverse& in) const {
    if (state >= d.size()) Fail("FSE state overflow");
    return d[state].base + in.Get(d[state].bits);
  }
};
const std::vector<int> kLL = {4, 3, 2, 2, 2, 2, 2, 2, 2,  2,  2,  2,
                              2, 1, 1, 1, 2, 2, 2, 2, 2,  2,  2,  2,
                              2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const std::vector<int> kML = {1, 4, 3, 2, 2,  2,  2,  2,  2,  1,  1, 1, 1, 1,
                              1, 1, 1, 1, 1,  1,  1,  1,  1,  1,  1, 1, 1, 1,
                              1, 1, 1, 1, 1,  1,  1,  1,  1,  1,  1, 1, 1, 1,
                              1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
const std::vector<int> kOF = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1,  1,  1,  1,  1, 1,
                              1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};
const std::vector<unsigned> kLLBase = {
    0,  1,  2,   3,   4,   5,    6,    7,    8,    9,     10,    11,
    12, 13, 14,  15,  16,  18,   20,   22,   24,   28,    32,    40,
    48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const std::vector<unsigned> kLLBits = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  1,  1,
    1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const std::vector<unsigned> kMLBase = {
    3,  4,   5,   6,   7,    8,    9,    10,   11,    12,    13,   14, 15, 16,
    17, 18,  19,  20,  21,   22,   23,   24,   25,    26,    27,   28, 29, 30,
    31, 32,  33,  34,  35,   37,   39,   41,   43,    47,    51,   59, 67, 83,
    99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const std::vector<unsigned> kMLBits = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  1,  1,  1, 1,
    2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
unsigned LengthCode(unsigned v, const std::vector<unsigned>& base,
                    const std::vector<unsigned>& bits) {
  for (unsigned s = 0; s < base.size(); ++s)
    if (v >= base[s] && v - base[s] < (1U << bits[s])) return s;
  Fail("length not encodable");
}
struct Sequence {
  unsigned ll, ml, distance;
};
struct Parsed {
  Bytes literals;
  std::vector<Sequence> seq;
};
Parsed Parse(const Bytes& data) {
  Parsed out;
  std::array<int, 65536> hash;
  hash.fill(-1);
  std::size_t p = 0, anchor = 0;
  auto h = [&](std::size_t i) {
    std::uint32_t x =
        std::uint32_t(data[i]) | (std::uint32_t(data[i + 1]) << 8) |
        (std::uint32_t(data[i + 2]) << 16) | (std::uint32_t(data[i + 3]) << 24);
    return (x * 2654435761U) >> 16;
  };
  while (p + 4 <= data.size()) {
    const auto slot = h(p);
    const int previous = hash[slot];
    hash[slot] = static_cast<int>(p);
    if (previous < 0 ||
        std::memcmp(data.data() + previous, data.data() + p, 4) != 0) {
      ++p;
      continue;
    }
    std::size_t length = 4;
    while (p + length < data.size() &&
           data[previous + length] == data[p + length])
      ++length;
    out.literals.insert(out.literals.end(), data.begin() + anchor,
                        data.begin() + p);
    out.seq.push_back({static_cast<unsigned>(p - anchor),
                       static_cast<unsigned>(length),
                       static_cast<unsigned>(p - previous)});
    const auto end = p + length;
    for (++p; p < end; ++p)
      if (p + 4 <= data.size()) hash[h(p)] = static_cast<int>(p);
    anchor = p;
  }
  out.literals.insert(out.literals.end(), data.begin() + anchor, data.end());
  return out;
}
Bytes RawLiterals(const Bytes& data) {
  Bytes out;
  if (data.size() < 32)
    Put(out, data.size() << 3, 1);
  else if (data.size() < 4096)
    Put(out, (data.size() << 4) | 4, 2);
  else
    Put(out, (data.size() << 4) | 12, 3);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}
struct HuffCode {
  unsigned value = 0, length = 0;
};
struct HuffTree {
  std::array<HuffCode, 256> codes{};
  std::vector<unsigned> weights;
  unsigned depth = 0;
};
HuffTree FromWeights(std::vector<unsigned> weights) {
  std::uint64_t sum = 0;
  for (unsigned w : weights) {
    if (w > 11) Fail("Huffman weight exceeds11");
    if (w) sum += 1ULL << (w - 1);
  }
  if (!sum) Fail("empty Huffman tree");
  unsigned depth = Log2(sum) + 1;
  if (depth > 11) Fail("Huffman depth exceeds11");
  const auto rest = (1ULL << depth) - sum;
  if ((rest & (rest - 1)) || !rest) Fail("invalid final Huffman weight");
  weights.push_back(Log2(rest) + 1);
  // A complete Huffman tree has a nonzero even count of deepest leaves.
  // A Kraft sum alone would accept [2,2], whose claimed depth has no leaves.
  const auto deepest = std::count(weights.begin(), weights.end(), 1U);
  if (deepest == 0 || deepest % 2 != 0) Fail("invalid deepest Huffman rank");
  if (weights.size() > 256) Fail("too many literal symbols");
  HuffTree tree;
  tree.weights = weights;
  tree.depth = depth;
  std::vector<unsigned> order;
  for (unsigned s = 0; s < weights.size(); ++s)
    if (weights[s]) order.push_back(s);
  std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
    return weights[a] == weights[b] ? a < b : weights[a] < weights[b];
  });
  unsigned code = 0, length = depth;
  for (unsigned s : order) {
    const unsigned n = depth + 1 - weights[s];
    if (n > length) Fail("Huffman length order");
    code >>= length - n;
    length = n;
    tree.codes[s] = {code++, length};
  }
  if (code != (1U << length)) Fail("incomplete Huffman tree");
  return tree;
}
Bytes HuffLiterals(const Bytes& data) {
  if (data.empty() || *std::max_element(data.begin(), data.end()) > 128)
    return {};
  std::array<std::uint64_t, 256> freq{};
  for (auto x : data) ++freq[x];
  struct Node {
    std::uint64_t count;
    int left = -1, right = -1, symbol = -1;
  };
  std::vector<Node> nodes;
  auto cmp = [&](int a, int b) {
    return nodes[a].count == nodes[b].count ? a > b
                                            : nodes[a].count > nodes[b].count;
  };
  std::priority_queue<int, std::vector<int>, decltype(cmp)> q(cmp);
  for (unsigned s = 0; s < 256; ++s)
    if (freq[s]) {
      nodes.push_back({freq[s], -1, -1, static_cast<int>(s)});
      q.push(nodes.size() - 1);
    }
  if (q.size() < 2) return {};
  while (q.size() > 1) {
    int a = q.top();
    q.pop();
    int b = q.top();
    q.pop();
    nodes.push_back({nodes[a].count + nodes[b].count, a, b, -1});
    q.push(nodes.size() - 1);
  }
  std::array<unsigned, 256> lengths{};
  unsigned depth = 0;
  auto walk = [&](auto&& self, int n, unsigned d) -> void {
    const auto& node = nodes[n];
    if (node.symbol >= 0) {
      lengths[node.symbol] = d;
      depth = std::max(depth, d);
    } else {
      self(self, node.left, d + 1);
      self(self, node.right, d + 1);
    }
  };
  walk(walk, q.top(), 0);
  if (depth > 11) return {};
  unsigned last = 255;
  while (!lengths[last]) --last;
  std::vector<unsigned> weights(last);
  for (unsigned s = 0; s < last; ++s)
    if (lengths[s]) weights[s] = depth + 1 - lengths[s];
  const HuffTree tree = FromWeights(weights);
  Bytes body;
  body.push_back(127 + weights.size());
  for (unsigned i = 0; i < weights.size(); i += 2)
    body.push_back((weights[i] << 4) |
                   (i + 1 < weights.size() ? weights[i + 1] : 0));
  // One Huffman stream requires both size fields below1024. Longer literals
  // use four independent backwards bitstreams and their standard jump table.
  const bool four = data.size() >= 1024;
  std::vector<Bytes> streams;
  const std::size_t chunk = four ? (data.size() + 3) / 4 : data.size();
  for (std::size_t start = 0; start < data.size(); start += chunk) {
    const auto end = std::min(data.size(), start + chunk);
    Bits bits;
    for (std::size_t i = end; i > start; --i) {
      auto c = tree.codes[data[i - 1]];
      bits.Add(c.value, c.length);
    }
    streams.push_back(bits.Finish());
  }
  if (four) {
    if (streams.size() != 4) return {};
    for (unsigned i = 0; i < 3; ++i) Put(body, streams[i].size(), 2);
  }
  for (const auto& b : streams) body.insert(body.end(), b.begin(), b.end());
  if (!four && body.size() >= 1024) return {};
  Bytes out;
  if (!four && body.size() < 1024)
    Put(out, 2 | (data.size() << 4) | (body.size() << 14), 3);
  else if (data.size() < 16384 && body.size() < 16384)
    Put(out, 2 | (2 << 2) | (data.size() << 4) | (body.size() << 18), 4);
  else if (data.size() < 262144 && body.size() < 262144)
    Put(out,
        2 | (3 << 2) | (std::uint64_t(data.size()) << 4) |
            (std::uint64_t(body.size()) << 22),
        5);
  else
    return {};
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
Bytes EncodeBlock(const Bytes& data, std::uint64_t& sequences,
                  std::uint64_t& huffman) {
  const Parsed p = Parse(data);
  Bytes out = RawLiterals(p.literals);
  Bytes h = HuffLiterals(p.literals);
  if (!h.empty() && h.size() < out.size()) {
    out = std::move(h);
    ++huffman;
  }
  const unsigned count = p.seq.size();
  if (count < 128)
    Put(out, count, 1);
  else if (count < 0x7f00) {
    out.push_back((count >> 8) + 128);
    out.push_back(count & 255);
  } else {
    out.push_back(255);
    Put(out, count - 0x7f00, 2);
  }
  if (!count) return out;
  ++sequences;
  out.push_back(0);
  const Fse ll(kLL, 6), ml(kML, 6), of(kOF, 5);
  Bits bits;
  struct Codes {
    unsigned ll, ml, of;
  };
  std::vector<Codes> codes;
  for (const auto& s : p.seq)
    codes.push_back({LengthCode(s.ll, kLLBase, kLLBits),
                     LengthCode(s.ml, kMLBase, kMLBits), Log2(s.distance + 3)});
  unsigned sl = ll.First(codes.back().ll), sm = ml.First(codes.back().ml),
           so = of.First(codes.back().of);
  for (std::size_t i = count; i-- > 0;) {
    const auto c = codes[i];
    const auto s = p.seq[i];
    if (i + 1 < count) {
      so = of.Previous(c.of, so, bits);
      sm = ml.Previous(c.ml, sm, bits);
      sl = ll.Previous(c.ll, sl, bits);
    }
    bits.Add(s.ll - kLLBase[c.ll], kLLBits[c.ll]);
    bits.Add(s.ml - kMLBase[c.ml], kMLBits[c.ml]);
    bits.Add(s.distance + 3 - (1U << c.of), c.of);
  }
  bits.Add(sm, ml.log);
  bits.Add(so, of.log);
  bits.Add(sl, ll.log);
  auto stream = bits.Finish();
  out.insert(out.end(), stream.begin(), stream.end());
  return out;
}
// Only bounded blocks are buffered; publication belongs to the caller's
// FileSink transaction. No codec path opens, renames or deletes the user's
// destination.
class Sink {
 public:
  ByteSinkAdapter adapter;
  std::uint64_t bytes = 0;
  void Write(const Bytes& data) {
    std::string error;
    if (!adapter.Write(data.data(), data.size(), &error)) Fail(error);
    bytes += data.size();
  }
};
Bytes ReadExact(SequentialReader& in, std::size_t n) {
  if (n > in.remaining()) Fail("truncated input stream");
  Bytes b(n);
  std::string error;
  if (!in.ReadExact(b.data(), n, &error)) Fail(error);
  return b;
}
Bytes ReadBlock(SequentialReader& in) {
  return ReadExact(in, std::min<std::uint64_t>(kBlock, in.remaining()));
}
// Streaming XXH64 with seed zero, following xxhash_spec.md (XXH64 steps 1-7).
// Zstandard stores the low 32 digest bits; this is an error detector, not a
// MAC.
class Hash64 {
  static constexpr std::uint64_t p1 = 0x9e3779b185ebca87ULL;
  static constexpr std::uint64_t p2 = 0xc2b2ae3d27d4eb4fULL;
  static constexpr std::uint64_t p3 = 0x165667b19e3779f9ULL;
  static constexpr std::uint64_t p4 = 0x85ebca77c2b2ae63ULL;
  static constexpr std::uint64_t p5 = 0x27d4eb2f165667c5ULL;
  std::array<std::uint64_t, 4> lanes = {p1 + p2, p2, 0, 0ULL - p1};
  std::array<std::uint8_t, 32> pending{};
  std::size_t used = 0;
  std::uint64_t total = 0;
  static std::uint64_t Rotate(std::uint64_t x, unsigned n) {
    return (x << n) | (x >> (64 - n));
  }
  static std::uint64_t Little(const std::uint8_t* b, unsigned n) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i) value |= std::uint64_t(b[i]) << (8 * i);
    return value;
  }
  static std::uint64_t Round(std::uint64_t acc, std::uint64_t value) {
    return Rotate(acc + value * p2, 31) * p1;
  }
  void Stripe(const std::uint8_t* b) {
    for (unsigned i = 0; i < 4; ++i)
      lanes[i] = Round(lanes[i], Little(b + 8 * i, 8));
  }

 public:
  void Add(const Bytes& b) {
    if (b.size() > std::numeric_limits<std::uint64_t>::max() - total)
      Fail("XXH64 input length overflow");
    total += b.size();
    std::size_t cursor = 0;
    if (used) {
      const auto n = std::min(b.size(), pending.size() - used);
      std::copy_n(b.begin(), n, pending.begin() + used);
      used += n;
      cursor += n;
      if (used == pending.size()) {
        Stripe(pending.data());
        used = 0;
      }
    }
    while (b.size() - cursor >= 32) {
      Stripe(b.data() + cursor);
      cursor += 32;
    }
    if (cursor < b.size()) {
      used = b.size() - cursor;
      std::copy(b.begin() + cursor, b.end(), pending.begin());
    }
  }
  std::uint64_t Digest() const {
    std::uint64_t acc = p5;
    if (total >= 32) {
      acc = Rotate(lanes[0], 1) + Rotate(lanes[1], 7) + Rotate(lanes[2], 12) +
            Rotate(lanes[3], 18);
      for (auto lane : lanes) acc = (acc ^ Round(0, lane)) * p1 + p4;
    }
    acc += total;
    std::size_t cursor = 0;
    while (used - cursor >= 8) {
      acc =
          Rotate(acc ^ Round(0, Little(pending.data() + cursor, 8)), 27) * p1 +
          p4;
      cursor += 8;
    }
    if (used - cursor >= 4) {
      acc =
          Rotate(acc ^ (Little(pending.data() + cursor, 4) * p1), 23) * p2 + p3;
      cursor += 4;
    }
    while (cursor < used)
      acc = Rotate(acc ^ (std::uint64_t(pending[cursor++]) * p5), 11) * p1;
    acc = (acc ^ (acc >> 33)) * p2;
    acc = (acc ^ (acc >> 29)) * p3;
    return acc ^ (acc >> 32);
  }
};

void Encode(SequentialReader& in, Sink& out) {
  Hash64 checksum;
  // Non-single-segment frame, 128 KiB declared window, explicit 64-bit content
  // size and content checksum. This permits restore to reject a wrong length
  // before producing output, independently of the outer authenticated header.
  Bytes frame = {0x28, 0xb5, 0x2f, 0xfd, 0xc4, 0x38};
  Put(frame, in.remaining(), 8);
  out.Write(frame);
  Bytes current = ReadBlock(in);
  for (;;) {
    Bytes next = ReadBlock(in);
    const bool last = next.empty();
    std::uint64_t sc = 0, hc = 0;
    checksum.Add(current);
    Bytes payload = EncodeBlock(current, sc, hc);
    unsigned type = 2;
    if (payload.size() >= current.size()) {
      type = 0;
      payload = current;
    }
    Bytes header;
    Put(header, (payload.size() << 3) | (type << 1) | (last ? 1 : 0), 3);
    out.Write(header);
    out.Write(payload);
    if (last) break;
    current = std::move(next);
  }
  Bytes footer;
  Put(footer, checksum.Digest(), 4);
  out.Write(footer);
}
Bytes DecodeLiterals(Cursor& c) {
  const auto first = c.Get(1);
  const unsigned type = first & 3, format = (first >> 2) & 3;
  std::uint64_t size = 0, compressed = 0;
  bool four = false;
  if (type < 2) {
    if ((format & 1) == 0)
      size = first >> 3;
    else if (format == 1)
      size = (first >> 4) | (c.Get(1) << 4);
    else
      size = (first >> 4) | (c.Get(2) << 4);
    if (size > 131072) Fail("literal size exceeds block bound");
    if (type == 0) return c.Take(size);
    const auto value = c.Get(1);
    return Bytes(size, value);
  }
  if (type == 3) Fail("unsupported: treeless Huffman literals");
  if (format < 2) {
    auto value = first | (c.Get(2) << 8);
    size = (value >> 4) & 1023;
    compressed = (value >> 14) & 1023;
    four = format == 1;
  } else if (format == 2) {
    auto value = first | (c.Get(3) << 8);
    size = (value >> 4) & 16383;
    compressed = (value >> 18) & 16383;
    four = true;
  } else {
    auto value = first | (c.Get(4) << 8);
    size = (value >> 4) & 262143;
    compressed = (value >> 22) & 262143;
    four = true;
  }
  // The four-stream literal format requires at least six regenerated bytes
  // (Zstandard format 0.4.5, literals section). Check before size - 3*chunk.
  if (four && size < 6) Fail("four-stream Huffman literals require six bytes");
  if (size > 131072 || compressed > 131072)
    Fail("Huffman section exceeds block bound");
  Bytes data = c.Take(compressed);
  Cursor section{data};
  const auto header = section.Get(1);
  if (header < 128) Fail("unsupported: FSE-compressed Huffman weights");
  const unsigned count = header - 127;
  std::vector<unsigned> weights;
  for (unsigned i = 0; i < count; i += 2) {
    const unsigned pair = section.Get(1);
    weights.push_back(pair >> 4);
    if (i + 1 < count) weights.push_back(pair & 15);
  }
  const auto tree = FromWeights(weights);
  std::vector<unsigned> lengths;
  if (four) {
    std::uint64_t sum = 0;
    for (unsigned i = 0; i < 3; ++i) {
      unsigned n = section.Get(2);
      lengths.push_back(n);
      sum += n;
    }
    if (sum > section.Left()) Fail("Huffman jump table overflow");
    lengths.push_back(section.Left() - sum);
  } else
    lengths.push_back(section.Left());
  Bytes out;
  const std::size_t chunk = (size + 3) / 4;
  for (unsigned stream = 0; stream < lengths.size(); ++stream) {
    Bytes b = section.Take(lengths[stream]);
    Reverse bits(b);
    const auto wanted = four ? (stream == 3 ? size - 3 * chunk : chunk) : size;
    for (std::size_t i = 0; i < wanted; ++i) {
      unsigned code = 0;
      bool found = false;
      for (unsigned n = 1; n <= tree.depth && !found; ++n) {
        code = (code << 1) | bits.Get(1);
        for (unsigned s = 0; s < tree.weights.size(); ++s)
          if (tree.codes[s].length == n && tree.codes[s].value == code) {
            out.push_back(s);
            found = true;
            break;
          }
      }
      if (!found) Fail("invalid Huffman code");
    }
    if (bits.bits) Fail("unused Huffman bits");
  }
  if (out.size() != size || section.Left()) Fail("Huffman length mismatch");
  return out;
}
struct Table {
  Fse fse;
  int rle = -1;
  Table(const std::vector<int>& norm, unsigned log) : fse(norm, log) {}
};
Table ReadTable(Cursor& c, unsigned mode, const std::vector<int>& norm,
                unsigned log, unsigned max) {
  Table t(norm, log);
  if (mode == 1) {
    t.rle = c.Get(1);
    if (static_cast<unsigned>(t.rle) > max) Fail("RLE symbol exceeds alphabet");
  } else if (mode != 0)
    Fail("unsupported: dynamic or repeated FSE sequence table");
  return t;
}
class History {
  Bytes ring;
  std::size_t pos = 0;
  std::uint64_t count = 0;

 public:
  explicit History(std::size_t window)
      : ring(std::max(std::size_t(1), window)) {}
  std::uint8_t Back(std::size_t distance) const {
    if (!distance || distance > count || distance > ring.size())
      Fail("match exceeds window/history");
    return ring[(pos + ring.size() - distance) % ring.size()];
  }
  void Append(const Bytes& b) {
    for (auto v : b) {
      ring[pos] = v;
      pos = (pos + 1) % ring.size();
      ++count;
    }
  }
  std::uint64_t Count() const { return count; }
  std::size_t Window() const { return ring.size(); }
};
Bytes DecodeBlock(const Bytes& input, History& history,
                  std::array<unsigned, 3>& rep, std::size_t block_limit) {
  Cursor c{input};
  Bytes literals = DecodeLiterals(c);
  unsigned count = c.Get(1);
  if (count == 255)
    count = c.Get(2) + 0x7f00;
  else if (count >= 128)
    count = ((count - 128) << 8) + c.Get(1);
  if (count > block_limit / 3) Fail("sequence count exceeds output bound");
  if (!count) {
    if (c.Left() || literals.size() > block_limit)
      Fail("trailing zero-sequence data");
    return literals;
  }
  const unsigned modes = c.Get(1);
  if (modes & 3) Fail("sequence reserved bits set");
  Table ll = ReadTable(c, modes >> 6, kLL, 6, 35),
        of = ReadTable(c, (modes >> 4) & 3, kOF, 5, 31),
        ml = ReadTable(c, (modes >> 2) & 3, kML, 6, 52);
  Bytes encoded = c.Take(c.Left());
  Reverse bits(encoded);
  unsigned sl = ll.rle < 0 ? bits.Get(ll.fse.log) : 0,
           so = of.rle < 0 ? bits.Get(of.fse.log) : 0,
           sm = ml.rle < 0 ? bits.Get(ml.fse.log) : 0;
  Bytes out;
  std::size_t lp = 0;
  for (unsigned seq = 0; seq < count; ++seq) {
    const unsigned lc = ll.rle >= 0 ? ll.rle : ll.fse.d.at(sl).symbol,
                   oc = of.rle >= 0 ? of.rle : of.fse.d.at(so).symbol,
                   mc = ml.rle >= 0 ? ml.rle : ml.fse.d.at(sm).symbol;
    const std::uint64_t oval = (1ULL << oc) + bits.Get(oc);
    const unsigned match = kMLBase.at(mc) + bits.Get(kMLBits.at(mc)),
                   literal = kLLBase.at(lc) + bits.Get(kLLBits.at(lc));
    if (literal > literals.size() - lp || literal > block_limit - out.size())
      Fail("literal copy exceeds section/block");
    out.insert(out.end(), literals.begin() + lp,
               literals.begin() + lp + literal);
    lp += literal;
    std::uint64_t distance = 0;
    if (oval > 3) {
      distance = oval - 3;
      rep = {static_cast<unsigned>(distance), rep[0], rep[1]};
    } else {
      const unsigned which = static_cast<unsigned>(oval) - 1 + (literal == 0);
      if (which == 3) {
        if (rep[0] <= 1) Fail("zero repeated distance");
        distance = rep[0] - 1;
        rep = {static_cast<unsigned>(distance), rep[0], rep[1]};
      } else {
        distance = rep.at(which);
        for (unsigned i = which; i > 0; --i) rep[i] = rep[i - 1];
        rep[0] = distance;
      }
    }
    if (!distance || distance > history.Window() ||
        distance > history.Count() + out.size() ||
        match > block_limit - out.size())
      Fail("match copy exceeds window/output bound");
    for (unsigned i = 0; i < match; ++i)
      out.push_back(distance <= out.size()
                        ? out[out.size() - distance]
                        : history.Back(distance - out.size()));
    if (seq + 1 < count) {
      if (ll.rle < 0) sl = ll.fse.Update(sl, bits);
      if (ml.rle < 0) sm = ml.fse.Update(sm, bits);
      if (of.rle < 0) so = of.fse.Update(so, bits);
    }
  }
  if (literals.size() - lp > block_limit - out.size())
    Fail("remaining literals overflow");
  out.insert(out.end(), literals.begin() + lp, literals.end());
  if (bits.bits) Fail("unused sequence bits");
  return out;
}
ZstdStreamInfo ReadHeader(SequentialReader& in) {
  ZstdStreamInfo info;
  Bytes header = ReadExact(in, 5);
  if (Bytes(header.begin(), header.begin() + 4) !=
      Bytes{0x28, 0xb5, 0x2f, 0xfd})
    Fail("not a Zstandard frame");
  const unsigned desc = header[4];
  if (desc & 8) Fail("reserved frame bit set");
  const bool has_checksum = desc & 4;
  if (desc & 3) Fail("unsupported: dictionary identifier");
  const bool single = desc & 32;
  std::uint64_t window = 0, content = 0;
  const unsigned flag = desc >> 6;
  const unsigned sizes[4] = {0, 2, 4, 8};
  unsigned fcs = sizes[flag];
  if (single && !flag) fcs = 1;
  if (!single) {
    const auto wd = ReadExact(in, 1)[0];
    const unsigned exponent = 10 + (wd >> 3);
    if (exponent > 23) Fail("window above8MiB policy");
    window = (1ULL << exponent) + ((1ULL << exponent) / 8) * (wd & 7);
  }
  if (fcs) {
    Bytes b = ReadExact(in, fcs);
    Cursor c{b};
    content = c.Get(fcs);
    if (fcs == 2) content += 256;
  }
  if (single) window = content;
  if (window > kWindowLimit) Fail("window above8MiB policy");
  info.has_content_size = fcs != 0;
  info.original_size = content;
  info.window_size = window;
  info.has_checksum = has_checksum;
  return info;
}
void Decode(SequentialReader& in, Sink& out, std::uint64_t maximum) {
  const auto info = ReadHeader(in);
  if (info.has_content_size && info.original_size != maximum)
    Fail("frame content size does not match expected output");
  const auto window = info.window_size;
  Hash64 checksum;
  History history(window);
  const auto block_limit = std::min<std::uint64_t>(window, 131072);
  std::array<unsigned, 3> rep = {1, 4, 8};
  for (;;) {
    Bytes b = ReadExact(in, 3);
    Cursor c{b};
    const unsigned bh = c.Get(3), type = (bh >> 1) & 3, size = bh >> 3;
    const bool last = bh & 1;
    if (type == 3 || size > block_limit) Fail("invalid block type/size");
    Bytes decoded;
    if (type == 0)
      decoded = ReadExact(in, size);
    else if (type == 1)
      decoded = Bytes(size, ReadExact(in, 1)[0]);
    else
      decoded = DecodeBlock(ReadExact(in, size), history, rep, block_limit);
    if (decoded.size() > maximum - out.bytes)
      Fail("output exceeds caller bound");
    checksum.Add(decoded);
    out.Write(decoded);
    history.Append(decoded);
    if (last) break;
  }
  if (info.has_checksum) {
    Bytes check = ReadExact(in, 4);
    Cursor field{check};
    if (field.Get(4) != static_cast<std::uint32_t>(checksum.Digest()))
      Fail("content checksum mismatch");
  }
  if (maximum != out.bytes) Fail("frame content size mismatch");
  if (in.remaining() != 0) Fail("unsupported: concatenation or trailing data");
}

void Error(std::string* error, const std::exception& exception) {
  if (error != nullptr) *error = "Zstandard: " + std::string(exception.what());
}
}  // namespace

bool ZstdCompress(const std::string& input, std::string* output,
                  std::string* error_message) {
  if (output == nullptr) {
    if (error_message) *error_message = "Zstandard output is required";
    return false;
  }
  try {
    SequentialReader reader;
    reader.OpenMemory(&input);
    std::string result;
    Sink sink;
    sink.adapter.OpenMemory(&result);
    Encode(reader, sink);
    *output = std::move(result);
    return true;
  } catch (const std::exception& e) {
    Error(error_message, e);
    return false;
  }
}

bool ZstdDecompress(const std::string& input,
                    std::uint64_t expected_original_size, std::string* output,
                    std::string* error_message) {
  if (output == nullptr) {
    if (error_message) *error_message = "Zstandard output is required";
    return false;
  }
  try {
    SequentialReader reader;
    reader.OpenMemory(&input);
    std::string result;
    Sink sink;
    sink.adapter.OpenMemory(&result);
    Decode(reader, sink, expected_original_size);
    *output = std::move(result);
    return true;
  } catch (const std::exception& e) {
    Error(error_message, e);
    return false;
  }
}

bool ZstdReadStreamInfo(const std::string& input_file, ZstdStreamInfo* info,
                        std::string* error_message) {
  if (info == nullptr) {
    if (error_message) *error_message = "Zstandard stream info is required";
    return false;
  }
  try {
    SequentialReader reader;
    if (!reader.OpenFile(input_file, error_message)) return false;
    *info = ReadHeader(reader);
    return true;
  } catch (const std::exception& e) {
    Error(error_message, e);
    return false;
  }
}

bool ZstdCompressStream(const std::string& input_file, FileSink* output,
                        std::uint64_t* original_size,
                        std::uint64_t* stream_bytes,
                        std::string* error_message) {
  try {
    SequentialReader reader;
    if (!reader.OpenFile(input_file, error_message)) return false;
    const auto size = reader.remaining();
    Sink sink;
    if (!sink.adapter.OpenFile(output, error_message)) return false;
    Encode(reader, sink);
    if (original_size) *original_size = size;
    if (stream_bytes) *stream_bytes = sink.bytes;
    return true;
  } catch (const std::exception& e) {
    Error(error_message, e);
    return false;
  }
}

bool ZstdDecompressStream(const std::string& input_file, FileSink* output,
                          std::uint64_t expected_original_size,
                          std::uint64_t* written, std::string* error_message) {
  try {
    SequentialReader reader;
    if (!reader.OpenFile(input_file, error_message)) return false;
    Sink sink;
    if (!sink.adapter.OpenFile(output, error_message)) return false;
    Decode(reader, sink, expected_original_size);
    if (written) *written = sink.bytes;
    return true;
  } catch (const std::exception& e) {
    Error(error_message, e);
    return false;
  }
}
}  // namespace compression
}  // namespace backupproject

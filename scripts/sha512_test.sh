#!/usr/bin/env bash
#
# sha512_test.sh —— 手写 SHA-512 的单元测试入口（PR #23）。
#
#   bash scripts/sha512_test.sh
#
# 两层：
#   A. 编译 tests/unit/sha512_test.cpp + src/crypto/sha512.cpp，跑官方向量、
#      流式一致性、边界长度、Reset 语义；
#   B. **独立 oracle 交叉验证**：对随机长度 / 随机内容的输入，把本实现的摘要
#      与 sha512sum（coreutils，独立实现）逐条比对。这一层不是"再跑一遍自己"。
#
# 环境变量 SHA512_TEST_EXTRA_FLAGS 可以追加编译参数（例如 sanitizer）。
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

set +u
EXTRA="$SHA512_TEST_EXTRA_FLAGS"
set -u

WORK_DIR="$(mktemp -d /tmp/sha512-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

BIN="$WORK_DIR/sha512_test"
BUILD_LOG="$WORK_DIR/build.log"
PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

# shellcheck disable=SC2086
if ! g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $EXTRA \
      tests/unit/sha512_test.cpp src/crypto/sha512.cpp src/crypto/sha256.cpp \
      -o "$BIN" >"$BUILD_LOG" 2>&1; then
  record_fail "编译 sha512_test" "$(tail -5 "$BUILD_LOG" | tr '\n' ' ')"
  echo "[sha512] passed=$PASS failed=$FAIL"
  exit 1
fi
if [ -s "$BUILD_LOG" ]; then
  record_fail "编译必须零警告" "$(head -3 "$BUILD_LOG" | tr '\n' ' ')"
else
  record_pass "零警告编译（-Wall -Wextra -Wpedantic）"
fi

if "$BIN" > "$WORK_DIR/run.log" 2>&1; then
  record_pass "官方向量 / 流式 / 边界（$(grep -oE 'passed=[0-9]+ failed=[0-9]+' "$WORK_DIR/run.log" | tail -1)）"
else
  record_fail "sha512_test 退出码" "$(grep -m3 FAIL "$WORK_DIR/run.log" | tr '\n' ' ')"
fi

if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)|runtime error:' "$WORK_DIR/run.log"; then
  record_fail "消毒剂报告" "$(grep -m2 -E 'ERROR|runtime error' "$WORK_DIR/run.log" | tr '\n' ' ')"
else
  record_pass "消毒剂报告 0 条"
fi

# ---- B. 独立 oracle：sha512sum ----
if ! command -v sha512sum >/dev/null 2>&1; then
  record_fail "独立 oracle" "本机没有 sha512sum"
else
  cat > "$WORK_DIR/driver.cpp" <<'CPP'
#include <cstdio>
#include <string>
#include <vector>
#include "crypto.h"
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::FILE* f = std::fopen(argv[1], "rb");
  if (f == nullptr) return 3;
  std::vector<char> buf;
  char chunk[65536];
  std::size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    buf.insert(buf.end(), chunk, chunk + n);
  }
  std::fclose(f);
  unsigned char out[backupproject::crypto::kSha512DigestSize];
  const char* data = buf.empty() ? "" : buf.data();
  backupproject::crypto::Sha512::Digest(data, buf.size(), out);
  std::printf("%s\n", backupproject::crypto::ToHex(out, sizeof(out)).c_str());
  return 0;
}
CPP
  # shellcheck disable=SC2086
  if ! g++ -std=c++17 -Wall -Wextra -Iinclude $EXTRA \
        "$WORK_DIR/driver.cpp" src/crypto/sha512.cpp src/crypto/sha256.cpp -o "$WORK_DIR/driver" >>"$BUILD_LOG" 2>&1; then
    record_fail "编译 oracle driver" "$(tail -3 "$BUILD_LOG" | tr '\n' ' ')"
  else
    ORACLE_FAIL=0
    ORACLE_CASES=0
    for i in $(seq 1 200); do
      case $((i % 4)) in
        0) LEN=$((RANDOM % 8)) ;;
        1) LEN=$((120 + RANDOM % 24)) ;;
        2) LEN=$((RANDOM % 4096)) ;;
        3) LEN=$((100000 + RANDOM % 50000)) ;;
      esac
      head -c "$LEN" /dev/urandom > "$WORK_DIR/in.bin"
      MINE="$("$WORK_DIR/driver" "$WORK_DIR/in.bin")"
      THEIRS="$(sha512sum "$WORK_DIR/in.bin" | cut -d' ' -f1)"
      ORACLE_CASES=$((ORACLE_CASES + 1))
      if [ "$MINE" != "$THEIRS" ]; then
        ORACLE_FAIL=$((ORACLE_FAIL + 1))
        if [ "$ORACLE_FAIL" -le 2 ]; then
          echo "  mismatch len=$LEN mine=$(printf %s "$MINE" | cut -c1-16) theirs=$(printf %s "$THEIRS" | cut -c1-16)" >&2
        fi
      fi
    done
    if [ "$ORACLE_FAIL" -eq 0 ]; then
      record_pass "独立 oracle（sha512sum）$ORACLE_CASES 例全部一致，mismatch = 0"
    else
      record_fail "独立 oracle" "mismatch=$ORACLE_FAIL / $ORACLE_CASES"
    fi
  fi
fi

echo "[sha512] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
#!/usr/bin/env bash
#
# secure_transport_test.sh —— PR #21 的加密传输层测试入口。
#
# 跑五个二进制：
#   1) x25519-test          —— 手写 X25519（RFC 7748）与 HKDF-SHA256（RFC 5869）
#                              的官方向量、随机对称性、退化输入拒绝、长度边界；
#   2) secure-transport-test —— BPSEC1 握手（真实 TCP loopback）、线上字节捕获
#                              （明文 marker 必须 0 次出现）、篡改矩阵（密文/tag/
#                              序号/截断/多余字节/超长/错误方向/重放）、握手篡改
#                              与身份 pin 校验；外加 PR #21 独立审查轮补的
#                              服务端持有私钥证明、握手分片矩阵、断连矩阵、
#                              失败后的状态机、记录层计数器纪律、声明长度边界、
#                              握手 DoS 边界；
#   3) review-counter-block    —— review-only：AES-CTR 计数器块唯一性
#                              （10 万个 (方向, 序号) 组合两两不同 + 方向分离）。
#                              它把 src/network/secure_transport.cpp 直接
#                              include 进来，所以**不能**再单独链接那份 .cpp；
#   4) review-sequence-exhaustion —— review-only：记录层序号耗尽（发送侧拒绝
#                              UINT64_MAX 且不回绕；接收侧回绕行为，见文件里的
#                              FINDING 注释）。同样自己 include 那份 .cpp；
#   5) review-identity-tail-eintr —— review-only：LoadTransportIdentity() 读满
#                              32 字节之后那一次"尾部 1 字节"读取遇到 EINTR 时
#                              必须重试，不能当成"没有多余字节"（否则一次信号
#                              打断就能让一个 >32 字节的文件被当成合法私钥）。
#                              它随 tests/review/read_eintr_interposer.cpp 一起
#                              链接：后者在链接期覆盖 read()，确定性地只在那一次
#                              读取上注入 EINTR（不需要 LD_PRELOAD，也不 sleep
#                              撞运气）。
#
# 产物一律放在 /tmp 下的临时目录，退出时自动清理。
# 环境变量 SECURE_TRANSPORT_TEST_EXTRA_FLAGS 可以追加编译参数（sanitizer 构建）。
# 编译出现任何 warning 都算失败（与 crypto_test.sh 同一条规矩）。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

WORK_DIR="$(mktemp -d /tmp/secure-transport-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

CXX="${CXX:-g++}"
EXTRA_FLAGS="${SECURE_TRANSPORT_TEST_EXTRA_FLAGS:-}"

# BPSEC2 把证书层拉进了传输层：secure_transport.cpp 现在会调用
# Bpcert1Parse / Bpcert1VerifySignature / TrustedRootStore。所以这个套件的
# 链接清单必须带上它们，否则连不上（这是"改了产品代码要同步改测试链接清单"
# 的正常代价，不是测试写错了）。
CRYPTO_SOURCES="src/crypto/sha256.cpp src/crypto/hmac.cpp src/crypto/pbkdf2.cpp \
src/crypto/aes.cpp src/crypto/random.cpp src/crypto/x25519.cpp src/crypto/hkdf.cpp \
src/crypto/sha512.cpp src/crypto/ed25519.cpp src/crypto/bpcert.cpp \
src/crypto/trusted_root_store.cpp"
NET_SOURCES="src/network/network_protocol.cpp src/network/secure_transport.cpp"
# review-only 工具自己 #include src/network/secure_transport.cpp（要拿到匿名
# 命名空间里的 CounterBlock / RecordTag），所以它们只链接 network_protocol.cpp。
NET_SOURCES_REVIEW="src/network/network_protocol.cpp"

FAILURES=0

run_one() {
    local name="$1"
    local sources="$2"
    local test_source="$3"
    local bin="$WORK_DIR/$name"
    local build_log="$WORK_DIR/$name.build.log"
    local run_log="$WORK_DIR/$name.run.log"

    echo "[secure-transport] 编译 $name（$WORK_DIR）"
    # shellcheck disable=SC2086
    $CXX $EXTRA_FLAGS -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -pthread \
        $sources "$test_source" -o "$bin" > "$build_log" 2>&1
    local build_code=$?
    if [ $build_code -ne 0 ]; then
        echo "[secure-transport] $name 编译失败（exit=$build_code）："
        cat "$build_log"
        echo "$name: 0/0 checks passed"
        FAILURES=$((FAILURES + 1))
        return
    fi
    if grep -q "warning:" "$build_log"; then
        echo "[secure-transport] $name 编译出现警告，按规定视为失败："
        cat "$build_log"
        echo "$name: 0/0 checks passed"
        FAILURES=$((FAILURES + 1))
        return
    fi
    echo "[secure-transport] $name 编译通过，0 warning"

    timeout --signal=KILL 600 "$bin" > "$run_log" 2>&1
    local run_code=$?
    cat "$run_log"

    local summary
    summary="$(grep -E "^$name: [0-9]+/[0-9]+ checks passed$" "$run_log" | tail -n 1)"
    local passed=0
    local total=0
    if [ -n "$summary" ]; then
        local numbers="${summary#*: }"
        passed="${numbers%%/*}"
        total="${numbers#*/}"
        total="${total%% *}"
    fi
    echo "$name: $passed/$total checks passed"

    if [ $run_code -ne 0 ]; then
        echo "[secure-transport] $name 存在失败项（exit=$run_code）" >&2
        FAILURES=$((FAILURES + 1))
        return
    fi
    if [ "$total" = "0" ] || [ "$passed" != "$total" ]; then
        echo "[secure-transport] $name 汇总行缺失或计数不一致：$summary" >&2
        FAILURES=$((FAILURES + 1))
    fi
}

run_one x25519-test "$CRYPTO_SOURCES" tests/unit/x25519_hkdf_test.cpp
run_one secure-transport-test "$CRYPTO_SOURCES $NET_SOURCES" tests/unit/secure_transport_test.cpp
run_one review-counter-block "$CRYPTO_SOURCES $NET_SOURCES_REVIEW" tests/review/counter_block_harness.cpp
run_one review-sequence-exhaustion "$CRYPTO_SOURCES $NET_SOURCES_REVIEW" tests/review/sequence_exhaustion_harness.cpp
# 读侧 EINTR 探针：read_eintr_interposer.cpp 不是"被测源码"，而是配套的链接期
# 注入器（它单独一个转换单元，故意不包含 <unistd.h>），所以放在 sources 位置。
run_one review-identity-tail-eintr \
    "$CRYPTO_SOURCES $NET_SOURCES tests/review/read_eintr_interposer.cpp" \
    tests/review/identity_tail_eintr.cpp

if [ $FAILURES -ne 0 ]; then
    echo "[secure-transport] 失败项：$FAILURES" >&2
    exit 1
fi
echo "[secure-transport] 全部通过"
exit 0

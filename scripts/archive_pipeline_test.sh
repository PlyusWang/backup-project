#!/usr/bin/env bash
#
# 归档流水线专项测试。
#
# 覆盖：
#   * 27 组合矩阵（3 pack × 3 compression × 3 encryption）的 backup/restore/diff
#   * 元数据（07777 / uid / gid / mtime 秒+纳秒）与特殊文件（symlink/hardlink/FIFO/设备）
#   * socket 语义、Filter 与扫描层的衔接
#   * 容器 header 语义、截断、尾部垃圾、篡改、目标目录原子性
#   * GNU tar 互操作（我们写 → tar 读；tar 写 → 我们读）
#   * baseline USTAR vs Fast USTAR 的打包性能对比
#
# 环境变量：
#   ARCHIVE_PIPELINE_OUT_DIR       指定产物目录（默认 mktemp -d，结束时删除）
#   ARCHIVE_PIPELINE_EXTRA_FLAGS   追加编译参数（例如 sanitizer）
#   ARCHIVE_PIPELINE_SKIP_BENCH=1  跳过 128 MiB 级别的打包 benchmark
#   ARCHIVE_PIPELINE_SKIP_MUTATION=1  跳过确定性 mutation 扫描（约 5 分钟）
#   ARCHIVE_PIPELINE_SKIP_RLIMIT=1    跳过 RLIMIT_AS 资源测试（256 MiB 语料）
#   ARCHIVE_PIPELINE_BENCH_BIG     大文件 MiB（默认 128）
#   ARCHIVE_PIPELINE_BENCH_SMALL   小文件个数（默认 10000）

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

CXX="${CXX:-g++}"
EXTRA_FLAGS="${ARCHIVE_PIPELINE_EXTRA_FLAGS:-}"

if [[ -n "${ARCHIVE_PIPELINE_OUT_DIR:-}" ]]; then
    OUT_DIR="${ARCHIVE_PIPELINE_OUT_DIR}"
    mkdir -p "$OUT_DIR"
    KEEP_OUT=1
else
    OUT_DIR="$(mktemp -d)"
    KEEP_OUT=0
fi

cleanup() {
    if [[ "$KEEP_OUT" -eq 0 ]]; then
        rm -rf "$OUT_DIR"
    else
        echo "产物保留在: $OUT_DIR"
    fi
}
trap cleanup EXIT

FAILURES=0

say() { echo "== $* =="; }

# diff -r 对 FIFO 一律报"文件 X 是先进先出文件，而文件 Y 是先进先出文件"并返回
# 非零——哪怕两边完全一样。所以这里排除 fifo 单独比对，再对它单独断言类型。
compare_trees() {
    local left="$1"
    local right="$2"
    if ! diff -r -x fifo "$left" "$right" >/dev/null 2>&1; then
        return 1
    fi
    if [[ -e "$left/fifo" || -e "$right/fifo" ]]; then
        if [[ ! -p "$left/fifo" || ! -p "$right/fifo" ]]; then
            return 1
        fi
    fi
    return 0
}

# 编译整个产品代码成一份静态库：被测的就是 src/ 下的真实实现。
build_library() {
    say "编译产品代码（src/**/*.cpp）"
    local objects_dir="$OUT_DIR/obj"
    mkdir -p "$objects_dir"
    local sources
    sources="$(find src -name '*.cpp' | sort)"
    if [[ -z "$sources" ]]; then
        echo "找不到任何产品源文件" >&2
        exit 1
    fi
    printf '%s\n' $sources | \
        xargs -P "$(nproc)" -I{} bash -c '
            src="$1"; out="$2"
            name="$(echo "$src" | tr "/" "_").o"
            '"$CXX"' -Iinclude -std=c++17 -Wall -Wextra -Wpedantic -O1 '"$EXTRA_FLAGS"' -c "$src" -o "$out/$name" || exit 1
        ' _ {} "$objects_dir"
    if [[ $? -ne 0 ]]; then
        echo "产品代码编译失败" >&2
        exit 1
    fi
    rm -f "$OUT_DIR/libarchivecore.a"
    ar rcs "$OUT_DIR/libarchivecore.a" "$objects_dir"/*.o
    echo "静态库: $OUT_DIR/libarchivecore.a"
}

compile_and_run() {
    local name="$1"
    local source="$2"
    shift 2
    say "编译并运行 $name"
    local binary="$OUT_DIR/$name"
    if ! "$CXX" -Iinclude -Itests/unit -std=c++17 -Wall -Wextra -Wpedantic \
        $EXTRA_FLAGS -O1 "$source" "$OUT_DIR/libarchivecore.a" -o "$binary" 2>&1; then
        echo "$name: 编译失败" >&2
        FAILURES=$((FAILURES + 1))
        return 1
    fi
    "$binary" "$@"
    local status=$?
    if [[ $status -ne 0 ]]; then
        FAILURES=$((FAILURES + 1))
    fi
    return $status
}

build_library

compile_and_run pipeline_test tests/unit/archive_pipeline_test.cpp | tee "$OUT_DIR/pipeline_test.log"

compile_and_run container_test tests/unit/archive_container_test.cpp | tee "$OUT_DIR/container_test.log"

# 确定性 mutation 扫描：对 v2 容器 / 裸 packed 流 / HUF1 / LZH1 逐偏移翻 bit，
# 要求"要么成功，要么干净失败"，绝不允许崩溃、越界、无限循环或半恢复。
# 它比较慢（几千次 restore），所以给了跳过开关。
if [[ "${ARCHIVE_PIPELINE_SKIP_MUTATION:-0}" == "1" ]]; then
    echo "SKIP  mutation sweep"
else
    compile_and_run mutation_test tests/unit/archive_mutation_test.cpp | tee "$OUT_DIR/mutation_test.log"
fi

# 压缩 wire format 与冻结 fixture 的逐字节对照 + 流式接口的尺寸交叉校验。
compile_and_run stream_test tests/unit/compression_stream_test.cpp \
    "$ROOT_DIR/tests/fixtures/compression" | tee "$OUT_DIR/stream_test.log"

# RLIMIT_AS：在 160 MiB 地址空间的子进程里跑 256 MiB 语料的备份 + 恢复。
# sanitizer 构建会把地址空间放大好几倍，和低 RLIMIT_AS 天生冲突，所以跳过。
if [[ "$EXTRA_FLAGS" == *sanitize* ]]; then
    echo "SKIP  RLIMIT_AS resource test（sanitizer 构建不适合低地址空间上限）"
elif [[ "${ARCHIVE_PIPELINE_SKIP_RLIMIT:-0}" == "1" ]]; then
    echo "SKIP  RLIMIT_AS resource test（ARCHIVE_PIPELINE_SKIP_RLIMIT=1）"
else
    compile_and_run rlimit_test tests/unit/stream_rlimit_test.cpp | tee "$OUT_DIR/rlimit_test.log"
fi

# ---- GNU tar 互操作 --------------------------------------------------------
say "GNU tar 互操作"
INTEROP="$OUT_DIR/ustar_interop"
if ! "$CXX" -Iinclude -Itests/unit -std=c++17 -Wall -Wextra -Wpedantic \
    $EXTRA_FLAGS -O1 tests/unit/ustar_interop.cpp "$OUT_DIR/libarchivecore.a" \
    -o "$INTEROP" 2>&1; then
    echo "ustar_interop: 编译失败" >&2
    FAILURES=$((FAILURES + 1))
fi

if ! command -v tar >/dev/null 2>&1; then
    echo "SKIP  GNU tar interoperability: tar 未安装"
else
    INTEROP_DIR="$OUT_DIR/interop"
    mkdir -p "$INTEROP_DIR"
    SRC="$INTEROP_DIR/src"
    mkdir -p "$SRC/dir"
    printf 'hello interop\n' > "$SRC/plain.txt"
    head -c 4096 /dev/urandom > "$SRC/blob.bin"
    printf 'inner\n' > "$SRC/dir/inner.txt"
    ln -s ../plain.txt "$SRC/dir/link"
    ln "$SRC/plain.txt" "$SRC/hard.txt"
    mkfifo "$SRC/fifo" 2>/dev/null || true
    chmod 0751 "$SRC"
    chmod 0640 "$SRC/plain.txt"

    for writer in write-baseline write-fast; do
        label="${writer#write-}"
        ARCHIVE="$INTEROP_DIR/$label.tar"
        "$INTEROP" "$writer" "$SRC" "$ARCHIVE" || FAILURES=$((FAILURES + 1))
        # A/B：GNU tar 必须能列出我们的 archive。
        LISTING="$INTEROP_DIR/$label.tar.list"
        if tar -tf "$ARCHIVE" > "$LISTING" 2>"$INTEROP_DIR/$label.tar.err"; then
            COUNT="$(wc -l < "$LISTING")"
            EXPECTED="$("$INTEROP" scan "$ARCHIVE" | grep -c '^ENTRY')"
            if [[ "$COUNT" -eq "$EXPECTED" ]]; then
                echo "PASS  tar -tf 列出 $label archive：$COUNT 条与我们的 reader 一致"
            else
                echo "FAIL  tar -tf 列出 $COUNT 条，我们的 reader 数出 $EXPECTED 条"
                FAILURES=$((FAILURES + 1))
            fi
        else
            echo "FAIL  tar -tf 无法读取 $label archive"
            cat "$INTEROP_DIR/$label.tar.err"
            FAILURES=$((FAILURES + 1))
        fi
        # C：GNU tar 解出来必须与源目录一致。
        DEST="$INTEROP_DIR/tar-out-$label"
        mkdir -p "$DEST"
        if (cd "$DEST" && tar -xf "$ARCHIVE") && compare_trees "$SRC" "$DEST"; then
            echo "PASS  GNU tar 解包 $label archive 后 diff -r 一致"
        else
            echo "FAIL  GNU tar 解包 $label archive 后与源目录不一致"
            FAILURES=$((FAILURES + 1))
        fi
        # 我们自己的 reader 恢复出来也必须一致。
        OURS="$INTEROP_DIR/our-out-$label"
        if "$INTEROP" restore "$ARCHIVE" "$OURS" >/dev/null && \
           compare_trees "$SRC" "$OURS"; then
            echo "PASS  我们的 reader 恢复 $label archive 后 diff -r 一致"
        else
            echo "FAIL  我们的 reader 恢复 $label archive 后与源目录不一致"
            FAILURES=$((FAILURES + 1))
        fi
    done

    # D：GNU tar --format=ustar 生成的 archive，我们的 reader 必须能恢复。
    GNU_ARCHIVE="$INTEROP_DIR/gnu-ustar.tar"
    if (cd "$SRC" && tar --format=ustar -cf "$GNU_ARCHIVE" .) 2>"$INTEROP_DIR/gnu.err"; then
        GNU_OUT="$INTEROP_DIR/gnu-out"
        if "$INTEROP" restore "$GNU_ARCHIVE" "$GNU_OUT" >/dev/null && \
           compare_trees "$SRC" "$GNU_OUT"; then
            echo "PASS  GNU tar --format=ustar 的输出能被我们恢复且 diff -r 一致"
        else
            echo "FAIL  GNU tar --format=ustar 的输出恢复后与源目录不一致"
            FAILURES=$((FAILURES + 1))
        fi
    else
        echo "FAIL  GNU tar --format=ustar 生成失败"
        cat "$INTEROP_DIR/gnu.err"
        FAILURES=$((FAILURES + 1))
    fi
fi

# ---- 打包性能对比 ----------------------------------------------------------
if [[ "${ARCHIVE_PIPELINE_SKIP_BENCH:-0}" == "1" ]]; then
    echo "SKIP  打包 benchmark（ARCHIVE_PIPELINE_SKIP_BENCH=1）"
else
    say "baseline USTAR vs Fast USTAR 打包性能"
    BENCH="$OUT_DIR/archive_bench"
    if "$CXX" -Iinclude -Itests/unit -std=c++17 -Wall -Wextra -Wpedantic \
        $EXTRA_FLAGS -O2 tests/unit/archive_bench.cpp "$OUT_DIR/libarchivecore.a" \
        -o "$BENCH" 2>&1; then
        "$BENCH" "${ARCHIVE_PIPELINE_BENCH_BIG:-128}" \
                 "${ARCHIVE_PIPELINE_BENCH_SMALL:-10000}" | tee "$OUT_DIR/bench.log"
        if ! grep -q '^TARBENCH' "$OUT_DIR/bench.log"; then
            FAILURES=$((FAILURES + 1))
        fi
        if grep -q 'FAIL$' "$OUT_DIR/bench.log"; then
            echo "FAIL  benchmark 里有失败语料"
            FAILURES=$((FAILURES + 1))
        fi
    else
        echo "archive_bench: 编译失败" >&2
        FAILURES=$((FAILURES + 1))
    fi
fi

# ---- 汇总 ------------------------------------------------------------------
echo
if [[ $FAILURES -eq 0 ]]; then
    echo "archive-pipeline suite: ALL PASS"
    exit 0
fi
echo "archive-pipeline suite: $FAILURES failing step(s)"
exit 1

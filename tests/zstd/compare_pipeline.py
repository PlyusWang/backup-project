#!/usr/bin/env python3
"""Compare the four shipped compression methods through the archive pipeline.

Run only in the project's serial test window, with an already-built archive-cli
and no concurrent compilation or other performance test. This script never
builds code, drops global caches, opens a server, or uses another codec library.

Example (from the real Git checkout):
  python3 tests/zstd/compare_pipeline.py \
    --archive-cli build-zstd-release/archive-cli \
    --output /absolute/new/compression-comparison \
    --build-log /absolute/existing/o2-build.log

Default: five deterministic datasets x 64 KiB/1 MiB/4 MiB x four methods x
three repetitions. Backup and restore timings cover the complete product
pipeline, including process startup, packing, digesting and fsync. Dataset
generation, recursive diff and SHA-256 verification are outside those timings.
All inputs, archives, restores, raw logs, CSV and JSON are retained.
"""
import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import resource
import shutil
import statistics
import struct
import subprocess
import sys
import time

METHODS = ("none", "huffman", "lzss-huffman", "zstd")
METHOD_IDS = {"none": 0, "huffman": 1, "lzss-huffman": 2, "zstd": 3}
PACK_IDS = {"mypack": 0, "ustar": 1, "fast-ustar": 2}
KINDS = ("text", "repeated", "structured", "random", "mixed")
CHUNK = 65536
SEED = 88781111
CSV_FIELDS = (
    "dataset", "payload_bytes", "method", "repetition", "execution_order",
    "pack", "backup_exit", "restore_exit", "backup_seconds", "restore_seconds",
    "backup_user_seconds", "backup_system_seconds", "restore_user_seconds",
    "restore_system_seconds", "archive_bytes", "packed_bytes", "compressed_bytes",
    "container_payload_bytes",
    "archive_to_source_ratio", "compressed_to_packed_ratio", "diff_exit",
    "manifest_match", "source_unchanged", "status", "error",
)


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            data = stream.read(CHUNK)
            if not data:
                break
            digest.update(data)
    return digest.hexdigest()


def json_file(path, data):
    # Every pathname belongs to this newly-created run directory. Rewriting a
    # result checkpoint must never replace a previous run or a user archive.
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def run_command(argv, logs, label, timeout, environment):
    stdout_path = logs / (label + ".stdout.log")
    stderr_path = logs / (label + ".stderr.log")
    start_usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    started_at = utc_now()
    started = time.perf_counter_ns()
    error = ""
    with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
        try:
            completed = subprocess.run(
                [str(value) for value in argv], stdin=subprocess.DEVNULL,
                stdout=stdout, stderr=stderr, env=environment, timeout=timeout,
                check=False,
            )
            exit_code = completed.returncode
        except subprocess.TimeoutExpired:
            exit_code = "timeout"
            error = "command exceeded timeout"
        except OSError as exception:
            exit_code = "launch-error"
            error = str(exception)
    elapsed = (time.perf_counter_ns() - started) / 1_000_000_000
    end_usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    result = {
        "argv": [str(value) for value in argv], "started_utc": started_at,
        "exit": exit_code, "seconds": elapsed,
        "user_seconds": end_usage.ru_utime - start_usage.ru_utime,
        "system_seconds": end_usage.ru_stime - start_usage.ru_stime,
        "stdout": str(stdout_path), "stderr": str(stderr_path), "error": error,
    }
    json_file(logs / (label + ".command.json"), result)
    return result


def manifest(directory):
    result = {}
    if not directory.is_dir():
        raise ValueError("restore directory does not exist")
    for path in sorted(directory.rglob("*")):
        name = path.relative_to(directory).as_posix()
        if path.is_symlink():
            raise ValueError("unexpected symbolic link: " + name)
        if path.is_dir():
            result[name + "/"] = {"kind": "directory"}
        elif path.is_file():
            result[name] = {"kind": "file", "bytes": path.stat().st_size,
                            "sha256": file_sha256(path)}
        else:
            raise ValueError("unexpected special file: " + name)
    return result


def save_manifest(path, entries):
    with path.open("x", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(("path", "kind", "bytes", "sha256"))
        for name, value in entries.items():
            writer.writerow((name, value["kind"], value.get("bytes", ""),
                             value.get("sha256", "")))


def repeat_to(data, size):
    return (data * ((size + len(data) - 1) // len(data)))[:size]


def dataset_bytes(kind, size):
    # Keep exactly the same generated bytes for all methods and repetitions.
    # Random data uses a fixed PRNG seed; this is test input, never a secret.
    rng = random.Random(SEED + size)
    text = ("Backup Project verifies restored files independently. "
            "备份恢复必须逐文件核对内容与中文路径。\n").encode()
    repeated = bytes(range(256)) * 16
    structured = b"".join(
        json.dumps({"id": index, "group": index % 17,
                    "name": "record-%05d" % index,
                    "flags": [True, False, index % 3 == 0]},
                   separators=(",", ":")).encode() + b"\n"
        for index in range(2048)
    )
    if kind == "text":
        return repeat_to(text, size)
    if kind == "repeated":
        return repeat_to(repeated, size)
    if kind == "structured":
        return repeat_to(structured, size)
    if kind == "random":
        return rng.randbytes(size)
    if kind == "mixed":
        # Interleave compressible text, records and independent random chunks.
        blocks = []
        produced = 0
        while produced < size:
            block = (repeat_to(text, 16384) + repeat_to(structured, 16384) +
                     rng.randbytes(32768))
            blocks.append(block[:min(size - produced, len(block))])
            produced += len(blocks[-1])
        return b"".join(blocks)
    raise ValueError("unknown dataset kind")


def make_dataset(root, kind, size):
    name = "%s-%d" % (kind, size)
    source = root / "datasets" / name
    (source / "子目录" / "空目录").mkdir(parents=True)
    (source / "子目录" / "中文数据.bin").write_bytes(dataset_bytes(kind, size))
    (source / "empty.txt").write_bytes(b"")
    # Metadata is identical for every method; file generation is not timed.
    for path in sorted(source.rglob("*"), reverse=True):
        os.utime(path, ns=(1700000000000000000, 1700000000000000000))
    os.utime(source, ns=(1700000000000000000, 1700000000000000000))
    expected = manifest(source)
    save_manifest(root / "manifests" / (name + "-source.tsv"), expected)
    return {"name": name, "kind": kind, "payload_bytes": size,
            "source": str(source), "manifest": expected}


def container_info(path, method, pack):
    # Read only the stable BKPCNT2 fields. Product restore performs the actual
    # validation; this independent check prevents a mislabeled codec passing.
    with path.open("rb") as stream:
        header = stream.read(160)
    if len(header) != 160 or header[:8] != b"BKPCNT2\0":
        raise ValueError("backup did not produce a BKPCNT2 container")
    version, header_size = struct.unpack_from("<HH", header, 8)
    pack_id, compression_id, encryption_id, flags = header[12:16]
    entries, packed, compressed, payload = struct.unpack_from("<QQQQ", header, 16)
    if (version, header_size, pack_id, compression_id, encryption_id, flags) != (
            2, 160, PACK_IDS[pack], METHOD_IDS[method], 0, 0):
        raise ValueError("container algorithm IDs differ from the requested options")
    if payload != compressed or path.stat().st_size != 160 + payload:
        raise ValueError("unexpected unencrypted container length")
    return {"entry_count": entries, "packed_bytes": packed,
            "compressed_bytes": compressed, "container_payload_bytes": payload,
            "archive_bytes": path.stat().st_size,
            "archive_sha256": file_sha256(path)}


def git_info(repo):
    result = {}
    for key, args in (("head", ["rev-parse", "HEAD"]),
                      ("branch", ["branch", "--show-current"]),
                      ("status", ["status", "--short", "--branch"])):
        query = subprocess.run(["git", "-C", str(repo), *args],
                               capture_output=True, text=True, check=False)
        result[key] = query.stdout.strip() if query.returncode == 0 else None
    diff = subprocess.run(["git", "-C", str(repo), "diff", "--binary", "HEAD"],
                          capture_output=True, check=False)
    result["tracked_diff_sha256"] = (hashlib.sha256(diff.stdout).hexdigest()
                                        if diff.returncode == 0 else None)
    result["codec_sources_sha256"] = {
        str(path.relative_to(repo)): file_sha256(path)
        for path in sorted((repo / "src" / "compression").glob("*.cpp"))
    }
    return result


def environment_info(binary, repo, args):
    result = {
        "started_utc": utc_now(), "python": sys.version,
        "platform": platform.platform(), "uname": list(platform.uname()),
        "binary": str(binary), "binary_sha256": file_sha256(binary),
        "script_sha256": file_sha256(Path(__file__).resolve()),
        "git": git_info(repo), "pack": args.pack, "encryption": "none",
        "sizes_bytes": args.sizes_bytes, "datasets": list(KINDS),
        "methods": list(METHODS), "repetitions": args.repetitions,
        "seed": SEED, "cache_policy": "no global cache modification; warm filesystem cache is possible",
        "measurement_scope": "complete archive-cli product backup/restore pipeline; verification excluded",
        "build_label": args.build_label or "not provided",
        "load_average_before": list(os.getloadavg()),
    }
    if hasattr(os, "sched_getaffinity"):
        result["cpu_affinity"] = sorted(os.sched_getaffinity(0))
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                result["cpu_model"] = line.partition(":")[2].strip()
                break
    if args.build_log:
        build_log = Path(args.build_log).resolve(strict=True)
        result["build_log"] = str(build_log)
        result["build_log_sha256"] = file_sha256(build_log)
    return result


def aggregate(rows):
    summary = []
    groups = {}
    for row in rows:
        groups.setdefault((row["dataset"], row["method"]), []).append(row)
    for (dataset, method), trials in sorted(groups.items()):
        successful = [trial for trial in trials if trial["status"] == "PASS"]
        item = {"dataset": dataset, "method": method, "trials": len(trials),
                "passed": len(successful), "failed": len(trials) - len(successful)}
        for key in ("backup_seconds", "restore_seconds", "archive_bytes",
                    "packed_bytes", "compressed_bytes"):
            values = [trial[key] for trial in successful]
            if values:
                item[key + "_median"] = statistics.median(values)
                item[key + "_min"] = min(values)
                item[key + "_max"] = max(values)
        summary.append(item)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--archive-cli", required=True)
    parser.add_argument("--output", required=True,
                        help="new absolute output directory; must not exist")
    parser.add_argument("--sizes-bytes", nargs=3, type=int,
                        default=[65536, 1048576, 4194304])
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--pack", choices=tuple(PACK_IDS), default="mypack")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--build-log")
    parser.add_argument("--build-label", default="")
    args = parser.parse_args()
    if (len(set(args.sizes_bytes)) != 3 or any(n <= 0 or n > (1 << 28) for n in args.sizes_bytes)
            or not 1 <= args.repetitions <= 20 or args.timeout <= 0):
        parser.error("need three distinct positive sizes <=256 MiB, 1..20 repetitions and positive timeout")
    binary = Path(args.archive_cli).resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("archive-cli must be an existing executable")
    if not shutil.which("diff"):
        parser.error("GNU/POSIX diff is required for recursive comparison")
    root = Path(args.output)
    if not root.is_absolute():
        parser.error("--output must be absolute")
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    for name in ("datasets", "archives", "restores", "logs", "manifests"):
        (root / name).mkdir()
    repo = Path(__file__).resolve().parents[2]
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    environment["TZ"] = "UTC"
    metadata = environment_info(binary, repo, args)
    json_file(root / "environment.json", metadata)
    rows = []
    state = {"status": "RUNNING", "expected_runs": len(KINDS) * 3 * len(METHODS) * args.repetitions}
    json_file(root / "state.json", state)
    try:
        datasets = [make_dataset(root, kind, size)
                    for size in args.sizes_bytes for kind in KINDS]
        json_file(root / "datasets.json", datasets)
        with (root / "results.csv").open("x", newline="") as csv_output:
            writer = csv.DictWriter(csv_output, fieldnames=CSV_FIELDS,
                                    lineterminator="\n", extrasaction="ignore")
            writer.writeheader()
            order = 0
            for data_index, data in enumerate(datasets):
                source = Path(data["source"])
                for repetition in range(1, args.repetitions + 1):
                    # Rotate ordering to reduce the same-method-first bias while
                    # preserving the complete serial execution order in results.
                    rotation = (data_index + repetition - 1) % len(METHODS)
                    methods = METHODS[rotation:] + METHODS[:rotation]
                    for method in methods:
                        order += 1
                        label = "%s-%s-r%d" % (data["name"], method, repetition)
                        archive = root / "archives" / (label + ".bak")
                        restored = root / "restores" / label
                        row = {"dataset": data["name"], "payload_bytes": data["payload_bytes"],
                               "method": method, "repetition": repetition, "execution_order": order,
                               "pack": args.pack, "status": "FAIL", "error": ""}
                        try:
                            backup = run_command(
                                [binary, "backup", source, archive, "--pack", args.pack,
                                 "--compression", method, "--encryption", "none"],
                                root / "logs", label + "-backup", args.timeout, environment)
                            row.update(backup_exit=backup["exit"], backup_seconds=backup["seconds"],
                                       backup_user_seconds=backup["user_seconds"],
                                       backup_system_seconds=backup["system_seconds"])
                            if backup["exit"] != 0:
                                raise ValueError("backup failed; inspect raw command logs")
                            row.update(container_info(archive, method, args.pack))
                            restore = run_command([binary, "restore", archive, restored],
                                                  root / "logs", label + "-restore", args.timeout,
                                                  environment)
                            row.update(restore_exit=restore["exit"], restore_seconds=restore["seconds"],
                                       restore_user_seconds=restore["user_seconds"],
                                       restore_system_seconds=restore["system_seconds"])
                            if restore["exit"] != 0:
                                raise ValueError("restore failed; inspect raw command logs")
                            diff = run_command(["diff", "-qr", source, restored],
                                               root / "logs", label + "-diff", args.timeout, environment)
                            row["diff_exit"] = diff["exit"]
                            restored_manifest = manifest(restored)
                            save_manifest(root / "manifests" / (label + "-restore.tsv"), restored_manifest)
                            row["manifest_match"] = restored_manifest == data["manifest"]
                            row["source_unchanged"] = manifest(source) == data["manifest"]
                            row["archive_to_source_ratio"] = row["archive_bytes"] / data["payload_bytes"]
                            row["compressed_to_packed_ratio"] = row["compressed_bytes"] / row["packed_bytes"]
                            if diff["exit"] != 0 or not row["manifest_match"] or not row["source_unchanged"]:
                                raise ValueError("recursive diff / per-file SHA-256 / source immutability failed")
                            row["status"] = "PASS"
                        except (OSError, ValueError) as exception:
                            row["error"] = str(exception)
                        rows.append(row)
                        writer.writerow(row)
                        csv_output.flush()
                        json_file(root / "results.json", rows)
                        print("%03d/%03d %s %s" % (order, state["expected_runs"], label, row["status"]), flush=True)
        state["status"] = "PASS" if (len(rows) == state["expected_runs"] and
                                             all(row["status"] == "PASS" for row in rows)) else "FAIL"
    except BaseException as exception:
        state["status"] = "INTERRUPTED" if isinstance(exception, KeyboardInterrupt) else "ERROR"
        state["error"] = repr(exception)
        raise
    finally:
        state["completed_runs"] = len(rows)
        state["passed"] = sum(row["status"] == "PASS" for row in rows)
        state["failed"] = len(rows) - state["passed"]
        state["finished_utc"] = utc_now()
        state["load_average_after"] = list(os.getloadavg())
        json_file(root / "state.json", state)
        json_file(root / "summary.json", aggregate(rows))
    return 0 if state["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())

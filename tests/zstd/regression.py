#!/usr/bin/env python3
"""Product-source Zstandard interoperability and malformed-input regression.

Usage: regression.py /absolute/zstd-codec /new/output-directory
Requires the independent standard `zstd` CLI. No product process is started.
"""
import collections
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys

binary = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2]).resolve()
root.mkdir(parents=True, exist_ok=False)
rng = random.Random(88781111)
def run(*args, timeout=120):
    p = subprocess.run([str(x) for x in args], capture_output=True, timeout=timeout)
    return {"exit": p.returncode, "stdout": p.stdout.decode(errors="replace"),
            "stderr": p.stderr.decode(errors="replace")}
def check(result):
    assert result["exit"] == 0, result
cases = {"empty": b"", "one": b"x", "binary": bytes(range(256))}
for n in (31, 32, 4095, 4096, 65535, 65536, 65537, 131072, 131073):
    cases[f"boundary-{n}"] = rng.randbytes(n)
for n in (65536, 1048576, 4194304):
    def sized(b): return (b * (n // len(b) + 1))[:n]
    text = sized(b"Handwritten RFC 8878 codec and independent verification.\n")
    repeated = sized(bytes(range(256)))
    structured = sized(b"".join(f'{{"id":{i},"value":"record-{i % 11}","on":true}}\n'.encode() for i in range(10000)))
    noise = rng.randbytes(n)
    mixed = (text[:n//3] + structured[:n//3] + noise)[:n]
    for kind, data in zip(("text", "repeated", "structured", "random", "mixed"),
                          (text, repeated, structured, noise, mixed)):
        cases[f"{kind}-{n}"] = data
cases["huffman-small"] = bytes(rng.choices(range(32, 123), weights=[40] + [1] * 90, k=800))
cases["huffman-large"] = bytes(rng.choices(range(32, 123), weights=[40] + [1] * 90, k=250000))
results = []
check(run(binary, "selftest"))
for name, data in cases.items():
    source, packed = root / name, root / (name + ".zst")
    source.write_bytes(data)
    check(run(binary, "encode", source, packed))
    check(run("zstd", "-t", packed))
    oracle, own = root / (name + ".oracle"), root / (name + ".own")
    check(run("zstd", "-d", packed, "-o", oracle))
    check(run(binary, "decode", packed, own, len(data)))
    assert own.read_bytes() == oracle.read_bytes() == data
    results.append({"name": name, "original": len(data), "compressed": packed.stat().st_size,
                    "sha256": hashlib.sha256(data).hexdigest(), "valid": True})
(root / "interop.json").write_text(json.dumps(results, indent=2))
standard = []
for name in ("empty", "one", "binary", "text-65536", "random-65536", "structured-65536", "huffman-small"):
    for level in (1, 3, 9, 19):
        for checksum in (True, False):
            source = root / name
            packed = root / f"standard-{name}-{level}-{checksum}.zst"
            check(run("zstd", f"-{level}", "--check" if checksum else "--no-check", source, "-o", packed))
            own = root / (packed.name + ".own")
            result = run(binary, "decode", packed, own, source.stat().st_size)
            if result["exit"] == 0:
                assert own.read_bytes() == source.read_bytes()
                status = "PASS"
            else:
                assert "unsupported:" in result["stderr"] or "window above8MiB policy" in result["stderr"], result
                assert not own.exists()
                status = "EXPLICIT_UNSUPPORTED"
            standard.append({"name": packed.name, "status": status, "result": result})
(root / "standard.json").write_text(json.dumps(standard, indent=2))
corpus, outputs = root / "corpus", root / "outputs"
corpus.mkdir(); outputs.mkdir()
entries = []
def add(name, data, expected, negative=False):
    (corpus / name).write_bytes(data)
    entries.append({"name": name, "expected": expected, "negative": negative})
magic = bytes.fromhex("28b52ffd")
def raw(data): return magic + b"\x00\x38" + ((len(data) << 3) | 1).to_bytes(3, "little") + data
add("valid-empty", raw(b""), 0)
add("valid-raw", raw(b"hello"), 5)
add("valid-rle", magic + b"\x00\x38" + ((65536 << 3) | 3).to_bytes(3, "little") + b"z", 65536)
for name, data, size in [
    ("bad-magic", b"badformat", 0),
    ("reserved-frame", magic + b"\x08\x38\x01\x00\x00", 0),
    ("dictionary", magic + b"\x01\x38\x00", 0),
    ("bad-checksum", magic + b"\x04\x38\x01\x00\x00" + b"\0" * 4, 0),
    ("window", magic + b"\x00\xff\x01\x00\x00", 0),
    ("huge-fcs", magic + b"\xe0" + (1 << 63).to_bytes(8, "little"), 10),
    ("reserved-block", magic + b"\x00\x38\x07\x00\x00", 0),
    ("truncated", raw(b"hello")[:-1], 5),
    ("bound", raw(b"hello"), 4),
    ("trailing", raw(b"hello") + b"trailing", 5),
    ("concatenation", raw(b"hello") * 2, 5),
    ("bad-weight-2", bytes.fromhex("28b52ffd00383d000012c00080200200"), 1),
    ("bad-weight-3", bytes.fromhex("28b52ffd00383d000012c00080300200"), 1),
]: add(name, data, size, True)
add("review-four-stream-regenerated-0", bytes.fromhex("28b52ffd003885000006000380100100010001000101010100"), 0, True)
add("review-four-stream-regenerated-3", bytes.fromhex("28b52ffd003885000036000380100100010001000202020100"), 3, True)
add("review-four-stream-regenerated-4", bytes.fromhex("28b52ffd003885000046000380100100010001000202020200"), 4, True)
seeds = []
for name in ("huffman-small", "huffman-large", "text-65536", "structured-65536", "boundary-4095"):
    data = (root / (name + ".zst")).read_bytes()
    size = len(cases[name])
    seeds.append((data, size))
    add("valid-" + name, data, size)
    positions = range(len(data)) if len(data) < 1200 else sorted(set([0, 1, 2, 3, 4, 5, 6, len(data)-1] + [i * len(data) // 64 for i in range(64)]))
    for pos in positions: add(f"truncate-{name}-{pos}", data[:pos], size, True)
for i in range(1500):
    seed, size = rng.choice(seeds)
    data = bytearray(seed)
    for _ in range(rng.randrange(1, 5)):
        data[rng.randrange(len(data))] ^= 1 << rng.randrange(8)
    add(f"mutation-{i}", data, size)
for i in range(150): add(f"noise-{i}", rng.randbytes(rng.randrange(4096)), 262144)
manifest = root / "manifest.tsv"
manifest.write_text("".join(f'{e["name"]}\t{e["expected"]}\n' for e in entries))
batch = run(binary, "batch", corpus, outputs, manifest, timeout=900)
(root / "batch.stdout.tsv").write_text(batch["stdout"])
(root / "batch.stderr.log").write_text(batch["stderr"])
check(batch)
statuses = {}
for line in batch["stdout"].splitlines():
    name, status, error = line.split("\t", 2)
    statuses[name] = status
assert len(statuses) == len(entries)
for entry in entries:
    name = entry["name"]
    status = statuses[name]
    entry["status"] = status
    if status == "ACCEPT":
        assert not entry["negative"], entry
        oracle = root / "oracle-fuzz"
        check(run("zstd", "-d", corpus / name, "-o", oracle))
        assert oracle.read_bytes() == (outputs / name).read_bytes(), entry
        oracle.unlink()
    else: assert not (outputs / name).exists(), entry
assert not list(root.glob("**/.codec-*"))
sentinel = root / "sentinel"; sentinel.write_bytes(b"KEEP-ME")
link = root / "symlink"; link.symlink_to(sentinel)
for destination in (sentinel, link):
    result = run(binary, "decode", corpus / "valid-raw", destination, 5)
    assert result["exit"] != 0 and sentinel.read_bytes() == b"KEEP-ME" and link.is_symlink()
assert not list(root.glob("**/.codec-*"))
(root / "safety.json").write_text(json.dumps(entries, indent=2))
summary = {"binary": str(binary), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
           "interop_pass": len(results), "standard": dict(collections.Counter(x["status"] for x in standard)),
           "safety": dict(collections.Counter(statuses.values())), "safety_total": len(entries),
           "existing_path_and_symlink": "PASS", "failed_memory_output_unchanged": "PASS"}
(root / "summary.json").write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))

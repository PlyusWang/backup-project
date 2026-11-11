#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

candidate = Path(__file__).resolve().parents[2] / 'packaging/ci-elf-report.sh'
records = []


def run(name, args, expected=0, path=None):
    env = dict(os.environ)
    if path:
        env['PATH'] = str(path) + ':' + env['PATH']
    result = subprocess.run(['bash', str(candidate), *map(str, args)], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    records.append(dict(name=name, expected_exit='nonzero' if expected else 0,
                        actual_exit=result.returncode, passed=(result.returncode != 0) if expected else result.returncode == 0,
                        stdout=result.stdout.decode(errors='replace'), stderr=result.stderr.decode(errors='replace')))


with tempfile.TemporaryDirectory(prefix='elf-candidate-controls-') as temporary:
    root = Path(temporary)
    good = root / 'host-ELF-fixture'
    shutil.copyfile('/usr/bin/true', good)
    good.chmod(0o755)
    bad = root / 'truncated-ELF-fixture'
    bad.write_bytes(b'\x7fELF' + b'\x00' * 8)
    ident = b'\x7fELF' + bytes([2, 1, 1, 0]) + bytes(8)
    hdr = ident + struct.pack('<HHIQQQIHHHHHH', 2, 62, 1, 0x400078, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    code = b'\xb8\x3c\x00\x00\x00\x31\xff\x0f\x05'
    size = len(hdr) + 56 + len(code)
    static = root / 'synthetic-static-ELF'
    static.write_bytes(hdr + struct.pack('<IIQQQQQQ', 1, 5, 0, 0x400000, 0x400000, size, size, 0x1000) + code)
    run('valid-dynamic-ELF-metadata-no-execution', [good])
    run('valid-static-ELF-metadata-no-execution', [static])
    run('truncated-ELF-fails', [bad], 1)
    run('missing-file-fails', [root / 'missing'], 1)
    run('no-input-fails', [], 1)
    run('mixed-good-and-bad-fails', [good, bad], 1)
    for name, tool, body in [
        ('ldd-nonzero-fails', 'ldd', 'printf "synthetic loader failure\\n" >&2; exit 2'),
        ('ldd-warning-fails', 'ldd', 'printf "synthetic diagnostic\\n" >&2; printf "linux-vdso.so.1 (0x1234)\\n"; exit 0'),
        ('ldd-empty-fails', 'ldd', 'exit 0'),
        ('ldd-missing-dependency-fails', 'ldd', 'printf "libmissing.so => not found\\n"; exit 0'),
        ('readelf-nonzero-fails', 'readelf', '/usr/bin/readelf "$@"; exit 3'),
        ('readelf-warning-fails', 'readelf', '/usr/bin/readelf "$@"; printf "synthetic warning\\n" >&2; exit 0'),
        ('strings-nonzero-fails', 'strings', 'exit 4'),
        ('developer-path-fails', 'strings', 'printf "/workspace/backup-project/build/fixture\\n"; exit 0'),
    ]:
        fake = root / name
        fake.mkdir()
        script = fake / tool
        script.write_text('#!/bin/sh\n' + body + '\n')
        script.chmod(0o755)
        run(name, [good], 1, fake)
    fake = root / 'ldd-must-not-be-called-for-static'
    fake.mkdir()
    (fake / 'ldd').write_text('#!/bin/sh\nprintf "ldd must not run for static fixture\\n" >&2\nexit 99\n')
    (fake / 'ldd').chmod(0o755)
    run('static-ELF-bypasses-ldd-deliberately', [static], 0, fake)
result = dict(candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
              tests=records, count=len(records), passed=all(x['passed'] for x in records),
              product_executed=False, product_built=False, applied_to_product_source=True)
print(json.dumps(result, ensure_ascii=False, indent=2))
raise SystemExit(0 if result['passed'] else 1)

#!/usr/bin/env python3
"""用真实服务器进程检查身份证书的读取边界与启动失败行为。"""

import argparse
import json
import os
from pathlib import Path
import resource
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binaries = args.bin_dir.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []

    def command(name, argv):
        completed = subprocess.run(argv, capture_output=True, timeout=30)
        (output / (name + ".log")).write_bytes(
            completed.stdout + completed.stderr
        )
        if completed.returncode != 0:
            raise RuntimeError(name + " failed")
        return completed.stdout.decode("utf-8")

    with tempfile.TemporaryDirectory(prefix="server-certificate-loading-") as tmp:
        fixture = Path(tmp)
        os.chmod(fixture, 0o700)
        key = fixture / "transport.key"
        root_key = fixture / "root.key"
        certificate = fixture / "server.bpcert"
        command("keygen", [str(binaries / "backup-server-keygen"),
                           "--output", str(key)])
        public_output = command(
            "public-key", [str(binaries / "backup-server-keygen"),
                           "--show", "--key-file", str(key)]
        )
        public_key = public_output.split("--server-key hex:")[1].split()[0]
        command("root-init", [str(binaries / "backup-cert-tool"),
                              "root-init", "--root-key", str(root_key),
                              "--root-id", "certificate-loading-root"])
        command("issue-server", [str(binaries / "backup-cert-tool"),
                                 "issue-server", "--root-key", str(root_key),
                                 "--server-id", "certificate-loading-server",
                                 "--server-pubkey", public_key,
                                 "--out", str(certificate)])
        secret = fixture / "secrets.env"
        secret.write_text("BACKUP_TOKEN_SECRET=" + "a" * 64 + "\n")
        secret.chmod(0o600)

        empty = fixture / "empty"
        empty.touch()
        boundary = fixture / "boundary"
        boundary.write_bytes(b"x" * 4096)
        oversize = fixture / "oversize"
        oversize.write_bytes(b"x" * 4097)
        large = fixture / "large-sparse"
        with large.open("wb") as stream:
            stream.truncate(256 * 1024 * 1024)

        cases = [
            ("valid", certificate, None),
            ("empty", empty, "文件是空的"),
            ("boundary4096", boundary, "服务器身份证书不合法"),
            ("oversize4097", oversize, "文件超过 4096 字节上限"),
            ("large-under-memory-limit", large, "文件超过 4096 字节上限"),
            ("directory", fixture, "读取文件失败"),
            ("missing", fixture / "missing", "打不开文件"),
        ]

        def limit_memory():
            resource.setrlimit(resource.RLIMIT_AS,
                               (128 * 1024 * 1024, 128 * 1024 * 1024))

        for name, cert_path, expected_error in cases:
            instance = fixture / ("case-" + name)
            instance.mkdir()
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            argv = [str(binaries / "backup-server"),
                    "--bind", "127.0.0.1", "--port", str(port),
                    "--root", str(instance / "data"),
                    "--db", str(instance / "metadata.sqlite3"),
                    "--secret-file", str(secret),
                    "--transport-key-file", str(key),
                    "--bpsec2-cert-file", str(cert_path), "--require-bpsec2",
                    "--log-file", str(instance / "server.log")]
            process = subprocess.Popen(
                argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                preexec_fn=limit_memory if name == "large-under-memory-limit"
                else None
            )
            listening = False
            deadline = time.monotonic() + 5
            try:
                while process.poll() is None and time.monotonic() < deadline:
                    try:
                        with socket.create_connection(("127.0.0.1", port),
                                                      timeout=0.05):
                            listening = True
                            break
                    except OSError:
                        time.sleep(0.02)
                if expected_error is None:
                    passed = listening and process.poll() is None
                    if process.poll() is None:
                        process.terminate()
                    raw, _ = process.communicate(timeout=5)
                    passed = passed and process.returncode == 0
                else:
                    raw, _ = process.communicate(timeout=5)
                    passed = (not listening and process.returncode == 1
                              and expected_error in raw.decode("utf-8"))
            except subprocess.TimeoutExpired:
                process.kill()
                raw, _ = process.communicate()
                passed = False
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
            (output / (name + ".log")).write_bytes(raw)
            results.append(dict(name=name, passed=passed,
                                exit_code=process.returncode,
                                listening=listening,
                                expected_error=expected_error))
            print(("PASS " if passed else "FAIL ") + name, flush=True)

    summary = dict(passed=all(row["passed"] for row in results),
                   cases=results, count=len(results))
    (output / "RESULT.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n"
    )
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

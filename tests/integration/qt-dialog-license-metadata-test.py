#!/usr/bin/env python3
"""验证显式 deb 许可元数据及拒绝分支；不启动产品或显示服务。"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "packaging/licenses/build-materials.sh"
OBJECT = "libbackup-dialog-license-fixture.so"
PACKAGE = "backup-dialog-license-fixture"
COPYRIGHT = (
    "Format: "
    "https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n\n"
    "Files: *\nCopyright: Test fixture\nLicense: LGPL-3\n"
)


def make_deb(root, name, copyright_text):
    package_root = root / name
    (package_root / "DEBIAN").mkdir(parents=True)
    (package_root / "DEBIAN/control").write_text(
        f"Package: {PACKAGE}\nVersion: 1\nArchitecture: all\n"
        "Maintainer: fixture <fixture@example.invalid>\n"
        "Description: license metadata test fixture\n",
        encoding="utf-8",
    )
    library = package_root / "usr/lib" / OBJECT
    library.parent.mkdir(parents=True)
    library.write_text("license metadata fixture\n", encoding="utf-8")
    if copyright_text is not None:
        copyright_file = package_root / "usr/share/doc" / PACKAGE / "copyright"
        copyright_file.parent.mkdir(parents=True)
        copyright_file.write_text(copyright_text, encoding="utf-8")
    result = root / f"{name}.deb"
    subprocess.run(
        ["dpkg-deb", "--build", str(package_root), str(result)],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    return result


def main():
    with tempfile.TemporaryDirectory(prefix="backup-qt-license-") as work:
        root = Path(work)
        objects = root / "objects"
        objects.mkdir()
        (objects / OBJECT).write_text("license metadata fixture\n")
        valid = make_deb(root, "valid", COPYRIGHT)
        no_copyright = make_deb(root, "no-copyright", None)
        invalid = root / "invalid.deb"
        invalid.write_text("invalid deb fixture\n")
        cases = [
            ("valid", str(valid), 0),
            ("missing", str(root / "missing.deb"), 1),
            ("ambiguous", f"{valid};{valid}", 1),
            ("invalid", str(invalid), 1),
            ("no-copyright", str(no_copyright), 1),
        ]
        results = []
        for name, metadata, expected in cases:
            dest = root / f"output-{name}"
            env = os.environ.copy()
            env["BACKUP_LICENSE_DEBS"] = metadata
            result = subprocess.run(
                ["bash", str(SCRIPT), str(objects), str(dest), "fixture"],
                env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            passed = result.returncode == expected
            if name == "valid" and passed:
                actual = dest / "licenses" / PACKAGE / "copyright"
                coverage = (dest / "licenses/coverage.tsv").read_text()
                passed = actual.read_text() == COPYRIGHT
                passed = passed and f"{OBJECT}\t{PACKAGE}\t1\tdeb:" in coverage
            row = {"case": name, "rc": result.returncode, "passed": passed}
            results.append(row)
            if not passed:
                print(result.stdout)
        print(json.dumps({"cases": results}, ensure_ascii=False, indent=2))
        return 0 if all(row["passed"] for row in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())

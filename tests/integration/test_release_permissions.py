#!/usr/bin/env python3
"""Exercise release-script setup and archive helpers using disposable tool fixtures."""
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tarfile
import tempfile

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / 'packaging/build-release.sh'


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def definition(text, name):
    found = re.findall(r'^' + re.escape(name) + r'\(\) \{.*?^\}', text, re.M | re.S)
    require(len(found) == 1, 'Expected one actual release helper: ' + name)
    return found[0]


def modes(archive):
    with tarfile.open(fileobj=archive, mode='r:*') as source:
        return {item.name.rstrip('/'): item.mode & 0o7777 for item in source.getmembers()}


def main():
    text = SCRIPT.read_text()
    lines = text.splitlines()
    source_at = next(i for i, line in enumerate(lines) if line.startswith('source '))
    prefix = '\n'.join(lines[:source_at]) + '\n'
    require(prefix.count('\numask 022\n') == 1, 'Release umask must be set before sourcing helpers or creating files')
    helpers = '\n'.join(definition(text, name) for name in
                        ('staged_cxxflags', 'write_build_info', 'tar_reproducible', 'build_deb'))
    fixture_script = '''
source "$1/packaging/lib/common.sh"
WORK="$2/work"
VERSION=0.0.0-test DEB_VERSION=0.0.0-test DEB_ARCH=amd64
OS_PRETTY=fixture GLIBC_VERSION=fixture CXX_VERSION=fixture QT_VERSION=fixture
COMMIT=fixture TREE=fixture BRANCH=fixture SOURCE_DATE_EPOCH=0 STAMP=fixture
export SOURCE_DATE_EPOCH
mkdir -p "$WORK" "$2/product/docs" "$2/debian"
printf 'fixture docs\\n' > "$2/product/docs/readme.md"
printf '#!/bin/sh\\nexit 0\\n' > "$2/launcher"
install -m 0755 "$2/launcher" "$2/product/install.sh"
write_build_info "$2/product" server
printf '%s\\n' "$VERSION" > "$2/product/VERSION"
write_manifest "$2/product" > /dev/null
tar_reproducible "$2/product.tar.xz" "$2" product
if [ "$3" = deb ]; then
  cat > "$2/debian/control.in" <<'EOF'
Package: release-permission-fixture
Version: @VERSION@
Architecture: @ARCH@
Maintainer: Fixture <fixture@example.invalid>
Description: Disposable permission test
EOF
  build_deb "$2/debian" "$2/product" "$2/product.deb" ''
fi
'''
    checks = []
    with tempfile.TemporaryDirectory(prefix='release-mode-controls-') as temporary:
        base = Path(temporary)
        for label, inherited, actual, deb in [('historical-077', 0o077, False, False),
                                               ('historical-022', 0o022, False, False),
                                               ('fixed-077', 0o077, True, True),
                                               ('fixed-022', 0o022, True, True)]:
            root = base / label
            root.mkdir()
            selected = prefix if actual else prefix.replace('\numask 022\n', '\n')
            script = selected + helpers + fixture_script
            result = subprocess.run(['bash', '-c', script, 'release-mode-fixture', str(REPO), str(root), 'deb' if deb else 'tar'],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    preexec_fn=lambda mask=inherited: os.umask(mask), timeout=30)
            require(result.returncode == 0, label + ': archive helper failed: ' + result.stderr.decode())
            with (root / 'product.tar.xz').open('rb') as stream:
                original = modes(stream)
            private = label == 'historical-077'
            require(original['product'] == (0o700 if private else 0o755), label + ': original top-level directory mode')
            for name in ('docs/readme.md', 'BUILD-INFO.txt', 'VERSION', 'MANIFEST.sha256'):
                require(original['product/' + name] == (0o600 if private else 0o644), label + ': original metadata mode: ' + name)
            require(original['product/install.sh'] == 0o755, label + ': explicitly installed launcher mode')
            checks.append(dict(name=label + '-original-tar-mode', passed=True, member_modes={k: oct(v) for k, v in original.items()}))
            if deb:
                for option, section in [('--fsys-tarfile', 'data'), ('--ctrl-tarfile', 'control')]:
                    result = subprocess.run(['dpkg-deb', option, root / 'product.deb'], check=True, stdout=subprocess.PIPE)
                    recorded = modes(io.BytesIO(result.stdout))
                    require(all(mode & 0o444 == 0o444 and mode & 0o7022 == 0 for mode in recorded.values()), label + ': original deb permissions ' + section)
                    require(recorded['.'] == 0o755, label + ': deb root traversable ' + section)
                    checks.append(dict(name=label + '-original-deb-' + section + '-mode', passed=True, member_modes={k: oct(v) for k, v in recorded.items()}))
                require(stat.S_IMODE((root / 'product.deb').stat().st_mode) == 0o644, 'Shareable external deb file')
        # The release default does not prevent identity code applying a stricter local mask.
        identity = base / 'identity-control'
        result = subprocess.run(['bash', '-c', prefix + '\numask 077\nmkdir "$1"\nprintf "synthetic\\n" > "$1/private"\n', 'identity-mask', str(identity)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        require(result.returncode == 0 and stat.S_IMODE(identity.stat().st_mode) == 0o700
                and stat.S_IMODE((identity / 'private').stat().st_mode) == 0o600, 'Explicit identity privacy remains effective')
        checks.append(dict(name='explicit-private-mask-remains-0700-0600', passed=True))
    print(json.dumps(dict(passed=True, count=len(checks), checks=checks,
                          scope='actual release prelude and archive helpers with tool fixtures; no product built or executed'), indent=2))


if __name__ == '__main__':
    main()

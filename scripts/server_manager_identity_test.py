#!/usr/bin/env python3
"""Exercise real key/certificate tools and the interactive initialization menu.

Creates synthetic identities only. Does not start a network listener or require
root. Each invocation requires a fresh output directory and preserves evidence.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir', required=True, type=Path)
args = parser.parse_args()
root = args.output_dir.resolve()
root.mkdir(parents=True, exist_ok=False)
repo = Path(__file__).resolve().parent.parent
keygen = repo / 'build/backup-server-keygen'
certtool = repo / 'build/backup-cert-tool'
materials = root / 'synthetic-private-materials'
materials.mkdir(mode=0o700)
results = []


def check(label, condition):
    results.append({'test': label, 'passed': bool(condition)})
    print(('PASS ' if condition else 'FAIL ') + label, flush=True)


def command(label, command_args, expected=0, env=None, data=None, timeout=15):
    process = subprocess.run([str(x) for x in command_args], input=data,
                             text=True, capture_output=True, env=env,
                             timeout=timeout)
    (root / (label + '.log')).write_text(process.stdout + process.stderr,
                                       encoding='utf-8')
    check(label, process.returncode == expected)
    return process


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def identity_public(path):
    process = subprocess.run([str(keygen), '--show', '--key-file', str(path)],
                             text=True, capture_output=True, check=True)
    return re.search(r'公钥 \(hex\): ([0-9a-f]{64})', process.stdout).group(1)


original = materials / 'original.key'
imported = materials / 'imported.key'
command('key-generate', [keygen, '--output', original])
original_digest = digest(original)
command('key-import', [keygen, '--import', original, '--output', imported])
check('identity-and-private-mode-preserved', digest(imported) == original_digest
      and imported.stat().st_mode & 0o777 == 0o600)
command('key-import-no-overwrite', [keygen, '--import', original,
                                   '--output', imported], expected=1)
check('existing-target-unchanged', digest(imported) == original_digest)
command('key-import-show-conflict', [keygen, '--show', '--import', original,
                                    '--key-file', original], expected=2)
command('key-import-empty-path', [keygen, '--import', '', '--output',
                                  materials / 'empty-path.output'], expected=2)
check('empty-import-never-generates-identity',
      not (materials / 'empty-path.output').exists())
command('key-show-empty-import', [keygen, '--show', '--import', '',
                                  '--key-file', original], expected=2)
for mode in ('wide-mode', 'short', 'long', 'symlink', 'fifo'):
    invalid = materials / (mode + '.key')
    if mode == 'symlink':
        invalid.symlink_to(original)
    elif mode == 'fifo':
        os.mkfifo(invalid, mode=0o600)
    else:
        invalid.write_bytes(original.read_bytes() if mode == 'wide-mode' else
                            b'0' * (31 if mode == 'short' else 33))
        invalid.chmod(0o644 if mode == 'wide-mode' else 0o600)
    target = materials / (mode + '.output')
    command('reject-' + mode, [keygen, '--import', invalid, '--output', target],
            expected=1, timeout=3)
    check('no-target-' + mode, not target.exists())
check('original-key-unchanged', digest(original) == original_digest)

root_key = materials / 'root.key'
certificate = materials / 'server.bpcert'
command('root-generate', [certtool, 'root-init', '--root-key', root_key,
                          '--root-id', 'manager-import-root'])
command('certificate-issue', [certtool, 'issue-server', '--root-key', root_key,
                              '--server-id', 'manager-import-server',
                              '--server-pubkey', 'hex:' + identity_public(original),
                              '--out', certificate])
root_pub = Path(str(root_key) + '.pub')
# Public root comments must never leak into client materials.
root_pub.write_text(root_pub.read_text() + '\n# synthetic comment to strip\n')
source_digests = {p: digest(p) for p in (original, root_key, root_pub, certificate)}


def run_menu(label, lines, success=True):
    instance = root / (label + ' instance')
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('BACKUP_SERVER_') and k != 'BACKUP_CERT_TOOL'}
    env.update(BACKUP_SERVER_ROOT=str(instance),
               BACKUP_SERVER_ADMIN=str(repo / 'build/backup-server-admin'),
               BACKUP_SERVER_BIN=str(repo / 'build/backup-server'),
               BACKUP_SERVER_KEYGEN=str(keygen), BACKUP_CERT_TOOL=str(certtool))
    process = command(label, ['bash', repo / 'scripts/backup-server-admin.sh'],
                      data='\n'.join(['1'] + lines + ['', '0', '']), env=env,
                      expected=0, timeout=30)
    config = instance / 'server.conf'
    check(label + '-transaction', config.exists() == success)
    if not success:
        check(label + '-no-partial-identity', not any(
            (instance / p).exists() for p in ['state/transport.key',
                                             'state/server.bpcert',
                                             'private/root.key',
                                             'private/root.key.pub',
                                             'secrets.env']))
    return instance, process


# Fresh default BPSEC1 and BPSEC2 initialization through the actual menu.
run_menu('new-bpsec1', ['', '', '21789', 'n', ''])
run_menu('new-bpsec2', ['', '', '21789', 'y', '', '', 'new-test-server'])
import_lines = ['', '', '21789', 'y', '2', str(original), 'IMPORT IDENTITY',
                '2', str(root_pub), str(certificate), 'IMPORT BPSEC2']
instance, process = run_menu('import-bpsec2', import_lines)
check('imported-key-matches-source', digest(instance / 'state/transport.key')
      == original_digest)
check('external-root-private-not-imported', not (instance / 'private/root.key').exists())
check('public-root-comments-stripped', 'synthetic comment' not in
      (instance / 'private/root.key.pub').read_text())
check('actual-server-id-and-trust-displayed', 'manager-import-server' in
      process.stdout and 'TRUSTED' in process.stdout)
check('absolute-client-paths-displayed', str(instance / 'private/root.key.pub')
      in process.stdout and str(instance / 'state/server.bpcert') in process.stdout)

# Nonloop is explicit and cannot enable BPSEC1 or bypass certificate checks.
public, process = run_menu('nonloop-confirmed', ['', '0.0.0.0'] + import_lines[2:]
                           + ['isolated menu test', 'ALLOW PUBLIC'])
text = (public / 'server.conf').read_text()
check('nonloop-strict-security-config', all(x in text for x in [
    'allow_public_bind = true', 'require_bpsec2 = true',
    'allow_public_bind_insecure = false', 'public_bind_reason = isolated menu test']))
check('wildcard-address-explained', '0.0.0.0 表示全部 IPv4 网卡' in process.stdout)
run_menu('nonloop-bpsec1-rejected', ['', '0.0.0.0', '21789', 'n'], False)
run_menu('nonloop-cancelled', ['', '0.0.0.0'] + import_lines[2:]
         + ['isolated menu test', 'CANCEL'], False)
run_menu('invalid-address', ['', '127.0.0.999', '21789'], False)
run_menu('identity-cancelled', ['', '', '21789', 'n', '2', str(original), 'CANCEL'], False)

other = materials / 'other.key'
command('other-key-generate', [keygen, '--output', other])
wrong_key_lines = list(import_lines)
wrong_key_lines[5] = str(other)
run_menu('certificate-wrong-key', wrong_key_lines, False)
wrong_root = materials / 'wrong-root.key'
command('wrong-root-generate', [certtool, 'root-init', '--root-key', wrong_root,
                                '--root-id', 'wrong-root'])
wrong_root_lines = list(import_lines)
wrong_root_lines[8] = str(wrong_root) + '.pub'
run_menu('certificate-untrusted-root', wrong_root_lines, False)
expired = materials / 'expired.bpcert'
command('expired-issue', [certtool, 'issue-server', '--root-key', root_key,
                         '--server-id', 'expired-server', '--server-pubkey',
                         'hex:' + identity_public(original), '--not-before', '1',
                         '--days', '1', '--out', expired])
expired_lines = list(import_lines)
expired_lines[9] = str(expired)
run_menu('certificate-expired', expired_lines, False)
check('all-original-material-preserved', all(digest(p) == before
      for p, before in source_digests.items())
      and original.stat().st_mode & 0o777 == 0o600)
summary = {'pass': sum(r['passed'] for r in results),
           'fail': sum(not r['passed'] for r in results), 'tests': results,
           'scope': 'real keygen/cert tools and menu; no listener started'}
(root / 'RESULT.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2)
                                 + '\n', encoding='utf-8')
print(f"{summary['pass']} PASS / {summary['fail']} FAIL")
raise SystemExit(bool(summary['fail']))

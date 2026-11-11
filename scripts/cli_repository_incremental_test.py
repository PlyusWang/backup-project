#!/usr/bin/env python3
"""Verify repository labels against real full/delta files and restored data.

Uses an already built backupctl; never compiles or starts a server. Creates and
preserves a fresh synthetic test directory. AES passwords are generated only in
memory and entered through a controlling PTY after terminal echo is disabled.
"""

import argparse
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import secrets
import select
import subprocess
import tempfile
import termios
import time


def file_digest(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(65536), b''):
            digest.update(chunk)
    return digest.hexdigest()


def tree_manifest(root):
    records = []
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            raise RuntimeError('Unexpected synthetic tree symlink: ' + relative)
        if path.is_dir():
            records.append({'path': relative, 'type': 'directory'})
        elif path.is_file():
            records.append({'path': relative, 'type': 'file',
                            'size': path.stat().st_size,
                            'sha256': file_digest(path)})
        else:
            raise RuntimeError('Unexpected synthetic tree entry: ' + relative)
    return records


def pty_command(command, env, password, prompts, timeout):
    master, slave = pty.openpty()
    process = None
    output = bytearray()
    prompt_index = 0
    scan_position = 0

    def controlling_terminal():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)

    try:
        process = subprocess.Popen(command, stdin=slave, stdout=slave,
                                   stderr=slave, env=env,
                                   preexec_fn=controlling_terminal)
        os.close(slave)
        slave = -1
        deadline = time.monotonic() + timeout
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError('backupctl PTY command timed out')
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not chunk:
                    break
                output.extend(chunk)
            elif process.poll() is not None:
                break
            if prompt_index < len(prompts):
                prompt = prompts[prompt_index].encode('ascii')
                position = output.find(prompt, scan_position)
                if position >= 0:
                    if termios.tcgetattr(master)[3] & termios.ECHO:
                        raise RuntimeError('Password prompt did not disable echo')
                    os.write(master, password.encode('ascii') + b'\n')
                    prompt_index += 1
                    scan_position = position + len(prompt)
        status = process.wait(timeout=5)
        if prompt_index != len(prompts):
            raise RuntimeError('Expected password prompt was not reached')
        return status, output.decode('utf-8', errors='replace').replace('\r\n', '\n')
    finally:
        # Popen owns this unreaped child; no PID-file or process-name cleanup.
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if slave >= 0:
            os.close(slave)
        os.close(master)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    repo = Path(__file__).resolve().parent.parent
    parser.add_argument('--backupctl', type=Path, default=repo / 'build/backupctl')
    parser.add_argument('--compression', default='none',
                        choices=('none', 'huffman', 'lzss-huffman', 'zstd'))
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    binary = args.backupctl.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error('--backupctl must name an already built executable')
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    if args.output_dir is None:
        root = Path(tempfile.mkdtemp(prefix='bp-cli-repository-'))
    else:
        root = args.output_dir.resolve()
        root.mkdir(parents=True, exist_ok=False)
    root.chmod(0o700)
    checks = []
    commands = []
    summary = {'backupctl': str(binary), 'binary_sha256_before': file_digest(binary),
               'compression': args.compression, 'output_dir': str(root),
               'checks': checks, 'commands': commands}

    def check(label, condition):
        checks.append({'test': label, 'passed': bool(condition)})
        print(('PASS ' if condition else 'FAIL ') + label, flush=True)
        if not condition:
            raise RuntimeError(label)

    try:
        for case, encryption, delta_version in (
                ('plain', 'none', 1), ('aes', 'aes-256-ctr-hmac-sha256', 2)):
            case_root = root / case
            source = case_root / 'source'
            repository = case_root / 'repository'
            source.mkdir(parents=True)
            (source / '子目录').mkdir()
            (source / '空目录').mkdir()
            (source / 'a.txt').write_text('original\n', encoding='utf-8')
            (source / 'remove.txt').write_text('remove in D1\n', encoding='utf-8')
            (source / '子目录/nested.txt').write_text('nested\n', encoding='utf-8')
            (source / 'empty.txt').touch()
            (source / 'stable.bin').write_bytes(bytes(range(256)) * 4096)
            runtime = case_root / 'runtime'
            runtime.mkdir(mode=0o700)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith('BACKUP_')}
            env.update(XDG_CONFIG_HOME=str(case_root / 'xdg-config'),
                       XDG_CACHE_HOME=str(case_root / 'xdg-cache'),
                       XDG_DATA_HOME=str(case_root / 'xdg-data'),
                       XDG_RUNTIME_DIR=str(runtime))
            prefix = [str(binary), '--config-file', str(case_root / 'config.json'),
                      '--schedule-file', str(case_root / 'schedule.json')]
            password = secrets.token_hex(24) if encryption != 'none' else ''

            def command(label, arguments, expected=0, prompts=()):
                argv = prefix + [str(value) for value in arguments]
                if prompts:
                    status, text = pty_command(argv, env, password, prompts,
                                               args.timeout)
                else:
                    process = subprocess.run(argv, env=env, text=True,
                                             stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT,
                                             timeout=args.timeout)
                    status, text = process.returncode, process.stdout
                leaked = bool(password and password in text)
                safe_text = text.replace(password, '[REDACTED]') if password else text
                log_name = label + '.log'
                (case_root / log_name).write_text(safe_text, encoding='utf-8')
                commands.append({'case': case, 'command': arguments[0],
                                 'label': label, 'exit': status, 'log': log_name})
                check(case + '-' + label + '-no-password-output', not leaked)
                check(case + '-' + label + '-exit', status == expected)
                return text

            command('configure', ['config', 'repository', 'set', repository])
            backup_options = ['--pack', 'mypack', '--compression', args.compression,
                              '--encryption', encryption]
            backup_prompts = ('Backup password: ', 'Confirm password: ') if password else ()
            full_output = command('F0', ['backup', source, '--strategy', 'full'] +
                                  backup_options, prompts=backup_prompts)
            full_match = re.search(r'^Archive:\s+(\S+)\s*$', full_output, re.MULTILINE)
            check(case + '-F0-has-product-archive-name', full_match is not None)
            full_name = full_match.group(1)
            full = repository / full_name
            check(case + '-F0-is-real-container', full.read_bytes()[:8] == b'BKPCNT2\0')
            (source / 'a.txt').write_text('modified in D1\n', encoding='utf-8')
            (source / '中文新增.txt').write_text('增量新增\n', encoding='utf-8')
            (source / 'remove.txt').unlink()
            delta_output = command('D1', ['backup', source, '--strategy', 'incremental'] +
                                   backup_options, prompts=backup_prompts)
            delta_match = re.search(r'^Archive:\s+(\S+)\s*$', delta_output, re.MULTILINE)
            check(case + '-D1-reports-real-delta', delta_match is not None and
                  re.search(r'^Kind:\s+delta\s*$', delta_output, re.MULTILINE) is not None)
            delta_name = delta_match.group(1)
            delta = repository / delta_name
            check(case + '-D1-distinct-from-F0', delta_name != full_name)
            expected_magic = ('BKPINC' + str(delta_version)).encode('ascii') + b'\0'
            check(case + '-D1-real-versioned-delta', delta.read_bytes()[:8] == expected_magic)
            check(case + '-exactly-two-snapshots', len(list(repository.glob('*.bak'))) == 2)

            def listing_row(text, name):
                rows = [line for line in text.splitlines()
                        if re.match(r'^  ' + re.escape(name) + r'\s', line)]
                check(case + '-unique-list-row-' + name, len(rows) == 1)
                return rows[0]

            listing = command('list-intact', ['repository', 'list'])
            full_row = listing_row(listing, full_name)
            delta_row = listing_row(listing, delta_name)
            check(case + '-full-label-kept', '  container-v2  ' in full_row and
                  'incremental-v' not in full_row)
            check(case + '-delta-label-is-extension-version',
                  '  incremental-v' + str(delta_version) + '  ' in delta_row and
                  'container-v2' not in delta_row and 'legacy-v0.1' not in delta_row)
            pipeline = '  mypack/' + args.compression + '/' + encryption + '  '
            check(case + '-full-and-delta-report-selected-pipeline',
                  pipeline in full_row and pipeline in delta_row)
            check(case + '-delta-row-names-parent', '  parent=' + full_name in delta_row)
            check(case + '-list-does-not-claim-authentication',
                  '  chain=parent-present-unverified' in delta_row)
            check(case + '-password-requirement-visible',
                  ('  password-required' in delta_row) == bool(password))
            restored = case_root / 'restored-D1'
            restore_prompts = ('Restore password: ',) if password else ()
            command('restore-D1', ['restore', delta_name, restored], prompts=restore_prompts)
            comparison = subprocess.run(['diff', '-qr', str(source), str(restored)],
                                        text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=args.timeout)
            (case_root / 'restore-diff.log').write_text(comparison.stdout, encoding='utf-8')
            check(case + '-D1-recursive-diff', comparison.returncode == 0)
            expected_tree, restored_tree = tree_manifest(source), tree_manifest(restored)
            (case_root / 'source-sha256.json').write_text(
                json.dumps(expected_tree, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
            (case_root / 'restored-sha256.json').write_text(
                json.dumps(restored_tree, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
            check(case + '-D1-file-set-directories-sizes-and-sha256', expected_tree == restored_tree)

            # Move only this test's F0 out of the repository; retain it as evidence.
            held_parent = case_root / ('held-' + full_name)
            full.rename(held_parent)
            delta_before = file_digest(delta)
            broken_listing = command('list-missing-parent', ['repository', 'list'])
            broken_row = listing_row(broken_listing, delta_name)
            check(case + '-broken-row-keeps-delta-version-and-parent',
                  '  incremental-v' + str(delta_version) + '  ' in broken_row and
                  '  parent=' + full_name in broken_row)
            check(case + '-broken-row-explains-missing-parent',
                  '  chain=unavailable' in broken_row and
                  '[parent snapshot is missing: ' + full_name + ']' in broken_row)
            failed_target = case_root / 'must-not-publish'
            command('restore-missing-parent', ['restore', delta_name, failed_target],
                    expected=1, prompts=restore_prompts)
            check(case + '-missing-parent-publishes-no-target', not failed_target.exists())
            check(case + '-read-only-list-and-failed-restore-preserve-delta',
                  file_digest(delta) == delta_before)
            check(case + '-held-parent-is-preserved', held_parent.is_file())
        check('binary-unchanged', file_digest(binary) == summary['binary_sha256_before'])
        summary['passed'] = True
    except Exception as error:
        summary['passed'] = False
        summary['error'] = type(error).__name__ + ': ' + str(error)
        print('FAIL ' + summary['error'], flush=True)
    finally:
        summary['binary_sha256_after'] = file_digest(binary)
        summary['passed_assertions'] = sum(check['passed'] for check in checks)
        summary['failed_assertions'] = sum(not check['passed'] for check in checks)
        (root / 'summary.json').write_text(
            json.dumps(summary, ensure_ascii=False, indent=2, default=str) + '\n', encoding='utf-8')
        print('Evidence: ' + str(root), flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

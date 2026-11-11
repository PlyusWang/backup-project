#!/usr/bin/env python3
"""Verify launcher public-bind opt-in without starting a server or opening a port."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launcher', type=Path,
                        default=repo / 'packaging/server/launch-server.sh')
    parser.add_argument('--output', type=Path,
                        help='new evidence directory; existing directories are refused')
    options = parser.parse_args()
    launcher = options.launcher.resolve()
    if not launcher.is_file():
        parser.error('launcher is not a regular file: ' + str(launcher))
    if options.output:
        output = options.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        parent = repo / 'tests/output'
        parent.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix='launcher-public-bind-', dir=parent))
    stub = output / 'no-network-server-stub'
    stub.write_text('#!/bin/sh\n'
                    'printf "STUB_SERVER_INVOKED\\n"\n'
                    'printf "%s\\0" "$@" > "$LAUNCHER_AUDIT_INVOCATION"\n')
    stub.chmod(0o700)
    # Each public positive case keeps a certificate. The explicit insecure
    # compatibility switch only relaxes require_bpsec2, never the public opt-in.
    cases = [
        ('public-omitted', None, False, {}, 'allow_public_bind = true'),
        ('public-false', 'false', False, {}, 'allow_public_bind = true'),
        ('public-off', 'off', False, {}, 'allow_public_bind = true'),
        ('public-no', 'no', False, {}, 'allow_public_bind = true'),
        ('public-zero', '0', False, {}, 'allow_public_bind = true'),
        ('public-true', 'true', True, {}, ''),
        ('public-yes', 'yes', True, {}, ''),
        ('public-one', '1', True, {}, ''),
        ('public-on', 'on', True, {}, ''),
        ('insecure-does-not-imply-optin', 'false', False,
         {'allow_public_bind_insecure': 'true', 'require_bpsec2': 'false'},
         'allow_public_bind = true'),
        ('explicit-insecure-existing-path', 'true', True,
         {'allow_public_bind_insecure': 'true', 'require_bpsec2': 'false'}, ''),
        ('missing-certificate', 'true', False, {'certificate': ''}, 'certificate'),
        ('missing-reason', 'true', False, {'public_bind_reason': ''},
         'public_bind_reason'),
        ('missing-require-bpsec2', 'true', False, {'require_bpsec2': 'false'},
         'require_bpsec2 = true'),
        ('loopback-false', 'false', True, {'bind': '127.0.0.1'}, ''),
        ('loopback-true', 'true', True, {'bind': '127.0.0.1'}, ''),
        ('invalid-boolean', 'not-a-boolean', False, {},
         '配置键 allow_public_bind 只接受 true / false'),
    ]
    results = []
    for name, allow, should_accept, updates, reason in cases:
        values = {'bind': '0.0.0.0', 'port': '28769',
                  'data_root': str(output / 'unused-data'),
                  'database': str(output / 'unused-database'),
                  'secret_file': str(output / 'unused-secret'),
                  'transport_key': str(output / 'unused-transport-key'),
                  'certificate': str(output / 'unused-certificate'),
                  'require_bpsec2': 'true',
                  'public_bind_reason': 'isolated argv regression; no network',
                  'allow_public_bind_insecure': 'false'}
        if allow is not None:
            values['allow_public_bind'] = allow
        values.update(updates)
        config = output / (name + '.conf')
        config.write_text(''.join(key + ' = ' + value + '\n'
                                  for key, value in values.items()))
        expected = ['--bind', values['bind'], '--port', values['port'],
                    '--root', values['data_root'], '--db', values['database'],
                    '--secret-file', values['secret_file'],
                    '--transport-key-file', values['transport_key']]
        if values['certificate']:
            expected += ['--bpsec2-cert-file', values['certificate']]
        if values['require_bpsec2'] == 'true':
            expected += ['--require-bpsec2']
        if values['bind'] != '127.0.0.1':
            expected += ['--allow-public-bind', values['public_bind_reason']]
        expected += ['--pid-file', '/run/backup-project-server/backup-server.pid',
                     '--workers', '4', '--io-timeout', '30',
                     '--max-upload-bytes', '8589934592',
                     '--max-login-failures', '5', '--login-lockout-seconds', '60']
        for mode in ('print', 'stub-run'):
            invocation = output / (name + '-' + mode + '.argv')
            environment = dict(os.environ, BACKUP_SERVER_BIN=str(stub),
                               LAUNCHER_AUDIT_INVOCATION=str(invocation))
            command = ['bash', str(launcher), '--config', str(config)]
            if mode == 'print':
                command.append('--print-command')
            process = subprocess.run(command, env=environment, text=True,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, timeout=5)
            log = process.stdout
            argv = None
            if should_accept and process.returncode == 0:
                if mode == 'stub-run' and invocation.is_file():
                    argv = [arg.decode() for arg in
                            invocation.read_bytes().split(b'\0')[:-1]]
                elif mode == 'print':
                    lines = [line for line in log.splitlines()
                             if line.startswith('backup-server ')]
                    if len(lines) == 1:
                        argv = shlex.split(lines[0])[1:]
                passed = argv == expected
                if mode == 'print':
                    passed = passed and not invocation.exists()
            else:
                passed = (not should_accept and process.returncode == 2 and
                          reason in log and not invocation.exists())
            row = {'case': name, 'mode': mode,
                   'expected_accept': should_accept, 'exit_code': process.returncode,
                   'pass': passed, 'server_stub_invoked': invocation.exists(),
                   'expected_argv': expected if should_accept else None,
                   'actual_argv': argv, 'expected_rejection': reason}
            results.append(row)
            (output / (name + '-' + mode + '.log')).write_text(log)
            print('LAUNCHER_PUBLIC_BIND ' + name + '/' + mode + '=' +
                  ('PASS' if passed else 'FAIL'))
    passed = sum(row['pass'] for row in results)
    failed = len(results) - passed
    (output / 'RESULT.json').write_text(json.dumps(
        {'launcher': str(launcher), 'passed': passed, 'failed': failed,
         'network_opened': False, 'real_server_started': False, 'cases': results},
        indent=2) + '\n')
    print('LAUNCHER_PUBLIC_BIND SUMMARY passed=' + str(passed) +
          ' failed=' + str(failed) + ' output=' + str(output))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Pure installer/desktop tests with tiny mock executables, never product code."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo-root',type=Path,default=Path(__file__).resolve().parents[2])
parser.add_argument('--out',type=Path,required=True)
args=parser.parse_args()
REPO=args.repo_root.resolve(strict=True)
OUT=args.out.resolve()
OUT.mkdir(parents=True,exist_ok=True)
ROOT=Path(tempfile.mkdtemp(prefix='fixture-',dir=OUT))
checks=0

def check(ok,label):
    global checks
    if not ok:
        raise RuntimeError(label)
    checks+=1
    print('PASS',label,flush=True)

def hashfile(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

env=dict(os.environ,HOME=str(ROOT/'home'),XDG_DATA_HOME=str(ROOT/'desktop data'))
Path(env['HOME']).mkdir()
package=ROOT/'package'
(package/'bin').mkdir(parents=True)
(package/'packaging/client').mkdir(parents=True)
for name in ('backupctl','backup-gui-modern'):
    p=package/'bin'/name
    p.write_text('#!/bin/sh\nprintf "%s\\n" "$0" "$@" >"$BP_DESKTOP_CAPTURE"\n')
    p.chmod(0o755)
shutil.copy2(REPO/'packaging/portable/install-client.sh',package/'install.sh')
shutil.copy2(REPO/'packaging/client/backup-project.desktop',package/'packaging/client/backup-project.desktop')
icon=package/'share/icons/hicolor/256x256/apps/backup-project.png'
icon.parent.mkdir(parents=True)
subprocess.run(['python3',str(REPO/'packaging/client/make-icon.py'),str(icon),'256'],check=True)
(package/'MANIFEST.sha256').write_text(''.join(hashfile(p)+'  '+p.relative_to(package).as_posix()+'\n'
                                          for p in sorted(package.rglob('*')) if p.is_file()))
check('Icon=backup-project\n' in (REPO/'packaging/client/backup-project.desktop').read_text(),
      'Debian desktop entry declares the official icon name')
prefixes=[ROOT/'中文 prefix with spaces',ROOT/('prefix & '+chr(34)+' quoted $ '+chr(96)+" apostrophe' slash"+chr(92)+'=tail')]
files=[]
percent=ROOT/'desktop percent%'
result=subprocess.run(['bash',str(package/'install.sh'),'--prefix',str(percent)],env=env,
                      stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
(ROOT/'install-percent-rejected.log').write_text(result.stdout)
check(result.returncode!=0 and '--no-desktop' in result.stdout and not percent.exists(),
      'unsupported desktop percent path is rejected with guidance before any installation write')
control=ROOT/'desktop\u0001control'
result=subprocess.run(['bash',str(package/'install.sh'),'--prefix',str(control)],env=env,
                      stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
(ROOT/'install-control-rejected.log').write_text(result.stdout)
check(result.returncode!=0 and not control.exists(),
      'control-character prefix is rejected before any installation write')
for index,prefix in enumerate(prefixes):
    log=ROOT/f'install-prefix-{index}.log'
    with log.open('w') as stream:
        subprocess.run(['bash',str(package/'install.sh'),'--prefix',str(prefix)],env=env,
                       stdout=stream,stderr=subprocess.STDOUT,check=True)
    key=hashlib.sha256(str(prefix/'lib/backup-project').encode()).hexdigest()[:16]
    desktop=Path(env['XDG_DATA_HOME'])/'applications'/f'backup-project-{key}.desktop'
    installed_icon=Path(env['XDG_DATA_HOME'])/'icons/hicolor/256x256/apps'/f'backup-project-{key}.png'
    check(desktop.is_file() and installed_icon.is_file() and hashfile(installed_icon)==hashfile(icon),
          f'prefix {index} installs its own desktop and official icon')
    subprocess.run(['desktop-file-validate',str(desktop)],check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    check(True,f'prefix {index} desktop passes freedesktop validation')
    info=Gio.DesktopAppInfo.new_from_filename(str(desktop))
    check(info is not None,f'prefix {index} TryExec resolves the entire executable path')
    capture=ROOT/f'capture-{index}'
    os.environ['BP_DESKTOP_CAPTURE']=str(capture)
    info.launch([],None)
    deadline=time.monotonic()+10
    while not capture.exists() and time.monotonic()<deadline:
        time.sleep(.05)
    check(capture.exists() and capture.read_text().splitlines()==[str(prefix/'bin/backup-gui-modern')],
          f'prefix {index} Exec launches exact path with no shell expansion or extra argument')
    keyfile=Gio.FileIcon
    check(info.get_icon().get_file().get_path()==str(installed_icon),
          f'prefix {index} Icon resolves the complete installed icon path')
    files.append((prefix,desktop,installed_icon))

# Program and desktop integration ownership: another prefix and arbitrary
# user desktop/icon files must survive removal of only the first installation.
other_desktop=Path(env['XDG_DATA_HOME'])/'applications/backup-project.desktop'
other_desktop.write_text('user-owned desktop\n')
foreign_icon=Path(env['XDG_DATA_HOME'])/'icons/hicolor/256x256/apps/foreign.png'
foreign_icon.write_bytes(b'foreign icon')
user_config=Path(env['HOME'])/'.config/backup-project/sentinel'
user_config.parent.mkdir(parents=True)
user_config.write_text('keep config\n')
backup=ROOT/'repository/sentinel'
backup.parent.mkdir()
backup.write_text('keep backup\n')
with (ROOT/'uninstall-prefix-0.log').open('w') as stream:
    subprocess.run(['bash',str(REPO/'packaging/portable/uninstall-client.sh'),'--prefix',str(files[0][0])],
                   env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
check(not files[0][1].exists() and not files[0][2].exists() and not (files[0][0]/'lib/backup-project').exists(),
      'uninstall removes only its own program, desktop and icon')
check(files[1][1].is_file() and files[1][2].is_file() and (files[1][0]/'bin/backup-gui-modern').is_symlink(),
      'another prefix desktop/icon/program survives uninstall')
check(other_desktop.read_text()=='user-owned desktop\n' and foreign_icon.read_bytes()==b'foreign icon',
      'uninstall preserves unrelated desktop/icon files')
check(user_config.read_text()=='keep config\n' and backup.read_text()=='keep backup\n',
      'uninstall preserves user configuration and backup data')
# A replaced entry/icon is no longer owned by this installation.
files[1][1].write_text('user replaced desktop\n')
files[1][2].write_bytes(b'user replaced icon')
with (ROOT/'uninstall-prefix-1-replaced.log').open('w') as stream:
    subprocess.run(['bash',str(REPO/'packaging/portable/uninstall-client.sh'),'--prefix',str(files[1][0])],
                   env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
check(files[1][1].read_text()=='user replaced desktop\n' and files[1][2].read_bytes()==b'user replaced icon',
      'uninstall does not delete replaced desktop/icon files')
# No desktop option creates no shell integration.
no_desktop=ROOT/'no desktop percent%'
with (ROOT/'install-no-desktop.log').open('w') as stream:
    subprocess.run(['bash',str(package/'install.sh'),'--prefix',str(no_desktop),'--no-desktop'],
                   env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
check((no_desktop/'bin/backup-gui-modern').is_symlink() and len(list((Path(env['XDG_DATA_HOME'])/'applications').glob('backup-project-*.desktop')))==1,
      '--no-desktop leaves desktop integration untouched')
print(f'DESKTOP_INTEGRATION_RESULT passed={checks} failed=0 fixture={ROOT}',flush=True)

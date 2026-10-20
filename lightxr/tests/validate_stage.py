#!/usr/bin/python3
"""Validate the actual install tree, session preset, modes, unit paths and PAM policy."""
import configparser, json, os, subprocess, sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
prefix=root/'usr/local'
required=['sbin/lightxr','libexec/lightxr/lightxr-greeter','libexec/lightxr/lightxr-presenter',
          'libexec/lightxr/lightxr-broker.py','libexec/lightxr/lightxr-session',
          'share/xgreeters/lightxr.desktop',
          'lib/systemd/system/lightxr.service','lib/systemd/system/lightxr-broker.service',
          'lib/sysusers.d/lightxr.conf','lib/tmpfiles.d/lightxr.conf','share/dbus-1/system.d/org.freedesktop.DisplayManager.conf']
for name in required:
    path=prefix/name
    assert path.is_file() and not path.is_symlink(),name
    if name.startswith(('sbin/','libexec/')):assert path.stat().st_mode&0o111,name
    assert not path.stat().st_mode&0o022,name
conf=configparser.ConfigParser(interpolation=None);conf.read(root/'etc/lightxr/lightdm.conf')
assert conf['Seat:*']['user-session']=='dome' and conf['Seat:*']['greeter-session']=='lightxr'
assert conf['Seat:*']['greeter-user']=='lightxr' and conf['Seat:*']['allow-guest']=='false'
assert conf['Seat:*']['pam-service']=='lightxr' and 'autologin-user' not in conf['Seat:*']
assert 'pam_permit.so' not in (root/'etc/pam.d/lightxr').read_text()
assert '@include common-auth' in (root/'etc/pam.d/lightxr').read_text()
assert (root/'etc/pam.d/lightxr-greeter').is_file()
assert isinstance(json.loads((root/'etc/lightxr/broker.json').read_text())['allow_users'],list)
for name in ['lightxr.service','lightxr-broker.service']:
    text=(prefix/'lib/systemd/system'/name).read_text()
    assert '@CMAKE_' not in text and 'ExecStart=' in text
assert 'RuntimeDirectory=lightxr-broker\n' in (prefix/'lib/systemd/system/lightxr-broker.service').read_text()
assert 'RuntimeDirectory=lightxr\n' in (prefix/'lib/systemd/system/lightxr.service').read_text()
assert '/usr/share/wayland-sessions' in conf['LightDM']['sessions-directory']
assert not (prefix/'share/lightxr/sessions/dome.desktop').exists(), 'Must not shadow installed DomeD entry'
for name in ['share/xgreeters/lightxr.desktop']:
    subprocess.run(['desktop-file-validate',str(prefix/name)],check=True)
print('PASS: staged executables, DomeD default, distinct runtime directories, disabled guest/autologin, PAM policy and desktop files')

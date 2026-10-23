#!/usr/bin/python3
"""Validate the actual install tree, session preset, modes, unit paths and PAM policy."""
import configparser, json, os, subprocess, sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
prefix=root/'usr/local'
required=['sbin/lightxr','libexec/lightxr/lightxr-greeter','libexec/lightxr/lightxr-presenter',
          'libexec/lightxr/lightxr-broker.py','libexec/lightxr/lightxr-session','libexec/lightxr/lightxr-wayland',
          'share/xgreeters/lightxr.desktop',
          'lib/systemd/system/lightxr.service','lib/systemd/system/lightxr-broker.service',
          'lib/sysusers.d/lightxr.conf','lib/tmpfiles.d/lightxr.conf']
for name in required:
    path=prefix/name
    assert path.is_file() and not path.is_symlink(),name
    if name.startswith(('sbin/','libexec/')):assert path.stat().st_mode&0o111,name
    assert not path.stat().st_mode&0o022,name
    parent=path.parent
    while root==Path('/') and parent!=root:
        assert parent.stat().st_mode&0o005==0o005, f'Greeter cannot traverse {parent}'
        parent=parent.parent
loader=subprocess.run(['/usr/bin/env','-u','LD_LIBRARY_PATH','ldd',str(prefix/'libexec/lightxr/lightxr-greeter')],text=True,capture_output=True,check=True)
assert 'not found' not in loader.stdout, 'Greeter dependencies must resolve without inherited LD_LIBRARY_PATH: '+loader.stdout
conf=configparser.ConfigParser(interpolation=None);conf.read(root/'etc/lightxr/lightdm.conf')
assert (root/'etc/dbus-1/system.d/lightxr.conf').is_file(), 'System D-Bus must load the display-manager policy even with a /usr/local prefix'
assert conf['Seat:*']['user-session']=='dome' and conf['Seat:*']['greeter-session']=='lightxr'
assert conf['LightDM']['greeter-user']=='lightxr' and conf['Seat:*']['allow-guest']=='false'
assert 'xserver-command' not in conf['Seat:*']
desktop=configparser.ConfigParser(interpolation=None);desktop.read(prefix/'share/xgreeters/lightxr.desktop')
assert desktop['Desktop Entry']['X-LightDM-Session-Type']=='wayland'
assert desktop['Desktop Entry']['Exec']=='/usr/local/libexec/lightxr/lightxr-wayland'
launcher=(prefix/'libexec/lightxr/lightxr-wayland').read_text()
assert 'FLTK_BACKEND=wayland' in launcher and 'WLR_BACKENDS=drm,libinput' in launcher
assert 'LIBSEAT_BACKEND=logind' in launcher and '/usr/bin/cage -s -- ' in launcher
assert conf['Seat:*']['pam-service']=='lightxr' and 'autologin-user' not in conf['Seat:*']
assert 'pam_permit.so' not in (root/'etc/pam.d/lightxr').read_text()
pam = (root/'etc/pam.d/lightxr').read_text()
assert '@include common-auth' in pam or ('substack      password-auth' in pam and 'include       postlogin' in pam)
assert 'pam_succeed_if.so user != root' in pam
assert (root/'etc/pam.d/lightxr-greeter').is_file()
assert isinstance(json.loads((root/'etc/lightxr/broker.json').read_text())['allow_users'],list)
for name in ['lightxr.service','lightxr-broker.service']:
    text=(prefix/'lib/systemd/system'/name).read_text()
    assert '@CMAKE_' not in text and 'ExecStart=' in text
assert 'RuntimeDirectory=lightxr-broker\n' in (prefix/'lib/systemd/system/lightxr-broker.service').read_text()
assert 'RuntimeDirectory=lightxr\n' in (prefix/'lib/systemd/system/lightxr.service').read_text()
assert '/usr/share/wayland-sessions' in conf['LightDM']['sessions-directory']
assert not (prefix/'share/lightxr/sessions/dome.desktop').exists(), 'Must not shadow installed DomeD entry'
clock=prefix/'share/lightxr/fonts/x14y24pxHeadUpDaisy.bdf'
provenance=json.loads((prefix/'share/doc/LightXR/fonts/HeadUpDaisy.provenance.json').read_text())
import hashlib
assert hashlib.sha256(clock.read_bytes()).hexdigest()==provenance['files'][clock.name]
assert b'PIXEL_SIZE 24' in clock.read_bytes()
assert (prefix/'share/doc/LightXR/fonts/LICENSE.HeadUpDaisy.html').is_file()
for name in ['share/xgreeters/lightxr.desktop']:
    subprocess.run(['desktop-file-validate',str(prefix/name)],check=True)
print('PASS: staged executables, DomeD default, distinct runtime directories, disabled guest/autologin, PAM policy and desktop files')

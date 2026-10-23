#!/usr/bin/python3
"""Brief clock preview through the installed production broker and real headset.
Run as root with --xr-user USER while no authentication greeter is active.
The isolated headless greeter uses the dedicated account; it cannot authenticate.
"""
import argparse
import json
import os
from pathlib import Path
import pwd
import re
import signal
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--xr-user', required=True)
parser.add_argument('--seconds', type=float, default=5)
args = parser.parse_args()
if os.geteuid() != 0:
    raise SystemExit(77)
if subprocess.run(['pgrep', '-u', 'lightxr', '-x', 'lightxr-greeter'], stdout=subprocess.DEVNULL).returncode == 0:
    raise SystemExit('An authentication greeter is active; leave its channel untouched.')
account = pwd.getpwnam('lightxr')
owner = pwd.getpwnam(args.xr_user)
assert owner.pw_uid > 0
assert args.xr_user in json.loads(Path('/etc/lightxr/broker.json').read_text())['allow_users']
broker = int(subprocess.check_output(['systemctl', 'show', 'lightxr-broker.service', '--value', '-p', 'MainPID'], text=True).strip())
assert broker > 0
report = {'broker_pid': broker, 'presenter_seen': False}
started = int(time.time())
with tempfile.TemporaryDirectory(prefix='lightxr-clock-proof-', dir='/run/lightxr') as directory:
    os.chown(directory, account.pw_uid, account.pw_gid)
    os.chmod(directory, 0o700)
    env = {'PATH': '/usr/local/bin:/usr/bin:/bin', 'HOME': account.pw_dir,
           'USER': 'lightxr', 'LOGNAME': 'lightxr', 'LANG': 'C.UTF-8',
           'XDG_RUNTIME_DIR': directory, 'WLR_BACKENDS': 'headless',
           'WLR_RENDERER': 'pixman', 'WLR_HEADLESS_OUTPUTS': '1',
           'WLR_XWAYLAND': '/usr/bin/false', 'FLTK_BACKEND': 'wayland',
           'LIBGL_ALWAYS_SOFTWARE': '1'}
    with tempfile.TemporaryFile(mode='w+') as log:
        process = subprocess.Popen(['/usr/bin/cage', '-s', '--', '/usr/local/libexec/lightxr/lightxr-greeter',
                                    '--lock-preview', '--windowed'], env=env,
                                   user=account.pw_uid, group=account.pw_gid,
                                   extra_groups=os.getgrouplist('lightxr', account.pw_gid),
                                   stdout=log, stderr=log, start_new_session=True)
        try:
            end = time.monotonic() + args.seconds
            while time.monotonic() < end:
                if process.poll() is not None:
                    log.seek(0)
                    raise AssertionError('Installed greeter failed: ' + log.read())
                for pid in Path(f'/proc/{broker}/task/{broker}/children').read_text().split():
                    try:
                        if Path(f'/proc/{pid}/exe').resolve().name != 'lightxr-presenter':
                            continue
                        status = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines())
                        uid = int(status['Uid'].split()[1])
                        assert uid > 0
                        assert all(int(status[field], 16) == 0 for field in ['CapPrm', 'CapEff', 'CapAmb'])
                        if uid == owner.pw_uid:
                            report.update(presenter_seen=True, presenter_uid=uid,
                                          presenter_pid=int(pid), presenter_capabilities_zero=True)
                    except FileNotFoundError:
                        pass
                time.sleep(.1)
            assert report['presenter_seen'], 'Installed broker did not start the trusted runtime presenter'
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
# The native presenter prints final submitted/visible frame counts on channel close.
end = time.monotonic() + 5
while time.monotonic() < end:
    journal = subprocess.check_output(['journalctl', '-u', 'lightxr-broker.service', '--since', f'@{started}', '--no-pager'], text=True)
    frames = re.findall(r'LightXR frames=(\d+) visible=(\d+)', journal)
    if frames and int(frames[-1][1]) > 0:
        report.update(frames=int(frames[-1][0]), visible_frames=int(frames[-1][1]))
        break
    time.sleep(.1)
assert report.get('visible_frames', 0) > 0, 'The headset never displayed the installed greeter'
print(json.dumps(report, indent=2))

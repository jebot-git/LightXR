#!/usr/bin/python3
"""Test LightXR's contract with the installed DomeD launcher/supervisor in a mount namespace.
Only the compositor executable is replaced by a recorder inside that namespace;
no physical display/input, installed file, PAM socket or live user bus is touched.
"""
import configparser, hashlib, json, os, shlex, shutil, signal, subprocess, sys, tempfile
from pathlib import Path
signal.pthread_sigmask(signal.SIG_SETMASK, [])
entry=Path('/usr/share/wayland-sessions/dome.desktop')
launcher=Path('/usr/local/bin/dome-session');supervisor=Path('/usr/local/libexec/dome-session-client')
if not shutil.which('bwrap') or not all(p.is_file() for p in [entry,launcher,supervisor]):raise SystemExit(77)
probe=subprocess.run(['bwrap','--ro-bind','/','/','--proc','/proc','--dev','/dev','--unshare-pid','--die-with-parent','--','/usr/bin/true'],capture_output=True)
if probe.returncode:raise SystemExit(77)
root=Path(__file__).resolve().parents[2]
out=root/'artifacts/dome-compatibility';out.mkdir(parents=True,exist_ok=True)
parser=configparser.ConfigParser(interpolation=None);parser.read(entry)
command=parser['Desktop Entry']['Exec'];assert shlex.split(command)==[str(launcher)],command
checks=[]
with tempfile.TemporaryDirectory(prefix='lightxr-dome-contract-') as directory:
    temp=Path(directory);temp.chmod(0o700)
    runtime=temp/'runtime';runtime.mkdir(mode=0o700)
    home=temp/'home';home.mkdir()
    recorder=temp/'compositor-recorder'
    recorder.write_text('#!/usr/bin/python3\nimport json,os,sys\nfrom pathlib import Path\nkeys=["XDG_RUNTIME_DIR","XDG_CURRENT_DESKTOP","XDG_SESSION_DESKTOP","XDG_SESSION_TYPE","XDG_SEAT","XDG_VTNR","XDG_SEAT_PATH","XDG_SESSION_PATH","DBUS_SESSION_BUS_ADDRESS","DISPLAY","WAYLAND_DISPLAY","WAYLAND_SOCKET","DOME_LOGIN_SESSION","DOME_BACKEND","LIBSEAT_BACKEND","DOME_START_XR","DOME_TERMINAL","DOME_TERMINAL_MODE","QT_QPA_PLATFORM","QT_IM_MODULE","GTK_IM_MODULE","XMODIFIERS","PAM_KWALLET5_LOGIN"]\nPath(os.environ["LIGHTXR_DOME_CONTRACT"]).write_text(json.dumps({"argv":sys.argv[1:],"env":{k:os.environ.get(k) for k in keys}}))\n')
    recorder.chmod(0o755)
    baseline={'PATH':'/usr/local/bin:/usr/bin:/bin','LANG':'C.UTF-8','HOME':str(home),
              'XDG_RUNTIME_DIR':str(runtime),'XDG_CONFIG_HOME':str(temp/'config'),'XDG_STATE_HOME':str(temp/'state'),
              'XDG_SESSION_TYPE':'wayland','XDG_SESSION_CLASS':'user','XDG_SEAT':'seat0','XDG_VTNR':'9',
              'XDG_SEAT_PATH':'/org/freedesktop/DisplayManager/Seat0',
              'XDG_SESSION_PATH':'/org/freedesktop/DisplayManager/Session0',
              'PAM_KWALLET5_LOGIN':str(temp/'fake-pam-socket')}
    for name,overrides in [('defaults',{}),('custom',{'DOME_START_XR':'0','DOME_SESSION_TERMINAL':'0','DOME_TERMINAL_MODE':'wayland'})]:
        report=temp/(name+'.json')
        env={**baseline,**overrides,'LIGHTXR_DOME_CONTRACT':str(report)}
        args=['bwrap','--ro-bind','/','/','--tmpfs','/tmp','--bind',str(temp),str(temp),'--ro-bind',str(recorder),'/usr/local/bin/dome-compositor',
              '--proc','/proc','--dev','/dev','--unshare-pid','--die-with-parent','--',sys.executable,
              str(root/'lightxr/packaging/lightxr-session'),command]
        result=subprocess.run(args,env=env,capture_output=True,text=True,timeout=15)
        (out/(name+'.log')).write_text(result.stdout+result.stderr+'\n'+''.join(p.read_text() for p in (temp/'state').rglob('*.log')))
        assert result.returncode==0,(name,result.returncode,(out/(name+'.log')).read_text())
        value=json.loads(report.read_text());e=value['env']
        expected={'XDG_RUNTIME_DIR':str(runtime),'XDG_CURRENT_DESKTOP':'Dome','XDG_SESSION_DESKTOP':'dome','XDG_SESSION_TYPE':'wayland',
                  'XDG_SEAT':'seat0','XDG_VTNR':'9','XDG_SEAT_PATH':baseline['XDG_SEAT_PATH'],'XDG_SESSION_PATH':baseline['XDG_SESSION_PATH'],
                  'DOME_LOGIN_SESSION':'1','DOME_BACKEND':'drm','LIBSEAT_BACKEND':'logind',
                  'DOME_START_XR':overrides.get('DOME_START_XR','1'),'DOME_TERMINAL':'/usr/local/bin/ghostty-xr-launch',
                  'DOME_TERMINAL_MODE':overrides.get('DOME_TERMINAL_MODE','overlay'),'QT_QPA_PLATFORM':'wayland',
                  'QT_IM_MODULE':'wayland','GTK_IM_MODULE':'fcitx','XMODIFIERS':'@im=fcitx','PAM_KWALLET5_LOGIN':baseline['PAM_KWALLET5_LOGIN']}
        for key,expected_value in expected.items():assert e[key]==expected_value,(name,key,e[key],expected_value)
        assert all(e[k] is None for k in ['DISPLAY','WAYLAND_DISPLAY','WAYLAND_SOCKET'])
        assert e['DBUS_SESSION_BUS_ADDRESS'].startswith('unix:'),'Installed launcher must establish its private session bus'
        assert value['argv']==['--backend','drm']+([] if overrides.get('DOME_SESSION_TERMINAL')=='0' else ['--terminal'])
        checks.append({'case':name,'passed':True,'captured':value})
    installed={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [entry,launcher,supervisor,Path('/usr/local/bin/dome-compositor'),Path('/usr/local/bin/dome-xr')]}
    (out/'report.json').write_text(json.dumps({'installed_sha256':installed,'checks':checks},indent=2)+'\n')
print('PASS: actual installed DomeD entry → LightXR wrapper → launcher → supervisor; logind/VT metadata, private D-Bus, KWallet handoff, IME, XR and terminal defaults/overrides')

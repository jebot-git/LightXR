#!/usr/bin/python3
"""Check final PAM metadata in the real session worker, without opening a PAM session."""
import os, pathlib, pwd, socket, struct, subprocess, sys, tempfile
if os.getuid()==0:
    raise SystemExit('This test must run unprivileged')
I=lambda n:struct.pack('=i',n)
Z=lambda n:struct.pack('@N',n)
S=lambda s:I(-1) if s is None else I(len(s.encode()))+s.encode()
def exact(peer,n):
    data=b''
    while len(data)<n:
        part=peer.recv(n-len(data))
        if not part:raise AssertionError('Worker disconnected early')
        data+=part
    return data
def string(peer):
    n,=struct.unpack('=i',exact(peer,4))
    return None if n<0 else exact(peer,n).decode()
with tempfile.TemporaryDirectory(prefix='lightxr-pam-handoff-') as directory:
    root=pathlib.Path(directory)
    for name,old_tty,old_display,new_tty,new_display,vt in [
        ('wayland-vt-switch','/dev/tty1',None,'/dev/tty7',None,'7'),
        ('x11-to-wayland','/dev/tty1',':old','/dev/tty8',None,'8'),
        ('same-vt','/dev/tty7',None,'/dev/tty7',None,'7'),
    ]:
        server,worker=socket.socketpair();server.settimeout(8)
        report=root/(name+'.txt')
        env={**os.environ,'LD_PRELOAD':str(pathlib.Path(sys.argv[2]).resolve()),'LIGHTXR_PAM_PROBE_REPORT':str(report)}
        process=subprocess.Popen([str(pathlib.Path(sys.argv[1]).resolve()),'--session-child',str(worker.fileno()),str(worker.fileno())],env=env,pass_fds=[worker.fileno()],stderr=subprocess.PIPE)
        worker.close()
        try:
            # Worker version 4; use our own UID and disable authentication only
            # in this unprivileged protocol fixture. The probe rejects open_session.
            server.sendall(I(4)+S('lightxr-fixture')+S(pwd.getpwuid(os.getuid()).pw_name)+I(0)+I(0)+S(None)+S(old_tty)+S(None)+S(old_display)+S(None))
            assert string(server)==pwd.getpwuid(os.getuid()).pw_name
            assert struct.unpack('=i',exact(server,4))[0]==1
            assert struct.unpack('=i',exact(server,4))[0]==0
            string(server)
            values=['XDG_SESSION_TYPE=wayland','XDG_SESSION_CLASS=user','XDG_SEAT=seat0','XDG_VTNR='+vt]
            message=S(str(root/(name+'.log')))+I(0)+S(new_tty)+S(None)+S(new_display)+S(None)+Z(len(values))
            message+=b''.join(S(v) for v in values)+Z(1)+S('/usr/bin/false')
            server.sendall(message)
            process.wait(timeout=8)
            assert process.returncode!=0, 'Probe must reject opening a real session'
            assert report.read_text().splitlines()==[new_tty,'(null)',vt], (name,report.read_text())
            print('PASS:',name,'updates real PAM_TTY/PAM_XDISPLAY before pam_open_session')
        finally:
            if process.poll() is None:process.kill();process.wait()
            server.close()

#!/usr/bin/python3
"""Exercise the FLTK greeter's real PAM protocol on Wayland, without X11 or a physical seat."""
import os, pathlib, shutil, signal, socket, struct, subprocess, sys, tempfile, threading, time
cage = os.environ.get('LIGHTXR_TEST_CAGE') or shutil.which('cage')
if not cage:
    raise SystemExit(77)
integer = lambda n: struct.pack('!I', n)
string = lambda b: integer(len(b)) + b

def exact(sock, count):
    data = b''
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise AssertionError('Unexpected greeter disconnect')
        data += part
    return data

with tempfile.TemporaryDirectory(prefix='lightxr-wayland-') as directory:
    root = pathlib.Path(directory)
    runtime = root / 'runtime'; runtime.mkdir(mode=0o700)
    listener = socket.socket(socket.AF_UNIX); listener.bind(str(root/'bridge')); listener.listen()
    listener.settimeout(15)
    server, client = socket.socketpair(); server.settimeout(15)
    bridge = []; frames = threading.Event()
    def drain():
        peer, _ = listener.accept(); bridge.append(peer)
        try:
            while True:
                size, = struct.unpack('!I', exact(peer, 4))
                payload = exact(peer, size)
                if payload[:1] == b'F':
                    assert struct.unpack('!II', payload[1:9]) == (1280, 800)
                    frames.set()
        except (OSError, AssertionError):
            pass
    thread = threading.Thread(target=drain, daemon=True); thread.start()
    env = {**os.environ, 'XDG_RUNTIME_DIR': str(runtime), 'FLTK_BACKEND': 'wayland',
           'WLR_BACKENDS': 'headless', 'WLR_RENDERER': 'pixman', 'WLR_HEADLESS_OUTPUTS': '1',
           'WLR_XWAYLAND': '/usr/bin/false', 'LIGHTDM_TO_SERVER_FD': str(client.fileno()),
           'LIGHTDM_FROM_SERVER_FD': str(client.fileno())}
    for key in ['DISPLAY','WAYLAND_DISPLAY','WAYLAND_SOCKET','XAUTHORITY']:
        env.pop(key, None)
    with (root/'cage.log').open('w+') as log:
        process = subprocess.Popen([cage, '-s', '--', '/usr/bin/env', '-u', 'DISPLAY',
                                    'GDK_BACKEND=wayland', str(pathlib.Path(sys.argv[1]).resolve()),
                                    '--bridge', str(root/'bridge')],
                                   env=env, pass_fds=[client.fileno()], stdout=log, stderr=log,
                                   start_new_session=True)
        client.close()
        def receive():
            kind, count = struct.unpack('!II', exact(server, 8))
            return kind, exact(server, count)
        def send(kind, body): server.sendall(integer(kind)+integer(len(body))+body)
        def remote(value):
            import json
            payload = b'I'+json.dumps(value).encode()
            bridge[0].sendall(integer(len(payload))+payload)
        def enter(): remote({'type':'key', 'key':0x01000004, 'text':''})
        try:
            assert receive()[0] == 0
            send(7, integer(1)+string(b'fixture')+integer(2)+string(b'default-session')+
                 string(b'dome')+string(b'select-user')+string(b'wayland-fixture'))
            assert frames.wait(10), 'Native greeter never exported its frame'
            # No pointer click first: kiosk focus must make the keyboard usable immediately.
            time.sleep(.3); enter()
            kind, body = receive()
            assert kind == 1 and body[8:] == b'wayland-fixture', (kind, body)
            sequence = body[:4]
            send(1, sequence+string(b'wayland-fixture')+integer(1)+integer(1)+string(b'Password:'))
            time.sleep(.3)
            remote({'type':'key','key':0,'text':'disposable-fixture-secreX'})
            remote({'type':'key','key':0x01000003,'text':''})
            remote({'type':'key','key':0,'text':'t'}); enter()
            assert receive() == (3, integer(1)+string(b'disposable-fixture-secret'))
            send(2, sequence+string(b'wayland-fixture')+integer(7))
            time.sleep(.3)
            remote({'type':'key','key':ord('A'),'text':'a','mods':0x04000000})
            remote({'type':'key','key':0,'text':'retry-fixture'}); enter()
            kind, body = receive()
            assert kind == 1 and body[8:] == b'retry-fixture'
            log.flush(); log.seek(0); text = log.read()
            assert 'Starting Xwayland' not in text, text
            print('PASS: native Wayland GL greeter, kiosk focus, real LightDM handshake, masked response, failed-login retry and XR frames; no Xorg/Xwayland')
        except BaseException:
            log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
            raise
        finally:
            os.killpg(process.pid, signal.SIGTERM)
            try: process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL); process.wait()
            server.close(); listener.close()
            for peer in bridge: peer.close()

#!/usr/bin/python3
"""Exercise the FLTK greeter's real PAM protocol on Wayland, without X11 or a physical seat."""
import os, pathlib, shutil, signal, socket, struct, subprocess, sys, tempfile, threading, time
cage = os.environ.get('LIGHTXR_TEST_CAGE') or shutil.which('cage')
if not cage:
    raise SystemExit(77)
locking = '--lock' in sys.argv
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
    bridge = []; samples = []; frames = threading.Event(); hidden = threading.Event()
    def drain():
        peer, _ = listener.accept(); bridge.append(peer)
        try:
            while True:
                size, = struct.unpack('!I', exact(peer, 4))
                payload = exact(peer, size)
                if payload[:1] == b'F':
                    assert struct.unpack('!II', payload[1:9]) == (1280, 800)
                    if len(samples) < 30: samples.append(payload[9:])
                    frames.set()
                elif payload == b'H': hidden.set()
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
            hints = string(b'default-session')+string(b'dome')+string(b'select-user')+string(b'wayland-fixture')
            if locking: hints += string(b'lock-screen')+string(b'true')
            send(7, integer(1)+string(b'fixture')+integer(3 if locking else 2)+hints)
            assert frames.wait(10), 'Native greeter never exported its frame'
            assert len(set(samples[-1])) > 3, 'XR frame was blank instead of actual GL pixels'
            if locking:
                time.sleep(2.1)
                assert len(set(samples)) >= 2, 'UNIX clock never updated its XR pixels'
                # Only clock glyphs occupy this ROI. Every native bitmap cell
                # must be an exact 2x2 block in the fixed XR canvas.
                pixels = samples[-1]
                artifact = pathlib.Path(__file__).resolve().parents[2]/'artifacts/lock/protocol-locked.ppm'
                artifact.parent.mkdir(parents=True,exist_ok=True)
                artifact.write_bytes(b'P6\n1280 800\n255\n'+b''.join(pixels[i:i+3] for i in range(0,len(pixels),4)))
                def native_blocks(phase_x, phase_y):
                    ink = 0
                    for y in range(342+phase_y,418,2):
                        for x in range(402+phase_x,878,2):
                            offsets = [((y+dy)*1280+x+dx)*4 for dy in (0,1) for dx in (0,1)]
                            cells = [pixels[i:i+4] for i in offsets]
                            if not all(c == cells[0] for c in cells): return False
                            ink += cells[0][:3] == bytes((237,243,251))
                    return ink > 100
                assert any(native_blocks(x,y) for x in (0,1) for y in (0,1)), 'Clock bitmap cells are not exact 2x2 blocks'
                enter() # Wake the cover; never submit a character or start PAM.
                server.settimeout(.3)
                try:
                    receive()
                    raise AssertionError('Wake key escaped the lock cover')
                except socket.timeout:
                    pass
                server.settimeout(15)
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
            assert kind == 1 and body[8:] == (b'wayland-fixture' if locking else b'retry-fixture')
            if locking:
                # Only the private daemon's successful PAM result may resume the
                # existing session. The XR greeter must hide at that handoff.
                send(2, body[:4]+string(b'wayland-fixture')+integer(0))
                assert receive() == (4, string(b'dome'))
                assert hidden.wait(10), 'Authenticated unlock did not hide its XR layer'
                send(3, integer(0))
            log.flush(); log.seek(0); text = log.read()
            assert 'Starting Xwayland' not in text, text
            if locking: print('PASS: lock hint, nonblank UNIX-clock frames, wake-event isolation, fixed account, failed-PAM retry and successful authenticated session handoff')
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

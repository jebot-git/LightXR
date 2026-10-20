#!/usr/bin/python3
"""LightXR's root-owned seat bridge. Authentication never passes through here."""
import argparse
import asyncio
import contextlib
import dataclasses
import grp
import json
import logging
import os
from pathlib import Path
import pwd
import signal
import socket
import stat
import struct
import time

LOG = logging.getLogger('lightxr-broker')
WIDTH, HEIGHT = 1280, 800
FRAME_SIZE = 1 + 8 + WIDTH * HEIGHT * 4
MAX_INPUT = 1024

async def packet(reader, limit):
    size, = struct.unpack('!I', await reader.readexactly(4))
    if not 0 < size <= limit:
        raise ValueError('Invalid bridge packet size')
    return await reader.readexactly(size)

async def send(writer, payload):
    writer.write(struct.pack('!I', len(payload)) + payload)
    await asyncio.wait_for(writer.drain(), 3)

def valid_frame(payload):
    return (len(payload) == FRAME_SIZE and payload[:1] == b'F' and
            struct.unpack('!II', payload[1:9]) == (WIDTH, HEIGHT))

def validate_input(payload):
    if not payload.startswith(b'I') or len(payload) > MAX_INPUT:
        raise ValueError('Invalid input packet')
    value = json.loads(payload[1:])
    if not isinstance(value, dict):
        raise ValueError('Invalid input object')
    kind = value.get('type')
    if kind in ('move', 'down', 'up'):
        if set(value) - {'type', 'x', 'y', 'button'}:
            raise ValueError('Unknown pointer field')
        if any(type(value.get(k)) not in (int, float) or not 0 <= value[k] < bound
               for k, bound in [('x', WIDTH), ('y', HEIGHT)]):
            raise ValueError('Pointer outside greeter')
        if type(value.get('button', 1)) is not int or value.get('button', 1) not in (1, 2):
            raise ValueError('Invalid pointer button')
    elif kind == 'key':
        if set(value) - {'type', 'key', 'text', 'mods'}:
            raise ValueError('Unknown keyboard field')
        key = value.get('key', 0)
        text = value.get('text', '')
        if type(key) is not int or not (0 <= key < 0x100 or key in
                (0x1000000, 0x1000001, 0x1000003, 0x1000004, 0x1000005,
                 0x1000007, 0x1000010, 0x1000011, 0x1000012, 0x1000013, 0x1000014, 0x1000015)):
            raise ValueError('Unsupported key')
        if not isinstance(text, str) or len(text.encode()) > 64 or any(ord(c) < 32 for c in text):
            raise ValueError('Invalid input text')
        if type(value.get('mods', 0)) is not int or value.get('mods', 0) & ~0x0e000000:
            raise ValueError('Unsupported modifiers')
    else:
        raise ValueError('Unsupported input event')
    return value

@dataclasses.dataclass(frozen=True)
class Runtime:
    uid: int
    gid: int
    user: str
    home: str
    directory: str
    manifest: str
    socket: str


def live_socket(endpoint):
    # Inspect the kernel's listening sockets without connecting: a probe could
    # wake a socket-activated server, contrary to "already running" semantics.
    try:
        for line in Path('/proc/net/unix').read_text().splitlines()[1:]:
            fields = line.split(maxsplit=7)
            if len(fields) == 8 and fields[7] == str(endpoint):
                return int(fields[3], 16) & 0x10000 != 0 and int(fields[4], 16) == 1
    except OSError:
        return False
    return False

def discover(config, runtime_root=Path('/run/user')):
    """Only root-configured owners and system-installed runtime manifests qualify."""
    result = []
    for user in config.get('allow_users', []):
        try:
            account = pwd.getpwnam(user)
            if account.pw_uid == 0:
                continue
            directory = runtime_root / str(account.pw_uid)
            info = directory.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != account.pw_uid or info.st_mode & 0o022:
                continue
            for candidate in config.get('runtimes', []):
                manifest = Path(candidate['manifest'])
                resolved = manifest.resolve(strict=True)
                info = resolved.stat()
                # A runtime loads code into its presenter, never the root broker.
                if info.st_uid != 0 or info.st_mode & 0o022 or not stat.S_ISREG(info.st_mode):
                    continue
                relative = Path(candidate['socket'])
                if relative.is_absolute() or '..' in relative.parts:
                    continue
                endpoint = directory / relative
                info = endpoint.lstat()
                if stat.S_ISSOCK(info.st_mode) and info.st_uid == account.pw_uid and live_socket(endpoint):
                    result.append(Runtime(account.pw_uid, account.pw_gid, user, account.pw_dir,
                                          str(directory), str(resolved), str(endpoint)))
        except (KeyError, FileNotFoundError, PermissionError):
            continue
    # Some installations offer several manifests for the same compositor socket.
    return list({(r.uid, r.socket): r for r in reversed(result)}.values())[:4]

class Presenter:
    def __init__(self, broker, runtime):
        self.broker, self.runtime = broker, runtime
        self.queue = asyncio.Queue(maxsize=1)
        self.process = None
        self.writer = None
        self.task = None

    def frame(self, data):
        if self.queue.full():
            self.queue.get_nowait()
        self.queue.put_nowait(data)

    async def run(self):
        parent, child = socket.socketpair()
        child.set_inheritable(True)
        r = self.runtime
        env = {'PATH': '/usr/local/bin:/usr/bin:/bin', 'HOME': r.home, 'USER': r.user,
               'LOGNAME': r.user, 'XDG_RUNTIME_DIR': r.directory, 'XR_RUNTIME_JSON': r.manifest,
               'XDG_CONFIG_HOME': '/var/empty/lightxr', 'XDG_DATA_HOME': '/var/empty/lightxr',
               'XDG_CONFIG_DIRS': '/etc/lightxr/empty', 'XDG_DATA_DIRS': '/usr/share',
               'LANG': 'C.UTF-8'}
        privileges = {'user': r.uid, 'group': r.gid, 'extra_groups': os.getgrouplist(r.user, r.gid)} if os.geteuid() == 0 else {}
        try:
            self.process = await asyncio.create_subprocess_exec(
                self.broker.presenter, '--bridge-fd', str(child.fileno()),
                pass_fds=(child.fileno(),), env=env, start_new_session=True, **privileges)
            child.close()
            reader, self.writer = await asyncio.open_connection(sock=parent)
            ready = asyncio.Event()
            async def frames():
                await asyncio.wait_for(ready.wait(), 20)
                while True:
                    await send(self.writer, await self.queue.get())
            async def inputs():
                tokens, start = 0, time.monotonic()
                while True:
                    payload = await packet(reader, MAX_INPUT)
                    if payload == b'R' and not ready.is_set():
                        ready.set()
                        continue
                    validate_input(payload)
                    now = time.monotonic()
                    if now-start >= 1:
                        tokens, start = 0, now
                    tokens += 1
                    if tokens > 500:
                        raise ValueError('Input rate exceeded')
                    if self.broker.greeter and self.broker.visible:
                        await self.broker.input(payload)
            tasks = [asyncio.create_task(frames()), asyncio.create_task(inputs()),
                     asyncio.create_task(self.process.wait())]
            try:
                await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            finally:
                for task in tasks: task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        finally:
            child.close()
            parent.close()
            if self.writer:
                self.writer.close()
            if self.process and self.process.returncode is None:
                self.process.terminate()
                try: await asyncio.wait_for(self.process.wait(), 3)
                except asyncio.TimeoutError:
                    self.process.kill(); await self.process.wait()

class Broker:
    def __init__(self, config, presenter, greeter_uid):
        self.config, self.presenter, self.greeter_uid = config, presenter, greeter_uid
        self.greeter = None
        self.visible = False
        self.latest = None
        self.presenters = {}
        self.input_lock = asyncio.Lock()

    async def input(self, payload):
        async with self.input_lock:
            if self.greeter:
                await send(self.greeter, payload)

    async def stop_presenters(self):
        tasks = [p.task for p in self.presenters.values() if p.task]
        self.presenters.clear()
        for task in tasks: task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        self.latest = None

    async def accept(self, reader, writer):
        raw = writer.get_extra_info('socket').getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        pid, uid, gid = struct.unpack('3i', raw)
        if uid != self.greeter_uid or self.greeter is not None:
            writer.close(); return
        self.greeter = writer
        LOG.info('Greeter connected (pid %d)', pid)
        try:
            while True:
                payload = await packet(reader, FRAME_SIZE)
                if payload == b'H':
                    self.visible = False
                    await self.stop_presenters()
                elif valid_frame(payload):
                    self.visible = True
                    self.latest = payload
                    for presenter in self.presenters.values(): presenter.frame(payload)
                else:
                    raise ValueError('Only fixed-size frames and hide are accepted from greeter')
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError, ValueError):
            pass
        finally:
            self.visible = False
            self.greeter = None
            await self.stop_presenters()
            writer.close()
            LOG.info('Greeter disconnected; XR presentation removed')

    async def monitor(self):
        while True:
            if self.greeter and self.visible:
                discovered = {(r.uid, r.socket): r for r in discover(self.config)}
                for key in list(self.presenters):
                    p = self.presenters[key]
                    if key not in discovered or p.task.done():
                        p.task.cancel()
                        await asyncio.gather(p.task, return_exceptions=True)
                        del self.presenters[key]
                for key, runtime in discovered.items():
                    if key not in self.presenters:
                        p = Presenter(self, runtime)
                        self.presenters[key] = p
                        if self.latest: p.frame(self.latest)
                        p.task = asyncio.create_task(p.run())
            await asyncio.sleep(1)

async def main(args):
    if os.geteuid() != 0 and not args.test:
        raise SystemExit('The production seat broker must run as root')
    config = json.loads(Path(args.config).read_text())
    if not args.test:
        info = Path(args.config).stat()
        if info.st_uid != 0 or info.st_mode & 0o022:
            raise SystemExit('Broker configuration must be root-owned and not group/world writable')
    uid = os.getuid() if args.test else pwd.getpwnam('lightxr').pw_uid
    broker = Broker(config, args.presenter, uid)
    endpoint = Path(args.socket)
    endpoint.parent.mkdir(mode=0o750, parents=True, exist_ok=True)
    if not args.test:
        os.chown(endpoint.parent, 0, grp.getgrnam('lightxr').gr_gid)
    # Never unlink an arbitrary path, or an active broker's endpoint.
    if endpoint.exists():
        raise SystemExit('Broker socket already exists; refusing to replace it')
    server = await asyncio.start_unix_server(broker.accept, path=endpoint)
    os.chmod(endpoint, 0o600 if args.test else 0o660)
    if not args.test: os.chown(endpoint, 0, grp.getgrnam('lightxr').gr_gid)
    monitor = asyncio.create_task(broker.monitor())
    try:
        async with server: await server.serve_forever()
    finally:
        monitor.cancel()
        await broker.stop_presenters()
        endpoint.unlink(missing_ok=True)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config', default='/etc/lightxr/broker.json')
    p.add_argument('--socket', default='/run/lightxr-broker/bridge.sock')
    p.add_argument('--presenter', default='/usr/local/libexec/lightxr/lightxr-presenter')
    p.add_argument('--test', action='store_true', help='Unprivileged isolated tests only')
    args = p.parse_args()
    logging.basicConfig(level=logging.INFO)
    try: asyncio.run(main(args))
    except KeyboardInterrupt: pass

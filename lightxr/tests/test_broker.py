#!/usr/bin/python3
import asyncio
import importlib.util
import json
import os
from pathlib import Path
import pwd
import socket
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('lightxr_broker', Path(__file__).parents[1]/'broker/lightxr-broker.py')
b = importlib.util.module_from_spec(spec);sys.modules[spec.name] = b;spec.loader.exec_module(b)
FRAME = b'F'+struct.pack('!II', b.WIDTH, b.HEIGHT)+bytes(b.WIDTH*b.HEIGHT*4)

def event(value): return b'I'+json.dumps(value).encode()

class Validation(unittest.TestCase):
    def test_frame_and_bounds(self):
        self.assertTrue(b.valid_frame(FRAME))
        for frame in [b'F', FRAME[:-1], b'I'+FRAME[1:], FRAME[:1]+struct.pack('!II',1,1)+FRAME[9:]]:
            self.assertFalse(b.valid_frame(frame))
    def test_input_whitelist(self):
        for kind in ['move','down','up']:
            self.assertEqual(b.validate_input(event({'type':kind,'x':100,'y':200,'button':1}))['type'],kind)
        b.validate_input(event({'type':'key','key':0,'text':'日本語'}))
        for value in [{'type':'command','exec':'sh'}, {'type':'move','x':-1,'y':0},
                      {'type':'move','x':1280,'y':0}, {'type':'move','x':float('nan'),'y':0},
                      {'type':'down','x':0,'y':0,'button':True},
                      {'type':'key','text':'a\n'}, {'type':'key','text':'x'*65},
                      {'type':'key','key':9999999}, {'type':'key','mods':1},
                      {'type':'key','text':'x','exec':'sh'}]:
            with self.assertRaises((ValueError,TypeError)): b.validate_input(event(value))
    def test_listener_after_connected_endpoints(self):
        rows = """Num RefCount Protocol Flags Type St Inode Path
0: 00000003 00000000 00000000 0001 03 1 /run/user/1000/wivrn/comp_ipc
1: 00000002 00000000 00010000 0001 01 2 /run/user/1000/wivrn/comp_ipc
"""
        with patch.object(Path, 'read_text', return_value=rows):
            self.assertTrue(b.live_socket('/run/user/1000/wivrn/comp_ipc'))
        with patch.object(Path, 'read_text', return_value=rows.split('1:')[0]):
            self.assertFalse(b.live_socket('/run/user/1000/wivrn/comp_ipc'))

    def test_discovery_owner_and_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);runtime=root/str(os.getuid());runtime.mkdir(mode=0o700)
            sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);sock.bind(str(runtime/'monado_comp_ipc'));sock.listen(1)
            try:
                config={'allow_users':[pwd.getpwuid(os.getuid()).pw_name],
                        'runtimes':[{'socket':'monado_comp_ipc','manifest':'/usr/share/openxr/1/openxr_monado.json'}]}
                if Path(config['runtimes'][0]['manifest']).exists():
                    result=b.discover(config,root);self.assertEqual(len(result),1)
                    self.assertEqual(result[0].uid,os.getuid())
                    self.assertEqual(b.discover({**config,'allow_users':[]},root),[])
                    runtime.chmod(0o777);self.assertEqual(b.discover(config,root),[]);runtime.chmod(0o700)
                    config['runtimes'][0]['socket']='../escape';self.assertEqual(b.discover(config,root),[])
            finally:sock.close()

class Bridge(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)/'bridge.sock'
        self.broker=b.Broker({'allow_users':[]},'/no/presenter',os.getuid())
        self.server=await asyncio.start_unix_server(self.broker.accept,path=self.path)
        self.clients=[]
    async def asyncTearDown(self):
        for reader,writer in self.clients:writer.close()
        await asyncio.sleep(.02)
        self.server.close();await self.server.wait_closed()
        await self.broker.stop_presenters();self.temp.cleanup()
    async def connect(self):
        pair=await asyncio.open_unix_connection(self.path);self.clients.append(pair);return pair
    async def wait(self,pred):
        for _ in range(100):
            if pred():return
            await asyncio.sleep(.01)
        self.fail('Broker state timed out')
    async def test_frames_hide_and_disconnect(self):
        reader,writer=await self.connect();await b.send(writer,FRAME)
        await self.wait(lambda:self.broker.visible)
        self.assertEqual(self.broker.latest,FRAME)
        await b.send(writer,b'H');await self.wait(lambda:not self.broker.visible)
        self.assertIsNone(self.broker.latest)
        await b.send(writer,FRAME);await self.wait(lambda:self.broker.visible)
        writer.close();await self.wait(lambda:self.broker.greeter is None)
        self.assertFalse(self.broker.visible);self.assertIsNone(self.broker.latest)
    async def test_reject_other_user(self):
        self.broker.greeter_uid=os.getuid()+1
        reader,writer=await self.connect()
        self.assertEqual(await asyncio.wait_for(reader.read(1),1),b'')
        self.assertIsNone(self.broker.greeter)
    async def test_only_one_greeter(self):
        one,writer=await self.connect();await b.send(writer,FRAME)
        await self.wait(lambda:self.broker.visible)
        two,other=await self.connect();self.assertEqual(await asyncio.wait_for(two.read(1),1),b'')
        self.assertTrue(self.broker.visible)
    async def test_reject_malformed_or_greeter_input(self):
        reader,writer=await self.connect();await b.send(writer,event({'type':'key','text':'spoof'}))
        self.assertEqual(await asyncio.wait_for(reader.read(1),1),b'')
        self.assertIsNone(self.broker.greeter)
    async def test_limit_before_allocation(self):
        reader,writer=await self.connect();writer.write(struct.pack('!I',0x7fffffff));await writer.drain()
        self.assertEqual(await asyncio.wait_for(reader.read(1),1),b'')
    async def test_input_relay(self):
        reader,writer=await self.connect();await b.send(writer,FRAME)
        await self.wait(lambda:self.broker.visible)
        payload=event({'type':'key','key':0,'text':'fixture'})
        await self.broker.input(payload)
        self.assertEqual(await b.packet(reader,1024),payload)
    async def test_presenter_reap_and_latest_frame(self):
        helper=Path(self.temp.name)/'fixture-presenter'
        helper.write_text('#!/usr/bin/python3\nimport socket,sys,struct,json\ns=socket.socket(fileno=int(sys.argv[2]));s.sendall(struct.pack("!I",1)+b"R")\ndef exact(n):\n b=b""\n while len(b)<n:b+=s.recv(n-len(b))\n return b\nn=struct.unpack("!I",exact(4))[0];data=exact(n)\nassert data[0:1]==b"F"\nx=b"I"+json.dumps({"type":"move","x":40,"y":50}).encode();s.sendall(struct.pack("!I",len(x))+x)\nwhile s.recv(4096):pass\n')
        helper.chmod(0o755)
        account=pwd.getpwuid(os.getuid());runtime=b.Runtime(os.getuid(),os.getgid(),account.pw_name,account.pw_dir,self.temp.name,'/unused','/unused')
        self.broker.presenter=str(helper)
        reader,writer=await self.connect();await b.send(writer,FRAME)
        await self.wait(lambda:self.broker.visible)
        presenter=b.Presenter(self.broker,runtime);presenter.frame(FRAME)
        presenter.task=asyncio.create_task(presenter.run());self.broker.presenters['fixture']=presenter
        payload=await asyncio.wait_for(b.packet(reader,1024),3)
        self.assertEqual(b.validate_input(payload)['x'],40)
        await b.send(writer,b'H');await self.wait(lambda:not self.broker.presenters)
        await self.wait(lambda:presenter.process.returncode is not None)

if __name__=='__main__':unittest.main()

#!/usr/bin/env python3
"""Exercise the real liblightdm protocol and FLTK XR bridge against a fake daemon."""
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
signal.pthread_sigmask(signal.SIG_SETMASK, [])
def integer(n): return struct.pack('!I', n)
def string(s):
    b=s.encode(); return integer(len(b))+b
def recvexact(s,n):
    data=b''
    while len(data)<n:
        chunk=s.recv(n-len(data))
        if not chunk: raise EOFError('Fixture disconnected')
        data+=chunk
    return data
with tempfile.TemporaryDirectory(prefix='lightxr-fltk-test-') as tmp:
    daemon,client=socket.socketpair();daemon.settimeout(10)
    bridge=socket.socket(socket.AF_UNIX);endpoint=tmp+'/bridge';bridge.bind(endpoint);bridge.listen();bridge.settimeout(10)
    env={**os.environ,'LIGHTDM_TO_SERVER_FD':str(client.fileno()),'LIGHTDM_FROM_SERVER_FD':str(client.fileno())}
    process=subprocess.Popen([sys.argv[1],'--windowed','--bridge',endpoint],env=env,pass_fds=(client.fileno(),),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    failures=[];attempts=0;starts=0;sequence=0;cancelled=threading.Event();failed=threading.Event();succeeded=threading.Event();prompt=threading.Event();hidden=threading.Event();frames=threading.Event()
    def send(id,body):daemon.sendall(integer(id)+integer(len(body))+body)
    def serve():
        global attempts, starts, sequence
        try:
            while True:
                id,n=struct.unpack('!II',recvexact(daemon,8));body=recvexact(daemon,n)
                if id==0:send(7,integer(1)+string('test-lightdm')+integer(2)+string('default-session')+string('plasma')+string('select-user')+string('alice'))
                elif id==1:
                    attempts+=1;sequence=struct.unpack('!I',body[:4])[0]
                    assert body[8:]==b'alice',repr(body)
                    send(1,integer(sequence)+string('alice')+integer(2)+integer(1)+string('Password:')+integer(2)+string('One-time code:'));prompt.set()
                elif id==3:
                    assert body==integer(2)+string('test-secret')+string('123456'),repr(body)
                    send(2,integer(sequence)+string('alice')+integer(0 if attempts>=3 else 7))
                    (succeeded if attempts>=3 else failed).set()
                elif id==5:send(2,integer(sequence)+string('alice')+integer(26));cancelled.set()
                elif id==4:
                    assert attempts==3,'No session before success'
                    assert body==string('dome'),repr(body)
                    starts+=1;send(3,integer(0))
                else:raise AssertionError(f'Unexpected LightDM message {id}')
        except (EOFError,OSError):pass
        except BaseException as e:failures.append(repr(e))
    thread=threading.Thread(target=serve,daemon=True);thread.start()
    peer=None
    def receive_bridge():
        try:
            while True:
                n=struct.unpack('!I',recvexact(peer,4))[0];assert n<=1280*800*4+9
                data=recvexact(peer,n)
                if data==b'H':hidden.set()
                elif data[:1]==b'F':
                    assert struct.unpack('!II',data[1:9])==(1280,800)
                    assert len(data)==9+1280*800*4
                    frames.set()
        except (EOFError,OSError):pass
        except BaseException as e:failures.append(repr(e))
    def remote(value):
        data=b'I'+json.dumps(value).encode();peer.sendall(integer(len(data))+data);time.sleep(.25)
    def click(x,y):
        remote({'type':'down','x':x,'y':y,'button':1});remote({'type':'up','x':x,'y':y,'button':1})
    def key(text):remote({'type':'key','key':0,'text':text})
    def wait(e,label):
        assert e.wait(8),label+': '+repr(failures)
        assert not failures,failures
    try:
        peer,_=bridge.accept();peer.settimeout(10);threading.Thread(target=receive_bridge,daemon=True).start();wait(frames,'Full-size frame')
        # Native FLTK selection and modifier handling through the XR wire protocol.
        click(300,120)
        remote({'type':'key','key':ord('A'),'text':'a','modifiers':0x04000000})
        key('wrong')
        remote({'type':'key','key':ord('A'),'text':'a','modifiers':0x04000000})
        key('alice')
        # Releasing outside the pressed button must cancel activation.
        remote({'type':'down','x':300,'y':280,'button':1})
        remote({'type':'move','x':1100,'y':450,'button':1})
        remote({'type':'up','x':1100,'y':450,'button':1})
        assert attempts==0,'Drag-out activated a button'
        click(300,280);wait(prompt,'First prompt');assert starts==0
        click(550,280);wait(cancelled,'Cancellation');time.sleep(.4);assert starts==0
        for attempt in (2,3):
            prompt.clear();click(300,280);wait(prompt,'Password prompt');key('test-secretX')
            remote({'type':'key','key':0x01000012}) # Left
            remote({'type':'key','key':0x01000007}) # Delete trailing X
            click(300,280);key('12345x')
            remote({'type':'key','key':0x01000003}) # Backspace
            key('6');click(300,280)
            wait(failed if attempt==2 else succeeded,'Authentication result');time.sleep(.4)
            if attempt==2:assert starts==0
        wait(hidden,'Hide before session handoff');assert starts==1;assert not failures,failures
        print('PASS: FLTK greeter, real LightDM multi-prompt protocol, cancellation/failure, gated DomeD session, XR native selection/modifiers/navigation/drag-out, RGBA frames and hide handoff')
    finally:
        process.terminate()
        try:out,err=process.communicate(timeout=3)
        except subprocess.TimeoutExpired:process.kill();out,err=process.communicate()
        if failures:print(err.decode(errors='replace'),file=sys.stderr)
        if peer:peer.close()
        bridge.close();daemon.close();client.close()

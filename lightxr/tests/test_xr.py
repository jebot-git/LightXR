#!/usr/bin/python3
"""Run the real greeter→broker→cylinder presenter against a running OpenXR runtime."""
import argparse, json, os, pwd, signal, subprocess, tempfile, time
from pathlib import Path
signal.pthread_sigmask(signal.SIG_SETMASK, [])
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=Path(__file__).resolve().parents[2]/'build')
p.add_argument('--manifest',default='/usr/share/openxr/1/openxr_monado.json')
p.add_argument('--runtime-socket',default='monado_comp_ipc')
p.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[2]/'artifacts/xr')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='lightxr-test-') as directory:
    temp=Path(directory);endpoint=temp/'bridge.sock';report=a.output/'presenter.json';report.unlink(missing_ok=True)
    wrapper=temp/'presenter';wrapper.write_text('#!/bin/sh\nexec "'+str(a.build/'lightxr-presenter')+'" "$@" --seconds 2 --report "'+str(report)+'"\n');wrapper.chmod(0o755)
    config=temp/'broker.json';config.write_text(json.dumps({'allow_users':[pwd.getpwuid(os.getuid()).pw_name],'runtimes':[{'manifest':a.manifest,'socket':a.runtime_socket}]}))
    children=[]
    with (a.output/'broker.log').open('w') as log, (a.output/'greeter.log').open('w') as glog:
        try:
            broker=subprocess.Popen(['/usr/bin/python3',str(root/'lightxr/broker/lightxr-broker.py'),'--test','--socket',str(endpoint),'--config',str(config),'--presenter',str(wrapper)],stdout=log,stderr=log);children.append(broker)
            deadline=time.monotonic()+20
            while not endpoint.exists():
                if broker.poll() is not None or time.monotonic()>deadline:raise AssertionError('Broker did not start')
                time.sleep(.05)
            greeter=subprocess.Popen([str(a.build/'lightxr-greeter'),'--demo','--windowed','--bridge',str(endpoint)],env={**os.environ,'QT_QPA_PLATFORM':'offscreen'},stdout=glog,stderr=glog);children.append(greeter)
            while not report.exists():
                if greeter.poll() is not None or broker.poll() is not None or time.monotonic()>deadline:raise AssertionError('XR report timed out; inspect '+str(a.output/'broker.log'))
                time.sleep(.1)
            result=json.loads(report.read_text());assert result['frames']>0 and result['visible']>0 and result['curved'] and result['distance_m']==2,result
            print('PASS: real greeter frames relayed through seat broker to native 2m cylinder;',result['visible'],'visible XR frames')
        finally:
            for child in reversed(children):
                if child.poll() is None:child.terminate()
            for child in reversed(children):
                try:child.wait(timeout=5)
                except subprocess.TimeoutExpired:child.kill();child.wait()

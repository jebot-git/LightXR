#!/usr/bin/python3
import json, pathlib, subprocess, sys, tempfile
wrapper=pathlib.Path(__file__).parents[1]/'packaging/lightxr-session'
with tempfile.TemporaryDirectory() as directory:
    root=pathlib.Path(directory);program=root/'session with spaces.py'
    program.write_text('import json,sys;print(json.dumps(sys.argv[1:]))')
    args=['/usr/bin/python3',str(program),'literal $HOME','$(echo unsafe)','space value']
    import shlex
    result=subprocess.run([sys.executable,str(wrapper),shlex.join(args)],capture_output=True,text=True,check=True)
    assert json.loads(result.stdout)==args[2:]
    assert subprocess.run([sys.executable,str(wrapper),''],capture_output=True).returncode!=0
print('PASS: desktop Exec quoting, literal shell metacharacters, and empty-session rejection')

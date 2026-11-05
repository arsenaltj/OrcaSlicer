"""Run one explicit build and reject any source-input change during compilation."""
import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys

parser=argparse.ArgumentParser()
parser.add_argument('--input-root',type=pathlib.Path,action='append',required=True)
parser.add_argument('--log',type=pathlib.Path,required=True)
parser.add_argument('--snapshot',type=pathlib.Path,required=True)
parser.add_argument('command',nargs=argparse.REMAINDER)
args=parser.parse_args()
command=args.command[1:] if args.command[:1]==['--'] else args.command
if not command:
    parser.error('An explicit build command is required')

def capture():
    inputs={}
    for root in args.input_root:
        for file in ([root] if root.is_file() else sorted(root.rglob('*'))):
            if file.is_file() and '__pycache__' not in file.parts:
                inputs[str(file.resolve())]=hashlib.sha256(file.read_bytes()).hexdigest()
    return inputs

args.log.parent.mkdir(parents=True,exist_ok=True)
args.snapshot.parent.mkdir(parents=True,exist_ok=True)
before=capture()
state={'schemaVersion':1,'startedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
       'command':command,'before':before,'state':'started'}
args.snapshot.write_text(json.dumps(state,indent=2),encoding='utf-8')
with args.log.open('w',encoding='utf-8') as output:
    result=subprocess.run(command,stdout=output,stderr=subprocess.STDOUT,shell=False)
after=capture()
changed=[name for name in sorted(set(before)|set(after)) if before.get(name)!=after.get(name)]
state.update({'finishedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'after':after,'changedInputs':changed,'buildExitCode':result.returncode,
              'state':'passed' if not changed and result.returncode==0 else 'failed'})
args.snapshot.write_text(json.dumps(state,indent=2),encoding='utf-8')
print('\n'.join(args.log.read_text(encoding='utf-8',errors='replace').splitlines()[-30:]))
print(json.dumps({'state':state['state'],'buildExitCode':result.returncode,'changedInputs':changed,
                  'snapshot':str(args.snapshot.resolve()),'log':str(args.log.resolve())}))
sys.exit(79 if changed else result.returncode)

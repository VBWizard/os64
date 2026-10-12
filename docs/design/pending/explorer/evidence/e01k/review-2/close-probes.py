from pathlib import Path
import subprocess, sys, time
phase=sys.argv[1]
log=Path('/tmp/e1k-vm/serial-close-'+phase+'.log')
qmp='/tmp/e1k-vm/qmp.py'
def text():return log.read_text(errors='replace')
def run(*args):subprocess.run(['python3',qmp,*args],check=True)
def wait_for(test,seconds=15):
 deadline=time.monotonic()+seconds
 while time.monotonic()<deadline:
  if test():return
  time.sleep(.1)
 raise SystemExit('Guest checkpoint timed out')
def exits():return [line for line in text().splitlines() if '(popuptest) exited code' in line]
for mode in ['owned','full','ordinary']:
 run('key','ctrl-alt-f1');time.sleep(.5)
 ready=text().count('close probe READY');count=len(exits())
 run('type',f'/tests/popuptest --close-{mode} > /home/close-{phase}-{mode}.txt')
 wait_for(lambda:text().count('close probe READY')>ready)
 run('key','ctrl-alt-f8');time.sleep(.5)
 run('key','alt-f4');time.sleep(.4);run('key','alt-f4')
 wait_for(lambda:len(exits())>count)
 result=exits()[-1];print(mode+': '+result,flush=True)
 expected=1 if phase=='before' and mode!='ordinary' else 0
 if not result.endswith('exited code '+str(expected)):raise SystemExit('Unexpected exit')
run('key','ctrl-alt-f1');time.sleep(.5);run('type','sync')

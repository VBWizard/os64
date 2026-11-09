from pathlib import Path
import subprocess, time
qmp='/tmp/e1k-vm/qmp.py'
log=Path('/tmp/e1k-vm/serial-wake-after.log')
def exits():
    return [x for x in log.read_text(errors='replace').splitlines() if '(popuptest) exited code' in x]
subprocess.run(['python3',qmp,'key','ctrl-alt-f1'],check=True)
time.sleep(1)
for n in range(1,11):
    count=len(exits())
    subprocess.run(['python3',qmp,'type',f'/tests/popuptest > /home/wake-after-{n}.txt'],check=True)
    for _ in range(150):
        result=exits()
        if len(result)>count:
            print(f'Run {n}: {result[-1]}',flush=True)
            if not result[-1].endswith('exited code 0'):raise SystemExit(1)
            break
        time.sleep(.1)
    else:raise SystemExit(f'Run {n}: no exit within 15 seconds')
subprocess.run(['python3',qmp,'type','sync'],check=True)

"""Run the current R6 regressions against the reviewed menu implementation."""
import argparse, os, subprocess, hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[7]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
revision='e37cca21190574902b60cd9c6e199f9a7bee2969'
flags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
       '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections',
       '-fdata-sections','-Wl,--gc-sections']
flags+=['-I'+str(ROOT/s) for s in ['userland/libos64/include','userland/libos64','abi/include','tools/fonts']]
modules=['ui_menu','ui_menu_widget','ui_dispatcher','ui_icon','ui_action','ui_status',
         'ui_font','ui','ui_controls','ui_list','ui_text','font_provider','font_adopt',
         'text','fmt','ui_theme','text_cache','text_decode','text_bitmap','text_draw','draw','str']
paths=[ROOT/'tools/test_ui_menu_host.c',ROOT/'tools/fonts/fake_backend.c']
for module in modules:
    path=ROOT/f'userland/libos64/{module}.c'
    if module in ('ui_menu','ui_menu_widget'):
        # R6 adds a private function declaration; the session/popup layouts
        # are compatible with this reviewed revision. Other modules are live.
        data=subprocess.check_output(['git','show',f'{revision}:userland/libos64/{module}.c'],cwd=ROOT)
        path=a.output/f'{module}.c';path.write_bytes(data)
        print(module,hashlib.sha256(data).hexdigest(),flush=True)
    paths.append(path)
binary=a.output/'baseline'
subprocess.run(['cc',*flags,*map(str,paths),'-o',str(binary)],check=True)
env=dict(os.environ,E1B_R6_ONLY='1');env.setdefault('ASAN_OPTIONS','detect_leaks=1')
result=subprocess.run([str(binary),str(ROOT/'userland/libfreetype/fixtures')],env=env,
                      stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
(a.output/'result.txt').write_text(result.stdout)
print(result.stdout,end='')
assert result.returncode and 'FAIL ' in result.stdout,'baseline unexpectedly passed'
assert 'runtime error:' not in result.stdout and 'ERROR: AddressSanitizer' not in result.stdout,'baseline fixture faulted'
print('Expected behavioral failures reproduced; no fixture fault.')

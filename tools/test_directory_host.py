#!/usr/bin/env python3
import argparse, os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parent.parent
parser=argparse.ArgumentParser();parser.add_argument('--build-only',action='store_true')
parser.add_argument('--output',type=Path)
args=parser.parse_args()
out_dir=args.output or Path(tempfile.mkdtemp(prefix='roam-model-directory-'))
out_dir.mkdir(parents=True,exist_ok=True)
out=out_dir/'test_directory_host'
flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
       '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-pthread',
       '-I'+str(root/'userland/libos64/include'),'-I'+str(root/'abi/include')]
src=[root/s for s in ['tools/test_directory_host_support.c','tools/test_directory_host.c',
                      'userland/libos64/directory.c','userland/libos64/ui_data.c']]
subprocess.run(['cc',*flags,*map(str,src),'-o',str(out)],check=True)
print('Artifacts:',out_dir,flush=True)
if args.build_only: raise SystemExit(0)
raise SystemExit(subprocess.run([str(out)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'}).returncode)

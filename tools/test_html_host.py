#!/usr/bin/env python3
"""Compare the native parser against the pinned upstream reference fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from html_reference import ROOT, cases, identity, inventory
STATES={'Data state':0,'RCDATA state':1,'RAWTEXT state':2,'Script data state':3,'PLAINTEXT state':4,'CDATA section state':5}

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--driver',required=True);ap.add_argument('--tokenizer-only',action='store_true');args=ap.parse_args()
    manifest=json.loads((ROOT/'PINNED.json').read_text())
    actual={str(p.relative_to(ROOT)) for folder in ('tokenizer','tree','standard') for p in (ROOT/folder).rglob('*') if p.is_file()}
    assert actual == set(manifest['files']), 'unrecorded or missing fixture file'
    for path, digest in manifest['files'].items():
        assert hashlib.sha256((ROOT/path).read_bytes()).hexdigest()==digest, path
    counts, excluded=inventory()
    assert counts==json.loads((ROOT/'INVENTORY.json').read_text()), 'stale inventory'
    assert excluded==(ROOT/'SKIPS.tsv').read_text().splitlines(), 'stale skip manifest'
    skips={line.split('\t')[0] for line in (ROOT/'SKIPS.tsv').read_text().splitlines()}
    selected=[c for c in cases() if identity(c) not in skips and (not args.tokenizer_only or c['suite']=='tokenizer')]
    with tempfile.TemporaryFile() as batch, tempfile.TemporaryFile() as output:
        for c in selected:
            if c['suite']=='tokenizer':
                data=b''.join(struct.pack('<I',ord(ch)) for ch in c['input']);last=c['lastStartTag'].encode();kind=0;state=STATES[c['state']]
            else:
                data=c['input'].encode('utf-8');last=b'';kind=1;state=0
            batch.write(struct.pack('<4I',kind,state,len(data),len(last))+data+last)
        batch.seek(0)
        subprocess.run([args.driver],stdin=batch,stdout=output,check=True,timeout=120)
        output.seek(0);failures=[]
        for c,line in zip(selected,output,strict=True):
            result=json.loads(line.decode('utf-8','surrogatepass'))
            if c['suite']=='tree':
                if result['tree']!=c['expected'] or result['refusal']:
                    failures.append({'id':identity(c),'input':c['input'],'expected':c['expected'],'actual':result['tree'],'refusal':result['refusal']})
                continue
            tokens=[]
            for t in result['tokens']:
                if t[0]=='Codepoint':
                    if tokens and tokens[-1][0]=='Character':tokens[-1][1]+=chr(t[1])
                    else:tokens.append(['Character',chr(t[1])])
                else:tokens.append(t)
            expected_errors=[e['code'] for e in c['errors']]
            if tokens!=c['expected'] or result['refusal'] or result['errors']!=expected_errors[:16] or result['parse_errors']!=len(expected_errors):
                failures.append({'id':identity(c),'input':c['input'],'state':c['state'],'expected':c['expected'],'actual':tokens,'refusal':result['refusal'],'expected_errors':expected_errors,'actual_errors':result['errors']})
        if failures:
            failure_path=Path(args.driver).parent/'failures.json'
            failure_path.write_text(json.dumps(failures,indent=2,ensure_ascii=True)+'\n')
            print(f'Failure details: {failure_path}')
        print(f'Reference: run={len(selected)} passed={len(selected)-len(failures)} failed={len(failures)} skipped={len(skips)}')
        for f in failures[:10]:print(json.dumps(f,ensure_ascii=True)[:1600])
        if failures:return True
    with tempfile.TemporaryFile() as batch:
        for c in selected:
            data=c['input'].encode('utf-8','surrogatepass')
            batch.write(struct.pack('<4I',1,0,len(data),0)+data)
        batch.seek(0)
        subprocess.run([args.driver,'--safety'],stdin=batch,check=True,timeout=300)
    return False
if __name__=='__main__':raise SystemExit(main())

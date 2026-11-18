#!/usr/bin/env python3
"""Replay pointer-free failure-observation counterexamples using retained runtime host artifacts.

First run JS_RUNTIME_KEEP=1 tools/test_js_runtime_host.sh. Retained driver,
adapter and engine objects must belong to this checkout; failures here are
negative sensitivity evidence, not positive sanitizer/leak evidence.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts",required=True,type=Path)
    parser.add_argument("--output",required=True,type=Path)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    artifacts=args.artifacts.resolve(); output=args.output.resolve()
    output.mkdir(parents=True,exist_ok=False)
    source=(root/'userland/libjs/runtime/runtime.c').read_text()
    variants={
        'substitute-primary-ID':('result.owner = found->owner;', 'result.owner = metadata->bindings[0].owner;'),
        'assign-unpublished-row':('found->assigned ? JS_OWNER_FAILURE_ASSIGNED : JS_OWNER_FAILURE_UNPUBLISHED;',
            'found->assigned ? JS_OWNER_FAILURE_ASSIGNED : JS_OWNER_FAILURE_ASSIGNED;'),
        'unmapped-looks-complete':('*mapped = false;', '(void)mapped;'),
        'callback-as-activity':('snapshot.job_activity = owner_failure_ref(runtime, record->job_activity, &snapshot.owners_mapped);',
            'snapshot.job_activity = owner_failure_ref(runtime, record->job_callback, &snapshot.owners_mapped);'),
        'omit-code-candidates':('snapshot.task_owner = owner_failure_ref(runtime, record->task_owner, &snapshot.owners_mapped);',
            'snapshot.realm_count = 0;\n    snapshot.task_owner = owner_failure_ref(runtime, record->task_owner, &snapshot.owners_mapped);'),
        'checked-observation-escapes':('const os64_js_failure_record_t *record = &runtime->failure_record;',
            'owner_legacy_escape(runtime);\n    const os64_js_failure_record_t *record = &runtime->failure_record;'),
        'global-replaces-original':('.original=record->original, .global=record->global, .deadline=record->deadline,',
            '.original=record->global, .global=record->global, .deadline=record->deadline,'),
        'none-becomes-primary':('if (!realm) return result;\n    const OwnerMetadata *metadata = runtime->owner.metadata;',
            'if (!realm) { result.kind = JS_OWNER_FAILURE_ASSIGNED; result.owner = runtime->owner.metadata->bindings[0].owner; return result; }\n    const OwnerMetadata *metadata = runtime->owner.metadata;'),
        'omit-scope-tracking':('os64_js_status_t result = js_owner_runtime_create(config, policy, caller_abi, limits, out, outcome);\n    if (result == OS64_JS_OK) {\n        (*out)->failure_records_enabled = true;\n        JS_EnableExecutionScopeTracking((*out)->engine);',
            'os64_js_status_t result = js_owner_runtime_create(config, policy, caller_abi, limits, out, outcome);\n    if (result == OS64_JS_OK) {\n        (*out)->failure_records_enabled = true;'),
    }
    flags=['cc','-O2','-g','-std=gnu11','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin',
        '-fno-tree-loop-distribute-patterns','-fno-stack-protector','-DOS64_JS_TARGET',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-Djs_owner_loan_begin=js_owner_observed_loan_begin',
        '-DJS_Call=js_realm_observed_call','-Djs_owner_incarnation_next=js_owner_observed_incarnation_next']
    for path in ['libmath/include','libjs/port','libjs/include','libjs/runtime','libos64/include']:
        flags+=['-I'+str(root/'userland'/path)]
    flags+=['-I'+str(root/'abi/include'),'-isystem',str(root/'userland/obj/js/upstream')]
    maths=[]
    for line in subprocess.check_output(['make','-s','-C',str(root/'userland'),'math-host-flags'],text=True).splitlines():
        if line.startswith('MATH_OBJS='): maths=shlex.split(line.split('=',1)[1])[0].split()
    if not maths: raise RuntimeError('missing target maths inventory')
    common=[str(artifacts/(name+'.o')) for name in
            ['owner_incarnation','owner_tickets','intl','str-local','fmt','test_js_port_calendar','test_js_runtime_host']]
    engines={'target':[str(root/'userland/obj/js/core.o')],
             'instrumented':[str(artifacts/(name+'.o')) for name in
                             ['quickjs','dtoa','libregexp','libunicode','cutils','allocator','platform','format']]}
    results=[]
    for name,(old,new) in variants.items():
        if source.count(old)!=1: raise RuntimeError('mutation must match once: '+name)
        mutated=output/(name+'.c')
        mutated.write_text(source.replace(old,new).replace('#include "../intl/intl.h"',
            '#include "'+str(root/'userland/libjs/intl/intl.h')+'"'))
        obj=output/(name+'.o')
        subprocess.run(flags+['-c',str(mutated),'-o',str(obj)],check=True)
        for mode,engine in engines.items():
            binary=output/(name+'-'+mode)
            subprocess.run(['cc','-fsanitize=address,undefined','-pthread','-Wl,-z,noexecstack',
                *engine,*maths,str(obj),*common,'-o',str(binary)],check=True)
            run=subprocess.run([str(binary),'--owner-failures'],capture_output=True,text=True,timeout=60)
            log=run.stdout+run.stderr; (output/(name+'-'+mode+'.log')).write_text(log)
            failures=[line for line in log.splitlines() if line.startswith('FAIL: checked failures:')]
            if run.returncode!=1 or not failures:
                raise RuntimeError('mutation escaped behavioral oracle: '+name+'/'+mode+'\n'+log)
            results.append({'variant':name,'mode':mode,'oracle':failures[0]})
            print(name+'/'+mode+': '+failures[0],flush=True)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(str(len(results))+' checked failure mutations rejected')


if __name__=='__main__': main()

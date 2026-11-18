#!/usr/bin/env python3
"""Replay scoped settings-owner counterexamples using retained runtime host artifacts.

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
    first=source.index('JSOwnerStatus js_owner_scope_settings(')
    last=source.index('JSOwnerStatus js_owner_scope_promote(',first)
    block=source[first:last]
    def replace(old,new):
        if block.count(old)!=1: raise RuntimeError('missing unique observation anchor: '+old)
        return block.replace(old,new)
    blocks={
      'admission-follows-selected-role':replace('            snapshot.kind = JS_OWNER_SETTINGS_MAPPED;',
          '            snapshot.admitting_owner = binding->owner;\n            snapshot.kind = JS_OWNER_SETTINGS_MAPPED;'),
      'selected-context-is-primary':replace('os64_js_realm_t *realm = realm_for_context(runtime, context);',
          'os64_js_realm_t *realm = primary(runtime);'),
      'absent-role-guesses-admitting-context':replace('    JSContext *context = settings_context(runtime, role);',
          '    JSContext *context = settings_context(runtime, role);\n    if (!context) context = scope->realm->context;'),
      'unmapped-is-absent':replace('snapshot.kind = JS_OWNER_SETTINGS_UNMAPPED;', 'snapshot.kind = JS_OWNER_SETTINGS_ABSENT;'),
      'lost-staged-fact':replace('.staged=realm->staged', '.staged=false'),
      'lost-retired-fact':replace('.retired=realm->retired', '.retired=false'),
      'lost-primary-fact':replace('.primary=realm == primary(runtime)', '.primary=false'),
      'query-without-wrapper-frame':replace(' || !runtime->settings_calls', ''),
      'refusal-clobbers-output':replace('    if (!scope || !out || !settings_role_valid(role))',
          '    if (out) os64_memset(out, 0, sizeof(*out));\n    if (!scope || !out || !settings_role_valid(role))'),
      'implicit-raw-export':replace('    JSContext *context = settings_context(runtime, role);',
          '    owner_legacy_escape(runtime);\n    JSContext *context = settings_context(runtime, role);'),
      'clear-previous-raw-veto':replace('    JSContext *context = settings_context(runtime, role);',
          '    runtime->owner.metadata->table._unknown_escape = false;\n    JSContext *context = settings_context(runtime, role);'),
      'poll-health-during-observation':replace('    JSContext *context = settings_context(runtime, role);',
          '    observe(runtime);\n    JSContext *context = settings_context(runtime, role);'),
      'consume-pending-exception':replace('    JSContext *context = settings_context(runtime, role);',
          '    JSValue error = JS_GetException(scope->realm->context);\n    JS_FreeValue(scope->realm->context, error);\n    JSContext *context = settings_context(runtime, role);'),
    }
    variants={name:(block,mutated) for name,mutated in blocks.items()}
    before='case OS64_JS_SETTINGS_NATIVE_CALLER: return JS_GetNativeCallerContext(runtime->engine);'
    variants['native-caller-follows-current']=(before,'case OS64_JS_SETTINGS_NATIVE_CALLER: return JS_GetCurrentContext(runtime->engine);')
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
            run=subprocess.run([str(binary),'--owner-settings-controls'],capture_output=True,text=True,timeout=60)
            log=run.stdout+run.stderr; (output/(name+'-'+mode+'.log')).write_text(log)
            failures=[line for line in log.splitlines() if line.startswith('FAIL: checked settings:')]
            if run.returncode!=1 or not failures or any(mark in log for mark in
                ('ERROR: AddressSanitizer', 'ERROR: LeakSanitizer', 'runtime error:', 'engine invariant failure')):
                raise RuntimeError('mutation escaped behavioral oracle: '+name+'/'+mode+'\n'+log)
            results.append({'variant':name,'mode':mode,'oracle':failures[0]})
            print(name+'/'+mode+': '+failures[0],flush=True)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(str(len(results))+' checked settings mutations rejected')


if __name__=='__main__': main()

#!/usr/bin/env python3
"""Replay checked settings-invocation counterexamples using retained runtime host artifacts.

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
    first=source.index('typedef struct {\n    OwnerScopedCall call;')
    last=source.index('os64_js_status_t js_owner_runtime_scoped_run(',first)
    block=source[first:last]
    def replace(old,new):
        if block.count(old)!=1: raise RuntimeError('missing unique invocation anchor: '+old)
        return block.replace(old,new)
    blocks={
      'entry-follows-primary':replace('JS_RunWithSettings(entry->context,','JS_RunWithSettings(primary(runtime)->context,'),
      'incumbent-follows-entry':replace('incumbent ? incumbent->context : NULL,','incumbent ? entry->context : NULL,'),
      'missing-incumbent-loan':replace('if (incumbent && js_owner_loan_begin(', 'if (false && incumbent && js_owner_loan_begin('),
      'incumbent-loan-pins-entry':replace('*incumbent_ticket, &incumbent_loan)', 'entry_ticket, &incumbent_loan)'),
      'early-entry-loan-end':replace('        JSValue value = JS_UNDEFINED;',
          '        js_owner_loan_end(&metadata->table, metadata->incarnation, invocation.call.scope.loan);\n        JSValue value = JS_UNDEFINED;'),
      'early-incumbent-loan-end':replace('        JSValue value = JS_UNDEFINED;',
          '        if (incumbent_loan.serial) js_owner_loan_end(&metadata->table, metadata->incarnation, incumbent_loan);\n        JSValue value = JS_UNDEFINED;'),
      'cleanup-without-wrapper-frame':replace('    JS_FreeValue(context, value);',
          '    invocation->call.scope.runtime->settings_calls = NULL;\n    JS_FreeValue(context, value);'),
      'cleanup-without-guard':replace('    JS_FreeValue(context, value);',
          '    invocation->call.scope.runtime->active = false;\n    JS_FreeValue(context, value);'),
      'missing-exception-diagnostic':replace('        diagnostic(invocation->call.scope.runtime, context, error, invocation->outcome);',
          '        (void)context;'),
      'implicit-raw-export':replace('    OwnerMetadata *metadata = runtime->owner.metadata;',
          '    owner_legacy_escape(runtime);\n    OwnerMetadata *metadata = runtime->owner.metadata;'),
      'active-outcome-clobber':replace('    os64_js_status_t result = settings_ready(runtime, caller_abi, outcome);',
          '    if (outcome) os64_memset(outcome, 0, sizeof(*outcome));\n    os64_js_status_t result = settings_ready(runtime, caller_abi, outcome);'),
      'skip-incumbent-eligibility':replace('        result = settings_eligible(runtime, incumbent, outcome);', '        result = OS64_JS_OK;'),
      'skip-entry-eligibility':replace('    result = settings_eligible(runtime, entry, outcome);', '    result = OS64_JS_OK;'),
      'renew-job-counter':replace('    bool borrowed = runtime->active;', '    runtime->jobs = 0;\n    bool borrowed = runtime->active;'),
      'renew-deadline':replace('    bool borrowed = runtime->active;',
          '    runtime->deadline = os64_micros() + (int64_t)(runtime->limits.execution_ms * 1000);\n    bool borrowed = runtime->active;'),
      'ignore-engine-nesting-refusal':replace('        if (!admitted) status(outcome, OS64_JS_ADMISSION_REFUSED, "settings scope nesting limit reached");',
          '        (void)admitted;'),
      'leak-entry-after-partial-refusal':replace('    if (js_owner_loan_end(&metadata->table, metadata->incarnation, invocation.call.scope.loan) != JS_OWNER_OK)',
          '    if (outcome->status == OS64_JS_OK && js_owner_loan_end(&metadata->table, metadata->incarnation, invocation.call.scope.loan) != JS_OWNER_OK)'),
    }
    variants={name:(block,mutated) for name,mutated in blocks.items()}
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
            run=subprocess.run([str(binary),'--owner-invocations-controls'],capture_output=True,text=True,timeout=60)
            log=run.stdout+run.stderr; (output/(name+'-'+mode+'.log')).write_text(log)
            failures=[line for line in log.splitlines() if line.startswith('FAIL: checked invocation:')]
            if run.returncode!=1 or not failures or any(mark in log for mark in
                ('ERROR: AddressSanitizer', 'ERROR: LeakSanitizer', 'runtime error:', 'engine invariant failure')):
                raise RuntimeError('mutation escaped behavioral oracle: '+name+'/'+mode+'\n'+log)
            results.append({'variant':name,'mode':mode,'oracle':failures[0]})
            print(name+'/'+mode+': '+failures[0],flush=True)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(str(len(results))+' checked settings invocation mutations rejected')


if __name__=='__main__': main()

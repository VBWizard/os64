#!/usr/bin/env python3
"""Replay checked rejection-consumption counterexamples using retained runtime host artifacts.

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
    first=source.index('static Rejection *owner_rejection_first(')
    last=source.index('JS_PUBLIC bool os64_js_realm_take_rejection(',first)
    block=source[first:last]
    end_loan="""    if (js_owner_loan_end(&runtime->owner.metadata->table, runtime->owner.metadata->incarnation,
            call.scope.loan) != JS_OWNER_OK)
        jsport_fatal("owner rejection invocation unwind", __FILE__, __LINE__);
"""
    def replace(old,new):
        if block.count(old)!=1: raise RuntimeError('missing unique rejection anchor: '+old)
        return block.replace(old,new)
    blocks={
      'wrong-owner-queue':replace('while (item && &realm->runtime->realms[item->realm_index] != realm)',
                                'while (false && item && &realm->runtime->realms[item->realm_index] != realm)'),
      'wrong-borrowed-reason':replace('context, item->promise, item->reason, item->tag, call->opaque',
                                    'context, item->promise, item->promise, item->tag, call->opaque'),
      'lose-host-tag':replace('item->reason, item->tag, call->opaque','item->reason, 0, call->opaque'),
      'claim-output-before-admission':replace('os64_js_status_t result = enter(runtime, outcome);',
                                             'if (consumed) *consumed = false;\n    os64_js_status_t result = enter(runtime, outcome);'),
      'replace-task-owner':replace('if (borrowed) invoke_body(realm, owner_rejection_body, &call, true, outcome);',
          'if (borrowed) { runtime->turn_owner = realm; invoke_body(realm, owner_rejection_body, &call, true, outcome); }'),
      'consume-renews-explicit-budget':replace('bool borrowed = runtime->task;','bool borrowed = false;'),
      'end-loan-before-queue-roots':block.replace(end_loan,'').replace('    if (call.item) {',end_loan+'    if (call.item) {'),
      'end-guard-before-queue-roots':replace('    if (call.item) {','    runtime->active = false;\n    if (call.item) {'),
      'omit-primary-owner-loan':block.replace('if (js_owner_loan_begin(', 'if (ticket.owner.serial != 1 && js_owner_loan_begin(').replace(
          'if (js_owner_loan_end(', 'if (call.scope.loan.serial && js_owner_loan_end('),
      'skip-preclaim-health':replace('    if (observe(runtime)) return leave(runtime, outcome);','    /* health omitted */'),
      'poll-empty-through-admission':replace('if (!owner_rejection_first(realm)) {','if (false && !owner_rejection_first(realm)) {'),
    }
    variants={name:(block,mutated) for name,mutated in blocks.items()}
    raw_first=source.index('JS_PUBLIC bool os64_js_realm_take_rejection(')
    raw_last=source.index('static bool write_output(',raw_first)
    raw=source[raw_first:raw_last]
    variants['legacy-take-without-veto']=(raw,raw.replace('    owner_legacy_escape(runtime);',''))
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
            run=subprocess.run([str(binary),'--owner-rejections'],capture_output=True,text=True,timeout=60)
            log=run.stdout+run.stderr; (output/(name+'-'+mode+'.log')).write_text(log)
            failures=[line for line in log.splitlines() if line.startswith('FAIL: checked rejections:')]
            if run.returncode!=1 or not failures or any(mark in log for mark in
                ('ERROR: AddressSanitizer', 'ERROR: LeakSanitizer', 'runtime error:', 'engine invariant failure')):
                raise RuntimeError('mutation escaped behavioral oracle: '+name+'/'+mode+'\n'+log)
            results.append({'variant':name,'mode':mode,'oracle':failures[0]})
            print(name+'/'+mode+': '+failures[0],flush=True)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(str(len(results))+' checked rejection mutations rejected')


if __name__=='__main__': main()

#!/usr/bin/env python3
"""Replay checked staged-installation counterexamples using retained runtime host artifacts.

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
        'allocator-failure-as-local-capacity':('if (ticket && runtime->allocator.failures)\n        return status(outcome, OS64_JS_FAILED_RUNTIME, "engine allocator has failed");', ''),
        'omit-staged-state':('realm->staged = true;\n        JS_InitJobGroup(&realm->setup_jobs, realm->context);',
            'realm->staged = false;\n        JS_InitJobGroup(&realm->setup_jobs, realm->context);'),
        'setup-through-raw-getter':('realm_context(realm, caller_abi, outcome, ticket == NULL)',
            'realm_context(realm, caller_abi, outcome, true)'),
        'activate-through-raw-getter':('realm_context(realm, caller_abi, outcome, exported)',
            'realm_context(realm, caller_abi, outcome, exported ? true : true)'),
        'omit-setup-loan':('if (ticket && js_owner_loan_begin(&runtime->owner.metadata->table, runtime->owner.metadata->incarnation,\n            *ticket, &scope.loan)',
            'if (ticket && false && js_owner_loan_begin(&runtime->owner.metadata->table, runtime->owner.metadata->incarnation,\n            *ticket, &scope.loan)'),
        'omit-deferred-rollback':('while (JS_DiscardJobGroup(runtime->engine, &realm->setup_jobs) ||\n               JS_DiscardFinalizationTask(runtime->engine, &realm->setup_jobs))',
            'while (JS_DiscardJobGroup(runtime->engine, &realm->setup_jobs))'),
        'omit-failed-retirement':('realm->retired = true;\n        /* Releasing either lane can enqueue transaction work through native',
            '/* Releasing either lane can enqueue transaction work through native'),
        'premature-activation':('if (!realm->staged || !realm->setup_complete || runtime->setup_realm != NULL)',
            'if (!realm->staged || runtime->setup_realm != NULL)'),
        'wrong-installer-context':('scoped_installer(&scope, realm->context, opaque)',
            'scoped_installer(&scope, primary(runtime)->context, opaque)'),
        'renew-nested-setup-budget':('result = setup_prepare(&call, false, 0, "<staged realm setup>");',
            'if (ticket) call.inherited = false;\n    result = setup_prepare(&call, false, 0, "<staged realm setup>");'),
    }
    first = source.index('static os64_js_status_t realm_setup(')
    last = source.index('JS_PUBLIC os64_js_status_t os64_js_realm_setup(', first)
    common = source[first:last]
    unwind = """    /* Candidate cleanup, group restoration and guard unwind precede loan end. */
    if (scope.loan.serial && js_owner_loan_end(&runtime->owner.metadata->table,
            runtime->owner.metadata->incarnation, scope.loan) != JS_OWNER_OK)
        jsport_fatal("owner setup invocation unwind", __FILE__, __LINE__);
"""
    if common.count(unwind) != 1: raise RuntimeError('missing setup unwind anchor')
    early = common.replace(unwind, '').replace('    setup_rejections(realm, outcome);',
        unwind + '    setup_rejections(realm, outcome);')
    variants['end-loan-before-rollback'] = (common, early)
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
            run=subprocess.run([str(binary),'--owner-setup'],capture_output=True,text=True,timeout=60)
            log=run.stdout+run.stderr; (output/(name+'-'+mode+'.log')).write_text(log)
            failures=[line for line in log.splitlines() if line.startswith('FAIL: checked setup:')]
            if run.returncode!=1 or not failures:
                raise RuntimeError('mutation escaped behavioral oracle: '+name+'/'+mode+'\n'+log)
            results.append({'variant':name,'mode':mode,'oracle':failures[0]})
            print(name+'/'+mode+': '+failures[0],flush=True)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(str(len(results))+' checked setup mutations rejected')


if __name__=='__main__': main()

#!/usr/bin/env python3
"""Replay actual realm-owner counterexamples using retained runtime host artifacts.

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
        'wrong-realm-map':('*out = (os64_js_realm_t *)&runtime->realms[binding->realm_index];',
            '*out = (os64_js_realm_t *)&runtime->realms[0];'),
        'accept-closed-ticket':('if (result != JS_OWNER_OK) return result;\n    OwnerBinding *binding = owner_binding(runtime, id);',
            'if (result != JS_OWNER_OK) id = ticket.owner;\n    OwnerBinding *binding = owner_binding(runtime, id);'),
        'skip-checked-retirement':('if (realm->retired)\n        return refuse(runtime, outcome, realm->failed ? OS64_JS_REALM_FAILED : OS64_JS_REALM_RETIRED, "realm has retired");\n    if (realm->staged)\n        return refuse(runtime, outcome, OS64_JS_REALM_INSTALLING, "realm is installing");\n    if ((source == NULL',
            'if (realm->retired && !ticket)\n        return refuse(runtime, outcome, realm->failed ? OS64_JS_REALM_FAILED : OS64_JS_REALM_RETIRED, "realm has retired");\n    if (realm->staged)\n        return refuse(runtime, outcome, OS64_JS_REALM_INSTALLING, "realm is installing");\n    if ((source == NULL'),
        'skip-checked-staging':('if (realm->staged)\n        return refuse(runtime, outcome, OS64_JS_REALM_INSTALLING, "realm is installing");\n    if ((source == NULL',
            'if (realm->staged && !ticket)\n        return refuse(runtime, outcome, OS64_JS_REALM_INSTALLING, "realm is installing");\n    if ((source == NULL'),
        'end-before-drain':('result = complete_run(runtime, result, outcome);\n    /* The same admitted loan',
            'if (loan.serial) { js_owner_loan_end(&runtime->owner.metadata->table, runtime->owner.metadata->incarnation, loan); loan.serial = 0; }\n    result = complete_run(runtime, result, outcome);\n    /* The same admitted loan'),
        'publish-failed-child':('if (ticket.serial && js_owner_runtime_close(runtime, ticket) != JS_OWNER_OK)',
            'if (ticket_out) *ticket_out = ticket;\n        if (ticket.serial && js_owner_runtime_close(runtime, ticket) != JS_OWNER_OK)'),
        'erase-consumed-row':('jsport_fatal("unpublished owner ticket", __FILE__, __LINE__);\n    } else {',
            'jsport_fatal("unpublished owner ticket", __FILE__, __LINE__);\n        if (binding) { runtime->owner.metadata->table._owners_used--; *binding = (OwnerBinding){0}; }\n    } else {'),
        'omit-raw-context-veto':('if (exported) owner_legacy_escape(runtime);', '(void)exported;'),
        'omit-raw-child-veto':('else owner_legacy_escape(runtime);\n        *out = realm;', '*out = realm;'),
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
            run=subprocess.run([str(binary),'--owner-realms'],capture_output=True,text=True,timeout=60)
            log=run.stdout+run.stderr; (output/(name+'-'+mode+'.log')).write_text(log)
            failures=[line for line in log.splitlines() if line.startswith('FAIL: realm owners:')]
            if run.returncode!=1 or not failures:
                raise RuntimeError('mutation escaped behavioral oracle: '+name+'/'+mode+'\n'+log)
            results.append({'variant':name,'mode':mode,'oracle':failures[0]})
            print(name+'/'+mode+': '+failures[0],flush=True)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(str(len(results))+' realm owner mutations rejected')


if __name__=='__main__': main()

#!/usr/bin/env python3
"""Replay checked module-provider counterexamples using retained runtime host artifacts.

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
    first=source.index('static os64_js_status_t owner_registration_realm(')
    last=source.index('\ntypedef struct {\n    JSOwnerRuntimeScope scope;\n    JSOwnerRejectionBody',first)
    resolver=source[first:last]
    first=source.index('os64_js_status_t js_owner_runtime_set_module_loader(')
    last=source.index('/* A module compiled, or read from its blob',first)
    providers=source[first:last]
    variants={
      'loader-targets-primary':(providers,providers.replace('os64_js_realm_set_module_loader(realm,', 'os64_js_realm_set_module_loader(primary(runtime),')),
      'base-targets-primary':(providers,providers.replace('os64_js_realm_set_import_base(realm,', 'os64_js_realm_set_import_base(primary(runtime),')),
      'loader-registration-dropped':(providers,providers.replace('caller_abi, loader, outcome)', 'caller_abi, loader ? NULL : loader, outcome)')),
      'base-registration-dropped':(providers,providers.replace('caller_abi, base, opaque, outcome)', 'caller_abi, base ? NULL : base, opaque ? NULL : opaque, outcome)')),
      'registration-creates-raw-export':(resolver,resolver.replace('    return OS64_JS_OK;', '    owner_legacy_escape(runtime);\n    return OS64_JS_OK;')),
      'failed-runtime-cannot-detach':(resolver,resolver.replace('    return OS64_JS_OK;', '    if (runtime->failure != OS64_JS_OK) return status(outcome, OS64_JS_FAILED_RUNTIME, "failed");\n    return OS64_JS_OK;')),
      'alias-clobbered-before-delegate':(resolver,resolver.replace('    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;','')),
      'stale-holder-detaches':(resolver,resolver.replace('        return status(outcome, OS64_JS_ADMISSION_REFUSED, "owner ticket does not resolve");','        *realm = primary(runtime);')),
    }
    first=source.index('static JSValue import_base_current(')
    last=source.index('JS_PUBLIC os64_js_status_t os64_js_enable_import_settings(',first)
    base=source[first:last]
    before="""    os64_js_outcome_t out;
    os64_js_realm_t *realm = os64_js_settings_realm(runtime, OS64_JS_ABI_ID,
                                                    OS64_JS_SETTINGS_CURRENT, &out);"""
    if base.count(before)!=1: raise RuntimeError('missing base settings anchor')
    variants['base-view-without-veto']=(base,base.replace(before,'    os64_js_realm_t *realm = realm_for_context(runtime, context);').replace('    owner_legacy_escape(runtime);',''))
    before='    if (exported) owner_legacy_escape(runtime);'
    variants['incidental-getter-exemption']=(before,'    if (false && exported) owner_legacy_escape(runtime);')
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
            run=subprocess.run([str(binary),'--owner-provider-controls'],capture_output=True,text=True,timeout=60)
            log=run.stdout+run.stderr; (output/(name+'-'+mode+'.log')).write_text(log)
            failures=[line for line in log.splitlines() if line.startswith('FAIL: checked providers:')]
            if run.returncode!=1 or not failures or any(mark in log for mark in
                ('ERROR: AddressSanitizer', 'ERROR: LeakSanitizer', 'runtime error:', 'engine invariant failure')):
                raise RuntimeError('mutation escaped behavioral oracle: '+name+'/'+mode+'\n'+log)
            results.append({'variant':name,'mode':mode,'oracle':failures[0]})
            print(name+'/'+mode+': '+failures[0],flush=True)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(str(len(results))+' checked provider mutations rejected')


if __name__=='__main__': main()

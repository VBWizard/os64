#!/usr/bin/env python3
"""Compile contract mutants against retained fully sanitized host objects.

Run test_dom_host.sh with DOM_HOST_KEEP=1 first, then pass its artifact path.
Source files are copied into temporary storage; the worktree is not mutated.
A mutant counts only when it compiles and the host assertions/sanitizers fail.
"""
import argparse
import pathlib
import shlex
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('objects',type=pathlib.Path)
parser.add_argument('--logs',type=pathlib.Path,default=pathlib.Path('/tmp/dom-d5a-mutants'))
parser.add_argument('--reclaim',action='store_true',help='run D6 binding ownership mutants')
args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True,exist_ok=True)
# Each mutation breaks a separately specified visible rule; it does not supply
# replacement expectations derived from the library's implementation.
mutants=[
 ('clone-state-routing','node','os64_page_node_clone(dom->state, node, deep != 0, &status)','os64_html_clone(dom->document, node, deep != 0, &status)'),
 ('character-data-state-routing','content','os64_page_node_set_text(dom->state, (os64_html_node_t *)node, string.data, string.length)','os64_html_set_text(dom->document, (os64_html_node_t *)node, string.data, string.length)'),
 ('clone-checked-dirty','page-state','fresh->on_dirty = old != NULL && old->on_dirty;','fresh->on_dirty = false;'),
 ('clone-clean-textarea','page-state','fresh->text_clean = textarea && !dirty;','fresh->text_clean = false;'),
 ('clone-template-descendants','page-state','if (source->template_contents != NULL && target->template_contents != NULL &&','if (false && source->template_contents != NULL && target->template_contents != NULL &&'),
 ('comment-parent-notification','page-state','tree_plan_textarea(&reserve, node->parent);\n    uint64_t version = os64_html_version(state->doc);','tree_plan_textarea(&reserve, node->kind == OS64_HTML_COMMENT ? NULL : node->parent);\n    uint64_t version = os64_html_version(state->doc);'),
 ('character-data-null','content','if (JS_IsNull(value) ||','if ((JS_IsNull(value) && property != D_DATA) ||'),
 ('generic-element-control-property','node','return 1u << D_PROTO_INPUT;','return (1u << D_PROTO_INPUT) | (1u << D_PROTO_ELEMENT);'),
 ('text-element-property','node','return D_PROTO_CHARACTER_DATA;','return D_PROTO_ELEMENT;'),

 ('wrapper-identity','node','if (entry != NULL) return JS_DupValue(ctx, entry->value);','if (entry != NULL && false) return JS_DupValue(ctx, entry->value);'),
 ('closed-callback','core','if (dom == NULL || dom->closed) {','if (dom == NULL) {'),
 ('binding-quota','core','if (total > dom->options.max_bytes - dom->bytes) return NULL;','if (false && total > dom->options.max_bytes - dom->bytes) return NULL;'),
 ('html-quota-name','core','else if (status <= OS64_HTML_TOO_LARGE && status >= OS64_HTML_WORK_EXHAUSTED)','else if (false && status <= OS64_HTML_TOO_LARGE && status >= OS64_HTML_WORK_EXHAUSTED)'),
 ('state-quota-name','core','status == -OS64_PAGE_REASON_NO_MEMORY ? "QuotaExceededError" : "TypeError"','status == -OS64_PAGE_REASON_NO_MEMORY ? "TypeError" : "TypeError"'),
 ('same-child-collection','collection','if (query != NULL) return JS_DupValue(ctx, query->entry->value);','if (query != NULL && false) return JS_DupValue(ctx, query->entry->value);'),
 ('live-query-version','collection','if (query->version == version) return true;','if (query->version != 0) return true;'),
 ('elements-only','collection','if (query->elements && node->kind != OS64_HTML_ELEMENT) return false;','if (false && query->elements && node->kind != OS64_HTML_ELEMENT) return false;'),
 ('html-query-case','collection','node->ns == OS64_HTML_NS_HTML ? query->folded : query->name','node->ns == OS64_HTML_NS_HTML ? query->name : query->name'),
 ('numeric-enumeration','collection','desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;','desc->flags = JS_PROP_CONFIGURABLE;'),
 ('nul-conversion','content','if (c == 0) {','if (c == 0 && false) {'),
 ('surrogate-conversion','content','} else if (c == 0xed && i + 2 < length &&','} else if (c == 0xee && i + 2 < length &&'),
 ('attribute-state-routing','node','os64_page_node_set_attr(dom->state, node, name.data, value.data,\n                                                value.length, magic == D_REMOVE_ATTR)','(magic == D_REMOVE_ATTR ? os64_html_remove_attr(dom->document, (os64_html_node_t *)node, name.data) : os64_html_set_attr(dom->document, (os64_html_node_t *)node, name.data, value.data, value.length))'),
 ('structural-state-routing','node','return os64_page_node_insert(dom->state, parent, node, before);','return os64_html_insert(dom->document, parent, node, before);'),
 ('registry-drain','core','dom->retained = 0;','dom->retained += 0;'),
 ('fragment-scripting-mode','content','string.length, dom->options.scripting, &status);','string.length, true, &status);'),
 ('numeric-readonly-hook','collection','return JS_DefineProperty(ctx, query->dom->index_guard->value, atom,','return JS_DefineProperty(ctx, object, atom,'),
 ('undefined-nullable-content','content','(JS_IsUndefined(value) && (property == D_NODE_VALUE || property == D_TEXT_CONTENT))','(false && JS_IsUndefined(value) && (property == D_NODE_VALUE || property == D_TEXT_CONTENT))'),
 ('attribute-name-equals','content'," || *at == '='",''),
 ('error-prototype-setter','core','JS_DefinePropertyValueStr(ctx, error, "name", text, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)','JS_SetPropertyStr(ctx, error, "name", text)'),
]
if args.reclaim:
    mutants=[
      ('wrapper-hold','node','os64_html_hold(dom->document, node);','(void)node;'),
      ('wrapper-release','core','os64_html_release(dom->document, entry->node);','(void)entry->node;'),
      ('query-root-hold','collection','os64_html_hold(dom->document, root);','(void)root;'),
      ('query-root-release','collection','os64_html_release(dom->document, query->root);','(void)query->root;'),
      ('query-answer-hold','collection','os64_html_hold(query->dom->document, at);','(void)at;'),
      ('query-answer-release','collection','os64_html_release(query->dom->document, old[i]);','(void)old[i];'),
      ('empty-content-consumption','page-state','return node != NULL ? os64_html_insert(state->doc, (os64_html_node_t *)parent,\n            (os64_html_node_t *)node, NULL) : OS64_HTML_OK;','return OS64_HTML_OK;'),
    ]
flags=['-O2','-g','-std=gnu11','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-tree-loop-distribute-patterns','-fno-stack-protector','-DOS64_JS_TARGET','-fsanitize=address,undefined','-fno-sanitize-recover=all']
for directory in ['libmath/include','libjs/port','libjs/include','libdom/include','libdom','libhtml/include','libpage/include','libpage/upstream/ryu','libos64/include']:
    flags+=['-I'+str(root/'userland'/directory)]
flags+=['-I'+str(root/'abi/include'),'-isystem',str(root/'userland/obj/js/upstream')]
flags+=['-Dd_alloc=dom_host_private_alloc','-Dos64_malloc=dom_host_malloc','-Dos64_calloc=dom_host_calloc','-Dos64_realloc=dom_host_realloc','-Dos64_free=dom_host_free','-Dos64_malloc_size=dom_host_malloc_size']
raw=subprocess.check_output(['make','-s','-C',str(root/'userland'),'math-host-flags'],text=True)
maths=[]
for line in raw.splitlines():
    if line.startswith('MATH_OBJS='): maths=shlex.split(line.split('=',1)[1])[0].split()
objects=sorted(args.objects.glob('*.o'))
objects=[p for p in objects if p.name!='userland_libos64_str.c.o']
objects.append(args.objects/'userland_libos64_str.c.o-local')
if not (args.objects/'engine-quickjs.o').exists(): raise SystemExit('fully sanitized baseline objects required')
caught=compiled=0
with tempfile.TemporaryDirectory(prefix='os64-dom-mutants.') as directory:
    directory=pathlib.Path(directory)
    for name,unit,old,new in mutants:
        page_unit=unit.removeprefix('page-') if unit.startswith('page-') else None
        source=(root/'userland'/('libpage' if page_unit else 'libdom')/f'{page_unit or unit}.c').read_text()
        if source.count(old)!=1:
            print(f'ANCHOR ERROR {name}: {source.count(old)} matches');continue
        path=directory/f'{name}.c';path.write_text(source.replace(old,new))
        obj=directory/f'{name}.o';binary=directory/name
        with (args.logs/f'{name}.log').open('w') as log:
            unit_flags=['-I'+str(root/'userland/libpage'),*flags[:-6]] if page_unit else flags
            result=subprocess.run(['cc',*unit_flags,'-c',str(path),'-o',str(obj)],stdout=log,stderr=log)
            if result.returncode: print(f'COMPILE ERROR {name}');continue
            replaced=f'userland_libpage_{page_unit}.c.o' if page_unit else f'dom-{unit}.o'
            selected=[p for p in objects if p.name!=replaced]
            result=subprocess.run(['cc','-fsanitize=address,undefined','-pthread','-Wl,-z,noexecstack',*maths,*map(str,selected),str(obj),'-o',str(binary)],cwd=root,stdout=log,stderr=log)
            if result.returncode: print(f'LINK ERROR {name}');continue
            compiled+=1
            try: result=subprocess.run([str(binary),'--reclaim' if args.reclaim else '--mutants'],stdout=log,stderr=log,timeout=45)
            except subprocess.TimeoutExpired:
                print(f'TIMEOUT {name} (not counted)');continue
            if result.returncode:
                caught+=1;print(f'CAUGHT {name}')
            else: print(f'SURVIVED {name}')
print(f'DOM mutants: {caught} caught / {compiled} compiling / {len(mutants)} proposed; logs {args.logs}')
raise SystemExit(0 if caught==compiled==len(mutants) else 1)

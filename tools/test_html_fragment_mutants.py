#!/usr/bin/env python3
"""Compile isolated rule violations and require independent fragment tests to catch them."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MUTANTS = [
    ('foreign-root-guard', 'tree.c', 'if (p->fragment_context && i == 1)\n                return false;', 'if (false)\n                return false;'),
    ('children-only-inversion', 'serialize.c', 'SWriter w = {out, out ? cap : 0, 0, scripting};', 'children_only = !children_only; SWriter w = {out, out ? cap : 0, 0, scripting};'),
    ('escape-amp', 'serialize.c', 'replacement = "&amp;";', 'replacement = "&";'),
    ('escape-nbsp', 'serialize.c', 'replacement = "&nbsp;";', 'replacement = " ";'),
    ('escape-lt', 'serialize.c', 'replacement = "&lt;";', 'replacement = "<";'),
    ('escape-gt', 'serialize.c', 'replacement = "&gt;";', 'replacement = ">";'),
    ('escape-quote', 'serialize.c', 'replacement = "&quot;";', 'replacement = "\\\"";'),
    ('raw-parent', 'serialize.c', 'if (raw_parent(n->parent, w->scripting))', 'if (raw_parent(n->parent, w->scripting) && false)'),
    ('noscript-mode', 'serialize.c', '(scripting && h_eq(n->name, "noscript"))', '((scripting || true) && h_eq(n->name, "noscript"))'),
    ('void-close', 'serialize.c', 'return html(n) && h_in(n->name,', 'return false && html(n) && h_in(n->name,'),
    ('template-contents', 'serialize.c', '? n->template_contents : n->first_child;', '? NULL : n->first_child;'),
    ('context-rcdata', 'fragment.c', 'p->state = T_RCDATA;', 'p->state = T_DATA;'),
    ('context-rawtext', 'fragment.c', 'p->state = T_RAWTEXT;', 'p->state = T_DATA;'),
    ('context-script', 'fragment.c', 'p->state = T_SCRIPT;', 'p->state = T_DATA;'),
    ('context-plaintext', 'fragment.c', 'p->state = T_PLAIN;', 'p->state = T_DATA;'),
    ('context-noscript-mode', 'fragment.c', '(scripting && h_eq(context->name, "noscript"))', '(h_eq(context->name, "noscript"))'),
    ('context-template', 'tree.c', 'push_template(p, M_TEMPLATE);\n    reset_mode(p);', 'push_template(p, M_BODY);\n    reset_mode(p);'),
    ('context-namespace', 'internal.h', 'p->fragment_context && p->stack.n == 1 ? (HNode *)p->fragment_context', 'false && p->stack.n == 1 ? (HNode *)p->fragment_context'),
    ('context-ancestor-form', 'fragment.c', 'p->form = (HNode *)n;', 'p->form = NULL;'),
    ('version-detached', 'fragment.c', 'd_transfer_blocks(owner, stage);', 'd_transfer_blocks(owner, stage); owner->version++;'),
    ('fragment-script-stop', 'fragment.c', 'why = temp->pub.refusal;', 'why = temp->pub.refusal; if (scripting && p->script) why = OS64_HTML_SCRIPT;'),
]
# The last-stack context selects insertion mode. Changing one arm leaves the
# reference algorithm intact everywhere except the named context rule.
for name, mode in [('select','M_SELECT'), ('row','M_ROW'), ('table-body','M_TABLE_BODY'),
                   ('caption','M_CAPTION'), ('colgroup','M_COLGROUP'), ('table','M_TABLE'),
                   ('frameset','M_FRAMESET'), ('html','p->head ? M_AFTER_HEAD : M_BEFORE_HEAD')]:
    MUTANTS.append(('context-'+name, 'tree.c', 'p->mode = '+mode+';', 'p->mode = '+('M_TABLE' if name == 'caption' else 'M_BODY')+';'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only')
    args = parser.parse_args()
    caught = compiled = selected = 0
    for name, source, before, after in MUTANTS:
        if args.only and args.only not in name:
            continue
        selected += 1
        original = (ROOT/'userland/libhtml'/source).read_text()
        # Some mode assignments occur at sibling sites. Restrict the insertion
        # mode mutations to reset_mode, the common context initialization seam.
        if name.startswith('context-') and source == 'tree.c' and name not in ('context-namespace', 'context-template'):
            start = original.index('static void reset_mode(')
            end = original.index('void h_fragment_start(', start)
            region = original[start:end]
            assert region.count(before) == 1, name
            altered = original[:start] + region.replace(before, after) + original[end:]
        elif name == 'fragment-script-stop':
            before2 = 'p->started = p->straight = true;'
            assert original.count(before2) == 1
            start = original.index('parsed:')
            tail = original[start:]
            assert tail.count(before) == 1
            altered = original[:start] + tail.replace(before, after, 1)
            altered = altered.replace(before2, 'p->started = true; p->straight = false;')
        else:
            assert original.count(before) == 1, (name, original.count(before))
            altered = original.replace(before, after)
        with tempfile.TemporaryDirectory(prefix='os64-html-mutant.') as temporary:
            tmp = Path(temporary)
            shutil.copytree(ROOT/'userland/libhtml', tmp/'userland/libhtml')
            (tmp/'tools').mkdir()
            for path in ('test_html_driver.c', 'test_html_fragment.inc'):
                shutil.copy2(ROOT/'tools'/path, tmp/'tools'/path)
            (tmp/'userland/libhtml'/source).write_text(altered)
            driver = tmp/'html_driver'
            command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-I'+str(tmp/'userland/libhtml/include'),
                       '-I'+str(ROOT/'userland/libos64/include'), '-I'+str(ROOT/'abi/include')]
            command += [str(tmp/'userland/libhtml'/s) for s in
                        ('core.c','encoding.c','tokenizer.c','tree.c','dom.c','fragment.c','serialize.c')]
            command += [str(tmp/'tools/test_html_driver.c'), '-o', str(driver)]
            build = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if build.returncode:
                print(name+': COMPILE FAILED\n'+build.stdout.decode()[:2000], flush=True)
                continue
            compiled += 1
            try:
                result = subprocess.run([str(driver), '--fragments'], stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=15)
                detected = result.returncode != 0
                if detected:
                    caught += 1
                print(f'{name}: {"CAUGHT" if detected else "SURVIVED"}', flush=True)
                if not detected:
                    print(result.stdout.decode()[-1500:], flush=True)
            except subprocess.TimeoutExpired:
                caught += 1
                print(name+': CAUGHT (bounded timeout)', flush=True)
    print(f'Mutants: selected={selected} compiled={compiled} caught={caught}', flush=True)
    return int(compiled != selected or caught != compiled)


if __name__ == '__main__':
    raise SystemExit(main())

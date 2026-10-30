#!/usr/bin/env python3
"""Exercise engine patch placement and refusal through the real preparer."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PrepareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='os64-js-prepare-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lib = self.root / 'userland/libjs'
        self.lib.mkdir(parents=True)
        self.manifest = json.loads((ROOT / 'userland/libjs/manifest.json').read_text())
        for entry in self.manifest['retained_files'] + self.manifest['patches']:
            target = self.lib / entry['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / 'userland/libjs' / entry['path'], target)
        (self.root / 'tools').mkdir()
        shutil.copyfile(ROOT / 'tools/js_prepare.py', self.root / 'tools/js_prepare.py')

    def prepare(self):
        # Rehash intentional fixture edits so the test reaches patch application.
        for entry in self.manifest['retained_files'] + self.manifest['patches']:
            entry['sha256'] = hashlib.sha256((self.lib / entry['path']).read_bytes()).hexdigest()
        (self.lib / 'manifest.json').write_text(json.dumps(self.manifest))
        return subprocess.run([sys.executable, str(self.root / 'tools/js_prepare.py'),
                               str(self.root / 'prepared')], text=True, capture_output=True)

    def test_pinned_series(self):
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        source = (self.root / 'prepared/quickjs.c').read_text()
        self.assertIn('return JS_EXCEPTION;\n}\n\n/* Browser compatibility:', source)
        self.assertIn('return JS_NULL;\n}\n\n#define GLOBAL_VAR_OFFSET', source)
        # 0008 hears a global miss at both ends of a lookup.
        self.assertIn('ctx->global_miss(ctx->global_miss_opaque, JS_AtomGetStr(ctx, buf, sizeof(buf), prop));\n}\n\n'
                      'JSValue JS_GetPropertyInternal(', source)
        self.assertIn('js_hear_global_miss(ctx, obj, atom);    \\\n                            val = JS_UNDEFINED;',
                      source)
        # 0009 names the method only at the call the record was made for.
        self.assertIn('                if (unlikely(ctx->nc_atom != JS_ATOM_NULL) &&\n'
                      '                    !JS_IsFunction(ctx, call_argv[-1])) {', source)
        # 0010 refuses a definition whose emit failed, before anything is
        # transformed.
        self.assertIn('    BOOL strip_var_debug;\n    \n    /* os64: a parse whose emit ran out of memory',
                      source)
        self.assertIn('    if (dbuf_error(&fd->byte_code)) {\n        JS_ThrowOutOfMemory(ctx);\n'
                      '        goto fail;\n    }\n\n    /* recompute scope linkage */', source)
        # 0011a: get_lvalue reads a failed bytecode buffer as out of memory.
        self.assertIn('        if (dbuf_error(&fd->byte_code)) {\n            JS_ThrowOutOfMemory(s->ctx);\n'
                      '            return -1;\n        }\n    invalid_lvalue:', source)
        # 0011a: each closure-variable add in resolve_scope_var returns its
        # failure (a with object, the two eval variable objects, a direct
        # eval's closure and its variable objects, a global, a local).
        for check in (
                'vd->var_name, FALSE, FALSE, JS_VAR_NORMAL);\n'
                '                /* os64: upstream skipped the with-object test on failure */\n'
                '                if (idx < 0)\n                    return -1;\n'
                '                dbuf_putc(bc, OP_get_var_ref);',
                'fd->var_object_idx, vd->var_name,\n                                  FALSE, FALSE, JS_VAR_NORMAL);\n'
                '            /* os64: upstream emitted index -1 on failure */\n'
                '            if (idx < 0)\n                return -1;\n',
                'fd->arg_var_object_idx, vd->var_name,\n                                  FALSE, FALSE, JS_VAR_NORMAL);\n'
                '            /* os64: upstream emitted index -1 on failure */\n'
                '            if (idx < 0)\n                return -1;\n',
                '                    idx = idx1;\n                }\n'
                '                /* os64: upstream used index -1 on failure */\n'
                '                if (idx < 0)\n                    return -1;\n'
                '                if (cv->closure_type == JS_CLOSURE_GLOBAL ||',
                '                    idx = idx1;\n                }\n'
                '                /* os64: upstream emitted index -1 on failure */\n'
                '                if (idx < 0)\n                    return -1;\n'
                '                dbuf_putc(bc, OP_get_var_ref);',
                '            idx = idx1;\n        }\n'
                '        /* os64: upstream emitted index -1 on failure */\n'
                '        if (idx < 0)\n            return -1;\n    has_global_idx:',
                '        if (idx < 0)\n            return -1;\n        if (idx >= 0) {\n        has_idx:'):
            self.assertIn(check, source)
        # 0011a: a failed `arguments` or function-name add in
        # resolve_scope_var returns its failure, in the function's own scope
        # and in an enclosing one.
        for check in (
                '            var_idx = add_arguments_var(ctx, s);\n'
                '            /* os64: the binding exists, so -1 is a failed add; upstream\n'
                '               went on to the enclosing scopes and the global */\n'
                '            if (var_idx < 0)\n                return -1;\n',
                '            var_idx = add_func_var(ctx, s, var_name);\n'
                '            /* os64: the name exists, so -1 is a failed add; upstream\n'
                '               went on to the enclosing scopes and the global */\n'
                '            if (var_idx < 0)\n                return -1;\n',
                '            var_idx = add_arguments_var(ctx, fd);\n'
                '            /* os64: the binding exists, so -1 is a failed add; upstream\n'
                '               broke out with it and captured fd->args[-1 - offset] */\n'
                '            if (var_idx < 0)\n                return -1;\n            break;\n',
                '            var_idx = add_func_var(ctx, fd, var_name);\n'
                '            /* os64: the name exists, so -1 is a failed add; upstream\n'
                '               broke out with it and captured fd->args[-1 - offset] */\n'
                '            if (var_idx < 0)\n                return -1;\n            break;\n'):
            self.assertIn(check, source)
        # 0011a: resolve_variables takes a failed resolve_scope_var to its
        # fail path before the atom is freed, at both calls.
        self.assertIn('NULL, NULL, pos_next);\n            /* os64: -1 is a failed variable add;', source)
        self.assertIn('            if (pos_next < 0)\n                goto fail;\n            JS_FreeAtom(ctx, var_name);\n'
                      '            break;\n        case OP_scope_make_ref:', source)
        self.assertIn('bc_buf, ls, pos_next);\n                /* os64: as above, -1 is a failed variable add */\n'
                      '                if (pos_next < 0)\n                    goto fail;\n'
                      '                JS_FreeAtom(ctx, var_name);', source)
        # 0011a: a child with no constant-pool slot is out of memory, before
        # the child is made; the assertion is gone.
        self.assertIn('        if (cpool_idx < 0) {\n            JS_ThrowOutOfMemory(ctx);\n            goto fail;\n'
                      '        }\n        func_obj = js_create_function(ctx, fd1);', source)
        self.assertNotIn('assert(cpool_idx >= 0);', source)
        # 0015: a read given a name renames each function as its debug
        # information is read, and JS_ReadObject reads with none.
        self.assertIn('        if (bc_get_atom(s, &b->debug.filename))\n            goto fail;\n'
                      '        if (s->filename != JS_ATOM_NULL) {\n'
                      '            JS_FreeAtom(ctx, b->debug.filename);\n'
                      '            b->debug.filename = JS_DupAtom(ctx, s->filename);\n        }\n', source)
        self.assertIn('    return JS_ReadObjectInternal(ctx, buf, buf_len, flags, JS_ATOM_NULL);\n', source)

    def test_engine_identity(self):
        # The identity a bytecode blob is tied to follows the prepared
        # engine: the same sources give the same one, and an edit to any of
        # them (a patch's effect included) gives another.
        def identity():
            result = self.prepare()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            text = (self.root / 'prepared/os64_js_engine_id.h').read_text()
            self.assertRegex(text, r'#define OS64_JS_ENGINE_ID "[0-9a-f]{64}"\n')
            return text
        first = identity()
        self.assertEqual(identity(), first)
        path = self.lib / 'upstream/quickjs-opcode.h'
        path.write_text(path.read_text() + '\n')
        self.assertNotEqual(identity(), first)

    def test_line_shifts_preserve_output(self):
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        expected = {name: (self.root / 'prepared' / name).read_text()
                    for name in ('quickjs.c', 'quickjs.h')}
        for name in expected:
            path = self.lib / 'upstream' / name
            path.write_text('\n' * 17 + path.read_text())
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name, original in expected.items():
            self.assertEqual((self.root / 'prepared' / name).read_text(), '\n' * 17 + original)

    def refuse_context_drift(self, name, anchor,
                             patch='patches/0007-browser-legacy-function-arguments.patch'):
        path = self.lib / 'upstream' / name
        original = path.read_text()
        self.assertEqual(original.count(anchor), 1)
        path.write_text(original.replace(anchor, anchor.rstrip('\n') + ' /* drift */\n'))
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('QuickJS patch failed: ' + patch, result.stderr)

    def test_c_context_drift_refused(self):
        # This edge line would be discarded by patch's default fuzz of two.
        self.refuse_context_drift('quickjs.c', '#define GLOBAL_VAR_OFFSET 0x40000000\n')

    def test_header_context_drift_refused(self):
        self.refuse_context_drift('quickjs.h',
                                 'int JS_DefineProperty(JSContext *ctx, JSValueConst this_obj,\n'
                                 '                      JSAtom prop, JSValueConst val,\n')

    def test_field_fast_path_drift_refused(self):
        # The inline field read's hunk: its other context lines are
        # backslash-padded braces that recur throughout the interpreter.
        self.refuse_context_drift('quickjs.c',
                                  '                        p = p->shape->proto;                            \\\n',
                                  'patches/0008-browser-global-miss-handler.patch')


if __name__ == '__main__':
    unittest.main()

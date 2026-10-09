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
        # 0009 names the method a call found not callable, at the call's throw.
        self.assertIn('        not_a_function:\n            return js_throw_not_a_function(caller_ctx);', source)

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

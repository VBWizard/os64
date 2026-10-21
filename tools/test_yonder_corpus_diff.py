#!/usr/bin/env python3
"""tools/yonder_corpus_diff.py on two hand-made replay directories: a page
that gained a line, one that lost one, one whose counts alone moved, one
the second replay did not write, and one only it wrote. Run with
`python3 -I`; tools/test_yonder_replay_host.sh runs it."""
import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest


def load(name):
    path = pathlib.Path(__file__).resolve().parent / f'{name}.py'
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


corpus_diff = load('yonder_corpus_diff')

BEFORE = {
    'worse-0001.txt': 'address: https://worse.test/\nverdict: 1 MISSING\nMISSING global fetch (1)\n',
    'better-0002.txt': ('address: https://better.test/\nverdict: 1 MISSING, 1 FAILED\n'
                        'MISSING global localStorage (4)\n'
                        "FAILED script https://better.test/a.js: ReferenceError: 'localStorage' is not defined (1)\n"),
    'counts-0003.txt': 'address: https://counts.test/\nverdict: 1 MISSING\nMISSING global fetch (1)\n',
    'gone-0004.txt': 'address: https://gone.test/\nverdict: clean\n',
}
AFTER = {
    'worse-0001.txt': ('address: https://worse.test/\nverdict: 1 MISSING, 1 FAILED\nMISSING global fetch (1)\n'
                       'FAILED script https://worse.test/b.js: TypeError: x is not a function (1)\n'),
    'better-0002.txt': 'address: https://better.test/\nverdict: clean\n',
    'counts-0003.txt': 'address: https://counts.test/\nverdict: 1 MISSING\nMISSING global fetch (9)\nbytes: 2\n',
    'new-0005.txt': 'address: https://new.test/\nverdict: clean\n',
}


def write(root, pages):
    root.mkdir()
    for name, text in pages.items():
        (root / name).write_text(text)


class DiffCases(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.before = pathlib.Path(self.dir.name) / 'before'
        self.after = pathlib.Path(self.dir.name) / 'after'
        write(self.before, BEFORE)
        write(self.after, AFTER)

    def tearDown(self):
        self.dir.cleanup()

    def run_main(self, before, after):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            rc = corpus_diff.main([str(before), str(after)])
        return rc, out.getvalue()

    def test_regression_progress_and_a_missing_page(self):
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 1)
        self.assertIn('regressions: 1 pages\n  worse-0001.txt  https://worse.test/\n'
                      '    + FAILED script https://worse.test/b.js: TypeError: x is not a function\n', out)
        self.assertIn('progress: 1 pages\n  better-0002.txt  https://better.test/\n'
                      "    - FAILED script https://better.test/a.js: ReferenceError: 'localStorage' is not defined\n"
                      '    - MISSING global localStorage\n', out)
        self.assertNotIn('counts-0003', out)
        self.assertIn('pages\n  gone-0004.txt  https://gone.test/\n', out)
        self.assertIn('only in', out)
        self.assertIn('before: 4 pages, 1 clean; after: 4 pages, 2 clean; 1 lines gained, 2 lines lost', out)

    def test_progress_alone_passes(self):
        (self.after / 'worse-0001.txt').write_text(BEFORE['worse-0001.txt'])
        (self.after / 'gone-0004.txt').write_text(BEFORE['gone-0004.txt'])
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('regressions: 0 pages\n', out)

    def test_a_missing_page_alone_fails(self):
        (self.after / 'worse-0001.txt').write_text(BEFORE['worse-0001.txt'])
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 1)
        self.assertIn('gone-0004.txt', out)

    def test_not_a_directory(self):
        rc, _ = self.run_main(self.before, self.after / 'nothere')
        self.assertEqual(rc, 2)


if __name__ == '__main__':
    unittest.main()

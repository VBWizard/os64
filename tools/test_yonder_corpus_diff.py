#!/usr/bin/env python3
"""tools/yonder_corpus_diff.py on two hand-made replay directories: a page
that gained a line, one that lost one, one whose counts alone moved, one
the second replay did not write, and one only it wrote; then a page whose
replay was incomplete, never reached a verdict, or recovered, in each
direction; and a page file that cannot be read. Run with `python3 -I`;
tools/test_yonder_replay_host.sh runs it."""
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
        self.assertIn('before: 4 pages, 1 clean; after: 4 pages, 2 clean, 0 incomplete, 0 broken; '
                      '1 lines gained, 2 lines lost', out)

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

    def settle_the_rest(self):
        (self.after / 'worse-0001.txt').write_text(BEFORE['worse-0001.txt'])
        (self.after / 'gone-0004.txt').write_text(BEFORE['gone-0004.txt'])

    def test_newly_incomplete_is_neither(self):
        # The replay met a script the corpus never kept: its record gained a
        # FAILED line and lost others, and neither counts.
        self.settle_the_rest()
        (self.after / 'better-0002.txt').write_text(
            'address: https://better.test/\nverdict: 1 FAILED\n'
            'FAILED script https://better.test/new.js: TypeError: no (1)\n'
            'REPLAY incomplete script https://better.test/M%49SSING.js\n')
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('regressions: 0 pages\nprogress: 0 pages\n', out)
        self.assertIn('newly incomplete: 1 pages\n'
                      '  better-0002.txt  https://better.test/\n'
                      '    ? script https://better.test/MISSING.js\n', out)
        self.assertIn('after: 5 pages, 2 clean, 1 incomplete, 0 broken; 0 lines gained, 0 lines lost', out)

    def test_still_incomplete_is_to_record_again(self):
        self.settle_the_rest()
        replay = 'address: https://better.test/\nverdict: clean\nREPLAY incomplete sheet https://better.test/s.css\n'
        (self.before / 'better-0002.txt').write_text(replay)
        (self.after / 'better-0002.txt').write_text(replay)
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('to record again: 1 pages\n  better-0002.txt', out)

    def test_a_replay_with_no_verdict_is_broken_never_progress(self):
        # The page never arrived: its record is short of every line the
        # judged one had, which is not progress.
        self.settle_the_rest()
        (self.after / 'better-0002.txt').write_text(
            'address: https://better.test/\nverdict: clean\nREPLAY unfinished it never arrived in 60 s\n')
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 1)
        self.assertIn('progress: 0 pages\n', out)
        self.assertIn('broken: 1 pages\n  better-0002.txt  https://better.test/\n'
                      '    ! unfinished it never arrived in 60 s\n', out)
        self.assertIn('after: 5 pages, 2 clean, 0 incomplete, 1 broken;', out)
        # Broken before as well: still broken, and no regression of its own.
        (self.before / 'better-0002.txt').write_text(
            'address: https://better.test/\nverdict: clean\nREPLAY unfinished it never arrived in 60 s\n')
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('still broken: 1 pages\n', out)

    def test_recovered_is_not_compared(self):
        self.settle_the_rest()
        (self.before / 'better-0002.txt').write_text(
            'address: https://better.test/\nverdict: clean\nREPLAY incomplete script https://better.test/a.js\n')
        (self.after / 'better-0002.txt').write_text(
            'address: https://better.test/\nverdict: 1 MISSING\nMISSING global fetch (1)\n')
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('regressions: 0 pages\n', out)
        self.assertIn('recovered: 1 pages\n  better-0002.txt', out)

    def test_an_unreadable_page_file_is_an_error(self):
        (self.after / 'odd.txt').mkdir()
        rc, _ = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)

    def test_not_a_directory(self):
        rc, _ = self.run_main(self.before, self.after / 'nothere')
        self.assertEqual(rc, 2)


if __name__ == '__main__':
    unittest.main()

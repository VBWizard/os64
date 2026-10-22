#!/usr/bin/env python3
"""tools/yonder_corpus_diff.py on two hand-made replay directories: a page
that gained a line, one that lost one, one whose counts alone moved, and
one the second replay did not write; one only it wrote, and a record no
replay wrote, each unjudged; then a page whose
replay was incomplete (newly, a gate), never reached a verdict (with an
incomplete line too, either order), or recovered, in each direction; the
outcomes of fetches the replay says it does not reproduce, compared on
neither side, and pictures and fonts decided without a fetch, compared; a
page file cut short (a replay's without its `end`), whose counts do not
reconcile, that says it is unjudged, or a `*.txt` that is not a page file,
each unjudged; and a page file or directories that cannot be read. Run with
`python3 -I`;
tools/test_yonder_replay_host.sh runs it."""
import contextlib
import importlib.util
import io
import os
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

def replay(address, state, body='verdict: clean\n', detail='', why=''):
    """A record as the replay runner writes it: the page's state second,
    the window's record from its verdict on, the replay's own lines, and
    `end`."""
    head = f'address: {address}\nREPLAY state: {state}{" " + why if why else ""}\n'
    if state == 'unjudged':
        return head + 'end\n'
    return head + body + detail + 'REPLAY not reproduced: font-fetch image picture-fetch\nend\n'


def as_replay(text):
    """A record yonder wrote, as a replay of it that reached its verdict
    writes it."""
    first, _, rest = text.partition('\n')
    return (f'{first}\nREPLAY state: verdict\n{rest}'
            'REPLAY not reproduced: font-fetch image picture-fetch\nend\n')


BEFORE = {
    'worse-0001.txt': as_replay('address: https://worse.test/\nverdict: 1 MISSING\nMISSING global fetch (1)\n'),
    'better-0002.txt': as_replay('address: https://better.test/\nverdict: 1 MISSING, 1 FAILED\n'
                                 'MISSING global localStorage (4)\n'
                                 "FAILED script https://better.test/a.js: ReferenceError: 'localStorage' is not defined (1)\n"),
    'counts-0003.txt': as_replay('address: https://counts.test/\nverdict: 1 MISSING\nMISSING global fetch (1)\n'),
    'gone-0004.txt': as_replay('address: https://gone.test/\nverdict: clean\n'),
}
AFTER = {
    'worse-0001.txt': as_replay('address: https://worse.test/\nverdict: 1 MISSING, 1 FAILED\nMISSING global fetch (1)\n'
                                'FAILED script https://worse.test/b.js: TypeError: x is not a function (1)\n'),
    'better-0002.txt': as_replay('address: https://better.test/\nverdict: clean\n'),
    'counts-0003.txt': as_replay('address: https://counts.test/\nverdict: 1 MISSING\nMISSING global fetch (9)\nbytes: 2\n'),
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
        self.assertIn('before: 4 pages, 1 clean; after: 3 pages, 1 clean, 0 incomplete, 0 broken; '
                      '1 lines gained, 2 lines lost', out)

    def test_a_page_only_after_has_is_unjudged(self):
        # Both replays read one kept corpus: a page the before one lacks is
        # one it never replayed (it stopped, or was partial).
        self.settle_the_rest()
        (self.after / 'new-0005.txt').write_text(as_replay('address: https://new.test/\nverdict: clean\n'))
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)
        self.assertIn('unjudged: 1 pages\n  new-0005.txt  new-0005.txt: ', out)
        self.assertIn('did not replay this page', out)

    def test_page_text_cannot_drive_the_terminal(self):
        self.settle_the_rest()
        (self.before / 'worse-0001.txt').write_text(as_replay(
            'address: https://worse.test/\\x1B[2J\nverdict: 1 MISSING\nMISSING global fetch (1)\n'))
        (self.after / 'worse-0001.txt').write_text(as_replay(
            'address: https://worse.test/\\x1B[2J\nverdict: 1 MISSING, 1 FAILED\nMISSING global fetch (1)\n'
            'FAILED script x.js: \\x1B]0;owned\\x07 (1)\n'))
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 1)
        self.assertNotIn('\x1b', out)
        self.assertIn('https://worse.test/\\x1B[2J', out)
        self.assertIn('FAILED script x.js: \\x1B]0;owned\\x07', out)

    def test_records_of_two_addresses_are_unjudged(self):
        # One name, two pages: no comparison, and never silent.
        self.settle_the_rest()
        (self.after / 'better-0002.txt').write_text(as_replay('address: https://other.test/\nverdict: clean\n'))
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)
        self.assertIn('progress: 0 pages\n', out)
        self.assertIn('its records name two addresses, https://better.test/ and https://other.test/', out)

    def test_a_record_no_replay_wrote_is_unjudged(self):
        # A kept record has no state line: the diff compares replays only.
        self.settle_the_rest()
        (self.before / 'better-0002.txt').write_text('address: https://better.test/\nverdict: clean\n')
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)
        self.assertIn("not a replay's record", out)

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

    def test_newly_incomplete_is_a_gate(self):
        # The replay met a script the corpus never kept: its record gained a
        # FAILED line and lost others, and neither counts.
        self.settle_the_rest()
        (self.after / 'better-0002.txt').write_text(replay(
            'https://better.test/', 'incomplete', 'verdict: 1 FAILED\nFAILED script https://better.test/new.js: TypeError: no (1)\n',
            'REPLAY incomplete script https://better.test/M%49SSING.js\n', 'script https://better.test/M%49SSING.js'))
        rc, out = self.run_main(self.before, self.after)
        # A gate: the corpus must be recorded again before this page can
        # be judged, and the answer says so.
        self.assertEqual(rc, 1)
        self.assertIn('regressions: 0 pages\nprogress: 0 pages\n', out)
        self.assertIn('newly incomplete: 1 pages (the corpus must be re-recorded before this change can be judged on them)\n'
                      '  better-0002.txt  https://better.test/\n'
                      '    ? script https://better.test/MISSING.js\n', out)
        self.assertIn('after: 4 pages, 1 clean, 1 incomplete, 0 broken; 0 lines gained, 0 lines lost', out)

    def test_still_incomplete_is_to_record_again(self):
        self.settle_the_rest()
        again = replay('https://better.test/', 'incomplete', detail='REPLAY incomplete sheet https://better.test/s.css\n',
                       why='sheet https://better.test/s.css')
        (self.before / 'better-0002.txt').write_text(again)
        (self.after / 'better-0002.txt').write_text(again)
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('to record again: 1 pages\n  better-0002.txt', out)

    def test_a_replay_with_no_verdict_is_broken_never_progress(self):
        # The page never arrived: its record is short of every line the
        # judged one had, which is not progress.
        self.settle_the_rest()
        broken = replay('https://better.test/', 'unfinished', detail='REPLAY unfinished it never arrived in 60 s\n',
                        why='it never arrived in 60 s')
        (self.after / 'better-0002.txt').write_text(broken)
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 1)
        self.assertIn('progress: 0 pages\n', out)
        self.assertIn('broken: 1 pages\n  better-0002.txt  https://better.test/\n'
                      '    ! unfinished it never arrived in 60 s\n', out)
        self.assertIn('after: 4 pages, 1 clean, 0 incomplete, 1 broken;', out)
        # Broken before as well: still broken, and no regression of its own.
        (self.before / 'better-0002.txt').write_text(broken)
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('still broken: 1 pages\n', out)

    def test_unfinished_outranks_incomplete(self):
        # A replay that met a script the corpus never kept AND reached no
        # verdict is broken, whichever line comes first: the incomplete
        # line is detail under it, never a way out of the broken heading.
        self.settle_the_rest()
        for lines in ('REPLAY incomplete script https://better.test/s.js\nREPLAY unfinished it never arrived\n',
                      'REPLAY unfinished it never arrived\nREPLAY incomplete script https://better.test/s.js\n'):
            with self.subTest(lines=lines):
                (self.after / 'better-0002.txt').write_text(replay('https://better.test/', 'unfinished', detail=lines,
                                                                   why='it never arrived'))
                rc, out = self.run_main(self.before, self.after)
                self.assertEqual(rc, 1)
                self.assertIn('broken: 1 pages\n  better-0002.txt  https://better.test/\n'
                              '    ! unfinished it never arrived\n'
                              '    ! incomplete script https://better.test/s.js\n', out)
                self.assertNotIn('incomplete: 1 pages', out)

    def test_kinds_a_replay_does_not_reproduce_are_compared_on_neither_side(self):
        # Lines of the kinds a fetch decides, on one side (a replay that
        # fetched, were there one): compared on neither.
        self.settle_the_rest()
        (self.before / 'better-0002.txt').write_text(as_replay(
            'address: https://better.test/\nverdict: 1 MISSING, 1 FAILED\nMISSING image webp (2)\n'
            'FAILED picture-fetch: https://better.test/p.png: HTTP 404 (1)\n'))
        (self.after / 'better-0002.txt').write_text(replay('https://better.test/', 'verdict'))
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('regressions: 0 pages\nprogress: 0 pages\n', out)
        self.assertIn('; not compared, as the replay does not reproduce them: font-fetch image picture-fetch', out)

    def test_a_picture_decided_without_a_fetch_is_compared(self):
        # A refused address and a font no face names are what the page did,
        # fetch or no fetch: losing them is progress, and the diff says so.
        self.settle_the_rest()
        (self.before / 'better-0002.txt').write_text(as_replay(
            'address: https://better.test/\nverdict: 2 MISSING, 1 FAILED\nMISSING picture-scheme data (1)\n'
            'MISSING font Arial (2)\nFAILED picture: x.png: the address was refused (1)\n'))
        (self.after / 'better-0002.txt').write_text(replay('https://better.test/', 'verdict'))
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('progress: 1 pages\n', out)
        self.assertIn('    - MISSING picture-scheme data\n', out)
        self.assertIn('    - MISSING font Arial\n', out)
        self.assertIn('    - FAILED picture: x.png: the address was refused\n', out)

    def test_a_replay_cut_short_is_unjudged_never_clean(self):
        # A write cut by a quota: its address and its state survive, its
        # `end` does not. Read as it is it would be a clean page.
        self.settle_the_rest()
        (self.after / 'better-0002.txt').write_text('address: https://better.test/\nREPLAY state: verdict\nverdict: clean\n')
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)
        self.assertIn('progress: 0 pages\n', out)
        self.assertIn("without its `end`", out)

    def test_a_replay_that_says_it_is_unjudged_is_unjudged(self):
        self.settle_the_rest()
        (self.after / 'better-0002.txt').write_text(
            replay('https://better.test/', 'unjudged', why='1 of the harness\'s own checks failed while it ran'))
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)
        self.assertIn("unjudged: 1 of the harness's own checks failed while it ran", out)

    def test_a_txt_that_is_not_a_page_file_is_unjudged(self):
        # Passed over, it would be a page left out of the comparison.
        self.settle_the_rest()
        (self.after / 'notes.txt').write_text('not a page file\n')
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)
        self.assertIn('unjudged: 1 pages\n  notes.txt  ', out)
        self.assertIn('not a page file', out)

    def test_recovered_is_not_compared(self):
        self.settle_the_rest()
        (self.before / 'better-0002.txt').write_text(replay(
            'https://better.test/', 'incomplete', detail='REPLAY incomplete script https://better.test/a.js\n',
            why='script https://better.test/a.js'))
        (self.after / 'better-0002.txt').write_text(
            replay('https://better.test/', 'verdict', 'verdict: 1 MISSING\nMISSING global fetch (1)\n'))
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 0)
        self.assertIn('regressions: 0 pages\n', out)
        self.assertIn('recovered: 1 pages\n  better-0002.txt', out)

    def test_a_before_record_cut_short_is_unjudged(self):
        # Read as it is, the cut record would make every failure it had
        # read as progress.
        self.settle_the_rest()
        (self.before / 'better-0002.txt').write_text('address: https://better.test/\n')
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)
        self.assertIn('progress: 0 pages\n', out)
        self.assertIn('unjudged: 1 pages\n  better-0002.txt  ', out)
        self.assertIn('its second line is not a verdict', out)

    def test_a_record_whose_counts_do_not_reconcile_is_unjudged(self):
        self.settle_the_rest()
        (self.after / 'worse-0001.txt').write_text(
            as_replay('address: https://worse.test/\nverdict: 2 MISSING\nMISSING global fetch (1)\n'))
        rc, out = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)
        self.assertIn('unjudged: 1 pages\n  worse-0001.txt  ', out)
        self.assertIn('its verdict counts 2 MISSING and 0 FAILED lines, and it holds 1 and 0', out)

    @unittest.skipIf(hasattr(os, 'geteuid') and os.geteuid() == 0, 'root lists any directory')
    def test_a_directory_that_cannot_be_listed_is_an_error(self):
        self.before.chmod(0)
        self.after.chmod(0)
        try:
            rc, _ = self.run_main(self.before, self.after)
        finally:
            self.before.chmod(0o755)
            self.after.chmod(0o755)
        self.assertEqual(rc, 2)

    def test_an_unreadable_page_file_is_an_error(self):
        (self.after / 'odd.txt').mkdir()
        rc, _ = self.run_main(self.before, self.after)
        self.assertEqual(rc, 2)

    def test_not_a_directory(self):
        rc, _ = self.run_main(self.before, self.after / 'nothere')
        self.assertEqual(rc, 2)


if __name__ == '__main__':
    unittest.main()

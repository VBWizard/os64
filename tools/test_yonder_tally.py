#!/usr/bin/env python3
"""tools/yonder_tally.py on three hand-made page files in a directory that
also holds a kept page's bytes and folder, which are not read; a record
that lost lines, counted and said; a replay whose page was incomplete,
said and not counted; a placeholder never written, an error; then a
`*.txt` that is not a page file, page files that are not whole (cut short,
or not matching their verdict), a path not there and a directory that
cannot be listed, each an error. Run with `python3 -I tools/test_yonder_tally.py`; tools/test_yonder_replay_host.sh
runs it with the replay's own cases."""
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


tally = load('yonder_tally')

RECORDS = {
    'a.test-0001.txt': b'''address: https://a.test/
verdict: 3 MISSING, 2 FAILED
MISSING global fetch (3)
MISSING global localStorage (10)
MISSING css-property aspect-ratio (2)
FAILED script https://a.test/x.js: ReferenceError: 'URL' is not defined (1)
FAILED picture: https://a.test/p.png: HTTP 404 (1)
bytes: 100
''',
    'a.test-0002.txt': b'''address: https://a.test/second
verdict: 2 MISSING, 1 FAILED
MISSING global fetch (1)
MISSING css-property cursor (5)
FAILED picture: https://a.test/q.png: HTTP 404 (1)
''',
    # A token and a tab in data come back as the page wrote them; two
    # scripts dying the same way are one file for that failure.
    'b.test-0003.txt': b'''address: http://user@B.test:8080/M%49SSING
verdict: 3 MISSING, 2 FAILED
MISSING global fetch (2)
MISSING global localStorage (1)
MISSING global M%49SSINGno\\x09tab (4)
FAILED script https://b.test/y.js: ReferenceError: 'URL' is not defined (1)
FAILED script https://b.test/z.js: ReferenceError: 'URL' is not defined (1)
''',
}


class TallyCases(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.dir.name)
        for name, text in RECORDS.items():
            (root / name).write_bytes(text)
        (root / 'a.test-0001.html').write_bytes(b'<p>MISSING global x (1)</p>\n')
        (root / 'a.test-0001.d').mkdir()
        (root / 'a.test-0001.d' / 'index').write_bytes(b'1\tscript\thttps://a.test/x.js\n')
        self.root = root
        self.records = tally.read_records([str(root)])

    def tearDown(self):
        self.dir.cleanup()

    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            rc = tally.main([str(self.root), *argv])
        return rc, out.getvalue()

    def test_only_page_files_are_read(self):
        self.assertEqual(len(self.records), 3)

    def test_decoding(self):
        b = [r for r in self.records if r.path.name.startswith('b.')][0]
        self.assertEqual(b.address, 'http://user@B.test:8080/MISSING')
        self.assertEqual(b.site, 'b.test')
        self.assertIn(('global', 'MISSINGno\ttab'), b.missing)

    def test_the_spelling_is_injective(self):
        # A literal `M%49SSING` is written `M%2549SSING`, the token `M%49SSING`,
        # a literal `%25` `%2525`, and any other `%` as it is.
        self.assertEqual(tally.unescape(b'q=M%2549SSING&r=M%49SSING&s=%2525&t=75%&u=%2'),
                         'q=M%49SSING&r=MISSING&s=%25&t=75%&u=%2')

    def test_ranked_by_files_then_asks(self):
        table = tally.tally_missing(self.records)
        globals_ = tally.ranked(table['global'])
        self.assertEqual([(n, r[0], r[1], len(r[2])) for n, r in globals_],
                         [('fetch', 3, 6, 2), ('localStorage', 2, 11, 2), ('MISSINGno\ttab', 1, 4, 1)])
        css = tally.ranked(table['css-property'])
        self.assertEqual([n for n, _ in css], ['cursor', 'aspect-ratio'])

    def test_table_text(self):
        rc, out = self.run_main('--top', '2')
        self.assertEqual(rc, 0)
        self.assertIn('3 page files, 2 sites', out)
        self.assertIn('MISSING global: 3 names', out)
        self.assertRegex(out, r'\n +3 +2 +6  fetch\n +2 +2 +11  localStorage\n +and 1 more\n')

    def test_surface_counts_files_naming_any(self):
        files, sites, asks, each = tally.surface(self.records, ['localStorage', 'cursor'], set())
        self.assertEqual((files, len(sites), asks), (3, 2, 16))
        self.assertEqual(each['cursor'][0], 1)
        files, sites, asks, _ = tally.surface(self.records, ['localStorage', 'cursor'], {'global'})
        self.assertEqual((files, asks), (2, 11))
        rc, out = self.run_main('--surface', 'fetch', 'localStorage')
        self.assertEqual(rc, 0)
        self.assertIn('surface fetch, localStorage: 3 of 3 files, 2 of 2 sites, 17 asks', out)

    def run_err(self, *paths):
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            rc = tally.main([str(p) for p in paths])
        return rc, err.getvalue()

    def test_a_txt_that_is_not_a_page_file_is_an_error(self):
        # Passed over, it would make a census of fewer pages than were
        # handed in read as one of all of them.
        (self.root / 'notes.txt').write_bytes(b'not a page file\n')
        rc, err = self.run_err(self.root)
        self.assertEqual(rc, 2)
        self.assertIn("notes.txt: not a page file: its first line is not `address: `", err)

    def test_a_mixed_directory(self):
        # A census is not a diff: a record that lost lines is counted (what
        # it kept was asked for) and said; a replay whose page was
        # incomplete is said and left out.
        (self.root / 'r.test-0007.txt').write_bytes(
            b'address: https://r.test/\nREPLAY state: incomplete script x\nverdict: 1 MISSING\n'
            b'MISSING global fetch (1)\nREPLAY incomplete script x\nREPLAY not reproduced: image\nend\n')
        (self.root / 'l.test-0008.txt').write_bytes(
            b'address: https://l.test/\nverdict: 1 MISSING\nMISSING global fetch (1)\nREPLAY unjudged: lines lost\n')
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(err):
            rc = tally.main([str(self.root)])
        self.assertEqual(rc, 0)
        self.assertIn('4 page files, 3 sites', out.getvalue())
        self.assertRegex(out.getvalue(), r'\n +4 +3 +7  fetch\n')
        self.assertIn('1 record(s) lost lines; the lines they kept are counted', err.getvalue())
        self.assertIn('not counted: 1 incomplete', err.getvalue())
        # A claimed name never written is damage: named, and the answer 2.
        (self.root / 'u.test-0009.txt').write_bytes(b'address: https://u.test/\nverdict: unwritten\n')
        rc, err = self.run_err(self.root)
        self.assertEqual(rc, 2)
        self.assertIn('u.test-0009.txt: damage: its name was claimed and its record never written', err)

    def test_a_record_cut_short_is_an_error(self):
        (self.root / 'cut-0009.txt').write_bytes(b'address: https://cut.test/\n')
        rc, err = self.run_err(self.root)
        self.assertEqual(rc, 2)
        self.assertIn('cut-0009.txt: not a whole page file: its second line is not a verdict', err)

    def test_a_record_whose_lines_do_not_match_its_verdict_is_an_error(self):
        (self.root / 'short-0009.txt').write_bytes(
            b'address: https://short.test/\nverdict: 2 MISSING, 1 FAILED\nMISSING global fetch (1)\n')
        rc, err = self.run_err(self.root)
        self.assertEqual(rc, 2)
        self.assertIn('its verdict counts 2 MISSING and 1 FAILED lines, and it holds 1 and 0', err)

    @unittest.skipIf(hasattr(os, 'geteuid') and os.geteuid() == 0, 'root lists any directory')
    def test_a_directory_that_cannot_be_listed_is_an_error(self):
        locked = self.root / 'locked'
        locked.mkdir()
        locked.chmod(0)
        try:
            rc, err = self.run_err(locked)
        finally:
            locked.chmod(0o755)
        self.assertEqual(rc, 2)
        self.assertIn('cannot be listed', err)

    def test_a_path_not_there_is_an_error(self):
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            rc = tally.main([str(self.root / 'nothere')])
        self.assertEqual(rc, 2)
        self.assertIn('no such file or directory', err.getvalue())

    def test_failed_ranked_with_pages(self):
        rc, out = self.run_main('--failed')
        self.assertEqual(rc, 0)
        lines = out.splitlines()
        self.assertEqual(lines[0], '3 page files, 2 distinct failures')
        # The picture's address is not its key: two files, one line each.
        self.assertRegex(lines[2], r"^ +2 +2 +3  script: ReferenceError: 'URL' is not defined$")
        self.assertIn('https://a.test/', lines[3])
        self.assertRegex(out, r'\n +2 +1 +2  picture: HTTP 404\n')


if __name__ == '__main__':
    unittest.main()

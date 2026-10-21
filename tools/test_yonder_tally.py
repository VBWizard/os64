#!/usr/bin/env python3
"""tools/yonder_tally.py on three hand-made page files, and a directory that
also holds what is not one: a note, and a kept page's bytes and folder.
Run with `python3 -I tools/test_yonder_tally.py`; tools/test_yonder_replay_host.sh
runs it with the replay's own cases."""
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
        (root / 'notes.txt').write_bytes(b'not a page file\n')
        (root / 'a.test-0001.html').write_bytes(b'<p>MISSING global x (1)</p>\n')
        (root / 'a.test-0001.d').mkdir()
        (root / 'a.test-0001.d' / 'index').write_bytes(b'1\tscript\thttps://a.test/x.js\n')
        self.root = root
        with contextlib.redirect_stderr(io.StringIO()) as err:
            self.records = tally.read_records([str(root)])
        self.err = err.getvalue()

    def tearDown(self):
        self.dir.cleanup()

    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            rc = tally.main([str(self.root), *argv])
        return rc, out.getvalue()

    def test_only_page_files_are_read(self):
        self.assertEqual(len(self.records), 3)
        self.assertIn('1 file(s) passed over', self.err)

    def test_decoding(self):
        b = [r for r in self.records if r.path.name.startswith('b.')][0]
        self.assertEqual(b.address, 'http://user@B.test:8080/MISSING')
        self.assertEqual(b.site, 'b.test')
        self.assertIn(('global', 'MISSINGno\ttab'), b.missing)

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

#!/usr/bin/env python3
"""tools/yonder_records.py, the one reader of page files: every way a file
can fail to be a whole record is named, and never read as a record of what
could be read of it; a whole record is read, and its REPLAY lines give its
state by the State order. Run with `python3 -I tools/test_yonder_records.py`;
tools/test_yonder_replay_host.sh runs it with the replay's own cases."""
import importlib.util
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


records = load('yonder_records')

WHOLE = b'address: https://a.test/\nverdict: 1 MISSING, 1 FAILED\nMISSING global fetch (2)\nFAILED page: no (1)\n'

# Each malformation, and the words its reason must hold.
MALFORMED = {
    'empty': (b'', 'not a page file'),
    'byte order mark': (b'\xef\xbb\xbf' + WHOLE, 'byte order mark'),
    'carriage returns': (WHOLE.replace(b'\n', b'\r\n'), 'carriage return'),
    'no address': (b'verdict: clean\n', 'not a page file'),
    'cut after the address': (b'address: https://a.test/\n', 'second line is not a verdict'),
    'a verdict of nothing it knows': (b'address: https://a.test/\nverdict: fine\n', 'second line is not a verdict'),
    'two verdict lines': (b'address: https://a.test/\nverdict: clean\nverdict: clean\n', 'a second verdict line'),
    'a line short of its verdict': (b'address: https://a.test/\nverdict: 2 MISSING\nMISSING global fetch (1)\n',
                                    'counts 2 MISSING and 0 FAILED lines, and it holds 1 and 0'),
    'a line past its verdict': (b'address: https://a.test/\nverdict: clean\nFAILED page: no (1)\n',
                                'counts 0 MISSING and 0 FAILED lines, and it holds 0 and 1'),
    'a record line with no count': (b'address: https://a.test/\nverdict: 1 MISSING\nMISSING global fetch\n',
                                    'a record line with no count'),
    'a REPLAY line on a record yonder wrote': (b'address: https://a.test/\nverdict: clean\nREPLAY lost its way\n',
                                               'a REPLAY line of no kind this reader knows'),
    'a NUL': (WHOLE + b'bytes: 1\x002\n', 'NUL'),
    'bytes that are not UTF-8': (WHOLE + b'title: caf\xe9\n', 'not UTF-8'),
    'an escape to bytes that are not UTF-8': (b'address: https://a.test/\\xff\nverdict: clean\n',
                                              'escapes to bytes that are not UTF-8'),
    'a verdict spelled with a sign': (b'address: https://a.test/\nverdict: +1 MISSING\nMISSING global x (1)\n',
                                      'second line is not a verdict'),
    "a replay's record cut before its end": (b'address: https://a.test/\nREPLAY state: verdict\nverdict: clean\n',
                                             'without its `end`'),
    "a replay's record with lines past its end": (b'address: https://a.test/\nREPLAY state: verdict\nverdict: clean\n'
                                                  b'end\nFAILED page: no (1)\nend\n', 'past its `end`'),
    'a state of nothing it knows': (b'address: https://a.test/\nREPLAY state: fine\nverdict: clean\nend\n',
                                    'second line is not a verdict'),
    'a verdict state with an incomplete line': (b'address: https://a.test/\nREPLAY state: verdict\nverdict: clean\n'
                                                b'REPLAY incomplete script x\nend\n', 'do not agree with its state'),
    'an incomplete state with nothing incomplete': (b'address: https://a.test/\nREPLAY state: incomplete x\n'
                                                    b'verdict: clean\nend\n', 'do not agree with its state'),
    'a stray kind on the not-reproduced line': (b'address: https://a.test/\nREPLAY state: verdict\nverdict: clean\n'
                                                b'REPLAY not reproduced: image global\nend\n',
                                                'naming what a replay does reproduce: global'),
}


def replayed(state, body=b'verdict: clean\n', detail=b'', why=b''):
    """A record as the replay runner writes it."""
    head = b'address: https://a.test/\nREPLAY state: ' + state + (b' ' + why if why else b'') + b'\n'
    return head + (b'end\n' if state == b'unjudged' else body + detail + b'REPLAY not reproduced: image\nend\n')


class ReaderCases(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def read(self, data, name='a.test-0001.txt'):
        path = self.root / name
        path.write_bytes(data)
        return records.read(path)

    def test_a_whole_record(self):
        rec = self.read(WHOLE)
        self.assertIsInstance(rec, records.Record)
        self.assertEqual(rec.missing, {('global', 'fetch'): 2})
        self.assertEqual(rec.failed, {('page', 'no'): 1})
        self.assertEqual(rec.state, records.State.VERDICT)

    def test_every_malformation_is_named(self):
        for what, (data, words) in MALFORMED.items():
            with self.subTest(what):
                rec = self.read(data)
                self.assertIsInstance(rec, records.Unjudged)
                self.assertIn(words, str(rec))

    def test_a_directory_is_not_a_page_file(self):
        (self.root / 'odd.txt').mkdir()
        rec = records.read(self.root / 'odd.txt')
        self.assertIsInstance(rec, records.Unjudged)
        self.assertIn('not a file', str(rec))

    @unittest.skipIf(hasattr(os, 'geteuid') and os.geteuid() == 0, 'root reads any file')
    def test_a_file_that_cannot_be_read(self):
        path = self.root / 'locked.txt'
        path.write_bytes(WHOLE)
        path.chmod(0)
        try:
            rec = records.read(path)
        finally:
            path.chmod(0o644)
        self.assertIsInstance(rec, records.Unjudged)
        self.assertIn('cannot be read', str(rec))

    def test_the_state_is_the_one_the_record_says(self):
        incomplete = b'REPLAY incomplete script https://a.test/M%49SSING.js\n'
        unfinished = b'REPLAY unfinished it never arrived\n'
        rec = self.read(replayed(b'incomplete', detail=incomplete, why=b'script'))
        self.assertEqual((rec.state, rec.incomplete), (records.State.INCOMPLETE, ['script https://a.test/MISSING.js']))
        rec = self.read(replayed(b'unfinished', detail=unfinished, why=b'it never arrived'))
        self.assertEqual((rec.state, rec.unfinished, rec.reason),
                         (records.State.UNFINISHED, ['it never arrived'], 'it never arrived'))
        # Both: unfinished, the incomplete line kept as detail.
        for lines in (incomplete + unfinished, unfinished + incomplete):
            rec = self.read(replayed(b'unfinished', detail=lines))
            self.assertEqual(rec.state, records.State.UNFINISHED)
            self.assertEqual(len(rec.incomplete), 1)
        rec = self.read(replayed(b'unjudged', why=b'its record could not be written'))
        self.assertEqual((rec.state, rec.reason), (records.State.UNJUDGED, 'its record could not be written'))

    def test_kinds_not_reproduced_are_read_and_are_no_state(self):
        rec = self.read(replayed(b'verdict'))
        self.assertEqual((rec.state, rec.not_reproduced), (records.State.VERDICT, {'image'}))

    def test_lines_lost_and_never_written_are_unjudged(self):
        # One marker, whatever the cause: a record that lost a line, even
        # past a full table, may have lost the one a change made.
        rec = self.read(b'address: https://a.test/\nverdict: clean\nfailures not kept: 12728\n'
                        b'REPLAY unjudged: lines lost\n')
        self.assertEqual((rec.state, rec.reason), (records.State.UNJUDGED, 'its record lost lines it could not keep'))
        rec = self.read(b'address: https://a.test/\nverdict: unwritten\n')
        self.assertEqual(rec.state, records.State.UNJUDGED)

    def test_every_txt_is_listed(self):
        (self.root / 'b.txt').write_bytes(b'')
        (self.root / 'a.txt').mkdir()
        (self.root / 'a.html').write_bytes(b'')
        self.assertEqual([p.name for p in records.page_files(self.root)], ['a.txt', 'b.txt'])

    @unittest.skipIf(hasattr(os, 'geteuid') and os.geteuid() == 0, 'root lists any directory')
    def test_a_directory_that_cannot_be_listed(self):
        locked = self.root / 'locked'
        locked.mkdir()
        locked.chmod(0)
        try:
            with self.assertRaises(records.Unreadable):
                records.page_files(locked)
        finally:
            locked.chmod(0o755)


if __name__ == '__main__':
    unittest.main()

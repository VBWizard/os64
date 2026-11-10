#!/usr/bin/env python3
"""Regression checks for the shared tracker and the HTTP/2 entry point."""
import csv
import io
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
TRACKER = runpy.run_path(str(TOOLS / 'tracker.py'))


def row(row_id='F.1', status='todo', **fields):
    value = dict(zip(TRACKER['COLUMNS'], [row_id, 'A slice', '', status, '', '']))
    value.update(fields)
    return value


class TrackerTests(unittest.TestCase):
    def test_real_boards_and_http2_compatibility_command(self):
        for name, prefix in [('IFRAME', 'F'), ('HTTP2', 'H2')]:
            path = TOOLS.parent / f'docs/design/pending/{name}_TRACKER.csv'
            self.assertEqual(TRACKER['check'](TRACKER['read'](path), prefix), [])
        result = subprocess.run([sys.executable, '-I', str(TOOLS / 'http2_tracker.py'), '--quiet'],
                                capture_output=True, text=True)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, '', ''))

    def test_slice_only_progress_and_html_escaping(self):
        rows = [row(status='done', done_in='#123', task='<script>bad()</script>'),
                row('F.2'), row('F.3', 'dropped')]
        page = TRACKER['page'](rows, 'a&b.csv', '<Frames>')
        self.assertIn('1 of 2 slices done.', page)
        self.assertIn('width:50%', page)
        self.assertNotIn('0/0', page)
        self.assertNotIn('sub-tasks done', page)
        self.assertIn('&lt;script&gt;bad()&lt;/script&gt;', page)
        self.assertIn('<title>&lt;Frames&gt;</title>', page)
        self.assertIn('a&amp;b.csv', page)

    def test_subtasks_before_parent_survive_grouping(self):
        rows = [row('H2.1.1', 'done', done_in='#42'), row('H2.1')]
        self.assertEqual(TRACKER['check'](rows, 'H2'), [])
        self.assertIn('1 of 1 sub-tasks done.', TRACKER['page'](rows, 'test.csv'))
        self.assertIn('[x] H2.1.1', TRACKER['board'](rows))

    def test_invalid_rows(self):
        cases = [([row(), row()], 'listed twice'),
                 ([row(status='unknown')], 'status'),
                 ([row(depends_on='F.9')], 'which has no row'),
                 ([row(status='done')], 'done without'),
                 ([row('Q1', 'ruled')], 'no ruling'),
                 ([row('F.1.1')], 'has no row'),
                 ([row(task=' ')], 'no task text'),
                 ([row('bad')], 'not a slice')]
        for rows, message in cases:
            with self.subTest(message=message):
                self.assertIn(message, '\n'.join(TRACKER['check'](rows)))
        self.assertIn('expected slice prefix H2', '\n'.join(TRACKER['check']([row()], 'H2')))

    def test_malformed_csv_cli_fails_without_writing_page(self):
        for text in ['F.1,Too,few\n', 'F.1,Too,many,todo,,,extra\n']:
            with self.subTest(text=text), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'bad.csv'
                output = Path(directory) / 'out.html'
                path.write_text(','.join(TRACKER['COLUMNS']) + '\n' + text)
                result = subprocess.run([sys.executable, '-I', str(TOOLS / 'tracker.py'),
                                         '--csv', str(path), '--html', str(output)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 1)
                self.assertIn('line 2: expected six fields', result.stderr)
                self.assertNotIn('Traceback', result.stderr)
                self.assertFalse(output.exists())

    def test_quoted_fields_and_blank_lines(self):
        data = io.StringIO()
        writer = csv.DictWriter(data, TRACKER['COLUMNS'])
        writer.writeheader()
        data.write('\n')
        wanted = row(task='Comma, and "quotes"')
        writer.writerow(wanted)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'quoted.csv'
            path.write_text(data.getvalue())
            self.assertEqual(TRACKER['read'](path), [wanted])


if __name__ == '__main__':
    unittest.main()

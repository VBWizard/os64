"""Comparator controls: small differences, metadata, reference logic and invalid evidence."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from PIL import Image

from compare import Fuzzy, Unsupported, compare, file_for, fuzzy_value, metadata, verdict
from run import run


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.a = self.root/'a.png'
        self.b = self.root/'b.png'
        Image.new('RGB', (4, 4)).save(self.a)
        Image.new('RGB', (4, 4)).save(self.b)

    def tearDown(self):
        self.temp.cleanup()

    def image(self, color, xy=(0, 0)):
        image = Image.new('RGB', (4, 4))
        image.putpixel(xy, color)
        image.save(self.b)

    def test_exact_and_mismatch(self):
        self.assertTrue(compare(self.a, self.b)['pass'])
        self.assertFalse(compare(self.a, self.b, 'mismatch')['pass'])
        self.image((0,0,1))
        result = compare(self.a, self.b)
        self.assertEqual((result['maxDifference'],result['totalPixels']), (1,1))
        self.assertFalse(result['pass'])
        self.assertTrue(compare(self.a,self.b,'mismatch')['pass'])

    def test_fuzzy_inclusive_and_inverted(self):
        self.image((3,4,5))
        fuzzy = Fuzzy((5,5),(1,1))
        self.assertTrue(compare(self.a,self.b,fuzzy=fuzzy)['pass'])
        self.assertFalse(compare(self.a,self.b,'mismatch',fuzzy)['pass'])
        for fuzzy in [Fuzzy((0,4),(0,1)), Fuzzy((0,5),(0,0)), Fuzzy((6,8),(1,1)), Fuzzy((5,5),(2,3))]:
            with self.subTest(fuzzy=fuzzy):
                self.assertFalse(compare(self.a,self.b,fuzzy=fuzzy)['pass'])

    def test_exact_fuzzy_zero_exception(self):
        self.assertTrue(compare(self.a,self.b,fuzzy=Fuzzy((10,20),(0,30)))['pass'])
        self.assertTrue(compare(self.a,self.b,fuzzy=Fuzzy((0,20),(10,30)))['pass'])
        self.assertFalse(compare(self.a,self.b,fuzzy=Fuzzy((1,20),(1,30)))['pass'])

    def test_no_edge_exemption(self):
        self.image((255,255,255), (3,3))
        self.assertFalse(compare(self.a,self.b,fuzzy=Fuzzy((0,24),(0,16)))['pass'])

    def test_multiple_rgb_channels_count_one_pixel(self):
        self.image((1,2,3))
        self.assertEqual(compare(self.a,self.b)['totalPixels'],1)

    def test_dimensions_are_error_not_mismatch_success(self):
        Image.new('RGB',(5,4)).save(self.b)
        with self.assertRaises(ValueError):
            compare(self.a,self.b,'mismatch')

    def test_alpha_requires_compositing(self):
        Image.new('RGBA',(4,4),(0,0,0,0)).save(self.b)
        with self.assertRaises(ValueError):
            compare(self.a,self.b)

    def test_palette_alpha_requires_compositing(self):
        image=Image.new('P',(4,4));image.info['transparency']=0;image.save(self.b)
        with self.assertRaises(ValueError):
            compare(self.a,self.b)

    def test_missing_and_corrupt_images_are_errors(self):
        self.b.write_bytes(b'not an image')
        with self.assertRaises(OSError): compare(self.a,self.b)
        self.b.unlink()
        with self.assertRaises(OSError): compare(self.a,self.b)

    def test_fuzzy_syntax(self):
        self.assertEqual(fuzzy_value('15;300'),Fuzzy((15,15),(300,300)))
        self.assertEqual(fuzzy_value('totalPixels=0-300;maxDifference=0-15'),Fuzzy((0,15),(0,300)))
        self.assertEqual(fuzzy_value('totalPixels=300;15'),Fuzzy((15,15),(300,300)))
        for value in ['15','15;300;4','-1;0','1-0;0','256;0','3.5;0','x=1;0','maxDifference=1;maxDifference=2']:
            with self.subTest(value=value),self.assertRaises(ValueError): fuzzy_value(value)

    def test_flat_reference_rules(self):
        def row(relation,passed):return {'relation':relation,'pass':passed}
        self.assertTrue(verdict([row('match',False),row('match',True),row('mismatch',True)]))
        self.assertFalse(verdict([row('match',True),row('mismatch',False)]))
        self.assertFalse(verdict([row('match',False),row('mismatch',True)]))
        self.assertTrue(verdict([row('mismatch',True),row('mismatch',True)]))
        self.assertFalse(verdict([row('mismatch',True),row('mismatch',False)]))
        with self.assertRaises(ValueError):verdict([])

    def test_metadata_overrides(self):
        (self.root/'test.html').write_text('''<link href="ref.html" rel="match"><link rel="mismatch" href="other.html">
        <meta name="fuzzy" content="0-10;0-100"><meta name="fuzzy" content="ref.html:2;3">''')
        meta=metadata(self.root,'/test.html')
        self.assertEqual(meta.references,[('/ref.html','match'),('/other.html','mismatch')])
        self.assertEqual(meta.tolerance('/ref.html'),Fuzzy((2,2),(3,3)))
        self.assertEqual(meta.tolerance('/other.html'),Fuzzy((0,10),(0,100)))

    def test_xml_metadata_namespace(self):
        (self.root/'test.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" xmlns:h="http://www.w3.org/1999/xhtml">
        <h:link rel="match" href="ref.svg"/><h:meta name="fuzzy" content="0-2;0-3"/></svg>''')
        self.assertEqual(metadata(self.root,'/test.svg').references,[('/ref.svg','match')])

    def test_invalid_metadata_and_unsupported(self):
        for text in ['<link rel="match">','<meta name="fuzzy" content="1;2"><meta name="fuzzy" content="1;2">',
                     '<meta name="fuzzy" content="missing.html:1;2">']:
            (self.root/'test.html').write_text(text)
            with self.assertRaises(ValueError):metadata(self.root,'/test.html')
        for text in ['<base href="/">','<link rel="match" href="https://example.com/ref">']:
            (self.root/'test.html').write_text(text)
            with self.assertRaises(Unsupported):metadata(self.root,'/test.html')

    def test_file_escape_and_symlink(self):
        with self.assertRaises(ValueError):file_for(self.root,'/%2e%2e/etc/passwd')
        (self.root/'link').symlink_to('/etc/passwd')
        with self.assertRaises(ValueError):file_for(self.root,'/link')

    def test_runner_errors_cannot_pass_negative_controls(self):
        fixture_root=self.root/'fixtures';fixture_root.mkdir()
        (fixture_root/'bad.html').write_text('<link rel="match" href="missing.html">')
        spec=self.root/'suite.json'
        spec.write_text(json.dumps({'cases':[{'url':'/bad.html','kind':'negative-control'}]}))
        class FakeCapture:
            def __init__(self,*args):self.provenance={}
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def screenshot(self,*args):raise AssertionError('missing metadata should stop capture')
        with patch('run.Capture',FakeCapture):
            self.assertFalse(run(fixture_root,spec,self.root/'out'))
        report=json.loads((self.root/'out/results.json').read_text())
        self.assertEqual(report['cases'][0]['status'],'ERROR')
        self.assertFalse(report['cases'][0]['accepted'])

    def test_reference_chain_is_visible_unsupported(self):
        fixture_root=self.root/'fixtures';fixture_root.mkdir()
        (fixture_root/'test.html').write_text('<link rel="match" href="ref.html">')
        (fixture_root/'ref.html').write_text('<link rel="match" href="test.html">')
        spec=self.root/'suite.json'
        spec.write_text(json.dumps({'cases':[{'url':'/test.html','kind':'fixture'}]}))
        class FakeCapture:
            def __init__(self,*args):self.provenance={}
            def __enter__(self):return self
            def __exit__(self,*args):pass
        with patch('run.Capture',FakeCapture):
            self.assertFalse(run(fixture_root,spec,self.root/'out'))
        report=json.loads((self.root/'out/results.json').read_text())
        self.assertEqual(report['cases'][0]['status'],'UNSUPPORTED')

    def test_bad_suite_stops_before_browser(self):
        spec=self.root/'suite.json';spec.write_text(json.dumps({'cases':[]}))
        with self.assertRaises(ValueError):run(self.root,spec,self.root/'output')


if __name__ == '__main__':
    unittest.main()

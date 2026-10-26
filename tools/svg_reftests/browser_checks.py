"""Browser-backed resource regressions; run explicitly with Playwright installed."""
import json
from pathlib import Path
import tempfile
import unittest
from PIL import Image

from run import run
from capture import Capture


class ResourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        self.root = self.folder/'fixtures'
        self.root.mkdir()
        (self.root/'bad.png').write_bytes(b'HTTP 200, but not a decodable image')
        (self.root/'bad.svg').write_text('<svg this is not well-formed')
        Image.new('RGB',(16,16),(32,145,222)).save(self.root/'good.png')

    def tearDown(self):
        self.temp.cleanup()

    def svg(self, href, attribute='href'):
        return ('<svg xmlns="http://www.w3.org/2000/svg" '
                'xmlns:xlink="http://www.w3.org/1999/xlink" width="16" height="16">'
                f'<image {attribute}="{href}" width="16" height="16"/></svg>')

    def page(self, name, body, reference=False, wait=False):
        (self.root/name).write_text('<!doctype html><html'+(' class="reftest-wait"' if wait else '')+
            '><meta charset="utf-8"><link rel="icon" href="data:,">'+
            ('<link rel="match" href="ref.html">' if not reference else '')+
            '<style>body{margin:0}svg{display:block}</style>'+body)

    def check(self, expected, kind='fixture'):
        suite=self.folder/'suite.json'
        suite.write_text(json.dumps({'viewport':[32,32],
            'cases':[{'url':'/test.html','kind':kind}]}))
        success=run(self.root,suite,self.folder/'output')
        report=json.loads((self.folder/'output/results.json').read_text())
        row=report['cases'][0]
        self.assertEqual(row['status'],expected, row)
        self.assertEqual(success, expected=='PASS' and kind=='fixture')
        return report

    def test_broken_svg_images_cannot_match_as_blank(self):
        self.page('test.html',self.svg('bad.png'))
        self.page('ref.html',self.svg('bad.png'),reference=True)
        report=self.check('ERROR')
        self.assertFalse(report['captures'])

    def test_broken_reference_is_error_not_negative_control(self):
        self.page('test.html','<svg width="16" height="16"></svg>')
        self.page('ref.html',self.svg('bad.svg','xlink:href'),reference=True)
        report=self.check('ERROR',kind='negative-control')
        self.assertNotIn('/ref.html',report['captures'])

    def test_valid_svg_image_and_legacy_href(self):
        self.page('test.html',self.svg('good.png'))
        self.page('ref.html',self.svg('good.png','xlink:href'),reference=True)
        report=self.check('PASS')
        with Image.open(self.folder/'output'/report['captures']['/test.html']['file']) as image:
            self.assertEqual(image.convert('RGB').getpixel((8,8)),(32,145,222))

    def test_decode_after_test_rendered_mutation(self):
        script='''<script>document.documentElement.addEventListener('TestRendered',()=>{
            document.querySelector('image').setAttribute('href','bad.png');
            document.documentElement.classList.remove('reftest-wait');
        });</script>'''
        self.page('test.html',self.svg('good.png')+script,wait=True)
        self.page('ref.html','<svg width="16" height="16"></svg>',reference=True)
        self.check('ERROR')

    def test_standalone_svg_document(self):
        (self.root/'standalone.svg').write_text(self.svg('good.png'))
        with Capture(self.root,32,32) as capture:
            capture.screenshot('/standalone.svg',self.folder/'standalone.png')
        with Image.open(self.folder/'standalone.png') as image:
            self.assertEqual(image.convert('RGB').getpixel((8,8)),(32,145,222))

    def test_data_svg_resource_decodes(self):
        from urllib.parse import quote
        data='data:image/svg+xml,'+quote('<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16" fill="#2091de"/></svg>')
        self.page('test.html',self.svg(data))
        self.page('ref.html',self.svg('good.png'),reference=True)
        self.check('PASS')

    def test_html_decode_failure_still_errors(self):
        self.page('test.html','<img src="bad.png">')
        self.page('ref.html','',reference=True)
        self.check('ERROR')


if __name__ == '__main__':
    unittest.main()

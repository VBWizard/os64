"""Local Chromium capture for selected static reftests, not a wptserve replacement."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import importlib.metadata
from pathlib import Path
import platform
from threading import Thread
from urllib.parse import urlsplit

from compare import Unsupported, file_for


SVG_IMAGE_ERRORS = """(() => {
    window.__os64SvgImageErrors = [];
    document.addEventListener('error', event => {
        const image = event.target;
        if (image.namespaceURI === 'http://www.w3.org/2000/svg' && image.localName === 'image')
            window.__os64SvgImageErrors.push('SVG image load failed: ' + image.href.baseVal);
    }, true);
})();"""

IMAGE_READINESS = """async () => {
    await Promise.race([(async () => {
        await document.fonts.ready;
        const htmlImages = Array.from(document.images || [], image => image.decode());
        // SVGImageElement has no decode(). Probe its resolved resource through
        // an HTML Image while retaining failures from the actual SVG element.
        const svgImages = Array.from(document.getElementsByTagNameNS(
            'http://www.w3.org/2000/svg', 'image'), async element => {
                const href = element.href.baseVal;
                if (!href) return;
                const probe = new Image();
                probe.src = new URL(href, element.baseURI).href;
                try { await probe.decode(); }
                catch (_) { throw Error('SVG image decode failed: ' + probe.src); }
            });
        await Promise.all([...htmlImages, ...svgImages]);
        await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
        if (window.__os64SvgImageErrors.length)
            throw Error(window.__os64SvgImageErrors.join('; '));
    })(), new Promise((_, reject) => setTimeout(() => reject(Error('readiness timeout')), 10000))]);
}"""


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


class Capture:
    def __init__(self, root, width=800, height=600):
        self.root, self.width, self.height = root.resolve(), width, height

    def __enter__(self):
        from playwright.sync_api import sync_playwright
        self.server = ThreadingHTTPServer(('127.0.0.1', 0),
            partial(QuietHandler, directory=str(self.root)))
        self.thread = Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.origin = f'http://127.0.0.1:{self.server.server_port}'
        try:
            self.pw = sync_playwright().start()
            self.browser = self.pw.chromium.launch(headless=True)
        except Exception:
            self.__exit__(None, None, None)
            raise
        self.provenance = {'browser': 'Chromium', 'version': self.browser.version,
            'playwright': importlib.metadata.version('playwright'),
            'platform': platform.platform(), 'viewport': [self.width, self.height],
            'deviceScaleFactor': 1, 'locale': 'en-US', 'timezone': 'UTC',
            'colorScheme': 'light', 'fonts': 'recorded per capture; fixtures contain no text'}
        return self

    def __exit__(self, *_):
        try:
            if hasattr(self, 'browser'):
                self.browser.close()
        finally:
            try:
                if hasattr(self, 'pw'):
                    self.pw.stop()
            finally:
                self.server.shutdown()
                self.server.server_close()
                self.thread.join()

    def screenshot(self, url, output):
        file_for(self.root, url)
        context = self.browser.new_context(viewport={'width': self.width, 'height': self.height},
            device_scale_factor=1, locale='en-US', timezone_id='UTC', color_scheme='light',
            service_workers='block')
        errors = []
        def route(request):
            address = request.request.url
            if address.startswith(('data:', 'blob:')):
                request.continue_()
                return
            parsed = urlsplit(address)
            if f'{parsed.scheme}://{parsed.netloc}' != self.origin:
                errors.append(f'blocked external request: {address}')
                request.abort()
                return
            if parsed.path == '/favicon.ico' and not (self.root/'favicon.ico').exists():
                request.fulfill(status=204)
                return
            try:
                file_for(self.root, parsed.path)
            except ValueError as e:
                errors.append(str(e))
                request.abort()
                return
            request.continue_()
        context.route('**/*', route)
        context.add_init_script(SVG_IMAGE_ERRORS)
        page = context.new_page()
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.on('response', lambda response: errors.append(f'HTTP {response.status}: {response.url}')
                if response.status >= 400 else None)
        page.set_default_timeout(10000)
        try:
            page.goto(self.origin + url, wait_until='load', timeout=10000)
            page.evaluate(IMAGE_READINESS)
            page.evaluate("""() => {
                if (document.documentElement.classList.contains('reftest-wait'))
                    document.documentElement.dispatchEvent(new Event('TestRendered', {bubbles:true}));
            }""")
            page.wait_for_function("!document.documentElement.classList.contains('reftest-wait')")
            # TestRendered may replace or introduce resources; validate the final
            # document as well as the initial one before accepting its pixels.
            page.evaluate(IMAGE_READINESS)
            if errors:
                raise ValueError('; '.join(errors))
            if page.evaluate("""() => (document.body?.innerText || '').trim() ||
                    Array.from(document.querySelectorAll('svg text')).some(e => e.textContent.trim())"""):
                raise Unsupported('text fixtures require a pinned font setup')
            fonts = page.evaluate('() => Array.from(document.fonts, f => ({family:f.family, status:f.status}))')
            page.screenshot(path=str(output), full_page=False, omit_background=False, timeout=10000)
            errors.extend(page.evaluate('() => window.__os64SvgImageErrors'))
            if errors:
                raise ValueError('; '.join(errors))
            return {'url': url, 'fonts': fonts}
        finally:
            context.close()

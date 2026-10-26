"""Local Chromium capture for selected static reftests, not a wptserve replacement."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import importlib.metadata
from pathlib import Path
import platform
from threading import Thread
from urllib.parse import urlsplit

from compare import Unsupported, file_for


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
        page = context.new_page()
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.on('response', lambda response: errors.append(f'HTTP {response.status}: {response.url}')
                if response.status >= 400 else None)
        page.set_default_timeout(10000)
        try:
            page.goto(self.origin + url, wait_until='load', timeout=10000)
            page.evaluate('''async () => {
                await Promise.race([(async () => {
                await document.fonts.ready;
                await Promise.all(Array.from(document.images, i => i.decode()));
                await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
                if (document.documentElement.classList.contains('reftest-wait'))
                    document.documentElement.dispatchEvent(new Event('TestRendered', {bubbles:true}));
                })(), new Promise((_, reject) => setTimeout(() => reject(Error('readiness timeout')), 10000))]);
            }''')
            page.wait_for_function("!document.documentElement.classList.contains('reftest-wait')")
            page.evaluate('() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)))')
            if errors:
                raise ValueError('; '.join(errors))
            if page.evaluate("""() => (document.body?.innerText || '').trim() ||
                    Array.from(document.querySelectorAll('svg text')).some(e => e.textContent.trim())"""):
                raise Unsupported('text fixtures require a pinned font setup')
            fonts = page.evaluate('() => Array.from(document.fonts, f => ({family:f.family, status:f.status}))')
            page.screenshot(path=str(output), full_page=False, omit_background=False, timeout=10000)
            if errors:
                raise ValueError('; '.join(errors))
            return {'url': url, 'fonts': fonts}
        finally:
            context.close()

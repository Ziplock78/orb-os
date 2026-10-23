"""Local browser checks using a simulated serial loader; never opens a real port."""
import functools
import http.server
import json
import threading
from pathlib import Path
from playwright.sync_api import sync_playwright
from package import ROOT, WORK


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args):
        pass


def main():
    handler = functools.partial(QuietHandler, directory=str(WORK / 'site'))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f'http://127.0.0.1:{server.server_port}/'
    table = list((ROOT / '.pio/build/esp32-s3-amoled-175-companion/partitions.bin').read_bytes().ljust(4096, b'\xff'))
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(channel='msedge', headless=True)
        page = browser.new_page(viewport={'width': 1200, 'height': 1100})
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url)
        page.get_by_role('button', name='Connect device', exact=True).wait_for()
        page.wait_for_function("!document.getElementById('connect').disabled")
        assert page.locator('#release option').inner_text() == 'v2.16.26-companion · Latest'
        page.screenshot(path=str(WORK / 'installer-desktop.png'), full_page=True)
        # Load the real pinned dependency independently of the simulated device.
        exports = page.evaluate("async () => Object.keys(await import('https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/bundle.js'))")
        assert 'ESPLoader' in exports and 'Transport' in exports
        assert page.evaluate("typeof SparkMD5.ArrayBuffer.hash") == 'function'
        page.close()

        context = browser.new_context(viewport={'width': 1200, 'height': 1100})
        context.add_init_script('window.__writes = []; window.__table = ' + json.dumps(table) + ';' + '''
          Object.defineProperty(navigator, 'serial', {value: {requestPort: async () => ({})}});
        ''')
        mock = '''
          export class Transport { async disconnect() {} }
          export class ESPLoader {
            constructor() { this.chip = {CHIP_NAME: 'ESP32-S3'}; }
            async main() {} async after() {} async detectFlashSize() { return '16MB'; }
            async readFlash(address, size) { return new Uint8Array(window.__table).slice(0, size); }
            async flashMd5sum(address, size) {
              return SparkMD5.ArrayBuffer.hash(new Uint8Array(window.__table).slice(0, size).buffer);
            }
            async writeFlash(options) {
              if (options.eraseAll) throw new Error('Full erase requested');
              window.__writes = options.fileArray.map(p => ({address:p.address,size:p.data.length}));
              options.fileArray.forEach((p,i) => options.reportProgress(i,p.data.length,p.data.length));
            }
          }
        '''
        context.route('**/esptool-js@0.7.0/bundle.js', lambda route: route.fulfill(body=mock, content_type='text/javascript'))
        page = context.new_page()
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url)
        page.get_by_role('button', name='Connect device', exact=True).click()
        page.get_by_role('button', name='Update both firmwares', exact=True).wait_for()
        page.get_by_role('button', name='Update both firmwares', exact=True).click()
        page.wait_for_function("document.getElementById('status').textContent.startsWith('Installation complete')")
        writes = page.evaluate('window.__writes')
        assert [part['address'] for part in writes] == [0x10000, 0xAD0000], writes
        print('Browser: release selection, dependency imports, device inspection and verified app-only update passed')

        # Failed asset integrity must cause zero device writes.
        page.evaluate('window.__writes = []')
        page.route('**/Orb-companion.bin', lambda route: route.fulfill(body=b'corrupt'))
        page.get_by_role('button', name='Connect device', exact=True).click()
        page.get_by_role('button', name='Update both firmwares', exact=True).click()
        page.wait_for_function("document.getElementById('status').textContent.includes('checksum mismatch')")
        assert page.evaluate('window.__writes') == []
        print('Browser: corrupt download rejected before writing')

        page.unroute('**/Orb-companion.bin')
        page.evaluate('window.__table[8] ^= 1')
        page.get_by_role('button', name='Connect device', exact=True).click()
        page.wait_for_function("document.getElementById('status').textContent.includes('another firmware layout')")
        assert page.locator('#ready').is_hidden()
        assert page.evaluate('window.__writes') == []
        print('Browser: unknown device layout rejected before writing')

        mobile = context.new_page()
        mobile.set_viewport_size({'width': 390, 'height': 844})
        mobile.goto(url)
        mobile.wait_for_function("!document.getElementById('release').disabled")
        assert mobile.evaluate('document.documentElement.scrollWidth <= innerWidth')
        mobile.screenshot(path=str(WORK / 'installer-mobile.png'), full_page=True)
        assert not errors, errors
        print('Browser: mobile layout fits; no JavaScript errors')
        browser.close()
    server.shutdown()


if __name__ == '__main__':
    main()

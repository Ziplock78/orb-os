"""Exercise the installer log in Edge with simulated messages and no serial device.

Requires the Python playwright package and Microsoft Edge.
"""
from pathlib import Path
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[2] / 'web/companion'
MOCK_LOADER = '''
export class Transport {}
export class ESPLoader {
  constructor(options) { window.testTerminal = options.terminal; }
  async main() { await new Promise(() => {}); }
}
'''


def main():
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(channel='msedge', headless=True)
        page = browser.new_page(viewport={'width': 1200, 'height': 1000})
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))

        def serve(route):
            url = route.request.url
            if 'esptool-js' in url:
                route.fulfill(body=MOCK_LOADER, content_type='text/javascript')
            elif 'spark-md5' in url:
                route.fulfill(body='window.SparkMD5 = {};', content_type='text/javascript')
            elif url.endswith('release-index.json'):
                route.fulfill(body='[{"tag":"test","manifest":"unused.json"}]',
                              content_type='application/json')
            elif url.startswith('https://installer.test/'):
                name = url.split('/')[-1] or 'index.html'
                mime = 'text/javascript' if name.endswith('.mjs') else (
                    'text/css' if name.endswith('.css') else 'text/html')
                route.fulfill(body=(ROOT / name).read_bytes(), content_type=mime)
            else:
                route.abort()

        def emit(batch):
            page.evaluate('''batch => {
              for (let i = 0; i < 100; i++) testTerminal.writeLine(
                `Batch ${batch} message ${i}: ` + 'reading flash and checking connection '.repeat(4));
            }''', batch)

        def assert_tail():
            page.wait_for_function('''() => {
              const log = document.getElementById('log');
              return log.scrollHeight > log.clientHeight &&
                Math.abs(log.scrollHeight - log.clientHeight - log.scrollTop) <= 1;
            }''', timeout=3000)

        page.route('**/*', serve)
        page.add_init_script("Object.defineProperty(navigator, 'serial', {value:{requestPort:async()=>({})}})")
        page.goto('https://installer.test/')
        page.get_by_role('button', name='Connect device', exact=True).click()
        page.wait_for_function('window.testTerminal')
        emit('closed')
        summary = page.get_by_text('Connection details', exact=True)
        summary.click()
        assert_tail()
        for batch in range(3):
            emit(batch)
            assert_tail()
        assert len(page.locator('#log').text_content()) == 14000
        # Narrowing the page reflows existing lines without a new log message.
        page.set_viewport_size({'width': 390, 'height': 844})
        assert_tail()
        summary.click()
        emit('closed again')
        summary.click()
        assert_tail()
        # A new message resumes following even after manually reading older output.
        page.locator('#log').evaluate('(log) => { log.scrollTop = 0; }')
        emit('latest')
        assert_tail()
        assert not errors, errors
        browser.close()
    print('Connection log: streaming, truncation, reflow, reopening and resuming passed')


if __name__ == '__main__':
    main()

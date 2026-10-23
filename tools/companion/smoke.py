"""Check existing Orb configuration and theme transports on a live device.

Writes and removes a uniquely named scratch folder; never activates a theme.
Run before and after visiting PrintSphere and compare the saved state.
"""
import argparse
import base64
import hashlib
import json
import time
import urllib.request
import uuid
from pathlib import Path
from probe import Orb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    parser.add_argument('--save', type=Path, required=True)
    parser.add_argument('--compare', type=Path)
    args = parser.parse_args()
    orb = Orb(args.port)
    folder = 'probe-' + uuid.uuid4().hex[:10]
    created = False
    try:
        hello = orb.command('hello')
        wifi = orb.command('wifi')
        themes = orb.command('themes')
        state = {key: hello[key] for key in ('slug', 'assets', 'weld', 'owner')}
        state['themes'] = themes
        state['ssid'] = wifi['ssid']
        base = 'http://' + wifi['ip']
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        for route in ('/', '/legacy', '/install', '/themes.json'):
            response = opener.open(base + route, timeout=20)
            body = response.read()
            assert response.status == 200 and body, route
            if route == '/legacy':
                state['configuration_sha256'] = hashlib.sha256(body).hexdigest()
                args.save.with_suffix('.config.html').write_bytes(body)
            print(f'GET {route}: OK ({len(body)} bytes)', flush=True)
        payload = b'Orb companion theme transport verification\n'
        created = True
        orb.command(f'put-begin {folder} usb.txt {len(payload)}')
        orb.command('put-data ' + base64.b64encode(payload).decode())
        orb.command('put-end')
        for filename in ('usb.txt', 'web.txt'):
            if filename == 'web.txt':
                boundary = 'orbcompanionprobe'
                body = (f'--{boundary}\r\nContent-Disposition: form-data; name="f"; filename="web.txt"\r\n'
                        'Content-Type: application/octet-stream\r\n\r\n').encode() + payload + f'\r\n--{boundary}--\r\n'.encode()
                request = urllib.request.Request(base + f'/sdput?path=/themes/{folder}/web.txt', data=body,
                        headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
                response = opener.open(request, timeout=20)
                assert response.status == 200, response.read()
                response.read()
            reply = orb.command(f'get-begin {folder} {filename}')
            assert reply['size'] == len(payload)
            actual = b''
            while True:
                reply = orb.command('get-data')
                if reply.get('done'):
                    break
                actual += base64.b64decode(reply['b64'])
            assert actual == payload, filename
            print(f'{filename}: upload and exact readback OK', flush=True)
        orb.command('delete ' + folder)
        created = False
        assert orb.command('hello')['assets'] == hello['assets']
        args.save.write_text(json.dumps(state, indent=2))
        if args.compare:
            assert state == json.loads(args.compare.read_text()), 'Settings/theme state changed'
            print('Configuration and theme state match the pre-switch snapshot', flush=True)
    finally:
        if created:
            try:
                orb.command('delete ' + folder)
            except Exception as error:
                print('Scratch cleanup failed:', error)
        orb.close()


if __name__ == '__main__':
    main()

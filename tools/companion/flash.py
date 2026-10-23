"""Install, update, or restore the local companion test device over USB."""
import argparse
import hashlib
import subprocess
import sys
from pathlib import Path
from package import ROOT, WORK, EXPECTED, partitions, require, check_app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['install', 'restore', 'update-orb', 'update-printer'])
    parser.add_argument('--port', required=True)
    parser.add_argument('--esptool', type=Path, default=Path.home() / '.platformio/packages/tool-esptoolpy/esptool.py')
    args = parser.parse_args()
    base = [sys.executable, str(args.esptool), '--chip', 'esp32s3', '--port', args.port, '--baud', '921600']
    if args.action in ('install', 'restore'):
        path = WORK / ('orb-printsphere-device.bin' if args.action == 'install' else 'orb-before-dualboot.bin')
        data = path.read_bytes()
        require(len(data) == 0x1000000, 'Expected full flash image')
        require(hashlib.sha256(data).hexdigest() == path.with_suffix('.sha256').read_text().strip(),
                'Image checksum mismatch')
        if args.action == 'install':
            require(partitions(data[0x8000:0x9000]) == EXPECTED, 'Wrong installation layout')
        address = 0
    else:
        slot = 'ota_0' if args.action == 'update-orb' else 'ota_1'
        path = (ROOT / '.pio/build/esp32-s3-amoled-175-companion/firmware.bin' if slot == 'ota_0'
                else WORK / 'ps-build/printsphere_idf.bin')
        check_app(path.read_bytes(), slot)
        address = EXPECTED[slot][2]
    # Read before writing. An ordinary standalone updater changes the table, so
    # app-only updates must never proceed on a device with that different layout.
    live_table = WORK / 'device-partitions.bin'
    subprocess.run(base + ['read_flash', '0x8000', '0x1000', str(live_table)], check=True)
    actual = partitions(live_table.read_bytes())
    if args.action == 'install':
        backup = (WORK / 'orb-before-dualboot.bin').read_bytes()
        require(actual == partitions(backup[0x8000:0x9000]), 'Device layout changed since backup')
    elif args.action != 'restore':
        require(actual == EXPECTED, 'Device is not using the companion partition table')
    subprocess.run(base + ['write_flash', hex(address), str(path)], check=True)


if __name__ == '__main__':
    main()

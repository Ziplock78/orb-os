"""Collect public release assets; never include device backups or settings."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path
from package import ROOT, WORK, EXPECTED, partitions, require, check_app

FILES = {
    'orb-bootloader.bin': (WORK / 'ps-build/bootloader/bootloader.bin', 0, 0x8000),
    'orb-partitions.bin': (ROOT / '.pio/build/esp32-s3-amoled-175-companion/partitions.bin', 0x8000, 0x1000),
    'Orb-companion.bin': (ROOT / '.pio/build/esp32-s3-amoled-175-companion/firmware.bin', 0x10000, 0x640000),
    'PrintSphere-companion.bin': (WORK / 'ps-build/printsphere_idf.bin', 0xAD0000, 0x400000),
}


def collect(output, version, commit, printer_commit):
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for name, (source, offset, capacity) in FILES.items():
        data = source.read_bytes()
        require(0 < len(data) <= capacity, f'Invalid size: {name}')
        if name == 'orb-partitions.bin':
            require(partitions(data) == EXPECTED, 'Invalid partition table')
            require(partitions((WORK / 'ps-build/partition_table/partition-table.bin').read_bytes()) == EXPECTED,
                    'PrintSphere layout differs')
        elif name == 'Orb-companion.bin':
            check_app(data, 'ota_0')
        elif name == 'PrintSphere-companion.bin':
            check_app(data, 'ota_1')
        else:
            require(data[0] == 0xE9, 'Invalid bootloader')
        shutil.copyfile(source, output / name)
        records.append({'path': name, 'offset': offset, 'size': len(data),
                        'sha256': hashlib.sha256(data).hexdigest()})
    manifest = {'schema': 1, 'layout': 'orb-printsphere-v1', 'version': version,
                'chip': 'ESP32-S3', 'flashSize': 0x1000000, 'commit': commit,
                'printSphereCommit': printer_commit, 'parts': records}
    (output / 'companion-release.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Collected four public firmware images and manifest in {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--printer-commit', required=True)
    args = parser.parse_args()
    collect(args.output, args.version, args.commit, args.printer_commit)

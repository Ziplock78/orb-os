"""Build a device-specific migration image, retaining Orb NVS and cached theme.

The output contains private device settings. Keep it in ignored .pio/companion.
"""
import argparse
import hashlib
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / '.pio/companion'
EXPECTED = {
    'nvs': (1, 2, 0x9000, 0x5000),
    'otadata': (1, 0, 0xE000, 0x2000),
    'ota_0': (0, 16, 0x10000, 0x640000),
    'themeart': (1, 64, 0x650000, 0x480000),
    'ota_1': (0, 17, 0xAD0000, 0x400000),
    'ps_nvs': (1, 2, 0xED0000, 0x80000),
    'sounds': (1, 131, 0xF50000, 0xA0000),
    'coredump': (1, 3, 0xFF0000, 0x10000),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def partitions(data):
    result = {}
    for position in range(0, len(data), 32):
        record = data[position:position + 32]
        if record[:2] != b'\xaa\x50':
            break
        _, kind, subtype, offset, size, label, flags = struct.unpack('<HBBII16sI', record)
        name = label.rstrip(b'\0').decode('ascii')
        require(name not in result and flags == 0, 'Unexpected partition entry')
        result[name] = kind, subtype, offset, size
    return result


def check_app(data, slot):
    require(len(data) <= EXPECTED[slot][3], f'{slot} image exceeds partition')
    require(len(data) > 256 and data[0] == 0xE9, 'Invalid ESP application image')
    require(struct.unpack_from('<I', data, 32)[0] == 0xABCD5432, 'Missing app descriptor')
    project = data[80:112].split(b'\0')[0].decode('ascii')
    version = data[48:80].split(b'\0')[0].decode('ascii')
    if slot == 'ota_1':
        require(project == 'printsphere_idf' and version.endswith(('-orbtest', '-orb')),
                'Only the adapted PrintSphere build is supported')
        require(b'ps_nvs\0' in data, 'PrintSphere private settings partition missing')
    else:
        require(b'Opening PrintSphere...' in data, 'Orb companion launcher missing')
    return project, version


def check_cache(backup):
    start, capacity = 0x650000, EXPECTED['themeart'][3]
    magic, version, count, used = struct.unpack_from('<4I', backup, start)
    if magic == 0xFFFFFFFF:
        return
    require(magic == 0x4F524254 and version == 6, 'Unknown theme cache format; migration stopped')
    require(count <= (8192 - 16) // 64 and used + 8192 <= capacity,
            'Current theme cache does not fit; migration stopped without changing device')
    for index in range(count):
        entry = struct.unpack_from('<20s24sIIHHB3sI', backup, start + 16 + 64 * index)
        offset, length = entry[2:4]
        require(offset >= 8192 and offset + length <= capacity,
                'Theme cache entry would be truncated')


def assemble(backup, boot, table, orb, printer):
    require(len(backup) == 0x1000000, 'A complete 16 MiB backup is required')
    old = partitions(backup[0x8000:0x9000])
    require(old.get('nvs') == EXPECTED['nvs'], 'Unexpected original settings layout')
    require(old.get('app0') == (0, 0, 0x10000, 0x640000), 'Unexpected original Orb slot')
    require(old.get('themeart') == (1, 64, 0x650000, 0x9A0000),
            'Migration requires the original standalone Orb layout')
    require(partitions(table) == EXPECTED, 'Unexpected companion partition table')
    require(len(table) <= 0x1000 and 0 < len(boot) <= 0x8000 and boot[0] == 0xE9,
            'Invalid bootloader or partition table size')
    check_app(orb, 'ota_0')
    check_app(printer, 'ota_1')
    check_cache(backup)
    output = bytearray(b'\xff' * 0x1000000)
    for begin, end in ((0x9000, 0xE000), (0x650000, 0xAD0000)):
        output[begin:end] = backup[begin:end]
    for begin, content in ((0, boot), (0x8000, table), (0x10000, orb), (0xAD0000, printer)):
        output[begin:begin + len(content)] = content
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', type=Path, default=WORK / 'orb-before-dualboot.bin')
    args = parser.parse_args()
    backup = args.backup.read_bytes()
    require(hashlib.sha256(backup).hexdigest() == args.backup.with_suffix('.sha256').read_text().strip(),
            'Backup checksum mismatch')
    orb_dir = ROOT / '.pio/build/esp32-s3-amoled-175-companion'
    ps_dir = WORK / 'ps-build'
    table = (orb_dir / 'partitions.bin').read_bytes()
    require(partitions(table) == partitions((ps_dir / 'partition_table/partition-table.bin').read_bytes()),
            'The firmware builds disagree on partitions')
    output = assemble(backup, (ps_dir / 'bootloader/bootloader.bin').read_bytes(), table,
                      (orb_dir / 'firmware.bin').read_bytes(), (ps_dir / 'printsphere_idf.bin').read_bytes())
    path = WORK / 'orb-printsphere-device.bin'
    path.write_bytes(output)
    path.with_suffix('.sha256').write_text(hashlib.sha256(output).hexdigest() + '\n')
    print(f'Validated migration image: {path}\nOrb settings and cached theme preserved byte-for-byte.')


if __name__ == '__main__':
    main()

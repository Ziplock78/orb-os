"""Migration regression checks against the local backup and built images."""
import unittest
from package import WORK, ROOT, assemble, check_app, check_cache


@unittest.skipUnless((WORK / 'orb-before-dualboot.bin').exists(), 'Needs local device backup')
class MigrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.backup = (WORK / 'orb-before-dualboot.bin').read_bytes()
        cls.boot = (WORK / 'ps-build/bootloader/bootloader.bin').read_bytes()
        cls.table = (ROOT / '.pio/build/esp32-s3-amoled-175-companion/partitions.bin').read_bytes()
        cls.orb = (ROOT / '.pio/build/esp32-s3-amoled-175-companion/firmware.bin').read_bytes()
        cls.printer = (WORK / 'ps-build/printsphere_idf.bin').read_bytes()

    def test_preserves_settings_and_theme_and_initializes_private_storage(self):
        result = assemble(self.backup, self.boot, self.table, self.orb, self.printer)
        self.assertEqual(result[0x9000:0xE000], self.backup[0x9000:0xE000])
        self.assertEqual(result[0x650000:0xAD0000], self.backup[0x650000:0xAD0000])
        self.assertEqual(result[0xED0000:0xFF0000], b'\xff' * 0x120000)
        self.assertEqual(result[0xE000:0x10000], b'\xff' * 8192)

    def test_rejects_truncated_backup(self):
        with self.assertRaises(ValueError):
            assemble(self.backup[:-1], self.boot, self.table, self.orb, self.printer)

    def test_rejects_old_partition_table(self):
        with self.assertRaises(ValueError):
            assemble(self.backup, self.boot, self.backup[0x8000:0x9000], self.orb, self.printer)

    def test_rejects_oversize_and_wrong_firmware(self):
        for image in (self.printer + b'\xff' * 0x400000, self.orb):
            with self.assertRaises(ValueError):
                check_app(image, 'ota_1')

    def test_rejects_cache_that_would_be_truncated(self):
        import struct
        backup = bytearray(self.backup)
        struct.pack_into('<I', backup, 0x650000 + 12, 0x480000)
        with self.assertRaises(ValueError):
            check_cache(backup)


if __name__ == '__main__':
    unittest.main()

"""Prepare an isolated PrintSphere build; never modify the source checkout."""
import argparse
import shutil
from pathlib import Path
from adapt_dial import adapt as adapt_dial

ROOT = Path(__file__).resolve().parents[2]


def replace(path, old, new):
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise RuntimeError(f"Expected one adaptation point in {path.name}: {old[:70]}")
    path.write_text(text.replace(old, new), encoding="utf-8")


def adapt_installer_links(destination):
    portal = destination / 'main/src/setup_portal.cpp'
    text = portal.read_text(encoding='utf-8')
    text = text.replace('Use the Capsule Companion web installer to update or repair all firmware slots at once.',
                        'Use the Orb Companion web installer to update this dual-boot device.')
    text = text.replace('Capsule Companion web installer', 'Orb Companion web installer')
    text = text.replace('https://lerxtwood.github.io/capsule-radar/printsphere-manifest.json', '')
    text = text.replace('https://lerxtwood.github.io/capsule-radar/', 'https://lerxtwood.github.io/orb-os/')
    portal.write_text(text, encoding='utf-8')


def prepare(source, destination):
    source = source.resolve()
    destination = destination.resolve()
    if not destination.is_relative_to(ROOT / ".pio" / "companion"):
        raise ValueError("Build copy must be inside .pio/companion")
    shutil.copytree(source, destination, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(".git", "build", "release", ".github", "Case", "flash", "sdkconfig", "sdkconfig.old"))
    shutil.copy2(ROOT / "partitions_16MB_companion.csv", destination / "partitions.csv")
    config = destination / "main/src/config_store.cpp"
    replace(config, 'constexpr char kNamespace[] = "printsphere";',
            'constexpr char kNamespace[] = "printsphere";\nconstexpr char kNvsPartition[] = "ps_nvs";')
    replace(config, 'std::string load_nvs_string(const char* ns, const char* key) {',
            'std::string load_nvs_string(const char* ns, const char* key, const char* partition = kNvsPartition) {')
    replace(config, 'nvs_open(ns, NVS_READONLY, &handle)',
            'nvs_open_from_partition(partition, ns, NVS_READONLY, &handle)')
    replace(config, 'nvs_open(kNamespace, NVS_READWRITE, &handle)',
            'nvs_open_from_partition(kNvsPartition, kNamespace, NVS_READWRITE, &handle)')
    replace(config, '''  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }''', '''  // Orb owns default NVS. Never erase it as part of PrintSphere recovery.
  ESP_RETURN_ON_ERROR(nvs_flash_init(), kTag, "Orb NVS initialization failed");
  esp_err_t err = nvs_flash_init_partition(kNvsPartition);
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase_partition(kNvsPartition));
    err = nvs_flash_init_partition(kNvsPartition);
  }''')
    replace(config, 'load_nvs_string(kSharedNamespace, "wifi_ssid")',
            'load_nvs_string("capsuleradar", "wifiBakSsid", "nvs")')
    replace(config, 'load_nvs_string(kSharedNamespace, "wifi_pass")',
            'load_nvs_string("capsuleradar", "wifiBakPass", "nvs")')
    replace(config, 'Imported shared Wi-Fi credentials from Capsule Radar (ssid=%s)',
            'Imported Orb Wi-Fi credentials (ssid=%s)')
    replace(destination / "main/src/ui.cpp",
            'set_label_text_if_changed(radar_button_label_, "Radar");',
            'set_label_text_if_changed(radar_button_label_, "Orb");')
    adapt_installer_links(destination)
    adapt_dial(destination)
    # Existing PrintSphere firmware writes to ota_1 explicitly, and its WiFi driver
    # already uses RAM storage. Assert both protections survive source updates.
    wifi = (destination / "main/src/wifi_manager.cpp").read_text(encoding="utf-8")
    assert "esp_wifi_set_storage(WIFI_STORAGE_RAM)" in wifi
    assert "nvs_flash_erase()" not in config.read_text(encoding="utf-8")
    print(f"Prepared isolated AMOLED PrintSphere source: {destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=ROOT.parent / "PrintSphere")
    parser.add_argument("--destination", type=Path, default=ROOT / ".pio/companion/PrintSphere")
    args = parser.parse_args()
    prepare(args.source, args.destination)

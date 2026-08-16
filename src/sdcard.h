#pragma once
#include <stdint.h>

// microSD (TF) slot. Verified SPI-mode pins from the official Waveshare
// ESP32-S3-Touch-AMOLED-1.75 schematic (SD-CARD block): GPIO1=MOSI, GPIO2=SCK,
// GPIO3=MISO, GPIO41=CS. Do NOT guess these — see docs/HARDWARE.md.
namespace sdcard {
    bool begin();          // mounts at /sd; false if no card is present
    bool mounted();
    uint64_t sizeBytes();
}

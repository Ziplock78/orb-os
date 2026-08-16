#include "sdcard.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// The Waveshare schematic's SD-CARD block labels the socket pins CMD/CLK/D0/D3 (generic
// microSD naming) but wires only the four SPI-mode lines to the ESP32: CMD->MOSI,
// CLK->SCK, D0->MISO, D3->CS (D1/D2/card-detect are not connected). So this is a plain
// SPI card, not SD_MMC, even though the silkscreen uses SD_MMC-style pin names.
static constexpr int SD_PIN_MOSI = 1;
static constexpr int SD_PIN_SCK  = 2;
static constexpr int SD_PIN_MISO = 3;
static constexpr int SD_PIN_CS   = 41;

static SPIClass  s_sdSpi(HSPI);
static bool      s_mounted   = false;
static uint64_t  s_sizeBytes = 0;

// The Arduino SD library defaults to a conservative 4 MHz SPI clock. That's the main
// cause of choppy Spy Cam playback (each frame is a fresh open/read/close of a JPEG
// file) — 20 MHz is a solid 5x speedup and still well inside what any real SD card
// handles reliably in SPI mode. Push higher only after confirming no read errors.
static constexpr uint32_t SD_SPI_HZ = 20000000;

bool sdcard::begin() {
    s_sdSpi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    if (!SD.begin(SD_PIN_CS, s_sdSpi, SD_SPI_HZ) || SD.cardType() == CARD_NONE) {
        Serial.println("[sd] no card detected");
        s_mounted = false;
        return false;
    }
    s_sizeBytes = SD.cardSize();
    const uint8_t type = SD.cardType();
    const char *typeName = type == CARD_MMC ? "MMC" : type == CARD_SD ? "SDSC" :
                            type == CARD_SDHC ? "SDHC" : "UNKNOWN";
    Serial.printf("[sd] %s card mounted, %.2f GB\n", typeName,
                  s_sizeBytes / (1024.0 * 1024.0 * 1024.0));
    s_mounted = true;
    return true;
}

bool sdcard::mounted() { return s_mounted; }
uint64_t sdcard::sizeBytes() { return s_sizeBytes; }

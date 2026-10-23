#include "config.h"
#if ORB_COMPANION
#include "companion.h"
#include "app_shell.h"
#include "orb_link.h"
#include "theme_pull.h"
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <lvgl.h>
#include <cstring>

namespace {
lv_obj_t *message = nullptr;
bool pending = false;
uint32_t requestedAt = 0;

void enter() {
    lv_label_set_text(message, "Printer\n\nPress to open PrintSphere\nTurn to go back");
}

void launch() {
    if (orb_link::transferActive() || theme_pull::active()) {
        lv_label_set_text(message, "Theme upload in progress\nTry again when it finishes");
        return;
    }
    lv_label_set_text(message, "Opening PrintSphere...");
    requestedAt = millis();
    pending = true;
}
}

void companion::registerPrinter() {
    lv_obj_t *screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    message = lv_label_create(screen);
    lv_obj_set_width(message, 320);
    lv_obj_set_style_text_font(message, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(message, lv_color_white(), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(message);
    enter();
    app_shell::add(screen, "Printer", launch, nullptr, false, enter);
}

void companion::poll() {
    if (!pending || millis() - requestedAt < 600) return;
    pending = false;
    if (orb_link::transferActive() || theme_pull::active()) {
        lv_label_set_text(message, "Theme upload in progress\nTry again when it finishes");
        return;
    }
    const esp_partition_t *target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    esp_app_desc_t description{};
    if (!target || esp_ota_get_partition_description(target, &description) != ESP_OK ||
        strcmp(description.project_name, "printsphere_idf") != 0) {
        lv_label_set_text(message, "PrintSphere is not installed\nUse the companion installer");
        return;
    }
    const esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        lv_label_set_text(message, "Could not start PrintSphere\nCheck its firmware image");
        Serial.printf("[companion] boot selection failed: %s\n", esp_err_to_name(err));
        return;
    }
    Serial.printf("[companion] starting PrintSphere %s at 0x%lx\n",
                  description.version, (unsigned long)target->address);
    Serial.flush();
    ESP.restart();
}
#endif

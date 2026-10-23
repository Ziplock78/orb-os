#include "printsphere/dial.hpp"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

#if !defined(PRINTSPHERE_HW_VARIANT_AMOLED_1_75)
#error "Orb companion dial wiring is only valid on the AMOLED 1.75 board"
#endif

namespace printsphere::dial {
namespace {
constexpr gpio_num_t kA = GPIO_NUM_18, kB = GPIO_NUM_17, kButton = GPIO_NUM_16;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Decoder decoder;

void IRAM_ATTR turn_isr(void*) {
  const uint8_t ab = (gpio_ll_get_level(&GPIO, kA) << 1) | gpio_ll_get_level(&GPIO, kB);
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  portENTER_CRITICAL_ISR(&mux);
  decoder.edge(ab, now);
  portEXIT_CRITICAL_ISR(&mux);
}
void IRAM_ATTR button_isr(void*) {
  const bool high = gpio_ll_get_level(&GPIO, kButton) != 0;
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  portENTER_CRITICAL_ISR(&mux);
  decoder.button(high, now);
  portEXIT_CRITICAL_ISR(&mux);
}
}

esp_err_t initialize() {
  gpio_config_t config{};
  config.pin_bit_mask = (1ULL << kA) | (1ULL << kB) | (1ULL << kButton);
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_ANYEDGE;
  ESP_RETURN_ON_ERROR(gpio_config(&config), "dial", "GPIO configuration failed");
  decoder.seed((gpio_get_level(kA) << 1) | gpio_get_level(kB), gpio_get_level(kButton),
               static_cast<uint32_t>(esp_timer_get_time() / 1000));
  const esp_err_t service = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (service != ESP_OK && service != ESP_ERR_INVALID_STATE) return service;
  ESP_RETURN_ON_ERROR(gpio_isr_handler_add(kA, turn_isr, nullptr), "dial", "A interrupt failed");
  ESP_RETURN_ON_ERROR(gpio_isr_handler_add(kB, turn_isr, nullptr), "dial", "B interrupt failed");
  ESP_RETURN_ON_ERROR(gpio_isr_handler_add(kButton, button_isr, nullptr), "dial", "Button interrupt failed");
  ESP_LOGI("dial", "Ready: GPIO18 A / GPIO17 B / GPIO16 button; Orb jig timing");
  return ESP_OK;
}

Snapshot snapshot() {
  portENTER_CRITICAL(&mux);
  const Snapshot result = decoder.snapshot();
  portEXIT_CRITICAL(&mux);
  return result;
}
}  // namespace printsphere::dial

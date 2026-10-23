"""Apply Orb dial navigation to the isolated PrintSphere companion checkout."""
import re
import shutil
from pathlib import Path

ASSETS = Path(__file__).resolve().parent / 'printsphere'


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Expected one dial adaptation point: {old[:80]}')
    return text.replace(old, new)


def adapt(destination):
    main = destination / 'main'
    for name in ('dial.hpp', 'dial_logic.hpp'):
        shutil.copyfile(ASSETS / name, main / 'include/printsphere' / name)
    for name in ('dial.cpp', 'dial_ui.inc'):
        shutil.copyfile(ASSETS / name, main / 'src' / name)
    cmake = main / 'CMakeLists.txt'
    cmake.write_text(once(cmake.read_text(), '        "src/ui.cpp"',
                         '        "src/ui.cpp"\n        "src/dial.cpp"'))
    header = main / 'include/printsphere/ui.hpp'
    text = header.read_text(encoding='utf-8')
    text = once(text, '#include "printsphere/printer_state.hpp"',
                '#include "printsphere/printer_state.hpp"\n#include "printsphere/dial.hpp"')
    text = once(text, '  static void radar_button_event_cb(lv_event_t* event);', '''  esp_err_t initialize_dial();
  static void dial_timer_cb(lv_timer_t* timer);
  void poll_dial();''')
    text = once(text, '  lv_obj_t* radar_button_ = nullptr;\n  lv_obj_t* radar_button_label_ = nullptr;', '''  lv_obj_t* dial_menu_ = nullptr;
  lv_obj_t* dial_menu_label_ = nullptr;
  dial::Router dial_router_{};
  dial::Snapshot dial_wake_seen_{};  // accessed only by update_power_save''')
    header.write_text(text, encoding='utf-8')
    path = main / 'src/ui.cpp'
    text = path.read_text(encoding='utf-8')
    start = text.index('  radar_button_ = lv_obj_create(lv_layer_top());')
    end = text.index('  badge_slot_ = lv_obj_create(page1_);', start)
    text = text[:start] + text[end:]
    start = text.index('void Ui::radar_button_event_cb(')
    end = text.index('void Ui::pause_button_event_cb(', start)
    text = text[:start] + text[end:]
    # Remove both touch pull-down escape paths. Touch cannot reboot the device.
    text, count = re.subn(r'    // Combined-firmware escape hatch:.*?\n    }\n', '', text, flags=re.DOTALL)
    if count != 2:
        raise ValueError('Expected two old touch escape paths')
    text = once(text, '  lv_obj_set_scroll_dir(pager_, LV_DIR_HOR);',
                '  lv_obj_set_scroll_dir(pager_, LV_DIR_NONE);\n  lv_obj_clear_flag(pager_, LV_OBJ_FLAG_SCROLLABLE);')
    # scroll_to_view skips a non-scrollable pager. Explicit coordinates work for
    # startup and later page availability changes as well as dial navigation.
    text = once(text, '  lv_obj_scroll_to_view(page1_, LV_ANIM_OFF);',
                '  lv_obj_scroll_to_x(pager_, lv_obj_get_x(page1_), LV_ANIM_OFF);')
    text = once(text, '    lv_obj_scroll_to_view(target_page, LV_ANIM_OFF);',
                '    lv_obj_scroll_to_x(pager_, lv_obj_get_x(target_page), LV_ANIM_OFF);')
    text = once(text, '  lv_obj_set_scroll_dir(pager_, locked ? LV_DIR_NONE : LV_DIR_HOR);',
                '  lv_obj_set_scroll_dir(pager_, LV_DIR_NONE);  // Pages are controlled by the dial.')
    text = once(text, '''  if (screen_power_mode_ == ScreenPowerMode::kOff &&
      gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {''', '''  const auto dial_input = dial::snapshot();
  const bool dial_activity = dial_input.position != dial_wake_seen_.position ||
      dial_input.press_sequence != dial_wake_seen_.press_sequence ||
      dial_input.rock_sequence != dial_wake_seen_.rock_sequence;
  dial_wake_seen_ = dial_input;
  if (screen_power_mode_ == ScreenPowerMode::kOff &&
      (gpio_get_level(BSP_LCD_TOUCH_INT) == 0 || dial_activity)) {''')
    text = once(text, '  initialized_ = true;\n  ESP_LOGI(kTag, "UI ready',
                '  ESP_RETURN_ON_ERROR(initialize_dial(), kTag, "dial UI failed");\n  initialized_ = true;\n  ESP_LOGI(kTag, "UI ready')
    text = once(text, '}  // namespace printsphere', '#include "dial_ui.inc"\n\n}  // namespace printsphere')
    if 'radar_button_' in text or 'return_to_radar_requested_.store(true)' in text:
        raise ValueError('A legacy touch-return path remains')
    path.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('destination', type=Path)
    adapt(parser.parse_args().destination)

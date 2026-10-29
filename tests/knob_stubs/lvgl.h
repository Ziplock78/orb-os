#pragma once
#include <stdint.h>
struct lv_obj_t;
constexpr int LV_OBJ_FLAG_HIDDEN = 1;
inline lv_obj_t *lv_scr_act() { return nullptr; }
inline bool lv_obj_has_flag(lv_obj_t *, int) { return false; }
uint32_t lv_tick_get();

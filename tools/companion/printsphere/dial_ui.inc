// Included inside namespace printsphere by the companion source adapter.
esp_err_t Ui::initialize_dial() {
  ESP_RETURN_ON_ERROR(dial::initialize(), kTag, "dial initialization failed");
  {
    LvglLockGuard lock(1000, "initialize_dial");
    if (!lock.locked()) return ESP_ERR_TIMEOUT;
    dial_menu_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(dial_menu_, board::kDisplayWidth, board::kDisplayHeight);
    lv_obj_center(dial_menu_);
    lv_obj_set_style_bg_color(dial_menu_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dial_menu_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(dial_menu_, 0, 0);
    lv_obj_set_style_radius(dial_menu_, 0, 0);
    lv_obj_clear_flag(dial_menu_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dial_menu_, LV_OBJ_FLAG_EVENT_BUBBLE);
    // Capture touches while open, but never treat a touch as return confirmation.
    lv_obj_add_flag(dial_menu_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dial_menu_, LV_OBJ_FLAG_HIDDEN);
    dial_menu_label_ = lv_label_create(dial_menu_);
    lv_label_set_text(dial_menu_label_, "Return to Orb");
    lv_obj_set_style_text_font(dial_menu_label_, &dosis_40, 0);
    lv_obj_set_style_text_color(dial_menu_label_, lv_color_hex(0xA8FFD2), 0);
    lv_obj_align(dial_menu_label_, LV_ALIGN_CENTER, 0, -18);
    lv_obj_t* hint = lv_label_create(dial_menu_);
    lv_label_set_text(hint, "Press dial to return\nTurn to cancel");
    lv_obj_set_style_text_font(hint, &dosis_20, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xC4CCC7), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 42);
    if (lv_timer_create(&Ui::dial_timer_cb, 20, this) == nullptr) return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void Ui::dial_timer_cb(lv_timer_t* timer) {
  auto* ui = static_cast<Ui*>(lv_timer_get_user_data(timer));
  ui->poll_dial();
}

void Ui::poll_dial() {
  if (!dial_router_.needs_poll(dial::snapshot())) return;
  // LVGL's timer callback already holds the display lock. Hardware interrupts
  // retain every detent/press while this worker is busy rendering.
  const auto input = dial::snapshot();
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  const auto action = dial_router_.poll(input, now);
  if (action.activity) {
    arm_quiet_hours_touch_wake(lv_tick_get());
    note_activity(true);
  }
  if (action.opened) {
    gesture_active_ = false;
    swipe_switched_ = false;
    if (overlay_visible_) {
      lv_obj_add_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
      overlay_visible_ = false;
    }
    lv_label_set_text(dial_menu_label_, "Return to Orb");
    lv_obj_clear_flag(dial_menu_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(dial_menu_);
    ESP_LOGI("dial", "Return menu opened; waiting for a new dial press");
  }
  if (action.closed) {
    lv_obj_add_flag(dial_menu_, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI("dial", "Return menu dismissed");
  }
  if (action.confirmed) {
    lv_label_set_text(dial_menu_label_, "Returning to Orb...");
    ESP_LOGI("dial", "Return to Orb confirmed by dial press");
    return_to_radar_requested_.store(true);
  }
  if (action.pressed && active_page_ == kPageIdxMain &&
      snapshot_has_active_print_center_cycle(last_snapshot_)) {
    click_sound_requested_.store(true);
    sync_runtime_center_mode_locked(last_snapshot_);
    toggle_runtime_center_mode_locked();
    apply_snapshot_locked(last_snapshot_, false);
  }
  if (action.turn != 0) {
    const int direction = action.turn > 0 ? 1 : -1;
    const int64_t count = action.turn > 0 ? action.turn : -static_cast<int64_t>(action.turn);
    int target = active_page_;
    // At most one pass across the finite screen list; no wrapping at either end.
    for (int64_t n = 0; n < count && n <= kPageIdxLast; ++n) {
      const int next = next_enabled_page(target, direction);
      if (next == target) break;
      target = next;
    }
    if (target != active_page_) {
      const int previous = active_page_;
      set_active_page(target);
      ESP_LOGI("dial", "%s: page %d -> %d", direction > 0 ? "CW/right" : "CCW/left", previous, target);
    }
  }
}

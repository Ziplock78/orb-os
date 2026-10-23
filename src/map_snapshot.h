#pragma once
#include <lvgl.h>

namespace radar {
// Ported from fix/flight-tracker-maps: reuse pixels instead of entering
// lv_snapshot_take()'s allocation assertion on fragmented PSRAM.
class MapSnapshot {
    lv_img_dsc_t image_{};
    void *pixels_=nullptr;
    uint32_t capacity_=0;
    bool valid_=false;
public:
    MapSnapshot()=default;
    MapSnapshot(const MapSnapshot &)=delete;
    MapSnapshot &operator=(const MapSnapshot &)=delete;
    ~MapSnapshot() {if(pixels_){lv_img_cache_invalidate_src(&image_);lv_mem_free(pixels_);}}
    bool take(lv_obj_t *object) {
        uint32_t required=lv_snapshot_buf_size_needed(object,LV_IMG_CF_TRUE_COLOR_ALPHA);
        valid_=false;if(!required)return false;
        if(required>capacity_) {
            void *larger=lv_mem_alloc(required);if(!larger)return false;
            if(pixels_){lv_img_cache_invalidate_src(&image_);lv_mem_free(pixels_);}
            pixels_=larger;capacity_=required;
        }
        lv_img_cache_invalidate_src(&image_);
        valid_=lv_snapshot_take_to_buf(object,LV_IMG_CF_TRUE_COLOR_ALPHA,&image_,pixels_,capacity_)==LV_RES_OK;
        return valid_;
    }
    lv_img_dsc_t *image(){return valid_?&image_:nullptr;}
};
}

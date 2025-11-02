#pragma once

#include "esp_mac.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"

class Calculator: public ESP_Brookesia_PhoneApp
{
public:
	Calculator();
	~Calculator();

    bool run(void);
    bool back(void);
    bool close(void);

    bool init(void) override;

    /* helper to show overlay for pressed button */
    void showOverlayForIndex(int idx);

    int formula_len;
    lv_obj_t *keyboard;
    lv_obj_t *history_label;
    lv_obj_t *formula_label;
    lv_obj_t *result_label;
    uint16_t _height;
    uint16_t _width;
    /* Overlay and timer for 5-second countdown screen (public so callbacks can access) */
    lv_obj_t *overlay_screen = nullptr;
    lv_obj_t *overlay_btn_label = nullptr;
    lv_obj_t *overlay_count_label = nullptr;
    lv_timer_t *overlay_timer = nullptr;
    int overlay_seconds = 0;
    lv_obj_t *grid_buttons[8] = {nullptr};
    
    /* Making process management */
    enum class MakingStage {
        GIF_PLAYING,
        SHOWING_FINISH
    };
    MakingStage current_stage = MakingStage::GIF_PLAYING;
    lv_obj_t *gif_obj = nullptr;
    lv_obj_t *finish_img_obj = nullptr;
    lv_obj_t *making_bg_img = nullptr;

private:
};

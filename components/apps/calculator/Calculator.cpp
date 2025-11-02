#include <math.h>
#include <vector>
#include "Calculator.hpp"
#include "esp_log.h"

using namespace std;

static const char *TAG = "Calculator";

LV_IMG_DECLARE(img_app_calculator);
LV_IMG_DECLARE(img_main_menu);
LV_IMG_DECLARE(img_making);
LV_IMG_DECLARE(making_finish);
// LV_IMG_DECLARE(gif_making);  // 不再需要.c文件声明

/* Ensure the img_main_menu descriptor is compiled into this translation unit.
 * This includes the previously-created main_menu_dsc.c so the symbol
 * `img_main_menu` is available at link time even if CMake hasn't been
 * re-run in the environment.
 */
//#include "assets/main_menu_dsc.c"

/* Forward declarations for callbacks (LVGL style) */
static void grid_button_event_cb(lv_event_t * e);
static void overlay_timer_cb(lv_timer_t * t);

Calculator::Calculator():
    ESP_Brookesia_PhoneApp("Coffee_UI", &img_app_calculator, true)
{
}

Calculator::~Calculator()
{

}

bool Calculator::run(void)
{
    /* Disable scrolling on main screen */
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    lv_area_t area = getVisualArea();
    _width = area.x2 - area.x1;
    _height = area.y2 - area.y1;
    formula_len = 1;
    // area.x1 = 0;
    // area.y1 = 0;
    // area.x2 = _width;
    // area.y2 = _height;

    /*
     * 创建并设置主界面背景图
     * 从 assets/main_menu.c 加载背景图像
     */
    lv_obj_t *bg_img = lv_img_create(lv_scr_act());
    lv_img_set_src(bg_img, &img_main_menu);
    /* 确保背景图位于可视区域正确位置 */
    lv_obj_set_pos(bg_img, 0, 0);
    /* 将背景图置于底层 */
    lv_obj_move_background(bg_img);

    /*
     * 获取可视区域并计算宽高
     * getVisualArea() 返回屏幕上该应用可用的区域（相对于整屏），
     * area.x2/area.x1 等为像素坐标，宽度为 x2-x1，高度为 y2-y1。
     */
    lv_area_t area2 = area;
    (void)area2; // 保留原 area 变量以便调试（不改变原逻辑）

    /*
     * Create a full-size transparent container and add 8 transparent buttons
     * arranged in 4 columns x 2 rows (top 4, bottom 4). Buttons do not scroll.
     */
    lv_obj_t *cont = lv_obj_create(lv_scr_act());
    /* Set container size equal to the visual area */
    lv_obj_set_size(cont, _width, _height);
    /* Position container at visual area's top-left */
    lv_obj_set_pos(cont, 0, 0);
    /* Make container fully transparent and remove padding/border */
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    /* Ensure container doesn't intercept scrolls (we want simple clickable areas) */
    //lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    int base_h = area.y1 + 50;
    int base_w = 50;
    int btn_w = (_width- base_w*2) / 4;
    int btn_h = (_height - base_h) / 2;

    for (int i = 0; i < 8; i++) {
        int col = i % 4;    /* 0..3 */
        int row = i / 4;    /* 0..1 */

        lv_obj_t *btn = lv_btn_create(cont);
        /* Save pointer so callback can find index */
        grid_buttons[i] = btn;
        /* Size and absolute position within container */
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_set_pos(btn, base_w + col * btn_w, base_h + row * btn_h);

        /* Transparent look: no background, no border, square corners */
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);

        /* Make sure button is clickable but not scrollable */
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        /* Add click event callback and pass `this` as user_data so callback
        * can access Calculator instance and resolve which button index was pressed. */
        lv_obj_add_event_cb(btn, grid_button_event_cb, LV_EVENT_CLICKED, this);
    }

    return true;
}

/* Find index for target button inside grid_buttons array */
static int find_button_index(Calculator *cal, lv_obj_t * target)
{
    for (int i = 0; i < 8; ++i) {
        if (cal->grid_buttons[i] == target) return i;
    }
    return -1;
}

/* Event callback for grid buttons */
static void grid_button_event_cb(lv_event_t * e)
{
    lv_obj_t * target = lv_event_get_target(e);
    Calculator *cal = (Calculator *)lv_event_get_user_data(e);
    if (!cal || !target) return;

    int idx = find_button_index(cal, target);
    if (idx < 0) return;

    cal->showOverlayForIndex(idx);
}

/* Timer callback: runs every second to update countdown; when reaches 0 remove overlay */
static void overlay_timer_cb(lv_timer_t * t)
{
    Calculator *cal = (Calculator *)t->user_data;
    if (!cal) return;

    if (cal->overlay_seconds <= 0) {
        if (cal->current_stage == Calculator::MakingStage::GIF_PLAYING) {
            /* GIF播放完成，切换到显示finish图片阶段 */
            ESP_LOGI(TAG, "Switching to SHOWING_FINISH stage");
            cal->current_stage = Calculator::MakingStage::SHOWING_FINISH;
            
            /* 隐藏GIF */
            if (cal->gif_obj) {
                ESP_LOGI(TAG, "Hiding GIF object");
                lv_obj_add_flag(cal->gif_obj, LV_OBJ_FLAG_HIDDEN);
            }
            
            /* 隐藏静态背景图 */
            if (cal->making_bg_img) {
                ESP_LOGI(TAG, "Hiding making background image");
                lv_obj_add_flag(cal->making_bg_img, LV_OBJ_FLAG_HIDDEN);
            }
            
            /* 创建并显示finish图片 */
            if (!cal->finish_img_obj && cal->overlay_screen) {
                ESP_LOGI(TAG, "Creating finish image");
                cal->finish_img_obj = lv_img_create(cal->overlay_screen);
                lv_img_set_src(cal->finish_img_obj, &making_finish);
                lv_obj_align(cal->finish_img_obj, LV_ALIGN_CENTER, 0, 0);
                lv_obj_move_foreground(cal->finish_img_obj);
                ESP_LOGI(TAG, "Finish image created and displayed");
            }
            
            /* 设置finish图片显示时间为2秒 */
            cal->overlay_seconds = 2;
            ESP_LOGI(TAG, "Set finish display time to 2 seconds");
            return;
        } else if (cal->current_stage == Calculator::MakingStage::SHOWING_FINISH) {
            /* Finish图片显示完成，清理所有内容 */
            ESP_LOGI(TAG, "Finish display completed, cleaning up overlay");
            if (cal->overlay_timer) {
                lv_timer_del(cal->overlay_timer);
                cal->overlay_timer = nullptr;
            }
            if (cal->overlay_screen) {
                lv_obj_del(cal->overlay_screen);
                cal->overlay_screen = nullptr;
                cal->overlay_btn_label = nullptr;
                cal->overlay_count_label = nullptr;
                cal->gif_obj = nullptr;
                cal->finish_img_obj = nullptr;
                cal->making_bg_img = nullptr;
            }
            /* 重置阶段 */
            cal->current_stage = Calculator::MakingStage::GIF_PLAYING;
            ESP_LOGI(TAG, "Overlay cleanup complete");
            return;
        }
    }

    cal->overlay_seconds -= 1;
    ESP_LOGI(TAG, "Timer tick: %d seconds remaining, stage: %d", 
             cal->overlay_seconds, (int)cal->current_stage);
    if (cal->overlay_count_label) {
        lv_label_set_text_fmt(cal->overlay_count_label, " ", cal->overlay_seconds);
    }
}

/* Show an overlay screen that displays which button was pressed and a 5s countdown */
void Calculator::showOverlayForIndex(int idx)
{
    ESP_LOGI(TAG, "showOverlayForIndex called for button %d", idx);
    
    /*
     * 在显示新的覆盖层前，先清理可能存在的旧覆盖层与定时器。
     * - 避免多个定时器并存导致回调混乱
     * - 确保界面上只有一个 overlay_screen
     */
    if (overlay_timer) {
        lv_timer_del(overlay_timer);
        overlay_timer = nullptr;
    }
    if (overlay_screen) {
        lv_obj_del(overlay_screen);
        overlay_screen = nullptr;
        overlay_btn_label = nullptr;
        overlay_count_label = nullptr;
        gif_obj = nullptr;
        finish_img_obj = nullptr;
        making_bg_img = nullptr;
    }
    
    /* 重置制作阶段为GIF播放 */
    current_stage = MakingStage::GIF_PLAYING;

    /* Create overlay full screen */
    overlay_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay_screen, _width, _height);
    /* Place at visual area origin (assume run()'s area positioning used) */
    lv_obj_set_pos(overlay_screen, 0, 0);
    /* Disable scrolling on overlay */
    lv_obj_clear_flag(overlay_screen, LV_OBJ_FLAG_SCROLLABLE);
    /* Semi-transparent dark background */
    lv_obj_set_style_bg_color(overlay_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_screen, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay_screen, 0, 0);

    

    // 创建制作中的背景图（第一阶段显示）
    ESP_LOGI(TAG, "Creating making background image");
    making_bg_img = lv_img_create(overlay_screen);
    lv_img_set_src(making_bg_img, &img_making);
    lv_obj_align(making_bg_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_background(making_bg_img);

    // 尝试创建并显示GIF（如果GIF加载失败，会显示making_bg_img）
    ESP_LOGI(TAG, "Attempting to create GIF");
    gif_obj = lv_gif_create(overlay_screen);
    lv_gif_set_src(gif_obj, "/spiffs/gif_making.gif");
    lv_obj_align(gif_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(gif_obj);
    ESP_LOGI(TAG, "GIF object created");

    /* Label showing which button */
    overlay_btn_label = lv_label_create(overlay_screen);
    lv_obj_set_style_text_font(overlay_btn_label, &lv_font_montserrat_28, 0);
    lv_label_set_text_fmt(overlay_btn_label, " ", idx + 1);
    lv_obj_align(overlay_btn_label, LV_ALIGN_CENTER, 0, -20);
    lv_obj_move_foreground(overlay_btn_label);  // 确保标签在最上层

    /* Countdown label */
    overlay_count_label = lv_label_create(overlay_screen);
    lv_obj_set_style_text_font(overlay_count_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(overlay_count_label, lv_color_white(), 0); // 确保文字在深色背景上可见
    overlay_seconds = 3; // GIF播放3秒
    lv_label_set_text_fmt(overlay_count_label, " ", overlay_seconds);
    lv_obj_align(overlay_count_label, LV_ALIGN_CENTER, 0, 30);
    lv_obj_move_foreground(overlay_count_label);  // 确保标签在最上层

    /* Create 1-second timer to countdown; pass `this` as user data */
    overlay_timer = lv_timer_create(overlay_timer_cb, 1000, this);
    ESP_LOGI(TAG, "Overlay setup complete, starting %d second countdown", overlay_seconds);
}

/**
 * @brief The function will be called when the left button of navigate bar is clicked.
 */
bool Calculator::back(void)
{
    notifyCoreClosed();

    return true;
}

/**
 * @brief The function will be called when app should be closed.
 */
bool Calculator::close(void)
{
    /* Cleanup any overlay or timers if app is closed */
    if (overlay_timer) {
        lv_timer_del(overlay_timer);
        overlay_timer = nullptr;
    }
    if (overlay_screen) {
        lv_obj_del(overlay_screen);
        overlay_screen = nullptr;
        overlay_btn_label = nullptr;
        overlay_count_label = nullptr;
        gif_obj = nullptr;
        finish_img_obj = nullptr;
        making_bg_img = nullptr;
    }
    /* 重置阶段 */
    current_stage = MakingStage::GIF_PLAYING;
    return true;
}

bool Calculator::init(void)
{

    return true;
}
#include "CoffeeMachine.hpp"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_check.h"

extern "C" {
    //#include "camera/app_video.h"
    #include "esp_cache.h"
    #include "esp_private/esp_cache_private.h"
    #include <sys/ioctl.h>
    #include <unistd.h>
    #include "linux/videodev2.h"
}

static const char *TAG = "CoffeeMachine";

// Declare image resources
LV_IMG_DECLARE(img_main_menu);
LV_IMG_DECLARE(img_making);
LV_IMG_DECLARE(making_finish);
LV_IMG_DECLARE(preference);

/* Forward declarations for callbacks */
static void grid_button_event_cb(lv_event_t * e);
static void overlay_timer_cb(lv_timer_t * t);
static void settings_button_event_cb(lv_event_t * e);
static void back_button_event_cb(lv_event_t * e);
static void camera_button_event_cb(lv_event_t * e);
static void camera_video_frame_callback(uint8_t *camera_buf, uint8_t camera_buf_index, 
                                       uint32_t camera_buf_hes, uint32_t camera_buf_ves, 
                                       size_t camera_buf_len);
static void camera_init_task(void *param);
static void face_name_save_btn_cb(lv_event_t * e);
static void face_name_cancel_btn_cb(lv_event_t * e);
static void face_action_timer_cb(lv_timer_t * t);
static void face_list_back_btn_cb(lv_event_t * e);
static void face_delete_btn_cb(lv_event_t * e);
static void face_list_refresh_timer_cb(lv_timer_t * t);
static void face_list_back_timer_cb(lv_timer_t * t);

/* Global flag for face recognition mode */
static volatile bool g_face_recognition_active = false;
static volatile bool g_face_detected_waiting = false;

/* Find index for target button inside grid_buttons array */
static int find_button_index(CoffeeMachine *machine, lv_obj_t *target_btn)
{
    for (int i = 0; i < 8; i++) {
        if (machine->grid_buttons[i] == target_btn) {
            return i;
        }
    }
    return -1;
}

static void grid_button_event_cb(lv_event_t * e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    CoffeeMachine *machine = (CoffeeMachine *)lv_event_get_user_data(e);
    
    if (!machine) {
        ESP_LOGE(TAG, "Machine pointer is NULL in button event");
        return;
    }
    
    int idx = find_button_index(machine, btn);
    
    if (idx >= 0) {
        /* Print which button was clicked (1-8) */
        ESP_LOGI(TAG, "Button %d clicked", idx + 1);
        
        /* Button 8 (index 7) opens camera screen */
        if (idx == 7) {
            machine->showCameraScreen();
        } else {
            /* Other buttons show coffee making overlay */
            machine->showOverlayForIndex(idx);
        }
    }
}

static void overlay_timer_cb(lv_timer_t * t)
{
    CoffeeMachine *machine = (CoffeeMachine *)t->user_data;
    if (!machine) return;
    
    if (machine->current_stage == CoffeeMachine::MakingStage::GIF_PLAYING) {
        /* Stage 1: GIF playing stage */
        machine->overlay_seconds++;
        
        /* Update countdown label */
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", 5 - machine->overlay_seconds);
        if (machine->overlay_count_label) {
            lv_label_set_text(machine->overlay_count_label, buf);
        }
        
        /* After 5 seconds, switch to finish screen */
        if (machine->overlay_seconds >= 5) {
            ESP_LOGI(TAG, "Making finished, showing completion screen");
            
            /* Hide GIF and show finish image */
            if (machine->gif_obj) {
                lv_obj_add_flag(machine->gif_obj, LV_OBJ_FLAG_HIDDEN);
            }
            if (machine->finish_img_obj) {
                lv_obj_clear_flag(machine->finish_img_obj, LV_OBJ_FLAG_HIDDEN);
            }
            
            /* Hide countdown label */
            if (machine->overlay_count_label) {
                lv_obj_add_flag(machine->overlay_count_label, LV_OBJ_FLAG_HIDDEN);
            }
            
            machine->current_stage = CoffeeMachine::MakingStage::SHOWING_FINISH;
            machine->overlay_seconds = 0;  // Reset counter for finish screen
        }
    } else if (machine->current_stage == CoffeeMachine::MakingStage::SHOWING_FINISH) {
        /* Stage 2: Showing finish screen */
        machine->overlay_seconds++;
        
        /* After 2 seconds, return to main screen */
        if (machine->overlay_seconds >= 2) {
            ESP_LOGI(TAG, "Finish screen timeout, returning to main screen");
            machine->cleanup_overlay();
            machine->showMainScreen();
        }
    }
}

static void settings_button_event_cb(lv_event_t * e)
{
    CoffeeMachine *machine = (CoffeeMachine *)lv_event_get_user_data(e);
    if (machine) {
        ESP_LOGI(TAG, "Settings button clicked");
        machine->showSettingsScreen();
    }
}

static void back_button_event_cb(lv_event_t * e)
{
    CoffeeMachine *machine = (CoffeeMachine *)lv_event_get_user_data(e);
    if (machine) {
        ESP_LOGI(TAG, "Back button clicked");
        machine->showMainScreen();
    }
}

static void face_action_timer_cb(lv_timer_t * t)
{
    CoffeeMachine *machine = (CoffeeMachine *)t->user_data;
    if (!machine || !machine->_pending_face_action) {
        return;
    }
    
    ESP_LOGI(TAG, "Face action timer: processing recognized face");
    
    /* Stop and delete the timer */
    lv_timer_del(machine->_face_action_timer);
    machine->_face_action_timer = nullptr;
    machine->_pending_face_action = false;
    
    /* Close camera screen */
    machine->closeCameraScreen();
    
    /* Show coffee making overlay */
    machine->showOverlayForIndex(0);
    
    ESP_LOGI(TAG, "Face action completed: coffee making started");
}

CoffeeMachine::CoffeeMachine()
{
    ESP_LOGI(TAG, "CoffeeMachine constructor called");
    
    /* Initialize all pointers to NULL */
    overlay_screen = nullptr;
    overlay_btn_label = nullptr;
    overlay_count_label = nullptr;
    overlay_timer = nullptr;
    overlay_seconds = 0;
    
    for (int i = 0; i < 8; i++) {
        grid_buttons[i] = nullptr;
    }
    
    current_stage = MakingStage::GIF_PLAYING;
    gif_obj = nullptr;
    finish_img_obj = nullptr;
    making_bg_img = nullptr;
    
    main_screen = nullptr;
    settings_screen = nullptr;
    settings_btn = nullptr;
    back_btn = nullptr;
    
    /* Camera screen - not initialized yet */
    camera_screen = nullptr;
    camera_canvas = nullptr;
    for (int i = 0; i < 3; i++) {
        camera_buttons[i] = nullptr;
    }
    _camera_ctlr_handle = -1;
    _camera_running = false;
    _camera_initialized = false;
    _camera_init_sem = nullptr;
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        _cam_buffer[i] = nullptr;
        _cam_buffer_size[i] = 0;
    }
    
    /* Face recognition - not initialized yet */
    _face_recognition_enabled = false;
    _face_count = 0;
    _face_detector = nullptr;
    _face_name_screen = nullptr;
    _face_name_textarea = nullptr;
    _face_name_keyboard = nullptr;
    _captured_face_buffer = nullptr;
    _face_action_timer = nullptr;
    _recognized_face_idx = -1;
    _pending_face_action = false;
    
    /* Face list screen - not initialized yet */
    _face_list_screen = nullptr;
    _face_list_refresh_timer = nullptr;
    _face_list_back_timer = nullptr;
    _pending_delete_idx = -1;
    for (int i = 0; i < MAX_FACES; i++) {
        _face_list_items[i] = nullptr;
        _face_delete_btns[i] = nullptr;
    }
    
    for (int i = 0; i < MAX_FACES; i++) {
        memset(&_stored_faces[i], 0, sizeof(FaceData));
        _stored_faces[i].is_used = false;
    }
    
    /* Load faces from NVS */
    loadFacesFromNVS();
}

CoffeeMachine::~CoffeeMachine()
{
    ESP_LOGI(TAG, "CoffeeMachine destructor called");
    cleanup_overlay();
    closeCameraScreen();
    
    /* Clean up face action timer */
    if (_face_action_timer) {
        lv_timer_del(_face_action_timer);
        _face_action_timer = nullptr;
    }
    
    /* Clean up face list refresh timer */
    if (_face_list_refresh_timer) {
        lv_timer_del(_face_list_refresh_timer);
        _face_list_refresh_timer = nullptr;
    }
    
    /* Clean up face list back timer */
    if (_face_list_back_timer) {
        lv_timer_del(_face_list_back_timer);
        _face_list_back_timer = nullptr;
    }
    
    /* Free camera buffers */
    for (int i = 0; i < _cam_buf_count; i++) {
        if (_cam_buffer[i]) {
            heap_caps_free(_cam_buffer[i]);
            _cam_buffer[i] = nullptr;
        }
    }
    
    /* Delete camera screen */
    if (camera_screen) {
        lv_obj_del(camera_screen);
        camera_screen = nullptr;
    }
    
    /* Delete face list screen */
    if (_face_list_screen) {
        lv_obj_del(_face_list_screen);
        _face_list_screen = nullptr;
    }
}

void CoffeeMachine::cleanup_overlay(void)
{
    ESP_LOGI(TAG, "Cleaning up overlay");
    
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
    
    overlay_seconds = 0;
    current_stage = MakingStage::GIF_PLAYING;
}

bool CoffeeMachine::init(void)
{
    ESP_LOGI(TAG, "Initializing Coffee Machine UI");
    
    /* Get display object and screen size */
    lv_disp_t *display = lv_disp_get_default();
    if (!display) {
        ESP_LOGE(TAG, "Failed to get default display");
        return false;
    }
    
    _width = lv_disp_get_hor_res(display);
    _height = lv_disp_get_ver_res(display);
    
    ESP_LOGI(TAG, "Display size: %dx%d", _width, _height);
    
    /* Save main screen reference */
    main_screen = lv_scr_act();
    
    /* Disable main screen scrolling */
    lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);
    
    /* Create and set main background image */
    lv_obj_t *bg_img = lv_img_create(main_screen);
    lv_img_set_src(bg_img, &img_main_menu);
    lv_obj_set_pos(bg_img, 0, 0);
    lv_obj_move_background(bg_img);
    
    /* Create transparent container for buttons */
    lv_obj_t *cont = lv_obj_create(main_screen);
    lv_obj_set_size(cont, _width, _height);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    
    /* Button layout: 
     * Boundaries: top=80, bottom=50, left=50, right=50
     * Layout: 2 rows x 4 columns
     * Row 1: buttons 1,2,3,4
     * Row 2: buttons 5,6,7,8
     */
    int top_margin = 80;
    int bottom_margin = 50;
    int left_margin = 50;
    int right_margin = 50;
    
    int available_width = _width - left_margin - right_margin;
    int available_height = _height - top_margin - bottom_margin;
    
    /* Calculate button size: 4 buttons per row, 2 rows */
    int btn_width = available_width / 4;
    int btn_height = available_height / 2;
    
    /* Create 8 buttons in 2 rows x 4 columns layout */
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 4; col++) {
            int idx = row * 4 + col;  // 0-7
            int x = left_margin + col * btn_width;
            int y = top_margin + row * btn_height;
            
            /* Create transparent button */
            grid_buttons[idx] = lv_btn_create(cont);
            lv_obj_set_size(grid_buttons[idx], btn_width, btn_height);
            lv_obj_set_pos(grid_buttons[idx], x, y);
            lv_obj_set_style_bg_opa(grid_buttons[idx], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(grid_buttons[idx], 0, 0);
            lv_obj_set_style_shadow_width(grid_buttons[idx], 0, 0);
            
            /* Add click event */
            lv_obj_add_event_cb(grid_buttons[idx], grid_button_event_cb, LV_EVENT_CLICKED, this);
            
            ESP_LOGI(TAG, "Button %d created at (%d, %d) size (%dx%d)", 
                     idx + 1, x, y, btn_width, btn_height);
        }
    }
    
    /* Create settings button (top left corner, transparent, no image) */
    settings_btn = lv_btn_create(main_screen);
    lv_obj_set_size(settings_btn, 100, 80);  // Width: 150-50=100, Height: 80-0=80
    lv_obj_set_pos(settings_btn, 50, 0);
    
    /* Make button transparent with no border or content */
    lv_obj_set_style_bg_opa(settings_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_btn, 0, 0);
    lv_obj_set_style_shadow_width(settings_btn, 0, 0);
    lv_obj_set_style_radius(settings_btn, 0, 0);
    
    /* Add click event */
    lv_obj_add_event_cb(settings_btn, settings_button_event_cb, LV_EVENT_CLICKED, this);
    
    ESP_LOGI(TAG, "Coffee Machine UI initialized successfully");
    return true;
}

void CoffeeMachine::showOverlayForIndex(int idx)
{
    ESP_LOGI(TAG, "showOverlayForIndex called for button %d", idx);
    
    /* Clean up old overlay */
    cleanup_overlay();
    
    /* Reset making stage to GIF playing */
    current_stage = MakingStage::GIF_PLAYING;
    
    /* Create overlay */
    overlay_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay_screen, _width, _height);
    lv_obj_set_pos(overlay_screen, 0, 0);
    lv_obj_clear_flag(overlay_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(overlay_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_screen, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay_screen, 0, 0);
    
    /* Create background image for making process */
    making_bg_img = lv_img_create(overlay_screen);
    lv_img_set_src(making_bg_img, &img_making);
    lv_obj_center(making_bg_img);
    
    /* Create GIF animation object (initially visible) */
    gif_obj = lv_gif_create(overlay_screen);
    lv_gif_set_src(gif_obj, "A:/mp4/3.gif");
    lv_obj_set_size(gif_obj, 400, 400);
    lv_obj_align(gif_obj, LV_ALIGN_CENTER, 0, -50);
    
    /* Create finish image (initially hidden) - centered without offset */
    finish_img_obj = lv_img_create(overlay_screen);
    lv_img_set_src(finish_img_obj, &making_finish);
    lv_obj_center(finish_img_obj);  // Center without any offset
    lv_obj_add_flag(finish_img_obj, LV_OBJ_FLAG_HIDDEN);
    
    /* Create countdown label */
    overlay_count_label = lv_label_create(overlay_screen);
    lv_label_set_text(overlay_count_label, "5");
    lv_obj_set_style_text_font(overlay_count_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(overlay_count_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(overlay_count_label, LV_ALIGN_CENTER, 0, 80);
    
    /* Start timer */
    overlay_seconds = 0;
    overlay_timer = lv_timer_create(overlay_timer_cb, 1000, this);
    
    ESP_LOGI(TAG, "Coffee %d is making", idx + 1);
}

void CoffeeMachine::showSettingsScreen(void)
{
    ESP_LOGI(TAG, "Showing settings screen");
    
    /* Clean up possible overlay */
    cleanup_overlay();
    
    /* Create settings screen if not exists */
    if (!settings_screen) {
        settings_screen = lv_obj_create(NULL);
        lv_obj_clear_flag(settings_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(settings_screen, lv_color_hex(0x000000), 0);
        
        /* Display preference image as the settings screen background */
        lv_obj_t *preference_img = lv_img_create(settings_screen);
        lv_img_set_src(preference_img, &preference);
        lv_obj_set_pos(preference_img, 0, 0);
        
        /* Create back button (transparent, no text, no border) */
        back_btn = lv_btn_create(settings_screen);
        lv_obj_set_size(back_btn, 100, 80);  // Width: 150-50=100, Height: 80-0=80
        lv_obj_set_pos(back_btn, 50, 0);
        
        /* Make button transparent with no border or content */
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(back_btn, 0, 0);
        lv_obj_set_style_shadow_width(back_btn, 0, 0);
        lv_obj_set_style_radius(back_btn, 0, 0);
        
        lv_obj_add_event_cb(back_btn, back_button_event_cb, LV_EVENT_CLICKED, this);
    }
    
    /* Switch to settings screen */
    lv_scr_load(settings_screen);
}

void CoffeeMachine::showMainScreen(void)
{
    ESP_LOGI(TAG, "Showing main screen");
    
    /* Clean up overlay */
    cleanup_overlay();
    
    /* Load main screen */
    if (main_screen) {
        lv_scr_load(main_screen);
    }
}

static CoffeeMachine *g_camera_machine = nullptr;
static volatile bool g_camera_callback_enabled = false;

static void camera_init_task(void *param)
{
    CoffeeMachine *machine = (CoffeeMachine *)param;
    
    ESP_LOGI(TAG, "Camera init task: Setting buffers and starting stream");
    
    /* Set camera buffers */
    esp_err_t ret = app_video_set_bufs(machine->_camera_ctlr_handle, machine->_cam_buf_count, 
                                      (const void **)machine->_cam_buffer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set video buffers: 0x%x", ret);
        xSemaphoreGive(machine->_camera_init_sem);
        vTaskDelete(NULL);
        return;
    }
    
    /* Start camera stream task */
    ret = app_video_stream_task_start(machine->_camera_ctlr_handle, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start video stream: 0x%x", ret);
        xSemaphoreGive(machine->_camera_init_sem);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Camera stream started successfully");
    machine->_camera_running = true;
    
    xSemaphoreGive(machine->_camera_init_sem);
    vTaskDelete(NULL);
}

static void camera_video_frame_callback(uint8_t *camera_buf, uint8_t camera_buf_index, 
                                       uint32_t camera_buf_hes, uint32_t camera_buf_ves, 
                                       size_t camera_buf_len)
{
    static bool size_logged = false;
    static int frame_count = 0;
    
    /* Check if callback is enabled and objects are valid */
    if (!g_camera_callback_enabled || !g_camera_machine || !g_camera_machine->camera_canvas) {
        return;
    }
    
    /* Log actual camera resolution once for debugging */
    if (!size_logged) {
        ESP_LOGI(TAG, "Camera frame callback: %dx%d", camera_buf_hes, camera_buf_ves);
        size_logged = true;
    }
    
    /* Face recognition processing (every 10 frames to reduce CPU load) */
    if (g_face_recognition_active && !g_face_detected_waiting && 
        g_camera_machine->_face_detector && (frame_count++ % 10 == 0)) {
        
        /* Run face detection */
        auto detect_results = app_humanface_detect((uint16_t *)camera_buf, camera_buf_hes, camera_buf_ves);
        
        if (!detect_results.empty()) {
            ESP_LOGI(TAG, "Face detected!");
            
            /* Try to recognize the face */
            int recognized_idx = g_camera_machine->recognizeFace(detect_results);
            
            if (recognized_idx >= 0) {
                /* Known face recognized */
                ESP_LOGI(TAG, "Welcome back, %s!", g_camera_machine->_stored_faces[recognized_idx].name);
                
                /* Send via UART */
                printf("COFFEE_FOR: %s\n", g_camera_machine->_stored_faces[recognized_idx].name);
                
                /* Disable face recognition immediately to prevent re-detection */
                g_face_recognition_active = false;
                g_camera_machine->_face_recognition_enabled = false;
                
                /* Schedule delayed action using LVGL timer (runs in LVGL task context) */
                if (!g_camera_machine->_pending_face_action && bsp_display_lock(0)) {
                    g_camera_machine->_recognized_face_idx = recognized_idx;
                    g_camera_machine->_pending_face_action = true;
                    
                    /* Create one-shot timer to process action in LVGL context (100ms delay) */
                    g_camera_machine->_face_action_timer = lv_timer_create(face_action_timer_cb, 100, g_camera_machine);
                    lv_timer_set_repeat_count(g_camera_machine->_face_action_timer, 1);
                    
                    ESP_LOGI(TAG, "Face action scheduled via timer");
                    bsp_display_unlock();
                }
                
                return;  // Don't update display this frame
            } else if (recognized_idx == -1) {
                /* Unknown face */
                ESP_LOGI(TAG, "Unknown face detected");
                
                /* Check if we can store more faces */
                if (g_camera_machine->_face_count < MAX_FACES) {
                    g_face_detected_waiting = true;
                    
                    /* Show face name input screen */
                    bsp_display_lock(0);
                    g_camera_machine->showFaceNameScreen();
                    bsp_display_unlock();
                    
                    return;  // Don't update display this frame
                } else {
                    ESP_LOGW(TAG, "Face storage full, cannot add new face");
                    /* Continue showing camera view */
                }
            }
        }
    }
    
    /* Get display lock with longer timeout for better frame capture */
    if (!bsp_display_lock(100)) {
        return;  // Skip this frame if can't get lock
    }
    
    /* Update canvas buffer to current camera buffer */
    lv_canvas_set_buffer(g_camera_machine->camera_canvas, camera_buf, 
                       camera_buf_hes, camera_buf_ves, 
                       LV_IMG_CF_TRUE_COLOR);
    
    /* Immediately refresh display for higher frame rate - same as Camera app */
    lv_refr_now(NULL);
    
    bsp_display_unlock();
}

static void camera_button_event_cb(lv_event_t * e)
{
    CoffeeMachine *machine = (CoffeeMachine *)lv_event_get_user_data(e);
    if (!machine) return;
    
    lv_obj_t *btn = lv_event_get_target(e);
    
    for (int i = 0; i < 3; i++) {
        if (machine->camera_buttons[i] == btn) {
            ESP_LOGI(TAG, "Camera button %d clicked", i + 1);
            
            if (i == 0) {
                ESP_LOGI(TAG, "Activating face recognition mode");
                machine->_face_recognition_enabled = true;
                g_face_recognition_active = true;
                
                if (machine->_face_detector == nullptr) {
                    machine->_face_detector = get_humanface_detect();
                }
            }
            else if (i == 1) {
                ESP_LOGI(TAG, "Opening face list screen");
                machine->_face_recognition_enabled = false;
                g_face_recognition_active = false;
                machine->showFaceListScreen();
            }
            else if (i == 2) {
                machine->_face_recognition_enabled = false;
                g_face_recognition_active = false;
                machine->closeCameraScreen();
                machine->showMainScreen();
            }
            break;
        }
    }
}

void CoffeeMachine::showCameraScreen(void)
{
    ESP_LOGI(TAG, "Showing camera screen");
    
    /* Clean up overlay if any */
    cleanup_overlay();
    
    /* Initialize camera hardware if not done */
    if (!_camera_initialized) {
        ESP_LOGI(TAG, "Initializing camera hardware...");
        
        i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_get_handle();
        esp_err_t ret = app_video_main(i2c_bus_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera init failed with error 0x%x", ret);
            return;
        }
        
        _camera_ctlr_handle = app_video_open((char*)EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT_RGB565);
        if (_camera_ctlr_handle < 0) {
            ESP_LOGE(TAG, "Camera open failed");
            return;
        }
        
        /* Get actual camera resolution (camera has its own fixed resolution) */
        struct v4l2_format format;
        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        
        if (ioctl(_camera_ctlr_handle, VIDIOC_G_FMT, &format) == 0) {
            ESP_LOGI(TAG, "Camera native resolution: %dx%d", 
                     format.fmt.pix.width, format.fmt.pix.height);
        }
        
        /* Use the format reported by the driver for allocation */
        uint32_t cam_width = format.fmt.pix.width ? format.fmt.pix.width : 1280;
        uint32_t cam_height = format.fmt.pix.height ? format.fmt.pix.height : 960;

        /* Allocate camera buffers for camera resolution, but try fewer buffers if memory is low */
        size_t data_cache_line_size = 0;
        ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));

        size_t single_buf_size = cam_width * cam_height * BSP_LCD_BITS_PER_PIXEL / 8;
        bool alloc_ok = false;
        int try_max = EXAMPLE_CAM_BUF_NUM;
        for (int fb = try_max; fb >= 2; fb--) {
            bool ok = true;
            for (int i = 0; i < fb; i++) {
                _cam_buffer[i] = (uint8_t *)heap_caps_aligned_alloc(data_cache_line_size, single_buf_size, MALLOC_CAP_SPIRAM);
                _cam_buffer_size[i] = single_buf_size;
                if (_cam_buffer[i] == NULL) {
                    ok = false;
                    break;
                }
            }

            if (ok) {
                _cam_buf_count = fb;
                alloc_ok = true;
                break;
            }

            /* Free any partial allocations and try with fewer buffers */
            for (int j = 0; j < fb; j++) {
                if (_cam_buffer[j]) {
                    heap_caps_free(_cam_buffer[j]);
                    _cam_buffer[j] = NULL;
                    _cam_buffer_size[j] = 0;
                }
            }
        }

        if (!alloc_ok) {
            ESP_LOGE(TAG, "Failed to allocate camera buffers (tried up to %d). Aborting camera init.", EXAMPLE_CAM_BUF_NUM);
            /* Close camera handle and leave camera uninitialized to avoid blue screen */
            if (_camera_ctlr_handle >= 0) {
                close(_camera_ctlr_handle);
                _camera_ctlr_handle = -1;
            }
            return;
        }

        /* Register frame callback AFTER successful allocation */
        g_camera_machine = this;
        ESP_ERROR_CHECK(app_video_register_frame_operation_cb(camera_video_frame_callback));

        _camera_initialized = true;
        ESP_LOGI(TAG, "Camera hardware initialized (buffers=%d size=%d)", _cam_buf_count, (int)single_buf_size);
    }
    
    /* Create camera screen if not exists */
    if (!camera_screen) {
        camera_screen = lv_obj_create(NULL);
        lv_obj_clear_flag(camera_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(camera_screen, lv_color_hex(0x000000), 0);
        
        /* 
         * Create canvas with CAMERA native resolution (1280x960).
         * No zoom/scaling - display will show center portion (1024x600).
         * This avoids software scaling overhead and maximizes frame rate.
         */
        uint32_t cam_width = 1280;
        uint32_t cam_height = 960;
        
        camera_canvas = lv_canvas_create(camera_screen);
        lv_obj_set_size(camera_canvas, cam_width, cam_height);
        lv_canvas_set_buffer(camera_canvas, _cam_buffer[0], cam_width, cam_height, LV_IMG_CF_TRUE_COLOR);
        
        /* Center the canvas - screen will show the center portion of the camera view */
        lv_obj_center(camera_canvas);
        
        ESP_LOGI(TAG, "Camera canvas: %dx%d (native resolution, center portion displayed on %dx%d screen)", 
                 cam_width, cam_height, _width, _height);
        
        const char *button_labels[] = {"Face ID", "Face List", "Back"};
        
        int btn_width = 200;
        int btn_height = 70;
        int btn_spacing = 40;
        int total_width = (btn_width * 3) + (btn_spacing * 2);
        int start_x = (_width - total_width) / 2;
        int btn_y = _height - btn_height - 20;
        
        for (int i = 0; i < 3; i++) {
            camera_buttons[i] = lv_btn_create(camera_screen);
            
            int btn_x = start_x + i * (btn_width + btn_spacing);
            lv_obj_set_size(camera_buttons[i], btn_width, btn_height);
            lv_obj_set_pos(camera_buttons[i], btn_x, btn_y);
            
            /* Set blue background - fully opaque to avoid blending issues */
            lv_obj_set_style_bg_color(camera_buttons[i], lv_color_hex(0x0080FF), 0);
            lv_obj_set_style_bg_opa(camera_buttons[i], LV_OPA_COVER, 0);  // 100% opacity
            
            /* Set rounded corners */
            lv_obj_set_style_radius(camera_buttons[i], 15, 0);
            
            /* Add subtle border and shadow */
            lv_obj_set_style_border_width(camera_buttons[i], 2, 0);
            lv_obj_set_style_border_color(camera_buttons[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_shadow_width(camera_buttons[i], 10, 0);
            lv_obj_set_style_shadow_color(camera_buttons[i], lv_color_hex(0x000000), 0);
            lv_obj_set_style_shadow_opa(camera_buttons[i], LV_OPA_30, 0);
            
            /* Add white text label */
            lv_obj_t *label = lv_label_create(camera_buttons[i]);
            lv_label_set_text(label, button_labels[i]);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
            lv_obj_center(label);
            
            /* Add click event */
            lv_obj_add_event_cb(camera_buttons[i], camera_button_event_cb, LV_EVENT_CLICKED, this);
            
            ESP_LOGI(TAG, "Camera button %d created at (%d, %d) size %dx%d", 
                     i + 1, btn_x, btn_y, btn_width, btn_height);
        }
    }
    
    /* Switch to camera screen */
    lv_scr_load(camera_screen);
    
    /* Start camera streaming if not already running */
    if (!_camera_running && _camera_initialized) {
        ESP_LOGI(TAG, "Starting camera stream task...");
        
        /* Create semaphore for initialization sync */
        if (_camera_init_sem == NULL) {
            _camera_init_sem = xSemaphoreCreateBinary();
            if (_camera_init_sem == NULL) {
                ESP_LOGE(TAG, "Failed to create camera init semaphore");
                return;
            }
        }
        
        /* Enable callback before starting stream */
        g_camera_callback_enabled = true;
        
        /* Create task to set buffers and start stream */
        xTaskCreatePinnedToCore(camera_init_task, "Camera Init", 4096, this, 2, NULL, 0);
        
        /* Wait for initialization to complete with timeout */
        if (xSemaphoreTake(_camera_init_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Camera init task timeout");
            g_camera_callback_enabled = false;
            return;
        }
        
        vSemaphoreDelete(_camera_init_sem);
        _camera_init_sem = NULL;
        
        ESP_LOGI(TAG, "Camera stream started successfully");
    } else if (_camera_running) {
        /* Re-enable callback if stream is already running */
        g_camera_callback_enabled = true;
    }
}

void CoffeeMachine::closeCameraScreen(void)
{
    ESP_LOGI(TAG, "Closing camera screen");
    
    /* Disable callback first */
    g_camera_callback_enabled = false;
    
    /* Stop camera streaming */
    if (_camera_running && _camera_ctlr_handle >= 0) {
        ESP_LOGI(TAG, "Stopping camera stream...");
        app_video_stream_task_stop(_camera_ctlr_handle);
        app_video_stream_wait_stop();
        _camera_running = false;
    }
    
    /* Disable face recognition */
    _face_recognition_enabled = false;
    g_face_recognition_active = false;
}

/* ==================== Face Recognition Functions ==================== */

bool CoffeeMachine::loadFacesFromNVS(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("face_storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No face data stored yet");
        return false;
    }
    
    size_t required_size = sizeof(FaceData) * MAX_FACES;
    err = nvs_get_blob(nvs_handle, "faces", _stored_faces, &required_size);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        _face_count = 0;
        for (int i = 0; i < MAX_FACES; i++) {
            if (_stored_faces[i].is_used) {
                _face_count++;
                ESP_LOGI(TAG, "Loaded face %d: %s", i, _stored_faces[i].name);
            }
        }
        return true;
    }
    
    return false;
}

bool CoffeeMachine::saveFacesToNVS(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("face_storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return false;
    }
    
    err = nvs_set_blob(nvs_handle, "faces", _stored_faces, sizeof(FaceData) * MAX_FACES);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Face data saved to NVS");
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to save face data");
        return false;
    }
}

int CoffeeMachine::recognizeFace(const std::list<dl::detect::result_t> &results)
{
    /* Simple recognition: just check if we have any stored faces */
    /* In real implementation, you would compare face features */
    
    if (results.empty()) {
        return -2;  // No face detected
    }
    
    /* For now, simulate recognition: */
    /* If we have stored faces, randomly "recognize" one of them */
    /* In production, you'd compare face embeddings/features here */
    
    if (_face_count > 0) {
        /* Check if this is a known face (simplified - always recognize first stored face) */
        for (int i = 0; i < MAX_FACES; i++) {
            if (_stored_faces[i].is_used) {
                ESP_LOGI(TAG, "Recognized: %s", _stored_faces[i].name);
                return i;  // Return index of recognized face
            }
        }
    }
    
    return -1;  // Unknown face
}

void CoffeeMachine::saveFaceData(const char *name)
{
    if (_face_count >= MAX_FACES) {
        ESP_LOGW(TAG, "Face storage full, replacing oldest");
        /* Find oldest or first slot */
        for (int i = 0; i < MAX_FACES; i++) {
            if (_stored_faces[i].is_used) {
                strncpy(_stored_faces[i].name, name, sizeof(_stored_faces[i].name) - 1);
                _stored_faces[i].name[sizeof(_stored_faces[i].name) - 1] = '\0';
                ESP_LOGI(TAG, "Updated face slot %d: %s", i, name);
                saveFacesToNVS();
                return;
            }
        }
    } else {
        /* Find empty slot */
        for (int i = 0; i < MAX_FACES; i++) {
            if (!_stored_faces[i].is_used) {
                _stored_faces[i].is_used = true;
                strncpy(_stored_faces[i].name, name, sizeof(_stored_faces[i].name) - 1);
                _stored_faces[i].name[sizeof(_stored_faces[i].name) - 1] = '\0';
                /* In production: extract and store face features here */
                _face_count++;
                ESP_LOGI(TAG, "Saved new face in slot %d: %s", i, name);
                saveFacesToNVS();
                return;
            }
        }
    }
}

static void face_name_save_btn_cb(lv_event_t * e)
{
    CoffeeMachine *machine = (CoffeeMachine *)lv_event_get_user_data(e);
    if (!machine || !machine->_face_name_textarea) return;
    
    const char *name = lv_textarea_get_text(machine->_face_name_textarea);
    if (name && strlen(name) > 0) {
        ESP_LOGI(TAG, "Saving face with name: %s", name);
        machine->saveFaceData(name);
        machine->closeFaceNameScreen();
        machine->showMainScreen();
    }
}

static void face_name_cancel_btn_cb(lv_event_t * e)
{
    CoffeeMachine *machine = (CoffeeMachine *)lv_event_get_user_data(e);
    if (machine) {
        machine->closeFaceNameScreen();
        machine->showCameraScreen();
    }
}

void CoffeeMachine::showFaceNameScreen(void)
{
    ESP_LOGI(TAG, "Showing face name input screen");
    
    /* Create face name screen if not exists */
    if (!_face_name_screen) {
        _face_name_screen = lv_obj_create(NULL);
        lv_obj_clear_flag(_face_name_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(_face_name_screen, lv_color_hex(0x000000), 0);
        
        /* Title label */
        lv_obj_t *title = lv_label_create(_face_name_screen);
        lv_label_set_text(title, "Enter Name for Face");
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
        
        /* Text area for name input */
        _face_name_textarea = lv_textarea_create(_face_name_screen);
        lv_obj_set_size(_face_name_textarea, 400, 60);
        lv_obj_align(_face_name_textarea, LV_ALIGN_TOP_MID, 0, 80);
        lv_textarea_set_placeholder_text(_face_name_textarea, "Name...");
        lv_textarea_set_max_length(_face_name_textarea, 31);
        lv_textarea_set_one_line(_face_name_textarea, true);
        
        /* Keyboard */
        _face_name_keyboard = lv_keyboard_create(_face_name_screen);
        lv_obj_set_size(_face_name_keyboard, _width, _height / 2);
        lv_obj_align(_face_name_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_textarea(_face_name_keyboard, _face_name_textarea);
        
        /* Save button */
        lv_obj_t *save_btn = lv_btn_create(_face_name_screen);
        lv_obj_set_size(save_btn, 150, 60);
        lv_obj_align(save_btn, LV_ALIGN_CENTER, -100, -50);
        lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x00FF00), 0);
        
        lv_obj_t *save_label = lv_label_create(save_btn);
        lv_label_set_text(save_label, "Save");
        lv_obj_center(save_label);
        
        lv_obj_add_event_cb(save_btn, face_name_save_btn_cb, LV_EVENT_CLICKED, this);
        
        /* Cancel button */
        lv_obj_t *cancel_btn = lv_btn_create(_face_name_screen);
        lv_obj_set_size(cancel_btn, 150, 60);
        lv_obj_align(cancel_btn, LV_ALIGN_CENTER, 100, -50);
        lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xFF0000), 0);
        
        lv_obj_t *cancel_label = lv_label_create(cancel_btn);
        lv_label_set_text(cancel_label, "Cancel");
        lv_obj_center(cancel_label);
        
        lv_obj_add_event_cb(cancel_btn, face_name_cancel_btn_cb, LV_EVENT_CLICKED, this);
    }
    
    /* Clear text area */
    if (_face_name_textarea) {
        lv_textarea_set_text(_face_name_textarea, "");
    }
    
    /* Switch to face name screen */
    lv_scr_load(_face_name_screen);
}

void CoffeeMachine::closeFaceNameScreen(void)
{
    ESP_LOGI(TAG, "Closing face name screen");
    
    /* Just switch away, don't delete the screen for reuse */
    _face_recognition_enabled = false;
    g_face_recognition_active = false;
    g_face_detected_waiting = false;
}

/* ==================== Face List Management Functions ==================== */

static void face_list_back_timer_cb(lv_timer_t * t)
{
    CoffeeMachine *machine = (CoffeeMachine *)t->user_data;
    if (!machine) return;
    
    ESP_LOGI(TAG, "Face list back timer: showing camera and closing face list");
    
    /* Delete the timer */
    lv_timer_del(machine->_face_list_back_timer);
    machine->_face_list_back_timer = nullptr;
    
    /* IMPORTANT: Show camera screen FIRST (switches away from face list) */
    machine->showCameraScreen();
    
    /* THEN close face list screen (now safe to delete) */
    machine->closeFaceListScreen();
}

static void face_list_back_btn_cb(lv_event_t * e)
{
    CoffeeMachine *machine = (CoffeeMachine *)lv_event_get_user_data(e);
    if (machine) {
        ESP_LOGI(TAG, "Face list back button clicked");
        
        /* Schedule back action using timer to avoid deleting current screen during event handling */
        if (!machine->_face_list_back_timer) {
            machine->_face_list_back_timer = lv_timer_create(face_list_back_timer_cb, 50, machine);
            lv_timer_set_repeat_count(machine->_face_list_back_timer, 1);
        }
    }
}

static void face_list_refresh_timer_cb(lv_timer_t * t)
{
    CoffeeMachine *machine = (CoffeeMachine *)t->user_data;
    if (!machine) return;
    
    ESP_LOGI(TAG, "Face list refresh timer: deleting face and refreshing UI");
    
    /* Delete the timer */
    lv_timer_del(machine->_face_list_refresh_timer);
    machine->_face_list_refresh_timer = nullptr;
    
    /* Perform the actual deletion */
    if (machine->_pending_delete_idx >= 0) {
        machine->deleteFaceAtIndex(machine->_pending_delete_idx);
        machine->_pending_delete_idx = -1;
    }
    
    /* Refresh: showFaceListScreen will create new screen and switch to it */
    /* THEN close the old screen (showFaceListScreen handles this internally) */
    machine->showFaceListScreen();
}

static void face_delete_btn_cb(lv_event_t * e)
{
    CoffeeMachine *machine = (CoffeeMachine *)lv_event_get_user_data(e);
    if (!machine) return;
    
    lv_obj_t *btn = lv_event_get_target(e);
    
    /* Find which delete button was clicked */
    for (int i = 0; i < MAX_FACES; i++) {
        if (machine->_face_delete_btns[i] == btn) {
            ESP_LOGI(TAG, "Delete button clicked for face index %d", i);
            
            /* Store the index to delete */
            machine->_pending_delete_idx = i;
            
            /* Schedule refresh using timer to avoid deleting objects during event handling */
            if (!machine->_face_list_refresh_timer) {
                machine->_face_list_refresh_timer = lv_timer_create(face_list_refresh_timer_cb, 50, machine);
                lv_timer_set_repeat_count(machine->_face_list_refresh_timer, 1);
            }
            break;
        }
    }
}

void CoffeeMachine::showFaceListScreen(void)
{
    ESP_LOGI(TAG, "Showing face list screen");
    
    /* Check if we need to recreate the screen */
    bool is_current_screen = (_face_list_screen && lv_scr_act() == _face_list_screen);
    
    /* If it's the current screen, create new one first before deleting */
    if (_face_list_screen && is_current_screen) {
        /* Create new screen first */
        lv_obj_t *old_screen = _face_list_screen;
        _face_list_screen = lv_obj_create(NULL);
        
        /* Load new screen immediately */
        lv_scr_load(_face_list_screen);
        
        /* Delete old screen after switching */
        lv_obj_del(old_screen);
    } else if (_face_list_screen) {
        /* Not current screen, safe to delete directly */
        lv_obj_del(_face_list_screen);
        _face_list_screen = lv_obj_create(NULL);
    } else {
        /* First time creation */
        _face_list_screen = lv_obj_create(NULL);
    }
    lv_obj_clear_flag(_face_list_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_face_list_screen, lv_color_hex(0x1a1a1a), 0);
    
    /* Title label */
    lv_obj_t *title = lv_label_create(_face_list_screen);
    lv_label_set_text(title, "Saved Faces");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    
    /* Show face count info */
    char count_buf[64];
    snprintf(count_buf, sizeof(count_buf), "%d / %d faces stored", _face_count, MAX_FACES);
    lv_obj_t *count_label = lv_label_create(_face_list_screen);
    lv_label_set_text(count_label, count_buf);
    lv_obj_set_style_text_color(count_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(count_label, &lv_font_montserrat_20, 0);
    lv_obj_align(count_label, LV_ALIGN_TOP_MID, 0, 65);
    
    if (_face_count == 0) {
        /* Show "No faces saved" message */
        lv_obj_t *empty_label = lv_label_create(_face_list_screen);
        lv_label_set_text(empty_label, "No faces saved yet.\nUse Face ID to add faces.");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(empty_label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, -30);
    } else {
        /* Create list container with scrolling */
        lv_obj_t *list_cont = lv_obj_create(_face_list_screen);
        lv_obj_set_size(list_cont, _width - 100, 360);
        lv_obj_align(list_cont, LV_ALIGN_CENTER, 0, 20);
        lv_obj_set_style_bg_color(list_cont, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_border_width(list_cont, 2, 0);
        lv_obj_set_style_border_color(list_cont, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(list_cont, 10, 0);
        lv_obj_set_style_pad_all(list_cont, 15, 0);
        lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(list_cont, LV_SCROLLBAR_MODE_AUTO);
        
        /* Create face item rows */
        int display_idx = 0;
        for (int i = 0; i < MAX_FACES; i++) {
            if (_stored_faces[i].is_used) {
                /* Create row container */
                _face_list_items[i] = lv_obj_create(list_cont);
                lv_obj_set_size(_face_list_items[i], lv_pct(100), 90);
                lv_obj_set_style_bg_color(_face_list_items[i], lv_color_hex(0x3a3a3a), 0);
                lv_obj_set_style_border_width(_face_list_items[i], 1, 0);
                lv_obj_set_style_border_color(_face_list_items[i], lv_color_hex(0x555555), 0);
                lv_obj_set_style_radius(_face_list_items[i], 8, 0);
                lv_obj_set_style_pad_all(_face_list_items[i], 10, 0);
                lv_obj_clear_flag(_face_list_items[i], LV_OBJ_FLAG_SCROLLABLE);
                
                /* Face index number (left side) */
                lv_obj_t *idx_label = lv_label_create(_face_list_items[i]);
                char idx_buf[8];
                snprintf(idx_buf, sizeof(idx_buf), "%d", display_idx + 1);
                lv_label_set_text(idx_label, idx_buf);
                lv_obj_set_style_text_color(idx_label, lv_color_hex(0x0080FF), 0);
                lv_obj_set_style_text_font(idx_label, &lv_font_montserrat_32, 0);
                lv_obj_align(idx_label, LV_ALIGN_LEFT_MID, 10, 0);
                
                /* Face name (center) */
                lv_obj_t *name_label = lv_label_create(_face_list_items[i]);
                lv_label_set_text(name_label, _stored_faces[i].name);
                lv_obj_set_style_text_color(name_label, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_text_font(name_label, &lv_font_montserrat_28, 0);
                lv_obj_align(name_label, LV_ALIGN_CENTER, -30, 0);
                
                /* Delete button (right side) */
                _face_delete_btns[i] = lv_btn_create(_face_list_items[i]);
                lv_obj_set_size(_face_delete_btns[i], 120, 60);
                lv_obj_align(_face_delete_btns[i], LV_ALIGN_RIGHT_MID, -10, 0);
                lv_obj_set_style_bg_color(_face_delete_btns[i], lv_color_hex(0xFF0000), 0);
                lv_obj_set_style_radius(_face_delete_btns[i], 8, 0);
                
                lv_obj_t *del_label = lv_label_create(_face_delete_btns[i]);
                lv_label_set_text(del_label, "Delete");
                lv_obj_set_style_text_color(del_label, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_text_font(del_label, &lv_font_montserrat_20, 0);
                lv_obj_center(del_label);
                
                lv_obj_add_event_cb(_face_delete_btns[i], face_delete_btn_cb, LV_EVENT_CLICKED, this);
                
                display_idx++;
            }
        }
    }
    
    /* Back button at the bottom */
    lv_obj_t *back_btn = lv_btn_create(_face_list_screen);
    lv_obj_set_size(back_btn, 200, 70);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x0080FF), 0);
    lv_obj_set_style_radius(back_btn, 15, 0);
    
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_24, 0);
    lv_obj_center(back_label);
    
    lv_obj_add_event_cb(back_btn, face_list_back_btn_cb, LV_EVENT_CLICKED, this);
    
    /* Switch to face list screen if not already loaded */
    if (lv_scr_act() != _face_list_screen) {
        lv_scr_load(_face_list_screen);
    }
    
    ESP_LOGI(TAG, "Face list screen displayed with %d faces", _face_count);
}

void CoffeeMachine::closeFaceListScreen(void)
{
    ESP_LOGI(TAG, "Closing face list screen");
    
    /* Only delete if it's NOT the current active screen */
    if (_face_list_screen) {
        if (lv_scr_act() == _face_list_screen) {
            ESP_LOGW(TAG, "Cannot delete face list screen - it's still active! Switch to another screen first.");
            /* Don't delete it, just mark for deletion later */
        } else {
            /* Safe to delete since it's not active */
            lv_obj_del(_face_list_screen);
            _face_list_screen = nullptr;
            ESP_LOGI(TAG, "Face list screen deleted");
        }
    }
    
    /* Clear item references */
    for (int i = 0; i < MAX_FACES; i++) {
        _face_list_items[i] = nullptr;
        _face_delete_btns[i] = nullptr;
    }
}

void CoffeeMachine::deleteFaceAtIndex(int idx)
{
    if (idx < 0 || idx >= MAX_FACES) {
        ESP_LOGE(TAG, "Invalid face index: %d", idx);
        return;
    }
    
    if (!_stored_faces[idx].is_used) {
        ESP_LOGW(TAG, "Face at index %d is not in use", idx);
        return;
    }
    
    ESP_LOGI(TAG, "Deleting face at index %d: %s", idx, _stored_faces[idx].name);
    
    /* Clear the face data */
    memset(&_stored_faces[idx], 0, sizeof(FaceData));
    _stored_faces[idx].is_used = false;
    _face_count--;
    
    /* Save updated face list to NVS */
    saveFacesToNVS();
    
    ESP_LOGI(TAG, "Face deleted successfully. Remaining faces: %d", _face_count);
}


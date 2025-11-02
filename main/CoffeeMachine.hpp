#pragma once

#include "lvgl.h"
#include "esp_mac.h"
#include "esp_brookesia.hpp"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Forward declarations for camera types */
extern "C" {
    #include "esp_video_init.h"
    #include "camera/app_video.h"
}

/* C++ headers - must be outside extern "C" */
#include "camera/app_humanface_detect.h"
#include <vector>
#include <string>
#include "nvs_flash.h"
#include "nvs.h"

#define MAX_FACES 3
#define FACE_FEATURE_SIZE 128

/* Face recognition data structure */
struct FaceData {
    char name[32];
    float feature[FACE_FEATURE_SIZE];
    bool is_used;
};

class CoffeeMachine
{
public:
    CoffeeMachine();
    ~CoffeeMachine();

    bool init(void);
    void showOverlayForIndex(int idx);
    void showSettingsScreen(void);
    void showMainScreen(void);
    void showCameraScreen(void);
    void closeCameraScreen(void);

    uint16_t _height;
    uint16_t _width;
    
    /* Overlay and timer for making process */
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
    
    /* Settings screen management */
    lv_obj_t *main_screen = nullptr;
    lv_obj_t *settings_screen = nullptr;
    lv_obj_t *settings_btn = nullptr;
    lv_obj_t *back_btn = nullptr;
    
    /* Camera screen management */
    lv_obj_t *camera_screen = nullptr;
    lv_obj_t *camera_canvas = nullptr;
    lv_obj_t *camera_buttons[3] = {nullptr};
    int _camera_ctlr_handle = -1;
    bool _camera_running = false;
    bool _camera_initialized = false;
    uint8_t *_cam_buffer[EXAMPLE_CAM_BUF_NUM] = {nullptr};
    size_t _cam_buffer_size[EXAMPLE_CAM_BUF_NUM] = {0};
    int _cam_buf_count = EXAMPLE_CAM_BUF_NUM;
    lv_img_dsc_t _camera_img_dsc;
    SemaphoreHandle_t _camera_init_sem = nullptr;
    
    /* Face recognition management */
    bool _face_recognition_enabled = false;
    FaceData _stored_faces[MAX_FACES];
    int _face_count = 0;
    HumanFaceDetect *_face_detector = nullptr;
    lv_obj_t *_face_name_screen = nullptr;
    lv_obj_t *_face_name_textarea = nullptr;
    lv_obj_t *_face_name_keyboard = nullptr;
    uint8_t *_captured_face_buffer = nullptr;
    
    /* Face recognition action delay */
    lv_timer_t *_face_action_timer = nullptr;
    int _recognized_face_idx = -1;
    bool _pending_face_action = false;
    
    /* Face list management screen */
    lv_obj_t *_face_list_screen = nullptr;
    lv_obj_t *_face_list_items[MAX_FACES] = {nullptr};
    lv_obj_t *_face_delete_btns[MAX_FACES] = {nullptr};
    lv_timer_t *_face_list_refresh_timer = nullptr;
    lv_timer_t *_face_list_back_timer = nullptr;
    int _pending_delete_idx = -1;
    
    /* Public methods */
    void cleanup_overlay(void);
    void showFaceNameScreen(void);
    void closeFaceNameScreen(void);
    void saveFaceData(const char *name);
    int recognizeFace(const std::list<dl::detect::result_t> &results);
    bool loadFacesFromNVS(void);
    bool saveFacesToNVS(void);
    void showFaceListScreen(void);
    void closeFaceListScreen(void);
    void deleteFaceAtIndex(int idx);
};

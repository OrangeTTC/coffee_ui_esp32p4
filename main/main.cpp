

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "CoffeeMachine.hpp"
#include "esp_mac.h"

#define LVGL_PORT_INIT_CONFIG()   \
    {                             \
        .task_priority = 4,       \
        .task_stack = 10 * 1024,  \
        .task_affinity = -1,      \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }

static const char *TAG = "app_main";

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mount successfully");

    ESP_ERROR_CHECK(bsp_sdcard_mount());
    ESP_LOGI(TAG, "SD card mount successfully");

    ESP_ERROR_CHECK(bsp_eth_init());
    ESP_LOGI(TAG, "Ethernet init successfully");

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hw_cfg =
            {
                .hdmi_resolution = BSP_HDMI_RES_NONE,
                .dsi_bus =
                    {
                        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
                        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
                    },
            },
        .flags =
            {
                .buff_dma = false,
                .buff_spiram = true,
                .sw_rotate = false,
            },
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Starting Coffee Machine UI");
    
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Starting Coffee Machine UI");
    
    
    bsp_display_lock(0);

    
    CoffeeMachine *coffee_machine = new CoffeeMachine();
    assert(coffee_machine != nullptr && "Failed to create coffee machine");
    
    
    assert(coffee_machine->init() && "Failed to initialize coffee machine UI");
    
    ESP_LOGI(TAG, "Coffee Machine UI started successfully");

    
    bsp_display_unlock();

    char buffer[128]; 
    size_t internal_free = 0;
    size_t internal_total = 0;
    size_t external_free = 0;
    size_t external_total = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000)); 
    }
}


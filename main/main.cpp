/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

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
    
    /**
     * To avoid errors caused by multiple tasks simultaneously accessing LVGL,
     * should acquire a lock before operating on LVGL.
     */
    bsp_display_lock(0);

    /* 创建咖啡机UI对象 */
    CoffeeMachine *coffee_machine = new CoffeeMachine();
    assert(coffee_machine != nullptr && "Failed to create coffee machine");
    
    /* 初始化UI */
    assert(coffee_machine->init() && "Failed to initialize coffee machine UI");
    
    ESP_LOGI(TAG, "Coffee Machine UI started successfully");

    /* Release the lock */
    bsp_display_unlock();

    // Safe memory monitoring without heap traversal
    char buffer[128]; /* Make sure buffer is enough for `sprintf` */
    size_t internal_free = 0;
    size_t internal_total = 0;
    size_t external_free = 0;
    size_t external_total = 0;

    while (1) {
        // // Only get basic memory info without traversing heap structures
        // internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        // internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        // external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        // external_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

        // sprintf(buffer,
        //         "   Biggest /     Free /    Total\n"
        //         "\t  SRAM : [%d / %d / %d] KB\n"
        //         "\t PSRAM : [%d / %d / %d] KB",
        //         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024, internal_free / 1024,
        //         internal_total / 1024, heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024, external_free / 1024,
        //         external_total / 1024);
        // ESP_LOGI("MEM", "%s", buffer);

        // // Check for critically low memory
        // if (internal_free < 10 * 1024) {
        //     ESP_LOGW("MEM", "WARNING: Internal memory critically low!");
        // }

        vTaskDelay(pdMS_TO_TICKS(5000)); // Increased delay to reduce overhead
    }
}

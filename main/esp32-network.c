#include <stdio.h>
#include <u8g2_esp32_hal.h>
#include <soc/gpio_num.h>
#include <iot_button.h>
#include <button_gpio.h>
#include <wifi_provisioner.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// 按键配置
#define BUTTON_GPIO_NUM     GPIO_NUM_21      // 根据你的开发板修改（ESP32-S2-Saola-1 的 BOOT 键是 GPIO0）
#define BUTTON_ACTIVE_LEVEL 1      // 0=按下低电平，1=按下高电平

static u8g2_t u8g2;
static wifi_prov_config_t wf_config = WIFI_PROV_DEFAULT_CONFIG();

static const char *TAG = "BUTTON";

// 回调函数
static void button_single_click_cb(void *arg, void *usr_data)
{
    static int started = 0;
    esp_err_t err;
    if (!started)
    {
        ESP_LOGI(TAG, "Starting button");
        err = wifi_prov_start(&wf_config);
    } else
    {
        ESP_LOGI(TAG, "Stop button");
        err = wifi_prov_stop();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "%s failed, err = %d", started ? "wifi_prov_stop": "wifi_prov_start" , err);
    } else
    {
        started = !started;
    }
}

static void button_double_click_cb(void *arg, void *usr_data)
{
    ESP_LOGI(TAG, "双击事件！");
    u8g2_ClearBuffer(&u8g2);
    u8g2_DrawStr(&u8g2, 0, 20, "Dual click");
    u8g2_SendBuffer(&u8g2);
}

static void button_long_press_cb(void *arg, void *usr_data)
{
    ESP_LOGI(TAG, "长按事件！");
    u8g2_ClearBuffer(&u8g2);
    u8g2_DrawStr(&u8g2, 0, 20, "Long press");
    u8g2_SendBuffer(&u8g2);
}


static void init_u8g2()
{
    u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
    u8g2_esp32_hal.bus.i2c.sda = GPIO_NUM_8;
    u8g2_esp32_hal.bus.i2c.scl = GPIO_NUM_9;
    u8g2_esp32_hal_init(u8g2_esp32_hal);
    u8x8_SetI2CAddress(&u8g2.u8x8, 0x78);
    u8g2_Setup_sh1106_i2c_128x64_noname_f(&u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
    u8g2_SetFont(&u8g2, u8g2_font_5x7_t_cyrillic );
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_ClearDisplay(&u8g2);
}

static void on_connected(void)
{
    ESP_LOGI(TAG, "WiFi connected!");
}

static void on_portal_start(void)
{
    ESP_LOGI(TAG, "Captive portal started — connect to the AP to configure WiFi.");
}

void app_main(void)
{
    init_u8g2();
    // 1. 配置按钮时间参数（可选）
    const button_config_t btn_cfg = {
        .long_press_time = 3000,      // 长按触发时间：3秒
        .short_press_time = 100,      // 短按消抖时间：200ms
    };

    // 2. 配置 GPIO
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = BUTTON_GPIO_NUM,
        .active_level = BUTTON_ACTIVE_LEVEL,
        .disable_pull = false,        // 启用内部上拉/下拉
    };

    // 3. 创建按键设备
    button_handle_t btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &btn));

    // 4. 注册事件回调
    iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, button_single_click_cb, NULL);
    iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, button_double_click_cb, NULL);
    iot_button_register_cb(btn, BUTTON_LONG_PRESS_UP, NULL, button_long_press_cb, NULL);

    ESP_LOGI(TAG, "按键示例已启动，按下 GPIO%d 试试", BUTTON_GPIO_NUM);
    wf_config.ap_ssid        = "MyDevice-Setup";
    wf_config.on_connected   = on_connected;
    wf_config.on_portal_start = on_portal_start;

    // 主循环保持运行
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
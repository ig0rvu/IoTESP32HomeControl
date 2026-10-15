#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wifi.h"
#include "tcp_client.h"
#include "gpio.h"
#include "rfid.h"
#include "ap_mode.h"

static const char *TAG = "main";

// Reads the same "pins" NVS key that ap_mode.c writes (compact "gpio:FUNCTION,..."
// string built from the AP config form) and parses it straight into a gpio_pin_list_t.
bool load_pins_from_nvs(gpio_pin_list_t *out)
{
    char pins_str[512] = {0};
    nvs_handle_t handle;
    if (nvs_open("wifi_cfg", NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(pins_str);
        nvs_get_str(handle, "pins", pins_str, &len);
        nvs_close(handle);
    }
    return gpio_pin_list_from_string(pins_str, out);
}
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    if(!ap_mode_has_wifi_credentials())
    {
        ESP_LOGI(TAG, "No WiFi credentials found, starting AP mode...");
        ap_mode_start();
        return;
    }
    ESP_LOGI(TAG, "Starting WiFi STA...");
    wifi_init_sta();
    gpio_pin_list_t cfg;
    load_pins_from_nvs(&cfg);
    gpio_ctrl_init(&cfg);
    // 16384, not 8192: register_controller()/replace_controller() each hold two full
    // controlhome_Envelope locals on the stack, and that struct (sized to its largest
    // oneof variant, RegisterController.pins[20]) grew by roughly 1KB once PinConfig
    // gained a 50-byte name[] field -- 8192 was enough before that, not after (crashed
    // deep in lwIP/FreeRTOS internals the moment register_controller() called send()).
    xTaskCreate(device_task, "device_task", 16384, NULL, 5, NULL);
    rfid_init();
}

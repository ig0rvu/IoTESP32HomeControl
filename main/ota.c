#include "ota.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

static void ota_task(void *pvParameters)
{
    char *url = (char *)pvParameters;

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Starting OTA download from %s", url);
    esp_err_t err = esp_https_ota(&ota_config);
    if (err == ESP_OK) {
        // esp_https_ota only returns ESP_OK after the new image is fully
        // written and its header validated (chip id, magic byte) -- safe to
        // commit to it. esp_ota_mark_app_valid_cancel_rollback() in app_main
        // is what protects against THIS image failing to come up, not this one.
        ESP_LOGI(TAG, "OTA succeeded, rebooting into new firmware");
        free(url);
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s -- staying on current firmware", esp_err_to_name(err));
    free(url);
    vTaskDelete(NULL);
}

void ota_start_update(const char *url)
{
    char *url_copy = strdup(url);
    if (url_copy == NULL) {
        ESP_LOGE(TAG, "ota_start_update: strdup failed, out of memory");
        return;
    }
    // Separate task: the download can take several seconds and must not
    // block command_task's read loop (or any other traffic on that socket).
    if (xTaskCreate(ota_task, "ota_task", 8192, url_copy, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ota_start_update: failed to create ota_task");
        free(url_copy);
    }
}

bool ota_is_newer_version(const char *candidate, const char *current_version)
{
    int c[3] = {0, 0, 0};
    int v[3] = {0, 0, 0};
    // Missing trailing segments in either string just stay 0 from the
    // initializer above -- sscanf simply won't fill them.
    sscanf(candidate, "%d.%d.%d", &c[0], &c[1], &c[2]);
    sscanf(current_version, "%d.%d.%d", &v[0], &v[1], &v[2]);

    for (int i = 0; i < 3; i++) {
        if (c[i] != v[i]) {
            return c[i] > v[i];
        }
    }
    return false; // equal
}

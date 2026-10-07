#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"


#define WIFI_MAX_RETRY 5
#define WIFI_RETRY_COUNT_KEY "retry_count"

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_event_group;


#define WIFI_CONNECTED_BIT BIT0
#define WIFI_RESTART_BIT BIT1
#define WIFI_AUTH_FAIL_BIT BIT2

static int s_retry_num = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) 
    {
        wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE(TAG, "disconnected, reason: %d", disconn->reason);
        bool definitely_bad_credentials = 
            disconn->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            disconn->reason == WIFI_REASON_NO_AP_FOUND ||
            disconn->reason == WIFI_REASON_AUTH_FAIL ||
            disconn->reason == WIFI_REASON_ASSOC_FAIL ||
            disconn->reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
        uint8_t retry_count = 0;
        nvs_handle_t handle;
        if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_get_u8(handle, WIFI_RETRY_COUNT_KEY, &retry_count);
            retry_count++;
            nvs_set_u8(handle, WIFI_RETRY_COUNT_KEY, retry_count);
            nvs_commit(handle);
            nvs_close(handle);
        }

        if(!definitely_bad_credentials && retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } 
        else if(disconn->reason == definitely_bad_credentials || retry_count >= WIFI_MAX_RETRY)
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_AUTH_FAIL_BIT);
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_RESTART_BIT);
        }
       
       
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        nvs_handle_t handle;
        if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u8(handle, WIFI_RETRY_COUNT_KEY, 0);
            nvs_commit(handle);
            nvs_close(handle);
        }
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        ESP_LOGI(TAG, "MAC address: " MACSTR, MAC2STR(mac));

        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
static void restart_task(void *pv)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_RESTART_BIT | WIFI_AUTH_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if(bits & WIFI_AUTH_FAIL_BIT){
        ESP_LOGE(TAG, "Authentication failed. Clearing stored credentials.");
        nvs_handle_t handle;
        if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) 
        {
            nvs_erase_key(handle, WIFI_NVS_KEY_SSID);
            nvs_erase_key(handle, WIFI_NVS_KEY_PASSWORD);
            nvs_commit(handle);
            nvs_close(handle);
        }
        ESP_LOGI(TAG, "Wifi credentials cleared. Restarting to AP mode...");
    }
    else
    {
        ESP_LOGI(TAG, "Restarting device...");
    }   
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    xTaskCreate(restart_task, "wifi_restart", 2048, NULL, 5, NULL);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));
    
    char ssid[33] = {0};
    char password[65] = {0};

    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle));

    size_t ssid_len = sizeof(ssid);
    ESP_ERROR_CHECK(nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &ssid_len));

    size_t pass_len = sizeof(password);
    nvs_get_str(handle, WIFI_NVS_KEY_PASSWORD, password, &pass_len);

    nvs_close(handle);

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_sta finished, connecting to SSID:%s", ssid);

    xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY
    );
    ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);   
}

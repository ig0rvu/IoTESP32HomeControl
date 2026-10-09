#include "ap_mode.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define NVS_KEY_SERVER_IP "server_ip"
#define NVS_KEY_MODE "mode"
#define NVS_KEY_PINS "pins"
#define NVS_KEY_REPLACE_ID "replace_id"
#define NVS_KEY_CTRL_ID "ctrl_id"
#define NVS_KEY_CTRL_DESC "ctrl_desc"


#define SAVE_FORM_BUF_SIZE 256

#define LED_GPIO_PIN 15

// httpd_query_key_value() only splits "key=value&key=value" pairs -- it does NOT
// decode %XX escapes or '+' that the browser applies to reserved characters
// (':', ',', spaces, ...) in application/x-www-form-urlencoded bodies. Decode in
// place; the decoded string is always shorter than or equal to the input.
static void url_decode(char *str)
{
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static const char *TAG = "ap_mode";
extern const char ap_config_html_start[] asm("_binary_ap_config_html_start");
extern const char ap_config_html_end[]   asm("_binary_ap_config_html_end");
extern const char pins_json_start[] asm("_binary_pins_json_start");
extern const char pins_json_end[]   asm("_binary_pins_json_end");

bool ap_mode_has_wifi_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) 
    {
        ESP_LOGI(TAG, "NVS namespace not found, no credentials yet: %s", NVS_NAMESPACE);
        return false;
    }

    size_t required_size = 0;
    err = nvs_get_str(handle, NVS_KEY_SSID, NULL, &required_size);
    nvs_close(handle);

    if (err != ESP_OK  || required_size <= 1) 
    {
        ESP_LOGI(TAG, "No stored SSID found in NVS");
        return false;
    } 
    return true;    
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    // EMBED_TXTFILES null-terminates the embedded content, so _end - _start
    // is one byte longer than the real file -- trim that trailing '\0' off,
    // it has no business being part of the HTTP response body.
    size_t html_len = ap_config_html_end - ap_config_html_start - 1;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ap_config_html_start, html_len);
    return ESP_OK;
}

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
};

static esp_err_t pins_json_get_handler(httpd_req_t *req)
{
    // Same trailing-'\0' trim as root_get_handler -- JSON.parse() is strict
    // and chokes on the extra byte, unlike HTML rendering which just ignores it.
    size_t json_len = pins_json_end - pins_json_start - 1;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, pins_json_start, json_len);
    return ESP_OK;
}

static const httpd_uri_t pins_json_uri = {
    .uri = "/pins.json",
    .method = HTTP_GET,
    .handler = pins_json_get_handler,
};


static esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[SAVE_FORM_BUF_SIZE];
    int remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form data too large");
        return ESP_FAIL;
    }

    int received = 0;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received, remaining);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request");
            return ESP_FAIL;
        }
        received += ret;
        remaining -= ret;
    }
    buf[received] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    char server_ip[16] = {0};

    if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }
    url_decode(ssid);
    httpd_query_key_value(buf, "password", password, sizeof(password));
    url_decode(password);
    httpd_query_key_value(buf, "server_ip", server_ip, sizeof(server_ip));
    url_decode(server_ip);

    char mode[16] = {0};
    httpd_query_key_value(buf, "mode", mode, sizeof(mode));
    url_decode(mode);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed");
        return ESP_FAIL;
    }

    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASSWORD, password);
    nvs_set_str(handle, NVS_KEY_SERVER_IP, server_ip);
    nvs_set_str(handle, NVS_KEY_MODE, mode);

    if (strcmp(mode, "new") == 0) {

        char ctrl_id[64] = {0};
        char ctrl_desc[128] = {0};
        httpd_query_key_value(buf, "ctrl_id", ctrl_id, sizeof(ctrl_id));
        url_decode(ctrl_id);
        httpd_query_key_value(buf, "ctrl_description", ctrl_desc, sizeof(ctrl_desc));
        url_decode(ctrl_desc);
        nvs_set_str(handle, NVS_KEY_CTRL_ID, ctrl_id);
        nvs_set_str(handle, NVS_KEY_CTRL_DESC, ctrl_desc);

        char pins[512] = {0};
        httpd_query_key_value(buf, "pins", pins, sizeof(pins));
        url_decode(pins);
        nvs_set_str(handle, NVS_KEY_PINS, pins);
        ESP_LOGI(TAG, "Saved new controller pins: %s", pins);

    } else if (strcmp(mode, "replace") == 0) {
        char replace_id[64] = {0};
        httpd_query_key_value(buf, "replace_id", replace_id, sizeof(replace_id));
        url_decode(replace_id);
        nvs_set_str(handle, NVS_KEY_REPLACE_ID, replace_id);
        ESP_LOGI(TAG, "Saved replace target: %s", replace_id);
    }

    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved WiFi credentials, SSID=%s, server_ip=%s", ssid, server_ip);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><h1>Sacuvano. Restartovanje...</h1></body></html>");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static const httpd_uri_t save_uri = {
    .uri = "/save",
    .method = HTTP_POST,
    .handler = save_post_handler,
};



static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &pins_json_uri);
        httpd_register_uri_handler(server, &save_uri);
        ESP_LOGI(TAG, "HTTP server started");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

static void led_timer_callback(void* arg)
{
    static bool led_state = false;
    led_state = !led_state;
    gpio_set_level(LED_GPIO_PIN, led_state);
        
}  

void ap_mode_start(void)
{
    gpio_reset_pin(LED_GPIO_PIN);
    gpio_set_direction(LED_GPIO_PIN, GPIO_MODE_OUTPUT); 
    gpio_set_level(LED_GPIO_PIN, 1);  // Turn off LED initially
    const esp_timer_create_args_t led_timer_args = {
        .callback = led_timer_callback,
        .name = "led_blink_timer"
    };
    
    esp_timer_handle_t led_timer;
    esp_timer_create(&led_timer_args, &led_timer);
    esp_timer_start_periodic(led_timer, 2000000);  // 500 ms
    
    ESP_LOGI(TAG, "LED blinking timer started");
    
    ESP_LOGI(TAG, "Starting AP mode for configuration...");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "HomeControl-Setup-%02X%02X", mac[4], mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ssid),
            .channel = 1,
            .password = "temp12345",   // privremeno, HMAC lozinka dolazi kasnije
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(wifi_config.ap.ssid, ssid, strlen(ssid));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started, SSID: %s", ssid);
    start_http_server();
    
}


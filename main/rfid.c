#include "rfid.h"
#include  "driver/uart.h"
#include "esp_log.h"
#include "tcp_client.h"
#include "string.h"
#include "stdint.h"
#include "stdbool.h"
#include "stdlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#define RFID_UART_NUM      UART_NUM_1
#define RFID_BAUD_RATE     9600
#define RFID_FRAME_LEN      14
#define RFID_STX          0x02
#define RFID_ETX          0x03

const char *TAG = "rfid";

static uint32_t s_rfid_rx_pin = 0;

uint32_t rfid_get_rx_pin(void)
{
    return s_rfid_rx_pin;
}

// Scans the same "pins" NVS string the AP config form writes for the one entry
// tagged RFID_RX (at most one is supported). Returns 0 if none is configured.
static uint32_t find_rfid_pin_from_nvs(void)
{
    char pins_str[512] = {0};
    nvs_handle_t handle;
    if (nvs_open("wifi_cfg", NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(pins_str);
        nvs_get_str(handle, "pins", pins_str, &len);
        nvs_close(handle);
    }

    char *token = strtok(pins_str, ",");
    while (token != NULL) {
        char *colon = strchr(token, ':');
        if (colon != NULL && strcmp(colon + 1, "RFID_RX") == 0) {
            *colon = '\0';
            return (uint32_t)atoi(token);
        }
        token = strtok(NULL, ",");
    }
    return 0;
}

static void rfid_uart_init(uint32_t rx_pin)
{
    uart_config_t uart_config = {
        .baud_rate = RFID_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(RFID_UART_NUM, &uart_config);
    uart_set_pin(RFID_UART_NUM, UART_PIN_NO_CHANGE, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uart_driver_install(RFID_UART_NUM, 256, 0, 0, NULL, 0);
}

static int hex_nibble (char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    } else if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    } else {
        return -1;
    }
}

static bool hex_pair_to_byte(const char *hex_pair, uint8_t *byte)
{
    int high_nibble = hex_nibble(hex_pair[0]);
    int low_nibble = hex_nibble(hex_pair[1]);
    if (high_nibble < 0 || low_nibble < 0) {
        return false;
    }
    *byte = (high_nibble << 4) | low_nibble;
    return true;
}

static bool validate_and_extract(const uint8_t *frame, char *tag_id_out, size_t tag_id_out_size)
{
    if (frame[0] != RFID_STX || frame[13] != RFID_ETX) {
        ESP_LOGW(TAG, "bad framing");
        return false;
    }

    uint8_t data_bytes[5];
    for (int i = 0; i < 5; i++) {
        if (!hex_pair_to_byte((const char *)&frame[1 + i * 2], &data_bytes[i])) {
            ESP_LOGW(TAG, "bad hex in data");
            return false;
        }
    }

    uint8_t checksum;
    if (!hex_pair_to_byte((const char *)&frame[11], &checksum)) {
        ESP_LOGW(TAG, "bad hex in checksum");
        return false;
    }

    uint8_t computed = 0;
    for (int i = 0; i < 5; i++) {
        computed ^= data_bytes[i];
    }
    if (computed != checksum) {
        ESP_LOGW(TAG, "checksum mismatch: got %02X expected %02X", checksum, computed);
        return false;
    }

    size_t n = (tag_id_out_size - 1 < 10) ? (tag_id_out_size - 1) : 10;
    memcpy(tag_id_out, &frame[1], n);
    tag_id_out[n] = '\0';
    return true;
}

void rfid_task(void *pvParameters)
{
    s_rfid_rx_pin = find_rfid_pin_from_nvs();
    if (s_rfid_rx_pin == 0) {
        ESP_LOGW(TAG, "No RFID_RX pin configured, rfid_task exiting");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "RFID reader active on gpio=%u", (unsigned)s_rfid_rx_pin);

    rfid_uart_init(s_rfid_rx_pin);
    static char s_last_tag[16] = {0};
    static int64_t last_sent_time_us = 0;
    
    #define RFID_SEND_INTERVAL_US 5000000


    uint8_t frame[RFID_FRAME_LEN];
    size_t frame_pos = 0;
    bool in_frame = false;

    while (1) {
        uint8_t byte;
        int len = uart_read_bytes(RFID_UART_NUM, &byte, 1, pdMS_TO_TICKS(100));
        
        if (len <= 0) {
            continue;
        }
            ESP_LOGI(TAG, "raw byte: 0x%02X", byte);
        if (!in_frame) {
            if (byte == RFID_STX) {
                in_frame = true;
                frame_pos = 0;
                frame[frame_pos++] = byte;
            }
            continue;
        }
        
        frame[frame_pos++] = byte;
        if (frame_pos == RFID_FRAME_LEN) 
        {
            char tag_id[16];
            if (validate_and_extract(frame, tag_id, sizeof(tag_id))) 
            {
                int64_t now_us = esp_timer_get_time();
                bool is_duplicate = (strcmp(tag_id, s_last_tag) == 0);
                if(is_duplicate && (now_us - last_sent_time_us < RFID_SEND_INTERVAL_US)) 
                {
                    ESP_LOGI(TAG, "Duplicate tag read too soon: %s", tag_id);
                } else 
                {
                    strcpy(s_last_tag, tag_id);
                    last_sent_time_us = now_us;
                if(strcmp(tag_id,s_last_tag)==0)
                ESP_LOGI(TAG, "tag read: %s", tag_id);
                tcp_client_send_rfid(tag_id);
                }
            }
            in_frame = false;
            frame_pos = 0;
        }
    }
}
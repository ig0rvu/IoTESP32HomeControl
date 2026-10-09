#include <string.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "controlhome.pb.h"
#include "tcp_client.h"
#include "gpio.h"
#include "rfid.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdlib.h>

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SERVER_IP "server_ip"
#define NVS_KEY_MODE "mode"
#define NVS_KEY_CTRL_ID "ctrl_id"
#define NVS_KEY_CTRL_DESC "ctrl_desc"
#define NVS_KEY_PINS "pins"
#define NVS_KEY_REPLACE_ID "replace_id"
#define MAX_REGISTER_PINS 20


// Fallback only -- the real value comes from NVS ("server_ip", set via the
// AP-mode config form alongside ssid/password). This covers devices that were
// provisioned before that field existed.
#define SERVER_IP_DEFAULT     "192.168.0.32"
#define SERVER_PORT           9000
#define MAX_FRAME_BYTES       512
#define RECONNECT_DELAY_MS    5000
// Fallback-only now: IoReport is sent on an actual pin-change interrupt (see
// gpio_ctrl_notify_task) or an explicit RequestStatus from the server. This
// interval only governs how often a lightweight Heartbeat goes out when
// NEITHER of those has happened, so DeviceLastSeenTracker on the server
// doesn't go stale on a quiet-but-healthy connection.
#define HEARTBEAT_INTERVAL_MS 300000

static const char *TAG = "tcp_client";

typedef struct {
    int sock;
    SemaphoreHandle_t write_lock;
    char mac[18];
    volatile bool connection_alive;
} connection_ctx_t;

static connection_ctx_t *volatile s_active_connection = NULL;

// strncpy doesn't guarantee null-termination if src is >= dest_size; nanopb's
// fixed char[] fields need to always be terminated since we treat them as C strings.
static void safe_strcpy(char *dest, size_t dest_size, const char *src)
{
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}
static size_t parse_pins_string(const char *pins_str, controlhome_PinConfig *out, size_t max_count)
{
    size_t count = 0;
    char buf[512];
    strncpy(buf, pins_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, ",");
    while (token != NULL && count < max_count) {
        char *colon = strchr(token, ':');
        if (colon != NULL) {
            *colon = '\0';
            out[count].gpio = (uint32_t)atoi(token);
            const char *func_str = colon + 1;

            if (strcmp(func_str, "GPIO_IN") == 0) {
                out[count].direction = controlhome_PinDirection_PIN_DIRECTION_IN;
                out[count].function = controlhome_PinFunction_PIN_FUNCTION_GPIO;
            } else if (strcmp(func_str, "GPIO_OUT") == 0) {
                out[count].direction = controlhome_PinDirection_PIN_DIRECTION_OUT;
                out[count].function = controlhome_PinFunction_PIN_FUNCTION_GPIO;
            } else if (strncmp(func_str, "SPI_", 4) == 0) {
                out[count].direction = controlhome_PinDirection_PIN_DIRECTION_BIDIRECTIONAL;
                out[count].function = controlhome_PinFunction_PIN_FUNCTION_SPI;
            } else if (strncmp(func_str, "I2C_", 4) == 0) {
                out[count].direction = controlhome_PinDirection_PIN_DIRECTION_BIDIRECTIONAL;
                out[count].function = controlhome_PinFunction_PIN_FUNCTION_I2C;
            } else if (strcmp(func_str, "RFID_RX") == 0) {
                out[count].direction = controlhome_PinDirection_PIN_DIRECTION_IN;
                out[count].function = controlhome_PinFunction_PIN_FUNCTION_UART;
            }
            count++;
        }
        token = strtok(NULL, ",");
    }
    return count;
}

// Reverse of parse_pins_string's function tagging. SPI/I2C's specific role (SCK vs
// MOSI vs ...) isn't carried on the wire by PinConfig, but it's fully determined by
// the fixed pin number on this board, so it's reconstructed from gpio alone. RFID_RX
// has no such fixed pin -- it's identified purely by function, so it's handled
// separately in build_pins_string() rather than through this gpio-based lookup.
static const char *resolve_special_function(uint32_t gpio)
{
    switch (gpio) {
        case 7:  return "SPI_SCK";
        case 9:  return "SPI_MISO";
        case 11: return "SPI_MOSI";
        case 12: return "SPI_SS";
        case 33: return "I2C_SDA";
        case 35: return "I2C_SCL";
        default: return NULL;
    }
}

static void build_pins_string(const controlhome_PinConfig *pins, size_t count, char *out, size_t out_size)
{
    size_t pos = 0;
    out[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        const char *func_str;
        if (pins[i].function == controlhome_PinFunction_PIN_FUNCTION_GPIO) {
            func_str = (pins[i].direction == controlhome_PinDirection_PIN_DIRECTION_OUT) ? "GPIO_OUT" : "GPIO_IN";
        } else if (pins[i].function == controlhome_PinFunction_PIN_FUNCTION_UART) {
            func_str = "RFID_RX";
        } else {
            func_str = resolve_special_function(pins[i].gpio);
            if (func_str == NULL) {
                func_str = "GPIO_IN";
            }
        }
        int written = snprintf(out + pos, out_size - pos, "%s%u:%s", (i > 0 ? "," : ""), (unsigned)pins[i].gpio, func_str);
        if (written > 0 && (size_t)written < out_size - pos) {
            pos += (size_t)written;
        }
    }
}

static void get_mac_string(char *out, size_t out_size)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int read_exact(int sock, uint8_t *buf, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        int r = recv(sock, buf + offset, len - offset, 0);
        if (r <= 0) {
            return -1;
        }
        offset += (size_t)r;
    }
    return 0;
}

static int send_envelope(int sock, const controlhome_Envelope *env)
{
    uint8_t buf[MAX_FRAME_BYTES];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, controlhome_Envelope_fields, env)) {
        ESP_LOGE(TAG, "pb_encode failed: %s", PB_GET_ERROR(&stream));
        return -1;
    }

    uint8_t len_prefix[4] = {
        (uint8_t)((stream.bytes_written >> 24) & 0xFF),
        (uint8_t)((stream.bytes_written >> 16) & 0xFF),
        (uint8_t)((stream.bytes_written >> 8) & 0xFF),
        (uint8_t)(stream.bytes_written & 0xFF),
    };

    if (send(sock, len_prefix, 4, 0) != 4) {
        return -1;
    }
    if (send(sock, buf, stream.bytes_written, 0) != (int)stream.bytes_written) {
        return -1;
    }
    return 0;
}

static int read_envelope(int sock, controlhome_Envelope *env)
{
    uint8_t len_prefix[4];
    if (read_exact(sock, len_prefix, 4) != 0) {
        return -1;
    }
    uint32_t len = ((uint32_t)len_prefix[0] << 24) | ((uint32_t)len_prefix[1] << 16) |
                   ((uint32_t)len_prefix[2] << 8) | len_prefix[3];
    if (len == 0 || len > MAX_FRAME_BYTES) {
        ESP_LOGE(TAG, "invalid frame length: %u", (unsigned)len);
        return -1;
    }

    uint8_t buf[MAX_FRAME_BYTES];
    if (read_exact(sock, buf, len) != 0) {
        return -1;
    }

    pb_istream_t stream = pb_istream_from_buffer(buf, len);
    *env = (controlhome_Envelope)controlhome_Envelope_init_zero;
    if (!pb_decode(&stream, controlhome_Envelope_fields, env)) {
        ESP_LOGE(TAG, "pb_decode failed: %s", PB_GET_ERROR(&stream));
        return -1;
    }
    return 0;
}

// Sends RegisterController and waits for the Ack. Treats both 200 (registered)
// and 409 (already exists) as success -- see design note: for now, without the
// AP config phase built yet, we register once per TCP connection unconditionally.
static bool register_controller(int sock, const char *mac, const char *id, const char *description,
                                 const controlhome_PinConfig *pins, size_t pins_count)
{
    controlhome_Envelope env = (controlhome_Envelope)controlhome_Envelope_init_zero;
    safe_strcpy(env.mac_address, sizeof(env.mac_address), mac);
    env.which_payload = controlhome_Envelope_register_controller_tag;
    safe_strcpy(env.payload.register_controller.id, sizeof(env.payload.register_controller.id), id);
    safe_strcpy(env.payload.register_controller.description, sizeof(env.payload.register_controller.description), description);

    env.payload.register_controller.pins_count = pins_count;
    for (size_t i = 0; i < pins_count; i++) {
        env.payload.register_controller.pins[i] = pins[i];
    }
    if (send_envelope(sock, &env) != 0) {
        return false;
    }

    controlhome_Envelope response;
    if (read_envelope(sock, &response) != 0) {
        return false;
    }

    if (response.which_payload == controlhome_Envelope_ack_tag) {
        int status = response.payload.ack.status_code;
        ESP_LOGI(TAG, "RegisterController ack: status=%d message='%s'", status, response.payload.ack.message);
        return (status == 200 || status == 409);
    }

    ESP_LOGW(TAG, "RegisterController: unexpected response payload case %d", (int)response.which_payload);
    return false;
}

// Sends ReplaceController and waits for either ControllerConfig (success, carries
// the pins already stored in the DB for idcontroller) or Ack (failure, e.g. 404).
static bool replace_controller(int sock, const char *mac, const char *idcontroller, controlhome_ControllerConfig *out_config)
{
    controlhome_Envelope env = (controlhome_Envelope)controlhome_Envelope_init_zero;
    safe_strcpy(env.mac_address, sizeof(env.mac_address), mac);
    env.which_payload = controlhome_Envelope_replace_controller_tag;
    safe_strcpy(env.payload.replace_controller.idcontroller, sizeof(env.payload.replace_controller.idcontroller), idcontroller);

    if (send_envelope(sock, &env) != 0) {
        return false;
    }

    controlhome_Envelope response;
    if (read_envelope(sock, &response) != 0) {
        return false;
    }

    if (response.which_payload == controlhome_Envelope_controller_config_tag) {
        *out_config = response.payload.controller_config;
        ESP_LOGI(TAG, "ReplaceController succeeded: %s", response.payload.controller_config.message);
        return true;
    }

    if (response.which_payload == controlhome_Envelope_ack_tag) {
        ESP_LOGE(TAG, "ReplaceController failed: status=%d message='%s'",
                 (int)response.payload.ack.status_code, response.payload.ack.message);
        return false;
    }

    ESP_LOGW(TAG, "ReplaceController: unexpected response payload case %d", (int)response.which_payload);
    return false;
}

static controlhome_IoReport build_ioreport(void)
{
    controlhome_IoReport report = (controlhome_IoReport)controlhome_IoReport_init_zero;

    report.digital_inputs_count = gpio_in_pin_count;
    for (size_t i = 0; i < report.digital_inputs_count; i++) {
        report.digital_inputs[i].gpio = gpio_in_pins[i];
        report.digital_inputs[i].value = gpio_ctrl_read(gpio_in_pins[i]);
        ESP_LOGI(TAG, "Digital input gpio=%u value=%d", (unsigned int)gpio_in_pins[i], (int)report.digital_inputs[i].value);
    }

    report.digital_outputs_count = gpio_out_pin_count;
    for (size_t i = 0; i < report.digital_outputs_count; i++) {
        report.digital_outputs[i].gpio = gpio_out_pins[i];
        report.digital_outputs[i].value = gpio_ctrl_read(gpio_out_pins[i]);
        ESP_LOGI(TAG, "Digital output gpio=%u value=%d", (unsigned int)gpio_out_pins[i], (int)report.digital_outputs[i].value);
    }
    
    return report;
}


// Owns ALL reading from the socket for the lifetime of one connection. Handles
// Ack (just logs it) and Command (executes it, then sends back a confirmation
// Ack so the server can log what was actually done). On any read error, marks
// the shared connection as dead so device_task's send loop stops too, and exits.
static void command_task(void *pvParameters)
{
    connection_ctx_t *ctx = (connection_ctx_t *)pvParameters;
    
    while (ctx->connection_alive) {
        controlhome_Envelope response;
        if (read_envelope(ctx->sock, &response) != 0) {
            ctx->connection_alive = false;
            break;
        }

        if (response.which_payload == controlhome_Envelope_ack_tag) {
            ESP_LOGD(TAG, "Ack received: status=%u", (unsigned int)response.payload.ack.status_code);
        } else if (response.which_payload == controlhome_Envelope_command_tag) {
            
            uint32_t gpio = response.payload.command.gpio;
            char action[32];
           
            safe_strcpy(action, sizeof(action), response.payload.command.action);
            uint32_t duration = response.payload.command.duration;
            ESP_LOGI(TAG, "Received Command: gpio=%u action=%s duration=%u", (unsigned int)gpio, action, (unsigned int)duration);
            if(strcmp(action, "off") == 0) {
                gpio_ctrl_set_output(gpio, false);
            } else if(strcmp(action, "on") == 0) {
                gpio_ctrl_set_output(gpio, true);
            } else if(strcmp(action, "toggle") == 0) {
                bool current_value = gpio_ctrl_read(gpio);
                gpio_ctrl_set_output(gpio, !current_value);
            } else if(strcmp(action, "pb") == 0) {
                uint32_t pulse_duration = duration > 0 ? duration : 2000; // default to 2 seconds if duration is 0            
                gpio_ctrl_set_output(gpio, true);
                vTaskDelay(pdMS_TO_TICKS(pulse_duration));
                gpio_ctrl_set_output(gpio, false);
            } else {
                ESP_LOGW(TAG, "Unknown action: %s", action);
            }

            
            controlhome_Envelope ack = (controlhome_Envelope)controlhome_Envelope_init_zero;
            safe_strcpy(ack.mac_address, sizeof(ack.mac_address), ctx->mac);
            ack.which_payload = controlhome_Envelope_ack_tag;
            ack.payload.ack.status_code = 200;
            snprintf(ack.payload.ack.message, sizeof(ack.payload.ack.message),
                     "Command executed: gpio=%u action=%s", (unsigned int)gpio, action);
            if(ctx->connection_alive) {
                xSemaphoreTake(ctx->write_lock, portMAX_DELAY);
                send_envelope(ctx->sock, &ack);
                xSemaphoreGive(ctx->write_lock);
                ESP_LOGI(TAG, "Sending Ack: status=%li message='%s'", ack.payload.ack.status_code, ack.payload.ack.message);
            } else {
                ESP_LOGW(TAG, "Connection lost before sending Ack");
                break;
            }
            
        } else if (response.which_payload == controlhome_Envelope_request_status_tag) {
            ESP_LOGI(TAG, "RequestStatus received, sending immediate IoReport");
            controlhome_Envelope env = (controlhome_Envelope)controlhome_Envelope_init_zero;
            safe_strcpy(env.mac_address, sizeof(env.mac_address), ctx->mac);
            env.which_payload = controlhome_Envelope_io_report_tag;
            env.payload.io_report = build_ioreport();

            if (ctx->connection_alive) {
                xSemaphoreTake(ctx->write_lock, portMAX_DELAY);
                send_envelope(ctx->sock, &env);
                xSemaphoreGive(ctx->write_lock);
            }
        } else {
            ESP_LOGW(TAG, "command_task: unexpected payload case %d", (int)response.which_payload);
        }
    }

    ESP_LOGW(TAG, "command_task: connection lost, exiting");
    vTaskDelete(NULL);
}

void device_task(void *pvParameters)
{
    char mac_str[18];
    
    get_mac_string(mac_str, sizeof(mac_str));
    gpio_ctrl_notify_task(xTaskGetCurrentTaskHandle());

    ESP_LOGI(TAG, "Device MAC: %s", mac_str);

    char mode[16] = {0};
    char ctrl_id[64] = {0};
    char ctrl_desc[128] = {0};
    char pins_str[512] = {0};
    char replace_id[64] = {0};
    char server_ip[16] = {0};

    controlhome_PinConfig pins[MAX_REGISTER_PINS];
    size_t pins_count = 0;

    nvs_handle_t nvs_h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_h) == ESP_OK) {
        size_t len;
        len = sizeof(mode);       nvs_get_str(nvs_h, NVS_KEY_MODE, mode, &len);
        len = sizeof(ctrl_id);    nvs_get_str(nvs_h, NVS_KEY_CTRL_ID, ctrl_id, &len);
        len = sizeof(ctrl_desc);  nvs_get_str(nvs_h, NVS_KEY_CTRL_DESC, ctrl_desc, &len);
        len = sizeof(pins_str);   nvs_get_str(nvs_h, NVS_KEY_PINS, pins_str, &len);
        len = sizeof(replace_id); nvs_get_str(nvs_h, NVS_KEY_REPLACE_ID, replace_id, &len);
        len = sizeof(server_ip);  nvs_get_str(nvs_h, NVS_KEY_SERVER_IP, server_ip, &len);
        nvs_close(nvs_h);
    }
    if (server_ip[0] == '\0') {
        // Not set -- device provisioned before this field existed, or NVS read failed.
        safe_strcpy(server_ip, sizeof(server_ip), SERVER_IP_DEFAULT);
    }
    ESP_LOGI(TAG, "Server IP: %s", server_ip);

    pins_count = parse_pins_string(pins_str, pins, MAX_REGISTER_PINS);
    ESP_LOGI(TAG, "Loaded %u pin(s) from NVS for registration", (unsigned)pins_count);

    while (1)
    {

        struct sockaddr_in server_addr = { 0 };
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
            ESP_LOGE(TAG, "connect() to %s:%d failed: errno %d", server_ip, SERVER_PORT, errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }
        ESP_LOGI(TAG, "Connected to server %s:%d", server_ip, SERVER_PORT);

        bool registered;
        if (strcmp(mode, "replace") == 0) {
            controlhome_ControllerConfig config;
            registered = replace_controller(sock, mac_str, replace_id, &config);
            if (registered) {
                pins_count = config.pins_count;
                for (size_t i = 0; i < pins_count && i < MAX_REGISTER_PINS; i++) {
                    pins[i] = config.pins[i];
                }

                char new_pins_str[512];
                build_pins_string(pins, pins_count, new_pins_str, sizeof(new_pins_str));

                nvs_handle_t h;
                if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_str(h, NVS_KEY_PINS, new_pins_str);
                    nvs_commit(h);
                    nvs_close(h);
                }

                gpio_pin_list_t live_cfg;
                gpio_pin_list_from_string(new_pins_str, &live_cfg);
                gpio_ctrl_init(&live_cfg);

                ESP_LOGI(TAG, "ReplaceController: saved and applied %u pin(s)", (unsigned)pins_count);
            }
        } else {
            registered = register_controller(sock, mac_str, ctrl_id, ctrl_desc, pins, pins_count);
        }

        if (!registered)
        {
            ESP_LOGE(TAG, "Registration failed, reconnecting...");
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        // "mode"/"replace_id" only matter for the one-time provisioning handshake
        // right after an AP-mode form submit -- clear them once it succeeds so
        // later reconnects (this boot or any future one) fall through to the
        // plain, 409-tolerant register_controller() path instead of repeating
        // ReplaceController/RegisterController forever.
        if (mode[0] != '\0') {
            nvs_handle_t mode_h;
            if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &mode_h) == ESP_OK) {
                nvs_erase_key(mode_h, NVS_KEY_MODE);
                nvs_erase_key(mode_h, NVS_KEY_REPLACE_ID);
                nvs_commit(mode_h);
                nvs_close(mode_h);
            }
            mode[0] = '\0';
        }

        connection_ctx_t ctx = {
            .sock = sock,
            .write_lock = xSemaphoreCreateMutex(),
            .connection_alive = true,
        };
        safe_strcpy(ctx.mac, sizeof(ctx.mac), mac_str);
        s_active_connection = &ctx;
        xTaskCreate(command_task, "command_task", 4096, &ctx, 5, NULL);

        // Baseline IoReport right after (re)connecting, so the server has a fresh
        // snapshot before settling into on-change-only reporting.
        {
            controlhome_IoReport report = build_ioreport();
            controlhome_Envelope env = (controlhome_Envelope)controlhome_Envelope_init_zero;
            safe_strcpy(env.mac_address, sizeof(env.mac_address), mac_str);
            env.which_payload = controlhome_Envelope_io_report_tag;
            env.payload.io_report = report;

            xSemaphoreTake(ctx.write_lock, portMAX_DELAY);
            int send_result = send_envelope(sock, &env);
            xSemaphoreGive(ctx.write_lock);
            if (send_result != 0) {
                ctx.connection_alive = false;
            }
        }

        while (ctx.connection_alive) {
            // Woken early (non-zero) by a GPIO ISR on an actual pin change -- send
            // a fresh IoReport. Timed out (zero) with nothing changed -- send a
            // lightweight Heartbeat instead, just to keep last-seen current.
            uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));

            controlhome_Envelope env = (controlhome_Envelope)controlhome_Envelope_init_zero;
            safe_strcpy(env.mac_address, sizeof(env.mac_address), mac_str);

            if (notified > 0) {
                env.which_payload = controlhome_Envelope_io_report_tag;
                env.payload.io_report = build_ioreport();
            } else {
                env.which_payload = controlhome_Envelope_heartbeat_tag;
                env.payload.heartbeat.uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
            }

            xSemaphoreTake(ctx.write_lock, portMAX_DELAY);
            int send_result = send_envelope(sock, &env);
            xSemaphoreGive(ctx.write_lock);

            if (send_result != 0) {
                ctx.connection_alive = false;
                break;
            }
        }

        s_active_connection = NULL;
        vSemaphoreDelete(ctx.write_lock);
        
        ESP_LOGW(TAG, "Connection lost, will reconnect");
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

bool tcp_client_send_rfid(const char *rfid)
{
    connection_ctx_t *ctx = s_active_connection;
    if (ctx == NULL || !ctx->connection_alive) {
        ESP_LOGW(TAG, "No active connection to send RFID");
        return false;
    }

    controlhome_Envelope env = (controlhome_Envelope)controlhome_Envelope_init_zero;
    safe_strcpy(env.mac_address, sizeof(env.mac_address), ctx->mac);
    env.which_payload = controlhome_Envelope_rfid_tag_tag;
    safe_strcpy(env.payload.rfid_tag.tag_id, sizeof(env.payload.rfid_tag.tag_id), rfid);
    env.payload.rfid_tag.gpio_number = rfid_get_rx_pin();
    
    ; // Not used in this context, but required by the protobuf definition

    xSemaphoreTake(ctx->write_lock, portMAX_DELAY);
    int send_result = send_envelope(ctx->sock, &env);
     // Ensure the active connection is still valid after sending
    xSemaphoreGive(ctx->write_lock);
   

    if (send_result != 0) {
        ESP_LOGE(TAG, "Failed to send RFID command");
        return false;
    }

    ESP_LOGI(TAG, "RFID command sent: %s", rfid);
    return true;
}

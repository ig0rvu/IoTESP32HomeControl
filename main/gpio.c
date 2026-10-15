#include "gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "gpio";    

static TaskHandle_t s_notify_task = NULL;
static bool s_isr_service_installed = false;

  uint32_t gpio_in_pins[MAX_GPIO_PINS];
  uint32_t gpio_out_pins[MAX_GPIO_PINS];

  size_t gpio_in_pin_count = 0;
  size_t gpio_out_pin_count = 0;

// Parallel to gpio_out_pins[] (same index) -- the level each OUT pin should
// idle at, from gpio_pin_config_t.initial_state.
static bool s_gpio_out_initial[MAX_GPIO_PINS];

void gpio_ctrl_notify_task(TaskHandle_t task)
{
    s_notify_task = task;
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    if (s_notify_task != NULL) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_notify_task, &higher_priority_task_woken);
       
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}
void gpio_ctrl_init(const gpio_pin_list_t *config)
{
    // Drop ISR handlers from whatever was classified IN/OUT by the *previous*
    // call before rebuilding the lists below -- otherwise a pin that gets
    // reclassified away from IN/OUT (e.g. repurposed to UART for RFID) keeps a
    // stale interrupt handler firing on every electrical edge that pin sees
    // from then on, including every UART bit transition, flooding IoReport
    // sends on every RFID read. No-op on the very first call (nothing
    // registered yet).
    if (s_isr_service_installed) {
        for (size_t i = 0; i < gpio_in_pin_count; i++) {
            gpio_isr_handler_remove(gpio_in_pins[i]);
        }
        for (size_t i = 0; i < gpio_out_pin_count; i++) {
            gpio_isr_handler_remove(gpio_out_pins[i]);
        }
    }

    gpio_in_pin_count = 0;
    gpio_out_pin_count = 0;

    for (size_t i = 0; i < config->pin_count; i++) {
        if (config->pins[i].direction == GPIO_DIRECTION_OUT) {
            s_gpio_out_initial[gpio_out_pin_count] = config->pins[i].initial_state;
            gpio_out_pins[gpio_out_pin_count++] = config->pins[i].gpio;
        } else if (config->pins[i].direction == GPIO_DIRECTION_IN) {
            gpio_in_pins[gpio_in_pin_count++] = config->pins[i].gpio;
        }
        // BIDIRECTIONAL -- odluka za kasnije, za sad preskočen
    }
    
    uint64_t out_mask = 0;
    for (size_t i = 0; i < gpio_out_pin_count; i++) {
        out_mask |= (1ULL << gpio_out_pins[i]);
    }
    gpio_config_t out_conf = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  
    };
    if (out_mask != 0) {
        gpio_config(&out_conf);
    }
    for (size_t i = 0; i < gpio_out_pin_count; i++) {
        gpio_set_level(gpio_out_pins[i], s_gpio_out_initial[i] ? 1 : 0);
    }

    uint64_t in_mask = 0;
    for (size_t i = 0; i < gpio_in_pin_count; i++) {
        in_mask |= (1ULL << gpio_in_pins[i]);
    }
    gpio_config_t in_conf = {
        .pin_bit_mask = in_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
      
    if (in_mask != 0) {
        gpio_config(&in_conf);
    }
    ESP_LOGI(TAG, "GPIO initialized: %d output(s), %d input(s), interrupt configured for both", (int)gpio_out_pin_count, (int)gpio_in_pin_count);
    if (!s_isr_service_installed) {
        gpio_install_isr_service(0);
        s_isr_service_installed = true;
    }

    for (size_t i = 0; i < gpio_in_pin_count; i++) {
        gpio_isr_handler_add(gpio_in_pins[i], gpio_isr_handler, NULL);
       
    }

    for(size_t i = 0; i < gpio_out_pin_count; i++) {
        gpio_isr_handler_add(gpio_out_pins[i], gpio_isr_handler, NULL);
    }
}

bool gpio_pin_list_from_string(const char *pins_str, gpio_pin_list_t *out)
{
    out->pin_count = 0;

    char buf[512];
    strncpy(buf, pins_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, ",");
    while (token != NULL && out->pin_count < MAX_GPIO_PINS) {
        char *colon = strchr(token, ':');
        if (colon != NULL) {
            *colon = '\0';
            uint32_t gpio = (uint32_t)atoi(token);
            char *func_str = colon + 1;

            // Optional 3rd ":"-segment is the OUT-pin initial level ("0"/"1").
            // Absent on strings saved before this field existed -- default to
            // 1 (idle/off for the active-low relays these boards drive).
            bool initial_state = true;
            char *second_colon = strchr(func_str, ':');
            if (second_colon != NULL) {
                *second_colon = '\0';
                initial_state = (second_colon[1] == '1');
            }

            gpio_pin_direction_t direction = GPIO_DIRECTION_IN;
            gpio_pin_function_t function = GPIO_PIN_FUNC_GPIO;

            if (strcmp(func_str, "GPIO_IN") == 0) {
                direction = GPIO_DIRECTION_IN;
                function = GPIO_PIN_FUNC_GPIO;
            } else if (strcmp(func_str, "GPIO_OUT") == 0) {
                direction = GPIO_DIRECTION_OUT;
                function = GPIO_PIN_FUNC_GPIO;
            } else if (strncmp(func_str, "SPI_", 4) == 0) {
                direction = GPIO_DIRECTION_BIDIRECTIONAL;
                function = GPIO_PIN_FUNC_SPI;
            } else if (strncmp(func_str, "I2C_", 4) == 0) {
                direction = GPIO_DIRECTION_BIDIRECTIONAL;
                function = GPIO_PIN_FUNC_I2C;
            } else if (strcmp(func_str, "RFID_RX") == 0) {
                // BIDIRECTIONAL here is just a marker so gpio_ctrl_init() skips it --
                // rfid.c owns this pin exclusively via its own uart_set_pin() call.
                direction = GPIO_DIRECTION_BIDIRECTIONAL;
                function = GPIO_PIN_FUNC_UART;
            }

            out->pins[out->pin_count].gpio = gpio;
            out->pins[out->pin_count].direction = direction;
            out->pins[out->pin_count].function = function;
            out->pins[out->pin_count].initial_state = initial_state;
            out->pin_count++;
        }
        token = strtok(NULL, ",");
    }
    return true;
}

bool gpio_ctrl_read(uint32_t gpio_num)
{
    return gpio_get_level((gpio_num_t)gpio_num) !=0;
}

void gpio_ctrl_set_output(uint32_t gpio_num, bool value)
{
    gpio_set_level(gpio_num, value ? 1 : 0);
}
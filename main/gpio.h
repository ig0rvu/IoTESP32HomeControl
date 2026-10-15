#ifndef GPIO_CTRL_H
#define GPIO_CTRL_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// GPIO pin definitions for the specific hardware
//IN pins 4,5,6,8 OUT pins 10,14,15,16
#define GPIO_OUT_LED 15
#define GPIO_IN_TEST 14
#define GPIO_IN_KAPIJA_POS1 4
#define GPIO_IN_KAPIJA_POS2 5
#define GPIO_IN_ZVONO 6
#define GPIO_IN_MALAKAPIJA_POS 8
#define GPIO_OUT_MALAKAPIJA 10
#define MAX_GPIO_PINS 20

extern  uint32_t gpio_in_pins[];
extern  size_t gpio_in_pin_count;

extern  uint32_t gpio_out_pins[];
extern  size_t gpio_out_pin_count;

typedef enum {
    GPIO_DIRECTION_IN,
    GPIO_DIRECTION_OUT,
    GPIO_DIRECTION_BIDIRECTIONAL
} gpio_pin_direction_t;

typedef enum{
    GPIO_PIN_FUNC_GPIO,
    GPIO_PIN_FUNC_I2C,
    GPIO_PIN_FUNC_SPI,
    GPIO_PIN_FUNC_UART,
    GPIO_PIN_FUNC_ONEWIRE,
    GPIO_PIN_FUNC_PWM,
    GPIO_PIN_FUNC_ADC,
    GPIO_PIN_FUNC_DAC,
    GPIO_PIN_FUNC_OTHER 
}gpio_pin_function_t;

typedef struct
{
    uint32_t gpio;
    gpio_pin_direction_t direction;
    gpio_pin_function_t function;
    // Level an OUT pin is driven to at init. Relays on these boards are
    // active-low, so true (idle/off) is the safe default. Ignored for
    // non-OUT pins.
    bool initial_state;
} gpio_pin_config_t ;

typedef struct 
{
    gpio_pin_config_t pins[MAX_GPIO_PINS];
    size_t pin_count;  
}gpio_pin_list_t;

void gpio_ctrl_init(const gpio_pin_list_t *config);

// Parses the compact "gpio:FUNCTION,gpio:FUNCTION" format (same one used for the
// NVS "pins" key) directly into a gpio_pin_list_t. Unrecognized function tokens
// fall back to GPIO_DIRECTION_IN / GPIO_PIN_FUNC_GPIO.
bool gpio_pin_list_from_string(const char *pins_str, gpio_pin_list_t *out);

bool gpio_ctrl_read(uint32_t gpio_num);

void gpio_ctrl_set_output(uint32_t gpio_num, bool value);

void gpio_ctrl_notify_task(TaskHandle_t task);

#endif // GPIO_CTRL_H
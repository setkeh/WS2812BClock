#include <stdbool.h>
#include <stdint.h>

#include "debug.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define BLINK_GPIO CONFIG_BLINK_GPIO


#define ACT_LED_MS 50

static esp_timer_handle_t act_timer;
static vprintf_like_t prev_vprintf;

static void act_off(void *arg) { gpio_set_level(BLINK_GPIO, 0); }

static void act_pulse(void) {
    gpio_set_level(BLINK_GPIO, 1);
    if (esp_timer_restart(act_timer, ACT_LED_MS * 1000) != ESP_OK)   // not running yet
        esp_timer_start_once(act_timer, ACT_LED_MS * 1000);
}

// TX: runs on every ESP_LOGx, then hands off to the normal log output
static int log_hook(const char *fmt, va_list args) {
    act_pulse();
    return prev_vprintf(fmt, args);
}

// RX: read incoming bytes and pulse on each batch
static void uart_rx_task(void *arg) {
    uint8_t buf[64];
    while (true) {
        int n = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0) {
            act_pulse();
            // handle buf[0..n) here if you want to act on commands
        }
    }
}

void activity_led_init(void) {
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    const esp_timer_create_args_t args = { .callback = act_off, .name = "act_led" };
    esp_timer_create(&args, &act_timer);

    prev_vprintf = esp_log_set_vprintf(log_hook);

    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    xTaskCreate(uart_rx_task, "uart_rx", 2048, NULL, 5, NULL);
}
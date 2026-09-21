#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "clock";

#define BLINK_GPIO CONFIG_BLINK_GPIO

void app_main(void)
{
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    bool led_on = false;
    while (true) {
        led_on = !led_on;
        ESP_LOGI(TAG, "LED %s", led_on ? "on" : "off");
        gpio_set_level(BLINK_GPIO, led_on);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BLINK_PERIOD_MS));
    }
}

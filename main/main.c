#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "debug.h"
#include "fan.h"

static const char *TAG = "clock";

// Define Config Build
#ifdef CONFIG_DEBUG_BUILD
#define DEBUG_BUILD 1
#else
#define DEBUG_BUILD 0
#endif

void app_main(void) {
    if (DEBUG_BUILD)
    {
        activity_led_init();
    }
    init_fan_pwm();
    
    if (DEBUG_BUILD)
    {
        ESP_LOGI(TAG, "Test: find start threshold");
    }
    for (int p = 0; p <= 100; p += 5) {          // rising: find start threshold
        if (DEBUG_BUILD)
        {
            ESP_LOGI(TAG, "Duty Cycle Rising Set to: %d", p);
        }
        fan_set(p);
        ESP_LOGI(TAG, "up %d%%", p);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    if (DEBUG_BUILD)
    {
        ESP_LOGI(TAG, "Test: find stall threshold");
    }
    for (int p = 100; p >= 0; p -= 5) {          // falling: find stall threshold
        if (DEBUG_BUILD)
        {
            ESP_LOGI(TAG, "Duty Cycle Falling Set to: %d", p);
        }
        fan_set(p);
        ESP_LOGI(TAG, "down %d%%", p);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// LED Blink Example code
/*void app_main(void)
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


    if (DEBUG_BUILD)
    {
        ESP_LOGI(TAG, "FAN Duty set to 0");
    }
}*/

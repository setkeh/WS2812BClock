#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "debug.h"
#include "fan.h"
#include "wifi.h"
#include "ntp.h"

#include "nvs_flash.h"

static const char *TAG = "clock";
static volatile bool s_realign = false;

// Define Config Build
#ifdef CONFIG_DEBUG_BUILD
#define DEBUG_BUILD 1
#else
#define DEBUG_BUILD 0
#endif

// Sleep until just after the next full second of the real clock
static void wait_for_next_second(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int ms_to_next = 1000 - (tv.tv_usec / 1000);
    // pdMS_TO_TICKS rounds down to whole 10 ms ticks, which could wake us a few ms
    // *before* the boundary (still showing the old second). +1 tick lands just after it.
    vTaskDelay(pdMS_TO_TICKS(ms_to_next) + 1);
}

static void on_ntp_synced(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const struct timeval *tv = data;
    struct tm local;
    char buf[40];
    localtime_r(&tv->tv_sec, &local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
    ESP_LOGI(TAG, "NTP synced: %s", buf);
    s_realign = true;
}

void app_main(void) {
    if (DEBUG_BUILD)
    {
        activity_led_init();
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (!wifi_init_sta()) {
        ESP_LOGW(TAG, "WiFi not up yet, will keep retrying in the background");
    }

    ESP_ERROR_CHECK(esp_event_handler_register(NTP_EVENT, NTP_EVENT_SYNCED, on_ntp_synced, NULL));
 
    ntp_init();

    wait_for_next_second();
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        if (ntp_time_is_valid()) {
            struct timeval tv;
            struct tm local;
            char buf[40];
            char zone[8];
            gettimeofday(&tv, NULL);
            localtime_r(&tv.tv_sec, &local);
            strftime(zone, sizeof(zone), "%Z", &local);
            strftime(buf, sizeof(buf), "%H:%M:%S", &local);
            // Show milliseconds while testing i can see the alignment working
            ESP_LOGI(TAG, "tick %s.%03ld %s", buf, (long)(tv.tv_usec / 1000), zone);
        } else {
            ESP_LOGI(TAG, "tick: waiting for first NTP sync");
        }

        if (s_realign) {
            s_realign = false;
            wait_for_next_second();            // sleep to the next boundary instead
            last_wake = xTaskGetTickCount();   // restart the fixed rhythm from there
        } else {
            xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
        }
    }

    /*
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
    }*/
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

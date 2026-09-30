#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "debug.h"
#include "fan.h"
#include "wifi.h"
#include "ntp.h"
#include "display.h"
#include "ota.h"
#include "logship.h"

#include "nvs_flash.h"

static const char *TAG = "clock";

// How often a healthy clock says so. Between these, a working clock is silent.
#define HEARTBEAT_MIN 5
static volatile bool s_realign = false;
static bool s_confirmed_healthy = false;

// Define Config Build
#ifdef CONFIG_DEBUG_BUILD
#define DEBUG_BUILD 1
#else
#define DEBUG_BUILD 0
#endif

// A clock that is fine says so every few minutes. The heap figures are what
// would show a slow leak, and a gap between beats shows a reboot nobody saw.
static void heartbeat(void)
{
    static int64_t next_us = 0;
    int64_t now = esp_timer_get_time();

    if (now < next_us) return;
    next_us = now + (int64_t)HEARTBEAT_MIN * 60 * 1000000;
    ESP_LOGI(TAG, "alive: up %llu s, free heap %" PRIu32 ", low water %" PRIu32,
             (unsigned long long)(now / 1000000),
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
}

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
    ota_log_running_version();


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

    // Capturing starts before WiFi, so the connection attempt itself ends up in
    // the log stream. Sending cannot start until the TCP/IP stack exists, so
    // that half waits for logship_start() below.
    logship_init();

    ESP_ERROR_CHECK(display_init());
    display_show_waiting();

    if (!wifi_init_sta()) {
        ESP_LOGW(TAG, "WiFi not up yet, will keep retrying in the background");
    }

    // Only now is lwip up. Resolving the collector's name before this point
    // asserts inside lwip, because its mailbox does not exist yet.
    logship_start();

    // After the sender exists, so a crash report from the last boot actually
    // gets off the device.
    logship_report_coredump();

    ESP_ERROR_CHECK(esp_event_handler_register(NTP_EVENT, NTP_EVENT_SYNCED, on_ntp_synced, NULL));
 
    ntp_init();

    wait_for_next_second();
    TickType_t last_wake = xTaskGetTickCount();
    int waiting = 0;

    while (true) {
        if (ntp_time_is_valid()) {
            struct timeval tv;
            struct tm local;
            char buf[40];
            char zone[8];
            gettimeofday(&tv, NULL);
            localtime_r(&tv.tv_sec, &local);
            display_show_time(&local);
            if (!s_confirmed_healthy) {
                s_confirmed_healthy = true;
                ota_mark_current_app_valid();   // network, time and display all up
#if CONFIG_OTA_CHECK_ON_BOOT
                ota_check_async();              // own task: TLS needs more stack than main has
#endif
            }
            if (DEBUG_BUILD) {
                // Milliseconds show the tick staying aligned to the real
                // second. One line a second is far too much to ship, so this
                // is the sort of thing the debug flag exists to compile out.
                strftime(zone, sizeof(zone), "%Z", &local);
                strftime(buf, sizeof(buf), "%H:%M:%S", &local);
                ESP_LOGI(TAG, "tick %s.%03ld %s", buf, (long)(tv.tv_usec / 1000), zone);
            }
            heartbeat();
        } else {
            display_show_waiting();
            // A state, not an event: report it every 30 s, not every tick.
            if (waiting++ % 30 == 0)
                ESP_LOGW(TAG, "waiting for the first NTP sync");
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

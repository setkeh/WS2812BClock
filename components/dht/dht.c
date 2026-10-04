#include "dht.h"

#include "sdkconfig.h"

#if CONFIG_DHT_ENABLE

#include <stdatomic.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dht";

#define DHT_GPIO  CONFIG_DHT_GPIO
#define BITS      40

// No phase of the exchange lasts longer than the 80 us response pulses, so
// anything beyond this means the sensor has stopped talking.
#define PHASE_TIMEOUT_US 200

// A data bit is a ~50 us low followed by a high: about 27 us for a zero and
// about 70 us for a one. Anything between the two separates them.
#define BIT_THRESHOLD_US 45

// The datasheet asks for a second of settling after power-up.
#define SETTLE_MS 1500

// Consecutive failed readings before the cached values stop being offered. At
// the default interval that is a couple of minutes of silence.
#define STALE_AFTER 5

#define TASK_STACK 2560

// Humidity and temperature are both 0-100 on a DHT11, so the pair packs into
// one atomic word and a reader can never catch a half-updated value.
// -1 means nothing trustworthy is held.
#define PACK(h, t)   (((h) << 8) | ((t) & 0xFF))
#define UNPACK_H(v)  (((v) >> 8) & 0xFF)
#define UNPACK_T(v)  ((v) & 0xFF)

static atomic_int s_reading = -1;

// Which phase of the exchange failed. They point at different faults, so the
// distinction is worth keeping: a line that never rises is a missing pull-up or
// a short, no response is an unpowered or miswired sensor, a truncated frame is
// a connection failing mid-read, and a complete frame failing its checksum is
// interference or preemption.
typedef enum {
    DHT_OK = 0,
    DHT_NO_RISE,
    DHT_NO_RESPONSE,
    DHT_NO_RESPONSE_HIGH,
    DHT_NO_FIRST_BIT,
    DHT_TRUNCATED,
    DHT_BAD_CHECKSUM,
} dht_status_t;

static const char *const STATUS_TEXT[] = {
    [DHT_OK]               = "ok",
    [DHT_NO_RISE]          = "line never rose after the start pulse (pull-up missing, or line shorted low)",
    [DHT_NO_RESPONSE]      = "no response (sensor unpowered, miswired, or not a DHT11)",
    [DHT_NO_RESPONSE_HIGH] = "response low began but never ended",
    [DHT_NO_FIRST_BIT]     = "response high began but no data followed",
    [DHT_TRUNCATED]        = "frame cut short mid-read",
    [DHT_BAD_CHECKSUM]     = "frame received but failed its checksum",
};

// Spin until the line reads `level`, returning how long that took. Returns -1
// if it never happened.
//
// Deliberately does not mask interrupts. A DHT11 exchange takes about 4 ms,
// and holding off interrupts for that long would disturb the RMT refills
// driving the LEDs and upset WiFi. A read preempted mid-bit simply fails its
// checksum and is retried, which costs nothing at this sampling rate.
static int wait_for(int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    int64_t waited;

    while (gpio_get_level(DHT_GPIO) != level) {
        waited = esp_timer_get_time() - start;
        if (waited > timeout_us) return -1;
    }
    return (int)(esp_timer_get_time() - start);
}

static dht_status_t sample(int *temperature_c, int *humidity_pct)
{
    uint8_t data[5] = {0};

    // Request: hold the line down long enough for the sensor to notice (it
    // wants at least 18 ms), then let go and listen.
    gpio_set_direction(DHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);

    // Wait for the pull-up to actually raise the line before looking for the
    // reply. Without this the line is still low from the start pulse, the first
    // wait returns immediately having consumed nothing, and the sensor's 80 us
    // response high gets measured as data bit zero -- shifting the whole frame
    // by a bit and guaranteeing a checksum failure.
    if (wait_for(1, PHASE_TIMEOUT_US) < 0) return DHT_NO_RISE;

    // The sensor answers with 80 us low, then 80 us high. Stepping past both
    // leaves us at the start of the first bit's low period.
    if (wait_for(0, PHASE_TIMEOUT_US) < 0) return DHT_NO_RESPONSE;
    if (wait_for(1, PHASE_TIMEOUT_US) < 0) return DHT_NO_RESPONSE_HIGH;
    if (wait_for(0, PHASE_TIMEOUT_US) < 0) return DHT_NO_FIRST_BIT;

    for (int i = 0; i < BITS; i++) {
        if (wait_for(1, PHASE_TIMEOUT_US) < 0) return DHT_TRUNCATED;  // bit's high begins
        int high = wait_for(0, PHASE_TIMEOUT_US);                     // and how long it lasts
        if (high < 0) return DHT_TRUNCATED;

        data[i / 8] <<= 1;
        if (high > BIT_THRESHOLD_US) data[i / 8] |= 1;
    }

    // The sensor sends its own sum, which is what makes an interrupted read
    // detectable rather than silently wrong.
    if (((data[0] + data[1] + data[2] + data[3]) & 0xFF) != data[4]) return DHT_BAD_CHECKSUM;

    *humidity_pct = data[0];
    *temperature_c = data[2];
    return DHT_OK;
}

static void dht_task(void *arg)
{
    int failures = 0;
    bool was_failing = false;

    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

    while (true) {
        int temperature = 0, humidity = 0;
        dht_status_t status = DHT_NO_RESPONSE;
        bool ok = false;

        for (int attempt = 0; attempt < CONFIG_DHT_RETRIES && !ok; attempt++) {
            if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(2000));   // it needs a moment between reads
            status = sample(&temperature, &humidity);
            ok = (status == DHT_OK);
        }

        if (ok) {
            atomic_store(&s_reading, PACK(humidity, temperature));
            if (was_failing) {
                ESP_LOGI(TAG, "sensor responding again: %d C, %d%%", temperature, humidity);
                was_failing = false;
            }
            failures = 0;
        } else if (++failures == STALE_AFTER) {
            // Said once on the way down rather than every cycle, and the
            // cached values are dropped so nothing stale is displayed.
            atomic_store(&s_reading, -1);
            was_failing = true;
            ESP_LOGW(TAG, "GPIO%d: %s (%d attempts)",
                     DHT_GPIO, STATUS_TEXT[status], failures);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_DHT_INTERVAL_S * 1000));
    }
}

void dht_init(void)
{
    // The external pull-up does the real work; this is a backstop so a missing
    // resistor shows up as flaky readings rather than nothing at all.
    gpio_set_pull_mode(DHT_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);

    if (xTaskCreate(dht_task, "dht", TASK_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the sensor task");
        return;
    }
    ESP_LOGI(TAG, "sampling GPIO%d every %d s", DHT_GPIO, CONFIG_DHT_INTERVAL_S);
}

bool dht_enabled(void) { return true; }

bool dht_read(int *temperature_c, int *humidity_pct)
{
    int v = atomic_load(&s_reading);

    if (v < 0) return false;
    if (temperature_c) *temperature_c = UNPACK_T(v);
    if (humidity_pct) *humidity_pct = UNPACK_H(v);
    return true;
}

#else  /* !CONFIG_DHT_ENABLE */

void dht_init(void) { }
bool dht_enabled(void) { return false; }
bool dht_read(int *temperature_c, int *humidity_pct) { return false; }

#endif

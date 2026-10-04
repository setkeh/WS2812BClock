#include "display.h"

#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "led_strip.h"
#include "dht.h"
#include "sdkconfig.h"

static const char *TAG = "display";

#define RING_GPIO   CONFIG_DISPLAY_RING_GPIO
#define RING_COUNT  CONFIG_DISPLAY_RING_COUNT
#define SEG_GPIO    CONFIG_DISPLAY_SEG_GPIO
#define SEG_COUNT   CONFIG_DISPLAY_SEG_COUNT

// Pixel 0 sits at 12 o'clock and the chain runs anticlockwise, so a clock
// position (0-59, clockwise from 12) maps to a pixel like this.
#define RING_PIXEL(pos) ((RING_COUNT - (pos)) % RING_COUNT)

// Digits left to right: H10 H1 : M10 M1 -> 0 1 2 3.
// The chain starts at the top-right of the right-hand plate and snakes by row,
// right-hand plate first. Verified on the hardware with the colour map test.
static const uint8_t seg_digit[SEG_COUNT] = {
    3, 2, 2, 2, 3, 3, 3, 2, 2, 2, 3, 3, 3, 2,   // right-hand plate
    1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0,   // left-hand plate
};

// a=0 top, b=1 top-right, c=2 bottom-right, d=3 bottom,
// e=4 bottom-left, f=5 top-left, g=6 middle
static const uint8_t seg_letter[SEG_COUNT] = {
    0, 0, 5, 1, 5, 1, 6, 6, 4, 2, 4, 2, 3, 3,
    0, 0, 5, 1, 5, 1, 6, 6, 4, 2, 4, 2, 3, 3,
};

// Which segments make up each digit, bit 0 = a ... bit 6 = g.
static const uint8_t font[10] = {
    0x3F, // 0: abcdef
    0x06, // 1: bc
    0x5B, // 2: abdeg
    0x4F, // 3: abcdg
    0x66, // 4: bcfg
    0x6D, // 5: acdfg
    0x7D, // 6: acdefg
    0x07, // 7: abc
    0x7F, // 8: abcdefg
    0x6F, // 9: abcdfg
};

enum { PHASE_TIME, PHASE_DATE, PHASE_TEMPERATURE, PHASE_HUMIDITY, PHASE_COUNT };

// Glyphs that are not digits, in the same bit order: bit 0 = a ... bit 6 = g.
#define GLYPH_BLANK  0x00
#define GLYPH_DASH   0x40   // g
#define GLYPH_DEGREE 0x63   // a b f g -- the small ring at the top
#define GLYPH_C      0x39   // a d e f
#define GLYPH_H      0x76   // b c e f g
#define GLYPH_R      0x50   // e g

typedef struct { uint8_t r, g, b; } rgb_t;

// Hand colours at full scale; brightness is applied when they are drawn.
static const rgb_t COLOUR_HOUR   = { 255,   0,   0 };   // red
static const rgb_t COLOUR_MINUTE = { 140,   0, 255 };   // violet
static const rgb_t COLOUR_SECOND = {   0, 255,   0 };   // green
static const rgb_t COLOUR_MARK   = {  60,  40,   0 };   // dim amber hour marks
static const rgb_t COLOUR_DIGIT  = { 255, 255, 255 };   // White

static led_strip_handle_t s_ring;
static led_strip_handle_t s_seg;

// (digit, segment letter) -> pixel index, built from the maps above.
static uint8_t s_seg_pixel[4][7];

static led_strip_handle_t chain_init(int gpio, uint32_t count)
{
    led_strip_handle_t strip = NULL;
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = gpio,
        .max_leds = count,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        // Two RMT memory blocks per chain (64 words each). A bigger buffer
        // means fewer refill interrupts, so WiFi activity is far less likely
        // to disturb the pixel timing mid-frame.
        .mem_block_symbols = 128,
        .flags = { .with_dma = false },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chain on GPIO%d failed: %s", gpio, esp_err_to_name(err));
        return NULL;
    }
    return strip;
}

esp_err_t display_init(void)
{
    for (uint32_t i = 0; i < SEG_COUNT; i++)
        s_seg_pixel[seg_digit[i]][seg_letter[i]] = i;

    s_ring = chain_init(RING_GPIO, RING_COUNT);
    s_seg = chain_init(SEG_GPIO, SEG_COUNT);
    if (!s_ring || !s_seg) return ESP_FAIL;

    ESP_ERROR_CHECK(led_strip_clear(s_ring));
    ESP_ERROR_CHECK(led_strip_clear(s_seg));
    ESP_LOGI(TAG, "ring: GPIO%d x%d, digits: GPIO%d x%d",
             RING_GPIO, RING_COUNT, SEG_GPIO, SEG_COUNT);
    return ESP_OK;
}

// The ring and the digits are dimmed separately: the diffused digits need to
// stay readable against the glare of the bare ring pixels.
static bool is_night(int hour)
{
    int start = CONFIG_DISPLAY_NIGHT_START_HOUR;
    int end = CONFIG_DISPLAY_NIGHT_END_HOUR;
    return (start <= end) ? (hour >= start && hour < end)
                          : (hour >= start || hour < end);
}

static int ring_brightness(int hour)
{
    return is_night(hour) ? CONFIG_DISPLAY_RING_NIGHT_BRIGHTNESS
                          : CONFIG_DISPLAY_RING_DAY_BRIGHTNESS;
}

static int seg_brightness(int hour)
{
    return is_night(hour) ? CONFIG_DISPLAY_SEG_NIGHT_BRIGHTNESS
                          : CONFIG_DISPLAY_SEG_DAY_BRIGHTNESS;
}

static uint8_t scale(uint8_t v, int percent)
{
    return (uint8_t)((v * percent) / 100);
}

// Hands own their pixel outright: later writes replace earlier ones, and the
// second trail skips the hand positions so their colours stay true.
static void ring_set(rgb_t *buf, int pos, rgb_t c, int percent)
{
    int i = RING_PIXEL(((pos % RING_COUNT) + RING_COUNT) % RING_COUNT);
    buf[i].r = scale(c.r, percent);
    buf[i].g = scale(c.g, percent);
    buf[i].b = scale(c.b, percent);
}

static void ring_flush(const rgb_t *buf)
{
    for (int i = 0; i < RING_COUNT; i++)
        ESP_ERROR_CHECK(led_strip_set_pixel(s_ring, i, buf[i].r, buf[i].g, buf[i].b));
    ESP_ERROR_CHECK(led_strip_refresh(s_ring));
}

static void draw_glyph(int digit, uint8_t bits, int percent)
{
    for (int letter = 0; letter < 7; letter++) {
        bool on = bits & (1 << letter);
        rgb_t c = on ? COLOUR_DIGIT : (rgb_t){ 0, 0, 0 };
        ESP_ERROR_CHECK(led_strip_set_pixel(s_seg, s_seg_pixel[digit][letter],
                                            scale(c.r, percent),
                                            scale(c.g, percent),
                                            scale(c.b, percent)));
    }
}

static void draw_digit(int digit, int value, int percent)
{
    draw_glyph(digit, font[value % 10], percent);
}

// A two-digit value across one plate, leading zero kept so the display does
// not change width as the value crosses ten.
static void draw_pair(int first_digit, int value, int percent)
{
    if (value < 0) value = 0;
    if (value > 99) value = 99;
    draw_digit(first_digit, value / 10, percent);
    draw_digit(first_digit + 1, value % 10, percent);
}

void display_show_time(const struct tm *local)
{
    if (!s_ring || !s_seg || !local) return;

    int percent = ring_brightness(local->tm_hour);
    int seg_percent = seg_brightness(local->tm_hour);
    rgb_t ring[RING_COUNT];
    memset(ring, 0, sizeof(ring));

    // Hour hand creeps forward through the hour: 5 positions per hour.
    int hour_pos = (((local->tm_hour % 12) * 5) + (local->tm_min / 12)) % RING_COUNT;
    int min_pos = local->tm_min % RING_COUNT;

    // Seconds fill in as they pass and stay lit until the minute rolls over,
    // stepping over the hour and minute hands so those keep their own colour.
    for (int s = 0; s <= local->tm_sec && s < RING_COUNT; s++) {
        if (s == hour_pos || s == min_pos) continue;
        ring_set(ring, s, COLOUR_SECOND, percent);
    }

    ring_set(ring, min_pos, COLOUR_MINUTE, percent);
    ring_set(ring, hour_pos, COLOUR_HOUR, percent);
    ring_flush(ring);

    // The plates rotate through time, date, temperature and humidity. The phase
    // comes from seconds since midnight rather than tm_sec, so it does not jump
    // at the top of each minute; 86400 divides exactly by the full cycle at the
    // default interval, so midnight is seamless too.
    int phase = PHASE_TIME;
#if CONFIG_DISPLAY_ALTERNATE_DATE
    int secs_of_day = local->tm_hour * 3600 + local->tm_min * 60 + local->tm_sec;
    int phases = dht_enabled() ? PHASE_COUNT : PHASE_DATE + 1;
    phase = (secs_of_day / CONFIG_DISPLAY_CYCLE_SECONDS) % phases;
#endif

    int temperature = 0, humidity = 0;
    bool have_reading = dht_read(&temperature, &humidity);

    switch (phase) {
    case PHASE_DATE:
        draw_pair(0, local->tm_mday, seg_percent);
        draw_pair(2, local->tm_mon + 1, seg_percent);
        break;

    case PHASE_TEMPERATURE:
        // Dashes rather than a stale or invented number: a sensor that has
        // stopped answering should be visible, not silently skipped.
        if (have_reading) draw_pair(0, temperature, seg_percent);
        else { draw_glyph(0, GLYPH_DASH, seg_percent); draw_glyph(1, GLYPH_DASH, seg_percent); }
        draw_glyph(2, GLYPH_DEGREE, seg_percent);
        draw_glyph(3, GLYPH_C, seg_percent);
        break;

    case PHASE_HUMIDITY:
        if (have_reading) draw_pair(0, humidity, seg_percent);
        else { draw_glyph(0, GLYPH_DASH, seg_percent); draw_glyph(1, GLYPH_DASH, seg_percent); }
        draw_glyph(2, GLYPH_R, seg_percent);     // rH, relative humidity
        draw_glyph(3, GLYPH_H, seg_percent);
        break;

    default:
        draw_pair(0, local->tm_hour, seg_percent);
        draw_pair(2, local->tm_min, seg_percent);
        break;
    }

    ESP_ERROR_CHECK(led_strip_refresh(s_seg));
}

void display_show_waiting(void)
{
    if (!s_ring || !s_seg) return;

    int percent = CONFIG_DISPLAY_RING_NIGHT_BRIGHTNESS;
    rgb_t ring[RING_COUNT];
    memset(ring, 0, sizeof(ring));
    for (int h = 0; h < 12; h++)
        ring_set(ring, h * 5, COLOUR_MARK, percent);
    ring_flush(ring);

    // Four dashes.
    for (int digit = 0; digit < 4; digit++)
        draw_glyph(digit, GLYPH_DASH, CONFIG_DISPLAY_SEG_NIGHT_BRIGHTNESS);
    ESP_ERROR_CHECK(led_strip_refresh(s_seg));
}

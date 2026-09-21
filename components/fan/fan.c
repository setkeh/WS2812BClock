#include "fan.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

//Fan Setup
#define FAN1_PWM_GPIO CONFIG_FAN1_PWM_GPIO
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL    LEDC_CHANNEL_0
#define LEDC_DUTY_RES   LEDC_TIMER_8_BIT // Set resolution to 13 bits (0-8191)
#define LEDC_FREQUENCY  (5000) // Frequency in Hertz (5 kHz)
#define FAN_MIN_PCT   20
#define FAN_KICK_MS   500

static uint32_t fan_pct = 0;

void init_fan_pwm(void) {
    // 1. Configure the LEDC timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    // 2. Configure the LEDC channel
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = FAN1_PWM_GPIO,
        .duty           = 0, // Set initial duty cycle to 0
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
}

static void set_pwm_duty(uint32_t percent) {
    uint32_t max = 1 << LEDC_DUTY_RES;                 // 256 at 8-bit = 100%
    uint32_t duty = (percent > 100 ? 100 : percent) * max / 100;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

void fan_set(uint32_t pct) {
    if (pct < FAN_MIN_PCT) pct = 0;
    if (fan_pct == 0 && pct > 0) {                 // starting from stopped
        set_pwm_duty(100);
        vTaskDelay(pdMS_TO_TICKS(FAN_KICK_MS));
    }
    set_pwm_duty(pct);
    fan_pct = pct;
}
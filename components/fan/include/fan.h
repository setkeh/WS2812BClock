#pragma once

#include <stdint.h>

void init_fan_pwm(void);
void fan_set(uint32_t pct);
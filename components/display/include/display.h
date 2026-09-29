#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

// Initialise both LED chains. Safe to call once, at startup.
esp_err_t display_init(void);

// Draw the given local time: hands on the ring, HH MM on the digits.
void display_show_time(const struct tm *local);

// Shown before the first NTP sync: dim hour marks and four dashes.
void display_show_waiting(void);

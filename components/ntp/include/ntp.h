#pragma once
#include <stdbool.h>
#include "esp_event.h"

ESP_EVENT_DECLARE_BASE(NTP_EVENT);      // like WIFI_EVENT / IP_EVENT
enum { NTP_EVENT_SYNCED };              // event data: struct timeval

void ntp_init(void);
bool ntp_time_is_valid(void);           // false until the first successful sync

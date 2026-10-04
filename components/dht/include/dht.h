#pragma once

#include <stdbool.h>

// Starts sampling in the background. Safe to call when the sensor is not
// configured, in which case it does nothing.
void dht_init(void);

// True when a sensor is configured at all, decided at build time. Lets a
// caller tell "no sensor fitted" apart from "sensor fitted but not answering",
// which want different handling.
bool dht_enabled(void);

// The most recent trustworthy reading. False before the first one arrives, and
// again if the sensor stops answering for long enough that the cached values
// should not be believed -- a stale temperature is worse than none.
bool dht_read(int *temperature_c, int *humidity_pct);

#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Log the running firmware version and which slot it booted from.
void ota_log_running_version(void);

// Call once the device has proven itself healthy (WiFi up, time synced).
// Until this is called, a freshly flashed image is on probation and the
// bootloader will roll back to the previous one after a reboot.
void ota_mark_current_app_valid(void);

// Fetch and install an update, then reboot on success. Returns an error and
// leaves the running image untouched on failure. Blocks, and needs a stack of
// several KB for TLS - do not call it from the main task.
esp_err_t ota_update_now(void);

// Run ota_update_now() on its own task with enough stack for TLS, and return
// immediately. This is what callers on the main task should use.
void ota_check_async(void);

// If the previous boot left a core dump in flash, send it to the crash
// receiver and erase it once the server has taken it. Does nothing when there
// is no dump, or when uploading is not configured.
//
// Runs on its own task: this is a TLS connection, which needs far more stack
// than the caller is likely to have. Call it once the network is up, and only
// after the summary has been logged -- a successful upload erases the dump.
void ota_upload_coredump_async(void);

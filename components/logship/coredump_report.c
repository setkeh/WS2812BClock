#include "logship.h"

#include "sdkconfig.h"

#if CONFIG_LOGSHIP_REPORT_COREDUMP

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_core_dump.h"
#include "esp_log.h"

static const char *TAG = "crash";

// 16 frames of " 0x........" plus room for the terminator.
#define BACKTRACE_CHARS 192

// The Xtensa EXCCAUSE values that actually show up in application faults.
// Anything else is printed as a bare number.
static const char *exccause_name(uint32_t cause)
{
    switch (cause) {
        case 0:  return "IllegalInstruction";
        case 1:  return "Syscall";
        case 2:  return "InstructionFetchError";
        case 3:  return "LoadStoreError";
        case 5:  return "Alloca";
        case 6:  return "IntegerDivideByZero";
        case 8:  return "PrivilegedInstruction";
        case 9:  return "LoadStoreAlignment";
        case 20: return "InstrFetchProhibited";
        case 28: return "LoadProhibited";
        case 29: return "StoreProhibited";
        default: return "unknown";
    }
}

void logship_report_coredump(void)
{
    esp_err_t err = esp_core_dump_image_check();
    if (err == ESP_ERR_NOT_FOUND) return;          // clean boot, nothing stored
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a core dump is stored but unreadable: %s", esp_err_to_name(err));
        return;
    }

    // ~600 bytes on xtensa, too much for the caller's stack.
    esp_core_dump_summary_t *s = calloc(1, sizeof(*s));
    if (!s) {
        ESP_LOGE(TAG, "no memory to read the core dump summary");
        return;
    }

    if (esp_core_dump_get_summary(s) != ESP_OK) {
        ESP_LOGE(TAG, "core dump present but the summary could not be read");
        free(s);
        return;
    }

    ESP_LOGE(TAG, "the previous boot crashed in task \"%s\"", s->exc_task);
    ESP_LOGE(TAG, "PC 0x%08" PRIx32 ", EXCCAUSE %" PRIu32 " (%s), EXCVADDR 0x%08" PRIx32,
             s->exc_pc, s->ex_info.exc_cause, exccause_name(s->ex_info.exc_cause),
             s->ex_info.exc_vaddr);

    // One line, so it survives the trip as a single syslog datagram and can be
    // pasted straight into addr2line.
    char bt[BACKTRACE_CHARS];
    int n = 0;
    for (uint32_t i = 0; i < s->exc_bt_info.depth && n < (int)sizeof(bt) - 12; i++)
        n += snprintf(bt + n, sizeof(bt) - n, " 0x%08" PRIx32, s->exc_bt_info.bt[i]);
    ESP_LOGE(TAG, "backtrace%s%s", n ? bt : " unavailable",
             s->exc_bt_info.corrupted ? " (corrupted)" : "");

    // Symbols only line up against the build this dump came from, so record
    // which one that was.
    ESP_LOGE(TAG, "crashing app elf sha256 %s", (const char *)s->app_elf_sha256);
    ESP_LOGW(TAG, "the dump stays in flash; run \"idf.py coredump-info\" for a full trace");

    free(s);
}

#else  /* !CONFIG_LOGSHIP_REPORT_COREDUMP */

void logship_report_coredump(void) { }

#endif

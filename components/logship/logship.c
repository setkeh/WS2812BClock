#include "logship.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_LOGSHIP_ENABLE

#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "logship";

// Not LINE_MAX: that is a POSIX macro from <limits.h>.
#define LINE_BYTES   CONFIG_LOGSHIP_LINE_MAX
#define QUEUE_DEPTH  CONFIG_LOGSHIP_QUEUE_DEPTH

// local0, the conventional facility for application logs.
// PRI = facility * 8 + severity.
#define SYSLOG_FACILITY 16

// Only string formatting and sendto, so nothing like the 8 KB the TLS-based
// OTA task needs — but the DNS resolver also runs here, so not 2 KB either.
#define SENDER_STACK 4096

// How long to wait before trying the collector's name again after a failure,
// so a collector that is down does not mean a DNS lookup per log line.
#define RESOLVE_RETRY_US (10 * 1000000LL)

// Anything at or after 2024 means NTP has landed and the wall clock is real.
#define CLOCK_VALID_EPOCH 1704067200LL

typedef struct {
    uint16_t len;
    uint8_t  sev;
    int64_t  mono_us;   // esp_timer_get_time() when the line was logged
    int64_t  wall_us;   // wall clock when logged, 0 if it was not set yet
    char     text[LINE_BYTES];
} line_t;

static QueueHandle_t     s_queue;
static SemaphoreHandle_t s_lock;     // guards the partial line below
static line_t            s_acc;      // the line currently being assembled
static TaskHandle_t      s_acc_owner;
static TaskHandle_t      s_sender;
static bool              s_started;
static vprintf_like_t    s_prev;
static atomic_uint      s_dropped;   // bumped from any task, cleared by the sender

static int      s_sock = -1;
static int64_t  s_next_resolve;
static struct sockaddr_in s_dest;

// ESP-IDF puts the level in the first character of the formatted line.
static uint8_t severity_of(char level)
{
    switch (level) {
        case 'E': return 3;    // err
        case 'W': return 4;    // warning
        case 'D':              // fall through
        case 'V': return 7;    // debug
        default:  return 6;    // info
    }
}

// Flatten a formatted log line into something a syslog frame can carry: no
// colour escapes, no embedded newlines, no other control characters. Writes
// back over the same buffer, which is safe because the output index never
// runs ahead of the input one. Returns the new length.
static uint16_t flatten(char *buf, int len)
{
    int out = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0x1b) {
            // ESP-IDF only emits SGR colour sequences, which end in 'm'.
            if (i + 1 < len && buf[i + 1] == '[') i++;
            while (i + 1 < len && buf[i + 1] != 'm') i++;
            i++;                       // step onto the 'm'; the loop skips it
            continue;
        }
        buf[out++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    while (out > 0 && buf[out - 1] == ' ') out--;   // trailing newline became a space
    buf[out] = '\0';
    return (uint16_t)out;
}

// Hands a finished line to the sender. The lock must be held.
static void emit_locked(void)
{
    s_acc.len = flatten(s_acc.text, s_acc.len);
    if (s_acc.len > 0) {
        s_acc.sev = severity_of(s_acc.text[0]);

        // Never wait for room: a full queue means the link is down, and the
        // clock must keep running regardless. When it is full, throw away the
        // OLDEST line instead of this one -- what led up to a fault is worth
        // more than what the device was doing at boot.
        if (xQueueSend(s_queue, &s_acc, 0) != pdTRUE) {
            line_t evicted;
            if (xQueueReceive(s_queue, &evicted, 0) == pdTRUE)
                atomic_fetch_add(&s_dropped, 1);
            if (xQueueSend(s_queue, &s_acc, 0) != pdTRUE)
                atomic_fetch_add(&s_dropped, 1);
        }
    }
    s_acc.len = 0;
    s_acc_owner = NULL;
}

// One logical log line does not always arrive in one call: the WiFi driver
// emits its banner as a prefix plus a separate body, and shipping each
// fragment as its own syslog message makes the stream unreadable. Fragments
// are appended until a newline shows the line is complete.
static void accumulate(const char *fmt, va_list args)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    struct timeval tv;
    size_t room;
    int wrote;
    bool complete;

    // The lock is held for a vsnprintf and a queue push -- microseconds -- so
    // this timeout should never be reached. It is a bound, not a wait: the
    // clock must not stall because something else is logging.
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1);
        return;
    }

    // A fragment from a different task means the line in progress will never
    // be finished. Flush it as it stands rather than interleaving two lines.
    if (s_acc.len > 0 && s_acc_owner != me)
        emit_locked();

    if (s_acc.len == 0) {
        // Timestamp the line where it starts, not where it ends.
        s_acc_owner = me;
        s_acc.mono_us = esp_timer_get_time();
        gettimeofday(&tv, NULL);
        s_acc.wall_us = (tv.tv_sec >= CLOCK_VALID_EPOCH)
                      ? (int64_t)tv.tv_sec * 1000000 + tv.tv_usec : 0;
    }

    room = sizeof(s_acc.text) - s_acc.len;
    wrote = vsnprintf(s_acc.text + s_acc.len, room, fmt, args);
    if (wrote < 0) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (wrote >= (int)room) wrote = room - 1;      // truncated to fit

    // Checked before flatten(), which turns the newline into a space.
    complete = memchr(s_acc.text + s_acc.len, '\n', wrote) != NULL;
    s_acc.len += wrote;

    if (complete || s_acc.len >= sizeof(s_acc.text) - 1)
        emit_locked();

    xSemaphoreGive(s_lock);
}

// Flushes a fragment that never got its newline, so a partial line cannot sit
// in the buffer indefinitely when the device goes quiet. Called from the
// sender when it has nothing else to do.
static void flush_stale(void)
{
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;
    if (s_acc.len > 0)
        emit_locked();
    xSemaphoreGive(s_lock);
}

// Runs on whichever task called ESP_LOGx, so it must not block and must not
// log anything itself.
static int log_hook(const char *fmt, va_list args)
{
    // A va_list can only be walked once and the next sink in the chain (the
    // activity LED, then the UART) still needs it, so format from a copy.
    va_list copy;
    va_copy(copy, args);

    int n = s_prev ? s_prev(fmt, args) : vprintf(fmt, args);

    // Anything the sender task logs would arrive straight back here; the DNS
    // resolver and lwip do log on failure, so that loop has to be cut.
    if (s_queue && xTaskGetCurrentTaskHandle() != s_sender)
        accumulate(fmt, copy);

    va_end(copy);
    return n;
}

// Whatever the clock is called on the network is what separates one clock
// from another in the log stream.
static const char *device_hostname(void)
{
    static char name[32];
    const char *h = NULL;

    if (name[0]) return name;

    if (strlen(CONFIG_LOGSHIP_HOSTNAME) > 0) {
        strlcpy(name, CONFIG_LOGSHIP_HOSTNAME, sizeof(name));
        return name;
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_hostname(sta, &h) == ESP_OK && h && *h)
        strlcpy(name, h, sizeof(name));
    else
        strlcpy(name, "esp32", sizeof(name));

    for (char *p = name; *p; p++)
        if (*p == ' ') *p = '-';    // the HOSTNAME field is space-delimited
    return name;
}

// The time a line is sent is not the time it happened: boot messages sit in
// the queue until the network is up, which can be seconds. Stamp them with
// when they were logged.
static void timestamp(char *out, size_t cap, const line_t *line)
{
    struct timeval now;
    struct tm utc;
    int64_t wall_us = line->wall_us;

    if (wall_us == 0) {
        // The clock was not set when this line was logged. If it has been set
        // since -- the normal case for boot messages, which queue up until NTP
        // lands -- recover the real time by working back from how long ago it
        // was logged, measured on the monotonic timer.
        gettimeofday(&now, NULL);
        if (now.tv_sec >= CLOCK_VALID_EPOCH) {
            int64_t now_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
            wall_us = now_us - (esp_timer_get_time() - line->mono_us);
        }
    }

    // Still nothing usable: "-" is the RFC 5424 way of saying "no timestamp",
    // which makes the collector stamp it on arrival rather than filing boot
    // messages half a century ago.
    if (wall_us == 0) {
        strlcpy(out, "-", cap);
        return;
    }

    time_t secs = (time_t)(wall_us / 1000000);
    gmtime_r(&secs, &utc);
    size_t n = strftime(out, cap, "%Y-%m-%dT%H:%M:%S", &utc);
    snprintf(out + n, cap - n, ".%06dZ", (int)(wall_us % 1000000));
}

static bool destination_ready(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    int64_t now;

    if (s_sock >= 0) return true;

    now = esp_timer_get_time();
    if (now < s_next_resolve) return false;
    s_next_resolve = now + RESOLVE_RETRY_US;

    // Everything logged from here on reaches the serial console but is never
    // shipped -- the recursion guard drops this task's own lines. That is the
    // right way round: these are the messages you need precisely when shipping
    // is what is broken.
    if (getaddrinfo(CONFIG_LOGSHIP_HOST, NULL, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "cannot resolve \"%s\", retrying in %d s",
                 CONFIG_LOGSHIP_HOST, (int)(RESOLVE_RETRY_US / 1000000));
        return false;
    }
    memcpy(&s_dest, res->ai_addr, sizeof(s_dest));
    freeaddrinfo(res);
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons(CONFIG_LOGSHIP_PORT);

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "cannot create the UDP socket: errno %d", errno);
        return false;
    }

    uint32_t a = ntohl(s_dest.sin_addr.s_addr);
    ESP_LOGI(TAG, "collector \"%s\" is %u.%u.%u.%u:%d",
             CONFIG_LOGSHIP_HOST, (unsigned)(a >> 24 & 0xff), (unsigned)(a >> 16 & 0xff),
             (unsigned)(a >> 8 & 0xff), (unsigned)(a & 0xff), CONFIG_LOGSHIP_PORT);
    return true;
}

// RFC 5424: <PRI>VERSION TIMESTAMP HOSTNAME APP-NAME PROCID MSGID SD MSG
static void ship(const line_t *line)
{
    static char frame[LINE_BYTES + 128];   // sender task only, so static is safe
    char ts[40];

    timestamp(ts, sizeof(ts), line);
    int n = snprintf(frame, sizeof(frame) - 1, "<%d>1 %s %s %s - - - %.*s",
                     SYSLOG_FACILITY * 8 + line->sev, ts, device_hostname(),
                     CONFIG_LOGSHIP_APP_NAME, (int)line->len, line->text);
    if (n <= 0) return;
    if (n >= (int)sizeof(frame) - 1) n = sizeof(frame) - 2;

    // RFC 6587 non-transparent framing. Receivers pick their parser from the
    // first byte -- '<' means "newline-delimited" -- and then treat the
    // datagrams from one sender as a stream, so without this terminator every
    // message runs into the next one.
    frame[n++] = '\n';

    if (sendto(s_sock, frame, n, 0, (struct sockaddr *)&s_dest, sizeof(s_dest)) < 0) {
        // Usually the route going away. Drop the socket so the name is looked
        // up again; the line itself is gone, which is the deal with UDP.
        ESP_LOGW(TAG, "send failed: errno %d, reopening", errno);
        close(s_sock);
        s_sock = -1;
    }
}

// Losing lines silently would make the stream lie about what happened, so say
// how many went missing. Built by hand rather than with ESP_LOGW, because a
// log call from this task is exactly what the recursion guard throws away.
static void report_drops(void)
{
    uint32_t d = atomic_exchange(&s_dropped, 0);
    line_t note;
    int n;

    if (d == 0) return;

    n = snprintf(note.text, sizeof(note.text),
                 "W (%lu) logship: dropped %lu log lines while the collector was unreachable",
                 (unsigned long)(esp_timer_get_time() / 1000), (unsigned long)d);
    if (n <= 0) return;
    if (n >= (int)sizeof(note.text)) n = sizeof(note.text) - 1;
    note.len = (uint16_t)n;
    note.sev = 4;
    note.mono_us = esp_timer_get_time();
    note.wall_us = 0;   // stamped from the monotonic timer like any other line
    ship(&note);
}

static void sender_task(void *arg)
{
    line_t line;

    // Set here as well as by xTaskCreate: this task runs at a higher priority
    // than the one that creates it, so it can reach the recursion guard before
    // xTaskCreate has returned and filled in the handle.
    s_sender = xTaskGetCurrentTaskHandle();

    while (true) {
        if (!destination_ready()) {
            // Lines stay queued meanwhile, so the boot messages survive the
            // wait for WiFi rather than being thrown away before it is up.
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (xQueueReceive(s_queue, &line, pdMS_TO_TICKS(1000)) != pdTRUE) {
            flush_stale();       // nothing came; release any unterminated line
            continue;
        }
        report_drops();
        ship(&line);
    }
}

void logship_init(void)
{
    if (strlen(CONFIG_LOGSHIP_HOST) == 0) {
        ESP_LOGW(TAG, "no collector configured, logs stay on the serial port");
        return;
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(line_t));
    s_lock = xSemaphoreCreateMutex();
    if (!s_queue || !s_lock) {
        ESP_LOGE(TAG, "could not allocate the log queue");
        s_queue = NULL;
        return;
    }

    // Capturing can start now; sending cannot. Nothing here touches lwip, so
    // this is safe before the TCP/IP stack exists.
    s_prev = esp_log_set_vprintf(log_hook);
}

void logship_start(void)
{
    if (!s_queue || s_started) return;
    s_started = true;

    if (xTaskCreate(sender_task, "logship", SENDER_STACK, NULL, 3, &s_sender) != pdPASS) {
        ESP_LOGE(TAG, "could not start the log sender");
        return;
    }

    ESP_LOGI(TAG, "shipping logs to %s:%d as %s",
             CONFIG_LOGSHIP_HOST, CONFIG_LOGSHIP_PORT, CONFIG_LOGSHIP_APP_NAME);
}

#else  /* !CONFIG_LOGSHIP_ENABLE */

void logship_init(void) { }
void logship_start(void) { }

#endif

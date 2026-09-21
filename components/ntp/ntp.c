#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_netif_sntp.h"
#include "lwip/ip_addr.h"
#include "esp_sntp.h"
#include "ntp.h"

#define TIME_SERVER CONFIG_SNTP_TIME_SERVER
#define TZ CONFIG_NTP_TZ

ESP_EVENT_DEFINE_BASE(NTP_EVENT);

static const char *TAG = "NTP:";

static volatile bool s_time_valid = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    s_time_valid = true;
    // Post a copy of the new time to anyone listening. Timeout 0: this runs on
    // the network stack's thread, so never block here.
    esp_event_post(NTP_EVENT, NTP_EVENT_SYNCED, tv, sizeof(*tv), 0);
}

bool ntp_time_is_valid(void)
{
    return s_time_valid;
}

void ntp_init(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(TIME_SERVER);
    config.start = true;                       // start SNTP service explicitly (after connecting)
    config.server_from_dhcp = false;            // accept NTP offers from DHCP server, if any (need to enable *before* connecting)
    config.renew_servers_after_new_IP = true;   // let esp-netif update configured SNTP server(s) after receiving DHCP lease
    config.sync_cb = time_sync_notification_cb; // Note: This is only needed if we want
    #ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
        config.smooth_sync = true;
    #endif
    #ifdef CONFIG_SNTP_TIME_SYNC_METHOD_IMMED
        config.smooth_sync = false;
    #endif

    setenv("TZ", TZ, 1);   // default "AEST-10AEDT,M10.1.0,M4.1.0/3"
    tzset();

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }

}
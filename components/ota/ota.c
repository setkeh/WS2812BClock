#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#if CONFIG_OTA_CERT_SOURCE_EMBEDDED
// components/ota/certs/server_ca.pem, embedded by the component CMakeLists
extern const uint8_t server_ca_pem_start[] asm("_binary_server_ca_pem_start");
#endif

#define MANIFEST_MAX 1024
#define URL_MAX      256
#define VERSION_MAX  32
#define OTA_FILE_MAX 96

void ota_log_running_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *part = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "firmware %s, built %s %s, running from %s",
             app->version, app->date, app->time, part ? part->label : "?");

    esp_ota_img_states_t state;
    if (part && esp_ota_get_state_partition(part, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "image is on probation: it will roll back unless it "
                      "marks itself valid this boot");
    }

    // A clock on a wall that cannot update itself is the worst way to fail,
    // because nothing about it looks wrong. Say so at every boot, loudly
    // enough that it shows up in the log stream.
    if (strlen(CONFIG_OTA_BASE_URL) == 0)
        ESP_LOGW(TAG, "NO UPDATE SERVER CONFIGURED: this build cannot update itself");
#if !CONFIG_OTA_CHECK_ON_BOOT
    ESP_LOGW(TAG, "update checking is disabled: this build will not fetch updates");
#endif
}

void ota_mark_current_app_valid(void)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (!part || esp_ota_get_state_partition(part, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;   // nothing to confirm

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "image marked valid, rollback cancelled");
    } else {
        ESP_LOGE(TAG, "could not mark image valid: %s", esp_err_to_name(err));
    }
}

// Runs once the HTTP client exists, before the request is sent.
static esp_err_t add_auth_header(esp_http_client_handle_t http)
{
    if (strlen(CONFIG_OTA_AUTH_TOKEN) > 0)
        return esp_http_client_set_header(http, "X-OTA-Token", CONFIG_OTA_AUTH_TOKEN);
    return ESP_OK;
}

// Shared TLS/auth settings, so the manifest and the image are fetched the
// same way.
static void fill_http_config(esp_http_client_config_t *cfg, const char *url)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = 20000;
    cfg->keep_alive_enable = true;
#if CONFIG_OTA_CERT_SOURCE_EMBEDDED
    cfg->cert_pem = (const char *)server_ca_pem_start;
#else
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
#endif
}

// GET <base>/<model>/latest.json and pull out "version" and "file".
static esp_err_t fetch_manifest(char *version, size_t version_len,
                                char *file, size_t file_len)
{
    char url[URL_MAX];
    snprintf(url, sizeof(url), "%s/%s/latest.json",
             CONFIG_OTA_BASE_URL, CONFIG_OTA_MODEL);

    esp_http_client_config_t cfg;
    fill_http_config(&cfg, url);

    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return ESP_FAIL;
    add_auth_header(http);

    esp_err_t err = esp_http_client_open(http, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "manifest: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http);
        return err;
    }

    esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    if (status != 200) {
        ESP_LOGE(TAG, "manifest: HTTP %d for %s", status, url);
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        return ESP_FAIL;
    }

    char body[MANIFEST_MAX];
    int len = esp_http_client_read(http, body, sizeof(body) - 1);
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    if (len <= 0) return ESP_FAIL;
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "manifest is not valid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *jfile = cJSON_GetObjectItemCaseSensitive(root, "file");
    err = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsString(jver) && cJSON_IsString(jfile)) {
        strlcpy(version, jver->valuestring, version_len);
        strlcpy(file, jfile->valuestring, file_len);
        err = ESP_OK;
    } else {
        ESP_LOGE(TAG, "manifest needs string fields \"version\" and \"file\"");
    }

    cJSON_Delete(root);
    return err;
}

esp_err_t ota_update_now(void)
{
    if (strlen(CONFIG_OTA_BASE_URL) == 0) {
        ESP_LOGW(TAG, "no update server configured");
        return ESP_ERR_INVALID_STATE;
    }

    char offered[VERSION_MAX] = {0};
    char file[OTA_FILE_MAX] = {0};
    esp_err_t err = fetch_manifest(offered, sizeof(offered), file, sizeof(file));
    if (err != ESP_OK) return err;

    const esp_app_desc_t *running = esp_app_get_description();
    ESP_LOGI(TAG, "manifest offers %s, running %s", offered, running->version);

    // Any difference is an instruction to install, so the manifest can also be
    // used to roll a fleet back to an earlier build.
    if (strcmp(offered, running->version) == 0) {
        ESP_LOGI(TAG, "already up to date");
        return ESP_OK;
    }

    char url[URL_MAX];
    snprintf(url, sizeof(url), "%s/%s/%s",
             CONFIG_OTA_BASE_URL, CONFIG_OTA_MODEL, file);
    ESP_LOGI(TAG, "installing %s", url);

    esp_http_client_config_t http_cfg;
    fill_http_config(&http_cfg, url);

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
        .http_client_init_cb = add_auth_header,
    };

    // Signature verification happens inside esp_https_ota when signed images
    // are enabled, so a corrupted or unsigned download is rejected here.
    err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "update installed, restarting");
    esp_restart();
    return ESP_OK;   // not reached
}

// TLS needs far more stack than the main task has (~3.5 KB by default), so the
// check runs on its own task and the caller is not blocked by the download.
#define OTA_TASK_STACK 8192

static void ota_task(void *arg)
{
    while (true) {
        ota_update_now();      // reboots on success, so this returns only
                               // when there is nothing to do or it failed
#if CONFIG_OTA_CHECK_INTERVAL_MIN > 0
        vTaskDelay(pdMS_TO_TICKS(CONFIG_OTA_CHECK_INTERVAL_MIN * 60 * 1000));
#else
        break;
#endif
    }
    vTaskDelete(NULL);
}

void ota_check_async(void)
{
    if (xTaskCreate(ota_task, "ota_check", OTA_TASK_STACK, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "could not start the update task");
}

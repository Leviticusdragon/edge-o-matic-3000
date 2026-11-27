#include "wifi_manager.h"
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sntp.h"
#include "system/http_server.h"
#include <stdbool.h>
#include <string.h>

static const char* TAG = "wifi_manager";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static char s_wifi_ip_addr_str[20] = "";
static bool s_initialized = false;
static wifi_manager_status_t s_wifi_status = WIFI_MANAGER_DISCONNECTED;
static bool s_mdns_running = false;
static esp_timer_handle_t s_retry_timer = NULL;
static TaskHandle_t s_services_task = NULL;

// After this many immediate retries, connect_to_ap() reports failure so boot can continue,
// but the device keeps retrying in the background every WIFI_BACKGROUND_RETRY_MS instead of
// giving up permanently (previously Wi-Fi stayed dead until a reboot).
#define WIFI_MAX_CONNECTION_RETRY 5
#define WIFI_BACKGROUND_RETRY_MS 5000

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static void wifi_apply_power_save(void) {
    // Default ESP-IDF station mode is MIN_MODEM power save: the radio sleeps between AP
    // beacons, which adds 100-300 ms latency and drops packets on a weak link. The EOM is
    // mains-powered, so keep the radio fully awake. Wi-Fi/BT coexistence requires modem
    // sleep, so only disable it when Bluetooth is off.
    if (Config.bt_on || Config.force_bt_coex) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {
        esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "Wi-Fi power save disabled: %s", esp_err_to_name(err));
    }
}

static void wifi_start_mdns(void) {
    if (s_mdns_running || !Config.mdns_enabled || Config.hostname[0] == '\0') return;

    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_hostname_set(Config.hostname);
        mdns_instance_name_set(Config.bt_display_name);
        s_mdns_running = true;
        ESP_LOGI(TAG, "mDNS started: %s.local", Config.hostname);
    } else {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
    }
}

static void wifi_start_services(void) {
    wifi_start_mdns();
    if (Config.websocket_port > 0) {
        esp_err_t err = http_server_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server failed to start: %s", esp_err_to_name(err));
        }
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}

// mDNS and the HTTP server are (re)started from this task rather than inside the event
// handler: the default event loop task only has a small stack. It is also the only place
// that starts them, so there is no start race with a task waiting in connect_to_ap().
static void wifi_services_task(void* arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_wifi_status == WIFI_MANAGER_CONNECTED) {
            wifi_start_services();
        }
    }
}

static void wifi_retry_timer_cb(void* arg) {
    if (s_wifi_status == WIFI_MANAGER_DISCONNECTING || s_wifi_status == WIFI_MANAGER_CONNECTED ||
        !Config.wifi_on)
        return;

    ESP_LOGI(TAG, "Background Wi-Fi reconnect attempt");
    esp_wifi_connect();
}


static void
event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            wifi_config_t config = { 0 };
            esp_wifi_get_config(WIFI_IF_STA, &config);

            if (config.ap.ssid[0] != '\0' && Config.wifi_on) {
                ESP_LOGI(TAG, "Auto-connect WiFi to: %s", config.ap.ssid);
                esp_wifi_connect();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            // mDNS is intentionally left running on an unexpected disconnect: the ESP-IDF mDNS
            // component disables/re-enables itself on the interface events, so eom3k.local comes
            // back after a reconnect. (Previously it was freed here and never restarted.)
            wifi_event_sta_disconnected_t* disc = (wifi_event_sta_disconnected_t*)event_data;

            if (s_wifi_status == WIFI_MANAGER_DISCONNECTING) {
                s_wifi_status = WIFI_MANAGER_DISCONNECTED;
                return;
            } else if (!Config.wifi_on) {
                s_wifi_status = WIFI_MANAGER_DISCONNECTED;
                return;
            } else if (s_retry_num < WIFI_MAX_CONNECTION_RETRY) {
                s_wifi_status = WIFI_MANAGER_RECONNECTING;
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(
                    TAG,
                    "WiFi Disconnected (reason %d), Retrying (attempt %d)",
                    disc ? disc->reason : -1,
                    s_retry_num
                );
            } else {
                s_wifi_status = WIFI_MANAGER_RECONNECTING;
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                ESP_LOGW(
                    TAG,
                    "WiFi Disconnected (reason %d), retrying every %d ms.",
                    disc ? disc->reason : -1,
                    WIFI_BACKGROUND_RETRY_MS
                );
                if (s_retry_timer != NULL) {
                    esp_timer_stop(s_retry_timer);
                    esp_timer_start_once(s_retry_timer, WIFI_BACKGROUND_RETRY_MS * 1000ULL);
                }
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            snprintf(s_wifi_ip_addr_str, 20, IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Got IP: %s", s_wifi_ip_addr_str);
            s_retry_num = 0;
            if (s_retry_timer != NULL) esp_timer_stop(s_retry_timer);
            s_wifi_status = WIFI_MANAGER_CONNECTED;

            // (Re)start network services after any connect, including a background reconnect
            // that happens after connect_to_ap() already gave up. Both are idempotent.
            if (s_services_task != NULL) {
                xTaskNotifyGive(s_services_task);
            } else {
                wifi_start_services();
            }
        }
    }
}

void wifi_manager_init(void) {
    if (s_initialized) return;

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_tls_init_global_ca_store());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id
    ));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip
    ));

    const esp_timer_create_args_t retry_timer_args = {
        .callback = &wifi_retry_timer_cb,
        .name = "wifi_retry",
    };
    esp_timer_create(&retry_timer_args, &s_retry_timer);
    xTaskCreate(wifi_services_task, "wifi_services", 4096, NULL, 5, &s_services_task);

    // Don't save WiFi creds.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    // initialize wifi with default config
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_apply_power_save();

    // Use Internet Time
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    s_initialized = true;
}

/** @todo Pelase actually implement me for the love of Yusuke Kitagawa. */
void wifi_manager_deinit(void) {
    wifi_manager_disconnect();
}

esp_err_t wifi_manager_connect_to_ap(const char* ssid, const char* key) {
    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "Connecting to AP requires SSID. Aborting.");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
            // Scan every channel and join the strongest AP with this SSID, instead of the
            // first one found. Matters when an extender/mesh pod shares the router's SSID.
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };

    strlcpy((char*)wifi_config.sta.ssid, ssid, 32);
    strlcpy((char*)wifi_config.sta.password, key, 64);

    if (s_retry_timer != NULL) esp_timer_stop(s_retry_timer);
    s_retry_num = 0;
    s_wifi_status = WIFI_MANAGER_RECONNECTING;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_apply_power_save();

    ESP_LOGI(TAG, "Connecting to WiFi: \"%s\"", wifi_config.sta.ssid);
    ESP_LOGD(TAG, "    Using Password: \"%s\"", wifi_config.sta.password);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY
    );

    if (bits & WIFI_CONNECTED_BIT) {
        // mDNS and the HTTP/WebSocket server were started by the GOT_IP handler.
        ESP_LOGI(TAG, "Connected to AP.");

        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Connection failure, check other logs.");
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Unknown bitmask for connection.");
        return ESP_FAIL;
    }
}

/**
 * Scan for WiFi networks and populate a buffer. *count should initially contain the maximum number
 * of networks to scan. After this function returns, it will contain the number of networks found.
 */
esp_err_t wifi_manager_scan(wifi_ap_record_t* ap_info, uint16_t* count) {
    esp_err_t err = ESP_OK;
    uint16_t len = *count;
    uint16_t ap_count = 0;
    memset(ap_info, 0, sizeof(wifi_ap_record_t) * len);

    // Start a blocking scan:
    err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_scan_get_ap_records(count, ap_info);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        return err;
    }

    if (ap_count < *count) {
        *count = ap_count;
    }
    return err;
}

void wifi_manager_disconnect(void) {
    s_wifi_status = WIFI_MANAGER_DISCONNECTING;
    if (s_retry_timer != NULL) esp_timer_stop(s_retry_timer);
    http_server_disconnect();
    if (s_mdns_running) {
        mdns_free();
        s_mdns_running = false;
        ESP_LOGI(TAG, "mDNS stopped (disconnect request)");
    }
    esp_wifi_disconnect();
}

wifi_manager_status_t wifi_manager_get_status(void) {
    return s_wifi_status;
}

int8_t wifi_manager_get_rssi(void) {
    wifi_ap_record_t ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

const char* wifi_manager_get_local_ip(void) {
    return s_wifi_ip_addr_str;
}
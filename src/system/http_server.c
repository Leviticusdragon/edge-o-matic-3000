#include "system/http_server.h"
#include "api/index.h"
#include "config.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "system/websocket_handler.h"
#include "util/fs.h"

#define DEFAULT_CACERT_PATH "cert.pem"
#define DEFAULT_PRVTKEY_PATH "key.pem"

static const char* TAG = "http_server";
static httpd_handle_t _server = NULL;

static const httpd_uri_t routes[] = { {
    .uri = "/",
    .method = HTTP_GET,
    .handler = &websocket_handler,
    .user_ctx = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = true,
} };

static void init_routes(httpd_handle_t server) {
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t route = routes[i];
        ESP_LOGI(TAG, "* Route: %s", route.uri);
        httpd_register_uri_handler(server, &route);
    }
}

static httpd_handle_t start_webserver(void) {
    esp_err_t err = ESP_OK;
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (Config.websocket_port > 0) {
        config.server_port = Config.websocket_port;
    }

    config.max_open_sockets = 3;

    // When all sockets are taken (e.g. by "zombie" connections left behind by a Wi-Fi drop
    // that the client never closed), close the least-recently-used one instead of refusing
    // the app's reconnect.
    config.lru_purge_enable = true;

    // Broadcasts are sent synchronously from the main loop; a stalled socket used to block
    // it for the 5 s default. Fail faster and let the failed client be closed.
    config.send_wait_timeout = 2;

    // Open/Close callbacks
    config.open_fn = websocket_open_fd;
    config.close_fn = websocket_close_fd;

    if (!Config.use_ssl) {
        ESP_LOGI(TAG, "Starting server on port: %d", config.server_port);
        err = httpd_start(&server, &config);
    } else {
        ESP_LOGI(
            TAG,
            "Starting SSL server on port: %d (%d bytes free)",
            config.server_port,
            esp_get_free_heap_size()
        );

        httpd_ssl_config_t ssl_config = HTTPD_SSL_CONFIG_DEFAULT();
        ssl_config.httpd = config;

        char path[CONFIG_PATH_MAX];

        sniprintf(
            path,
            CONFIG_PATH_MAX,
            "%s/%s",
            fs_sd_root(),
            Config.ssl_cacert_path ? Config.ssl_cacert_path : DEFAULT_CACERT_PATH
        );

        ssl_config.cacert_len = fs_read_file(path, (char**)&ssl_config.cacert_pem);

        if (ssl_config.cacert_len <= 0) {
            ESP_LOGE(TAG, "Error reading CA Certificate.");
            return NULL;
        } else {
            ESP_LOGI(TAG, "%s", ssl_config.cacert_pem);
        }

        sniprintf(
            path,
            CONFIG_PATH_MAX,
            "%s/%s",
            fs_sd_root(),
            Config.ssl_prvtkey_path ? Config.ssl_prvtkey_path : DEFAULT_PRVTKEY_PATH
        );

        ssl_config.prvtkey_len = fs_read_file(path, (char**)&ssl_config.prvtkey_pem);

        if (ssl_config.prvtkey_len <= 0) {
            ESP_LOGE(TAG, "Error reading SSL Private Key.");
            free(ssl_config.cacert_pem);
            return NULL;
        } else {
            ESP_LOGI(TAG, "%s", ssl_config.prvtkey_pem);
        }

        ESP_LOGI(TAG, "SSL Certificates provisioned. (%d bytes free)", esp_get_free_heap_size());
        err = httpd_ssl_start(&server, &ssl_config);

        free(ssl_config.cacert_pem);
        free(ssl_config.prvtkey_pem);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server: %s", esp_err_to_name(err));
        return NULL;
    }

    ESP_LOGI(TAG, "Server started, registering routes.");
    init_routes(server);

    return server;
}

static void stop_webserver(httpd_handle_t server) {
    httpd_stop(server);
}

esp_err_t http_server_init(void) {
    // The server is started by wifi_manager on IP_EVENT_STA_GOT_IP (and from connect_to_ap).
    // It is deliberately NOT stopped on a Wi-Fi drop: its listening socket is bound to any
    // address and survives a reconnect, and restarting it would kill every client session.
    // (The old GOT_IP/DISCONNECTED handlers here were registered before the default event
    // loop existed, so they never actually ran.)
    api_register_all();
    return ESP_OK;
}

esp_err_t http_server_connect(void) {
    if (_server != NULL) {
        return ESP_OK; // already running
    }

    ESP_LOGI(TAG, "Starting webserver...");
    httpd_handle_t server = start_webserver();
    if (server == NULL) {
        return ESP_FAIL;
    }
    _server = server;
    return ESP_OK;
}

void http_server_disconnect(void) {
    if (_server == NULL) return;
    stop_webserver(_server);
    _server = NULL;
}
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "microlink.h"

static const char *TAG = "c6_probe";
static EventGroupHandle_t wifi_events;
static const int GOT_IP = BIT0;
static char ssid[33], password[65], relay[64];
static microlink_t *ml;

static void heap_log(const char *stage) {
    ESP_LOGI(TAG, "%s: internal free=%lu largest=%lu", stage,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void load_settings(void) {
    nvs_handle_t nvs;
    if (nvs_open("probe", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t n = sizeof(ssid); nvs_get_str(nvs, "ssid", ssid, &n);
    n = sizeof(password); nvs_get_str(nvs, "pass", password, &n);
    n = sizeof(relay); nvs_get_str(nvs, "relay", relay, &n);
    nvs_close(nvs);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, GOT_IP);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, GOT_IP);
    }
}

static void url_decode(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '+') { *d++ = ' '; s++; }
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0}; *d++ = (char)strtol(hex, NULL, 16); s += 3;
        } else *d++ = *s++;
    }
    *d = 0;
}

static void form_field(char *body, const char *name, char *out, size_t size) {
    char prefix[24]; snprintf(prefix, sizeof(prefix), "%s=", name);
    size_t plen = strlen(prefix);
    for (char *p = body; p && *p; p = strchr(p, '&')) {
        if (*p == '&') p++;
        if (strncmp(p, prefix, plen) == 0) {
            char *end = strchr(p + plen, '&');
            size_t n = end ? (size_t)(end - (p + plen)) : strlen(p + plen);
            if (n >= size) n = size - 1;
            memcpy(out, p + plen, n); out[n] = 0; url_decode(out); return;
        }
        if (!strchr(p, '&')) break;
    }
}

static esp_err_t config_get(httpd_req_t *req) {
    const char *page = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<h2>Clawdmeter C6 network probe</h2><form method=post action=/save>"
        "Wi-Fi SSID <input name=ssid maxlength=32 required><br>"
        "Wi-Fi password <input name=pass type=password maxlength=64><br>"
        "Relay Tailscale IPv4:port (optional) <input name=relay placeholder=100.x.y.z:8765><br>"
        "<button>Save and restart</button></form>"
        "<p>After Wi-Fi joins, read the USB serial log for the Tailscale login URL.</p>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t config_post(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len >= 256) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    char body[256] = {0};
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Short form");
        got += n;
    }
    char new_ssid[33] = {0}, new_pass[65] = {0}, new_relay[64] = {0};
    form_field(body, "ssid", new_ssid, sizeof(new_ssid));
    form_field(body, "pass", new_pass, sizeof(new_pass));
    form_field(body, "relay", new_relay, sizeof(new_relay));
    if (!new_ssid[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    nvs_handle_t nvs;
    if (nvs_open("probe", NVS_READWRITE, &nvs) != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS failed");
    esp_err_t err = nvs_set_str(nvs, "ssid", new_ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass", new_pass);
    if (err == ESP_OK) err = nvs_set_str(nvs, "relay", new_relay);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    memset(new_pass, 0, sizeof(new_pass));
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    httpd_resp_sendstr(req, "Saved. Restarting. Watch the USB serial log for Tailscale login.");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void provisioning_ap(void) {
    ESP_LOGW(TAG, "Wi-Fi not configured or unavailable; starting setup AP");
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t cfg = {0};
    strcpy((char *)cfg.ap.ssid, "Clawdmeter-Setup");
    strcpy((char *)cfg.ap.password, "clawdmeter-setup");
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = 1;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    httpd_handle_t server = NULL;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &http));
    httpd_uri_t get = {.uri="/", .method=HTTP_GET, .handler=config_get};
    httpd_uri_t post = {.uri="/save", .method=HTTP_POST, .handler=config_post};
    httpd_register_uri_handler(server, &get);
    httpd_register_uri_handler(server, &post);
    ESP_LOGW(TAG, "Join Clawdmeter-Setup (password clawdmeter-setup), then open http://192.168.4.1/");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void ml_state(microlink_t *handle, microlink_state_t state, void *unused) {
    ESP_LOGI(TAG, "MicroLink state=%d", state);
    heap_log("MicroLink state change");
    if (state == ML_STATE_NEEDS_LOGIN) {
        char url[MICROLINK_AUTH_URL_MAX];
        if (microlink_get_auth_url(handle, url, sizeof(url)) == ESP_OK)
            ESP_LOGW(TAG, "Open this Tailscale login URL: %s", url);
    }
    if (state == ML_STATE_CONNECTED) {
        char ip[16]; microlink_ip_to_str(microlink_get_vpn_ip(handle), ip);
        ESP_LOGI(TAG, "Tailnet connected: %s", ip);
    }
}

static void poll_relay(void) {
    if (!relay[0]) return;
    char host[64]; strncpy(host, relay, sizeof(host)-1); host[sizeof(host)-1] = 0;
    char *colon = strchr(host, ':');
    int port = 8765;
    if (colon) { *colon++ = 0; port = atoi(colon); }
    uint32_t ip = microlink_parse_ip(host);
    if (!ip || port < 1 || port > 65535) { ESP_LOGE(TAG, "Bad relay address"); return; }
    microlink_tcp_socket_t *sock = microlink_tcp_connect(ml, ip, (uint16_t)port, 5000);
    if (!sock) { ESP_LOGW(TAG, "Relay TCP connect failed"); return; }
    const char *request = "GET /usage HTTP/1.0\r\nConnection: close\r\n\r\n";
    if (microlink_tcp_send(sock, request, strlen(request)) == ESP_OK) {
        char buf[1025]; int n = microlink_tcp_recv(sock, buf, sizeof(buf)-1, 5000);
        if (n > 0) { buf[n] = 0; ESP_LOGI(TAG, "Relay response (%d bytes): %.400s", n, buf); }
        else ESP_LOGW(TAG, "Relay read failed: %d", n);
    }
    microlink_tcp_close(sock);
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    heap_log("boot");
    load_settings();
    if (!relay[0]) snprintf(relay, sizeof(relay), "100.0.0.0:8765");
    if (!ssid[0]) provisioning_ap();
    wifi_events = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid)-1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password)-1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    if (!(xEventGroupWaitBits(wifi_events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000)) & GOT_IP)) {
        esp_wifi_stop(); esp_wifi_deinit(); provisioning_ap();
    }
    heap_log("Wi-Fi connected");
    microlink_config_t mlcfg = {
        .auth_key = "", .device_name = "clawdmeter-c6-probe",
        .enable_derp = true, .enable_stun = true, .enable_disco = true,
        .max_peers = 16,
    };
    ml = microlink_init(&mlcfg);
    if (!ml) { ESP_LOGE(TAG, "MicroLink init failed"); return; }
    microlink_set_state_callback(ml, ml_state, NULL);
    ESP_ERROR_CHECK(microlink_start(ml));
    heap_log("MicroLink started");
    while (1) {
        if (microlink_is_connected(ml)) poll_relay();
        heap_log("loop");
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

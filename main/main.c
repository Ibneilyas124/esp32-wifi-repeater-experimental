/*
 * ESP32 WiFi Repeater / Internet Sharing Router (NAPT)
 * Board: ESP32 DevKit V1        Built by Sarfraz Qureshi - Ibn e Ilyas Technologies
 *
 * - STA connects to the main router, AP re-shares the internet through NAT
 * - DNS handed to clients is configurable (default 8.8.8.8) - fixes "connected, no internet"
 * - Name shown in the main router's client list is configurable (DHCP hostname)
 * - Styled login page (fixed user "Sarfraz", default password "admin", password changeable)
 * - Advanced settings table (all editable, all with defaults), MAC whitelist with device names
 * - Hardware factory reset: hold the BOOT button ~8 s, release when the LED blinks fast
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "repeater";

#define NVS_NS            "repeater"
#define ADMIN_USER        "Sarfraz"        /* fixed, not changeable */
#define WL_BUF            1024             /* whitelist storage buffer */
#define RESET_BTN_GPIO    GPIO_NUM_0       /* BOOT button on DevKit V1 */
#define LED_GPIO          GPIO_NUM_2       /* onboard blue LED on DevKit V1 */
#define RESET_HOLD_MS     8000

/* HTML pages embedded from login.html / admin.html (see main/CMakeLists.txt) */
extern const char login_html_start[] asm("_binary_login_html_start");
extern const char admin_html_start[] asm("_binary_admin_html_start");

static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_sta_connected = false;
static char s_sta_ip[16] = "0.0.0.0";
static int64_t s_boot_time_us = 0;
static char s_session_token[40] = "";

/* ================= Settings table (single source of truth) ================= */

typedef struct {
    const char *key;
    const char *label;
    const char *def;     /* default value */
    const char *type;    /* text | password | password_keep | number | bool */
    const char *hint;
} setting_t;

static const setting_t SETTINGS[] = {
    {"ap_ssid",    "Repeater WiFi name (SSID)",        "Sarfraz",                  "text",          "1-32 characters"},
    {"ap_pass",    "Repeater WiFi password",           "Sarfraz1",                 "password",      "8-63 characters. Blank = open network (no password)"},
    {"ap_hidden",  "Hide repeater WiFi name",          "0",                        "bool",          "Hidden networks must be added manually on phones"},
    {"ap_channel", "Repeater channel",                 "1",                        "number",        "1-13. Automatically follows the main router while connected"},
    {"ap_max_conn","Max connected devices",            "8",                        "number",        "1-10"},
    {"ap_ip",      "Repeater IP address",              "192.168.4.1",              "text",          "Change only if it clashes with the main router's network"},
    {"dns1",       "DNS server given to clients",      "8.8.8.8",                  "text",          "If sites do not open try 8.8.8.8, 8.8.4.4, 1.1.1.1 or your ISP DNS"},
    {"hostname",   "Name shown in main router",        "Ibn-e-Ilyas-Technologies", "text",          "1-32 characters. Letters, digits and hyphen work best on routers"},
    {"sta_mac",    "Custom MAC toward main router",    "",                         "text",          "Blank = factory MAC. Format AA:BB:CC:DD:EE:FF, first byte must be even"},
    {"tx_power",   "Transmit power (dBm)",             "20",                       "number",        "2-20. Lower it if the device overheats"},
    {"admin_pass", "Admin password",                   "admin",                    "password_keep", "4-32 characters. Leave blank to keep the current one"},
};
#define N_SETTINGS (sizeof(SETTINGS) / sizeof(SETTINGS[0]))

/* ================= NVS helpers ================= */

static void nvs_get_string(const char *key, char *out, size_t out_len, const char *def) {
    nvs_handle_t h;
    size_t len = out_len;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, key, out, &len) == ESP_OK) {
            nvs_close(h);
            return;
        }
        nvs_close(h);
    }
    strncpy(out, def, out_len - 1);
    out[out_len - 1] = '\0';
}

static void nvs_set_string(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void wipe_settings(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

static const setting_t *find_setting(const char *key) {
    for (size_t i = 0; i < N_SETTINGS; i++) {
        if (strcmp(SETTINGS[i].key, key) == 0) return &SETTINGS[i];
    }
    return NULL;
}

static void get_setting(const char *key, char *out, size_t out_len) {
    const setting_t *s = find_setting(key);
    nvs_get_string(key, out, out_len, s ? s->def : "");
}

static int get_setting_int(const char *key, int lo, int hi, int fallback) {
    char b[16];
    get_setting(key, b, sizeof(b));
    if (b[0] == '\0') return fallback;
    int v = atoi(b);
    if (v < lo || v > hi) return fallback;
    return v;
}

/* ================= Validation ================= */

static bool valid_host_ipv4(const char *s) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a < 1 || a > 223 || b > 255 || c > 255 || d < 1 || d > 254) return false;
    return true;
}

static bool parse_mac(const char *s, uint8_t out[6]) {
    unsigned v[6];
    char extra;
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &extra) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

static bool int_in_range(const char *v, int lo, int hi) {
    if (*v == '\0' || strlen(v) > 4) return false;
    for (const char *p = v; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    int x = atoi(v);
    return x >= lo && x <= hi;
}

static bool validate_setting(const char *key, const char *v, char *err, size_t n) {
    size_t len = strlen(v);
    if (strcmp(key, "ap_ssid") == 0) {
        if (len < 1 || len > 32) { snprintf(err, n, "must be 1-32 characters"); return false; }
    } else if (strcmp(key, "ap_pass") == 0) {
        if (len != 0 && (len < 8 || len > 63)) { snprintf(err, n, "must be 8-63 characters (or blank for open network)"); return false; }
    } else if (strcmp(key, "ap_hidden") == 0) {
        if (strcmp(v, "0") != 0 && strcmp(v, "1") != 0) { snprintf(err, n, "must be 0 or 1"); return false; }
    } else if (strcmp(key, "ap_channel") == 0) {
        if (!int_in_range(v, 1, 13)) { snprintf(err, n, "must be 1-13"); return false; }
    } else if (strcmp(key, "ap_max_conn") == 0) {
        if (!int_in_range(v, 1, 10)) { snprintf(err, n, "must be 1-10"); return false; }
    } else if (strcmp(key, "ap_ip") == 0 || strcmp(key, "dns1") == 0) {
        if (!valid_host_ipv4(v)) { snprintf(err, n, "must be a valid IPv4 address like 192.168.4.1"); return false; }
    } else if (strcmp(key, "hostname") == 0) {
        if (len < 1 || len > 32) { snprintf(err, n, "must be 1-32 characters"); return false; }
        for (size_t i = 0; i < len; i++) {
            if ((unsigned char)v[i] < 0x20) { snprintf(err, n, "contains invalid characters"); return false; }
        }
    } else if (strcmp(key, "sta_mac") == 0) {
        uint8_t m[6];
        if (len != 0 && (!parse_mac(v, m) || (m[0] & 1))) {
            snprintf(err, n, "must look like AA:BB:CC:DD:EE:FF with an even first byte");
            return false;
        }
    } else if (strcmp(key, "tx_power") == 0) {
        if (!int_in_range(v, 2, 20)) { snprintf(err, n, "must be 2-20"); return false; }
    } else if (strcmp(key, "admin_pass") == 0) {
        if (len < 4 || len > 32) { snprintf(err, n, "must be 4-32 characters"); return false; }
    }
    return true;
}

/* ================= MAC whitelist: "MAC|Name,MAC|Name,..." (empty = allow all) ================= */

static void whitelist_get(char *out, size_t out_len) {
    nvs_get_string("whitelist", out, out_len, "");
}

static void whitelist_set(const char *csv) {
    nvs_set_string("whitelist", csv);
}

static bool mac_str_eq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static void split_entry(const char *entry, char *mac_out, size_t mac_len, char *name_out, size_t name_len) {
    const char *bar = strchr(entry, '|');
    if (bar) {
        size_t mlen = (size_t)(bar - entry);
        if (mlen >= mac_len) mlen = mac_len - 1;
        memcpy(mac_out, entry, mlen);
        mac_out[mlen] = '\0';
        strncpy(name_out, bar + 1, name_len - 1);
        name_out[name_len - 1] = '\0';
    } else {
        strncpy(mac_out, entry, mac_len - 1);
        mac_out[mac_len - 1] = '\0';
        name_out[0] = '\0';
    }
}

static bool whitelist_contains(const char *mac) {
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    if (strlen(list) == 0) return true;
    char *copy = strdup(list);
    if (!copy) return true;
    char *tok = strtok(copy, ",");
    bool found = false;
    while (tok) {
        while (*tok == ' ') tok++;
        char m[20], n[64];
        split_entry(tok, m, sizeof(m), n, sizeof(n));
        if (mac_str_eq(m, mac)) { found = true; break; }
        tok = strtok(NULL, ",");
    }
    free(copy);
    return found;
}

static bool whitelist_get_name(const char *mac, char *name_out, size_t name_len) {
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    if (strlen(list) == 0) return false;
    char *copy = strdup(list);
    if (!copy) return false;
    char *tok = strtok(copy, ",");
    bool found = false;
    while (tok) {
        while (*tok == ' ') tok++;
        char m[20], n[64];
        split_entry(tok, m, sizeof(m), n, sizeof(n));
        if (mac_str_eq(m, mac)) {
            strncpy(name_out, n, name_len - 1);
            name_out[name_len - 1] = '\0';
            found = true;
            break;
        }
        tok = strtok(NULL, ",");
    }
    free(copy);
    return found;
}

static void whitelist_remove(const char *mac) {
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    char out[WL_BUF] = "";
    char *copy = strdup(list);
    if (!copy) return;
    char *tok = strtok(copy, ",");
    bool first = true;
    while (tok) {
        while (*tok == ' ') tok++;
        char m[20], n[64];
        split_entry(tok, m, sizeof(m), n, sizeof(n));
        if (!mac_str_eq(m, mac)) {
            if (!first) strncat(out, ",", sizeof(out) - strlen(out) - 1);
            strncat(out, tok, sizeof(out) - strlen(out) - 1);
            first = false;
        }
        tok = strtok(NULL, ",");
    }
    free(copy);
    whitelist_set(out);
}

/* Adds or renames an entry. Returns false when the list is full. */
static bool whitelist_add(const char *mac, const char *name) {
    whitelist_remove(mac);
    char clean[48];
    size_t j = 0;
    for (size_t i = 0; name && name[i] && j < sizeof(clean) - 1; i++) {
        clean[j++] = (name[i] == ',' || name[i] == '|') ? ' ' : name[i];
    }
    clean[j] = '\0';

    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    char entry[96];
    snprintf(entry, sizeof(entry), "%.20s|%.47s", mac, clean);
    if (strlen(list) + strlen(entry) + 2 >= WL_BUF) return false;
    if (strlen(list) == 0) {
        whitelist_set(entry);
    } else {
        char buf[WL_BUF + 128];
        snprintf(buf, sizeof(buf), "%s,%s", list, entry);
        whitelist_set(buf);
    }
    return true;
}

/* ================= Session auth ================= */

static void generate_session_token(char *out, size_t len) {
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        memcpy(bytes + i * 4, &r, 4);
    }
    size_t pos = 0;
    for (int i = 0; i < 16 && pos + 2 < len; i++) {
        out[pos++] = hex[(bytes[i] >> 4) & 0xF];
        out[pos++] = hex[bytes[i] & 0xF];
    }
    out[pos] = '\0';
}

static bool check_session(httpd_req_t *req) {
    if (strlen(s_session_token) == 0) return false;
    char cookie[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    char needle[64];
    snprintf(needle, sizeof(needle), "session=%s", s_session_token);
    return strstr(cookie, needle) != NULL;
}

static esp_err_t require_session_api(httpd_req_t *req) {
    if (check_session(req)) return ESP_OK;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

/* compares without leaking where the first mismatch is */
static bool secure_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la != lb);
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

/* ================= WiFi ================= */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_sta_connected = false;
            strcpy(s_sta_ip, "0.0.0.0");
            ESP_LOGW(TAG, "Upstream WiFi disconnected, retrying...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
            char mac[18];
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     ev->mac[0], ev->mac[1], ev->mac[2], ev->mac[3], ev->mac[4], ev->mac[5]);
            if (!whitelist_contains(mac)) {
                ESP_LOGW(TAG, "Rejecting non-whitelisted client %s", mac);
                esp_wifi_deauth_sta(ev->aid);
            } else {
                ESP_LOGI(TAG, "Client connected: %s", mac);
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        ESP_LOGI(TAG, "Upstream IP %s - enabling NAT", s_sta_ip);
        esp_netif_napt_enable(s_ap_netif);
    }
}

/* AP address + the DNS server that the AP's DHCP server hands to clients.
 * Without an explicit DNS offer the AP advertises itself as DNS, but the ESP32
 * runs no resolver - that is the classic "connected, no internet" symptom. */
static void configure_ap_network(void) {
    char ip[16], dns[16];
    get_setting("ap_ip", ip, sizeof(ip));
    get_setting("dns1", dns, sizeof(dns));

    esp_netif_ip_info_t info = { 0 };
    info.ip.addr = esp_ip4addr_aton(ip);
    info.gw.addr = info.ip.addr;
    info.netmask.addr = esp_ip4addr_aton("255.255.255.0");

    esp_netif_dns_info_t dnsinfo = { 0 };
    dnsinfo.ip.u_addr.ip4.addr = esp_ip4addr_aton(dns);
    dnsinfo.ip.type = IPADDR_TYPE_V4;

    uint8_t offer_dns = 0x02;   /* OFFER_DNS bit of the (private) lwip dhcps option enum */

    esp_netif_dhcps_stop(s_ap_netif);
    esp_err_t e1 = esp_netif_set_ip_info(s_ap_netif, &info);
    esp_err_t e2 = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns));
    esp_err_t e3 = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dnsinfo);
    esp_err_t e4 = esp_netif_dhcps_start(s_ap_netif);
    ESP_LOGI(TAG, "AP %s, DNS %s (ip:%s opt:%s dns:%s start:%s)", ip, dns,
             esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3), esp_err_to_name(e4));
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    char hostname[40];
    get_setting("hostname", hostname, sizeof(hostname));
    esp_netif_set_hostname(s_sta_netif, hostname);

    configure_ap_network();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    char ap_ssid[33], ap_pass[65], sta_ssid[33], sta_pass[65], sta_mac[20];
    get_setting("ap_ssid", ap_ssid, sizeof(ap_ssid));
    get_setting("ap_pass", ap_pass, sizeof(ap_pass));
    get_setting("sta_mac", sta_mac, sizeof(sta_mac));
    nvs_get_string("sta_ssid", sta_ssid, sizeof(sta_ssid), "");
    nvs_get_string("sta_pass", sta_pass, sizeof(sta_pass), "");

    wifi_config_t ap_config = { 0 };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    strncpy((char *)ap_config.ap.password, ap_pass, sizeof(ap_config.ap.password));
    ap_config.ap.max_connection = get_setting_int("ap_max_conn", 1, 10, 8);
    ap_config.ap.channel = get_setting_int("ap_channel", 1, 13, 1);
    ap_config.ap.ssid_hidden = get_setting_int("ap_hidden", 0, 1, 0);
    ap_config.ap.authmode = strlen(ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    wifi_config_t sta_config = { 0 };
    strncpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, sta_pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    uint8_t mac[6];
    if (sta_mac[0] != '\0' && parse_mac(sta_mac, mac) && !(mac[0] & 1)) {
        esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, mac);
        ESP_LOGI(TAG, "Custom STA MAC %s: %s", sta_mac, esp_err_to_name(e));
    }

    if (esp_wifi_set_config(WIFI_IF_AP, &ap_config) != ESP_OK) {
        ESP_LOGW(TAG, "AP config rejected - falling back to an open network");
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }
    if (strlen(sta_ssid) > 0) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    int dbm = get_setting_int("tx_power", 2, 20, 20);
    esp_wifi_set_max_tx_power((int8_t)(dbm * 4));

    ESP_LOGI(TAG, "AP up: SSID=%s hostname=%s", ap_ssid, hostname);
}

/* ================= Hardware reset button (BOOT = GPIO0) =================
 * Hold ~8 s -> LED starts blinking fast -> release -> settings erased, reboot.
 * We MUST wait for release before restarting: GPIO0 low at reset means
 * "enter download mode" and the firmware would not start. */

static void button_task(void *arg) {
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << RESET_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led);
    gpio_set_level(LED_GPIO, 0);

    int held_ms = 0;
    while (1) {
        if (gpio_get_level(RESET_BTN_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= RESET_HOLD_MS) {
                ESP_LOGW(TAG, "Factory reset armed - release the button now");
                int level = 0;
                while (gpio_get_level(RESET_BTN_GPIO) == 0) {
                    level = !level;
                    gpio_set_level(LED_GPIO, level);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(300));
                wipe_settings();
                ESP_LOGW(TAG, "Settings erased - restarting with defaults");
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ================= HTTP helpers ================= */

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len) {
    int total = 0;
    while (total < (int)buf_len - 1) {
        int r = httpd_req_recv(req, buf + total, buf_len - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
    free(out);
    cJSON_Delete(root);
    return r;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *err) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", ok);
    if (!ok) cJSON_AddStringToObject(r, "error", err ? err : "error");
    return send_json(req, r);
}

static void restart_soon(void) {
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
}

/* ================= HTTP handlers ================= */

static esp_err_t root_get_handler(httpd_req_t *req) {
    if (check_session(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/admin");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, login_html_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t admin_get_handler(httpd_req_t *req) {
    if (!check_session(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, admin_html_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t login_post_handler(httpd_req_t *req) {
    char buf[256];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    bool ok = false;
    if (j) {
        cJSON *user = cJSON_GetObjectItem(j, "username");
        cJSON *pass = cJSON_GetObjectItem(j, "password");
        char admin_pass[40];
        get_setting("admin_pass", admin_pass, sizeof(admin_pass));
        if (cJSON_IsString(user) && cJSON_IsString(pass) &&
            secure_equal(user->valuestring, ADMIN_USER) &&
            secure_equal(pass->valuestring, admin_pass)) {
            ok = true;
        }
        cJSON_Delete(j);
    }
    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(1000));   /* slows down password guessing */
        httpd_resp_set_status(req, "401 Unauthorized");
        return send_result(req, false, "wrong password");
    }
    generate_session_token(s_session_token, sizeof(s_session_token));
    char cookie_hdr[96];
    snprintf(cookie_hdr, sizeof(cookie_hdr), "session=%s; Path=/; HttpOnly", s_session_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_hdr);
    return send_result(req, true, NULL);
}

static esp_err_t logout_post_handler(httpd_req_t *req) {
    s_session_token[0] = '\0';
    httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; Max-Age=0");
    return send_result(req, true, NULL);
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "sta_connected", s_sta_connected);
    cJSON_AddStringToObject(root, "sta_ip", s_sta_ip);
    cJSON_AddNumberToObject(root, "uptime_s", (double)((esp_timer_get_time() - s_boot_time_us) / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());

    char v[40];
    nvs_get_string("sta_ssid", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_ssid", v);
    get_setting("ap_ip", v, sizeof(v));
    cJSON_AddStringToObject(root, "ap_ip", v);

    cJSON *clients = cJSON_CreateArray();
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        esp_netif_pair_mac_ip_t pairs[10] = { 0 };
        int count = sta_list.num > 10 ? 10 : sta_list.num;
        for (int i = 0; i < count; i++) {
            memcpy(pairs[i].mac, sta_list.sta[i].mac, 6);
        }
        esp_netif_dhcps_get_clients_by_mac(s_ap_netif, count, pairs);

        for (int i = 0; i < count; i++) {
            char mac[18];
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     sta_list.sta[i].mac[0], sta_list.sta[i].mac[1], sta_list.sta[i].mac[2],
                     sta_list.sta[i].mac[3], sta_list.sta[i].mac[4], sta_list.sta[i].mac[5]);
            char ipstr[16];
            snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&pairs[i].ip));
            char name[64] = "";
            whitelist_get_name(mac, name, sizeof(name));

            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "mac", mac);
            cJSON_AddStringToObject(c, "ip", ipstr);
            cJSON_AddStringToObject(c, "name", name);
            cJSON_AddItemToArray(clients, c);
        }
    }
    cJSON_AddItemToObject(root, "clients", clients);

    cJSON *wl = cJSON_CreateArray();
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    if (strlen(list) > 0) {
        char *copy = strdup(list);
        if (copy) {
            char *tok = strtok(copy, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                char m[20], n[64];
                split_entry(tok, m, sizeof(m), n, sizeof(n));
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "mac", m);
                cJSON_AddStringToObject(e, "name", n);
                cJSON_AddItemToArray(wl, e);
                tok = strtok(NULL, ",");
            }
            free(copy);
        }
    }
    cJSON_AddItemToObject(root, "whitelist", wl);
    return send_json(req, root);
}

static esp_err_t whitelist_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[320];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (!j) return send_result(req, false, "invalid data");

    cJSON *action = cJSON_GetObjectItem(j, "action");
    cJSON *mac = cJSON_GetObjectItem(j, "mac");
    cJSON *name = cJSON_GetObjectItem(j, "name");
    bool ok = true;
    const char *err = NULL;

    uint8_t m[6];
    if (!cJSON_IsString(action) || !cJSON_IsString(mac) || !parse_mac(mac->valuestring, m)) {
        ok = false;
        err = "MAC must look like AA:BB:CC:DD:EE:FF";
    } else {
        char norm[18];
        snprintf(norm, sizeof(norm), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        if (strcmp(action->valuestring, "add") == 0) {
            if (!whitelist_add(norm, cJSON_IsString(name) ? name->valuestring : "")) {
                ok = false;
                err = "whitelist is full";
            }
        } else if (strcmp(action->valuestring, "remove") == 0) {
            whitelist_remove(norm);
        }
    }
    cJSON_Delete(j);
    return send_result(req, ok, err);
}

static esp_err_t wifi_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[300];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (!j) return send_result(req, false, "invalid data");

    cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
    cJSON *pass = cJSON_GetObjectItem(j, "password");
    bool ok = true;
    const char *err = NULL;
    if (cJSON_IsString(ssid) && strlen(ssid->valuestring) > 32) { ok = false; err = "WiFi name is too long (max 32)"; }
    if (ok && cJSON_IsString(pass)) {
        size_t pl = strlen(pass->valuestring);
        if (pl != 0 && (pl < 8 || pl > 63)) { ok = false; err = "password must be 8-63 characters (blank = open network)"; }
    }
    if (ok) {
        if (cJSON_IsString(ssid)) nvs_set_string("sta_ssid", ssid->valuestring);
        if (cJSON_IsString(pass)) nvs_set_string("sta_pass", pass->valuestring);
    }
    cJSON_Delete(j);
    send_result(req, ok, err);
    if (ok) restart_soon();
    return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    for (size_t i = 0; i < N_SETTINGS; i++) {
        char v[80];
        if (strcmp(SETTINGS[i].type, "password_keep") == 0) v[0] = '\0';
        else get_setting(SETTINGS[i].key, v, sizeof(v));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", SETTINGS[i].key);
        cJSON_AddStringToObject(o, "label", SETTINGS[i].label);
        cJSON_AddStringToObject(o, "type", SETTINGS[i].type);
        cJSON_AddStringToObject(o, "value", v);
        cJSON_AddStringToObject(o, "def", SETTINGS[i].def);
        cJSON_AddStringToObject(o, "hint", SETTINGS[i].hint);
        cJSON_AddItemToArray(items, o);
    }
    cJSON_AddItemToObject(root, "items", items);
    return send_json(req, root);
}

static esp_err_t settings_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char *buf = malloc(1536);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
    read_body(req, buf, 1536);
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return send_result(req, false, "invalid data");

    char err[160] = "";
    bool ok = true;
    for (size_t i = 0; i < N_SETTINGS && ok; i++) {
        cJSON *it = cJSON_GetObjectItem(j, SETTINGS[i].key);
        if (!cJSON_IsString(it)) continue;
        if (strcmp(SETTINGS[i].key, "admin_pass") == 0 && it->valuestring[0] == '\0') continue;
        char why[80] = "";
        if (!validate_setting(SETTINGS[i].key, it->valuestring, why, sizeof(why))) {
            snprintf(err, sizeof(err), "%.50s: %.80s", SETTINGS[i].label, why);
            ok = false;
        }
    }
    if (ok) {
        for (size_t i = 0; i < N_SETTINGS; i++) {
            cJSON *it = cJSON_GetObjectItem(j, SETTINGS[i].key);
            if (!cJSON_IsString(it)) continue;
            if (strcmp(SETTINGS[i].key, "admin_pass") == 0 && it->valuestring[0] == '\0') continue;
            nvs_set_string(SETTINGS[i].key, it->valuestring);
        }
    }
    cJSON_Delete(j);
    send_result(req, ok, err);
    if (ok) restart_soon();
    return ESP_OK;
}

static esp_err_t config_export_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    char v[WL_BUF];
    for (size_t i = 0; i < N_SETTINGS; i++) {
        get_setting(SETTINGS[i].key, v, sizeof(v));
        cJSON_AddStringToObject(root, SETTINGS[i].key, v);
    }
    nvs_get_string("sta_ssid", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_ssid", v);
    nvs_get_string("sta_pass", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_pass", v);
    whitelist_get(v, sizeof(v));
    cJSON_AddStringToObject(root, "whitelist", v);

    char *out = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t config_import_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char *buf = malloc(3072);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
    read_body(req, buf, 3072);
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return send_result(req, false, "not valid JSON");

    char err[160] = "";
    bool ok = true;
    for (size_t i = 0; i < N_SETTINGS && ok; i++) {
        cJSON *it = cJSON_GetObjectItem(j, SETTINGS[i].key);
        if (!cJSON_IsString(it)) continue;
        char why[80] = "";
        if (!validate_setting(SETTINGS[i].key, it->valuestring, why, sizeof(why))) {
            snprintf(err, sizeof(err), "%.50s: %.80s", SETTINGS[i].label, why);
            ok = false;
        }
    }
    cJSON *wl = cJSON_GetObjectItem(j, "whitelist");
    if (ok && cJSON_IsString(wl) && strlen(wl->valuestring) >= WL_BUF) { ok = false; snprintf(err, sizeof(err), "whitelist too long"); }
    if (ok) {
        for (size_t i = 0; i < N_SETTINGS; i++) {
            cJSON *it = cJSON_GetObjectItem(j, SETTINGS[i].key);
            if (cJSON_IsString(it)) nvs_set_string(SETTINGS[i].key, it->valuestring);
        }
        const char *extra[] = {"sta_ssid", "sta_pass", "whitelist"};
        for (int i = 0; i < 3; i++) {
            cJSON *it = cJSON_GetObjectItem(j, extra[i]);
            if (cJSON_IsString(it) && strlen(it->valuestring) < 200 + (i == 2 ? WL_BUF : 0)) nvs_set_string(extra[i], it->valuestring);
        }
    }
    cJSON_Delete(j);
    send_result(req, ok, err);
    if (ok) restart_soon();
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    send_result(req, true, NULL);
    restart_soon();
    return ESP_OK;
}

static esp_err_t factory_reset_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    wipe_settings();
    send_result(req, true, NULL);
    restart_soon();
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }
    httpd_uri_t uris[] = {
        {.uri = "/",                   .method = HTTP_GET,  .handler = root_get_handler},
        {.uri = "/admin",              .method = HTTP_GET,  .handler = admin_get_handler},
        {.uri = "/api/login",          .method = HTTP_POST, .handler = login_post_handler},
        {.uri = "/api/logout",         .method = HTTP_POST, .handler = logout_post_handler},
        {.uri = "/api/status",         .method = HTTP_GET,  .handler = status_get_handler},
        {.uri = "/api/whitelist",      .method = HTTP_POST, .handler = whitelist_post_handler},
        {.uri = "/api/wifi",           .method = HTTP_POST, .handler = wifi_post_handler},
        {.uri = "/api/settings",       .method = HTTP_GET,  .handler = settings_get_handler},
        {.uri = "/api/settings",       .method = HTTP_POST, .handler = settings_post_handler},
        {.uri = "/api/config/export",  .method = HTTP_GET,  .handler = config_export_get_handler},
        {.uri = "/api/config/import",  .method = HTTP_POST, .handler = config_import_post_handler},
        {.uri = "/api/reboot",         .method = HTTP_POST, .handler = reboot_post_handler},
        {.uri = "/api/factory_reset",  .method = HTTP_POST, .handler = factory_reset_post_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "Web dashboard started on port 80");
}

void app_main(void) {
    s_boot_time_us = esp_timer_get_time();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_boot_time_us = esp_timer_get_time();
    wifi_init();
    start_webserver();
    xTaskCreate(button_task, "reset_btn", 3072, NULL, 5, NULL);
}

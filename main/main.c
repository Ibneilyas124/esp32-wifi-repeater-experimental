/*
 * ESP32 WiFi Repeater / Internet Sharing Router (NAPT)
 * Board: ESP32 DevKit V1        Built by Sarfraz Qureshi - Ibn e Ilyas Technologies
 *
 * - STA connects to the main router, AP re-shares the internet through NAT
 * - DNS handed to clients is configurable (default 8.8.8.8) - fixes "connected, no internet"
 * - Name shown in the main router's client list is configurable (DHCP hostname)
 * - Styled login page (fixed user "Sarfraz", default password "admin", password changeable)
 * - Advanced settings table (all editable, all with defaults), MAC whitelist with device names
 * - MAC whitelist = INTERNET permission: anyone with the WiFi password can join and open
 *   the dashboard, but only whitelisted MACs get internet (empty whitelist = everyone allowed)
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
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"

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

static void split_entry(const char

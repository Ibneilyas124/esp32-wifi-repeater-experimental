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
#include <inttypes.h>
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
#include "esp_private/wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include <time.h>
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
extern const char tools_html_start[] asm("_binary_tools_html_start");
extern const char chat_html_start[] asm("_binary_chat_html_start");

#define FW_VERSION "2.1.0"

static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_sta_connected = false;
static char s_sta_ip[16] = "0.0.0.0";
static int64_t s_boot_time_us = 0;
static char s_session_token[40] = "";
static uint32_t s_subnet_base = 0, s_subnet_mask = 0;
static volatile bool s_guest_filter_on = true;

/* ---- deauth monitor ring buffer ---- */
#define DEAUTH_RING 24
typedef struct { int64_t t; uint8_t type; uint8_t src[6]; uint8_t dst[6]; int8_t rssi; bool ours; } deauth_evt_t;
static deauth_evt_t s_deauth_ring[DEAUTH_RING];
static volatile int s_deauth_head = 0, s_deauth_count = 0, s_deauth_total = 0;

/* ---- upstream connection stability log ---- */
#define DISCON_RING 10
typedef struct { int64_t t; uint8_t reason; } discon_evt_t;
static discon_evt_t s_discon_ring[DISCON_RING];
static volatile int s_discon_head = 0, s_discon_count = 0, s_discon_total = 0;
static int64_t s_sta_since_us = 0;   /* time of last successful upstream connect */

/* ---- per-device usage tracking + optional uplink rate limit (whitelisted devices only) ---- */
#define TRACK_MAX 16
typedef struct {
    uint8_t mac[6];
    bool used;
    uint64_t total_bytes;
    uint32_t week_bytes;  int week_no;
    uint32_t month_bytes; int month_no;
    uint32_t bucket_tokens;
    int64_t bucket_last_us;
} usage_entry_t;
static usage_entry_t s_usage[TRACK_MAX];
static bool s_time_synced = false;

/* ---- repeater-wide (all devices combined) download tracking + rate cap.
 * Unlike per-device usage above, this is measured on the upstream (STA)
 * side, so it is the one number in this firmware that genuinely reflects
 * real internet usage in BOTH directions for the whole repeater - not just
 * uploads. It cannot be split back out per device (no public API exposes
 * the NAT table), so it is reported and capped in aggregate only. */
static usage_entry_t s_down_usage;           /* .mac unused - whole-repeater totals */
static uint32_t s_down_bucket_tokens = 0;
static int64_t s_down_bucket_last_us = 0;
static uint32_t s_up_bucket_tokens = 0;       /* overall (all-clients) uplink bucket */
static int64_t s_up_bucket_last_us = 0;

/* ---- speed-history: periodic latency samples to 1.1.1.1 (every ~15 min) ---- */
#define SPEEDHIST_MAX 48
typedef struct { int64_t t; int ms; bool ok; } speedhist_t;
static speedhist_t s_speedhist[SPEEDHIST_MAX];
static int s_speedhist_head = 0, s_speedhist_count = 0;

/* ---- background scan / diagnose state ---- */
static volatile int s_scan_state = 0;    /* 0 idle/done, 1 running, 2 error */
static volatile int s_diag_state = 0;    /* 0 idle, 1 running, 2 done */
static cJSON *s_diag_result = NULL;

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
    {"guest_filter","Guests: connect but no internet",  "1",                        "bool",          "1 = wrong-listed devices can join & reach this page but get no internet. 0 = they get disconnected immediately"},
    {"deauth_monitor","Deauth attack monitor",          "1",                        "bool",          "Passively watches for deauth/disassoc floods nearby. See WiFi Tools > Deauth"},
    {"deauth_alert","Deauth alert threshold",           "10",                       "number",        "Frames in 10s that count as an attack (1-50)"},
    {"sta_static_ip","Remote-management IP (static)",   "",                         "text",          "Blank = automatic (from main router). Set this to stop it changing, e.g. if you run 2+ repeaters"},
    {"sta_gateway", "Static IP: main router's address", "",                         "text",          "Required only if Remote-management IP above is set. Usually the main router's own IP"},
    {"sta_netmask", "Static IP: subnet mask",            "255.255.255.0",            "text",          "Required only if Remote-management IP above is set. Leave default unless you know why"},
    {"auto_reboot_hours","Auto-restart every N hours",   "0",                        "number",        "0 = never. Otherwise the repeater reboots itself every N hours (1-168) to stay fresh"},
    {"overall_up_kbps",  "Overall upload cap (all devices)", "0",                    "number",        "0 = unlimited. Total upload speed shared by every connected device combined, in Kbps"},
    {"overall_down_kbps","Overall download cap (all devices)","0",                   "number",        "0 = unlimited. Total download/streaming speed shared by every connected device combined, in Kbps"},
    {"ai_enabled",  "Enable public AI chat (/chat)",     "0",                        "bool",          "OFF by default. Anyone connected can use it once ON - it spends YOUR API key's money per message"},
    {"ai_api_key",  "AI API key",                        "",                         "password",      "Your own OpenAI (or compatible) API key. Stored only on this device, never shown to chat users"},
    {"ai_model",    "AI model name",                     "gpt-4o-mini",              "text",          "e.g. gpt-4o-mini (cheap, fast) or gpt-4o (smarter, costs more)"},
    {"ai_api_base", "AI API endpoint URL",                "https://api.openai.com/v1/chat/completions", "text", "Change only if pointing at a different OpenAI-compatible provider"},
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

/* Separate, never-wiped namespace, used only to PROVE a factory reset really
 * happened (a reset counter) - since some default values (e.g. the WiFi
 * name/password) look identical before and after a reset if they were never
 * customised, which was confusing to verify otherwise. */
#define DIAG_NS "repeater_diag"

static int bump_reset_counter(void) {
    nvs_handle_t h;
    int32_t count = 0;
    if (nvs_open(DIAG_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_i32(h, "resets", &count);
        count++;
        nvs_set_i32(h, "resets", count);
        nvs_commit(h);
        nvs_close(h);
    }
    return count;
}

static int get_reset_counter(void) {
    nvs_handle_t h;
    int32_t count = 0;
    if (nvs_open(DIAG_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "resets", &count);
        nvs_close(h);
    }
    return count;
}

static void wipe_settings(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    bump_reset_counter();
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

static bool valid_ipv4_any(const char *s) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

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
    } else if (strcmp(key, "sta_static_ip") == 0 || strcmp(key, "sta_gateway") == 0) {
        if (len != 0 && !valid_ipv4_any(v)) { snprintf(err, n, "must be a valid IPv4 address, or blank"); return false; }
    } else if (strcmp(key, "sta_netmask") == 0) {
        if (!valid_ipv4_any(v)) { snprintf(err, n, "must be a valid IPv4 mask like 255.255.255.0"); return false; }
    } else if (strcmp(key, "auto_reboot_hours") == 0) {
        if (!int_in_range(v, 0, 168)) { snprintf(err, n, "must be 0-168"); return false; }
    } else if (strcmp(key, "overall_up_kbps") == 0 || strcmp(key, "overall_down_kbps") == 0) {
        if (!int_in_range(v, 0, 9999)) { snprintf(err, n, "must be 0-9999 Kbps (0 = unlimited)"); return false; }
    } else if (strcmp(key, "ai_enabled") == 0) {
        if (strcmp(v, "0") != 0 && strcmp(v, "1") != 0) { snprintf(err, n, "must be 0 or 1"); return false; }
    } else if (strcmp(key, "ai_model") == 0) {
        if (len < 1 || len > 40) { snprintf(err, n, "must be 1-40 characters"); return false; }
    } else if (strcmp(key, "ai_api_base") == 0) {
        if (len < 9 || len > 127 || strncmp(v, "https://", 8) != 0) { snprintf(err, n, "must be a https:// URL"); return false; }
    } else if (strcmp(key, "ai_api_key") == 0) {
        if (len > 127) { snprintf(err, n, "too long (max 127 characters)"); return false; }
    } else if (strcmp(key, "guest_filter") == 0 || strcmp(key, "deauth_monitor") == 0) {
        if (strcmp(v, "0") != 0 && strcmp(v, "1") != 0) { snprintf(err, n, "must be 0 or 1"); return false; }
    } else if (strcmp(key, "deauth_alert") == 0) {
        if (!int_in_range(v, 1, 50)) { snprintf(err, n, "must be 1-50"); return false; }
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

static void split_entry3(const char *entry, char *mac_out, size_t mac_len, char *name_out, size_t name_len, int *limit_out) {
    const char *bar1 = strchr(entry, '|');
    if (!bar1) {
        strncpy(mac_out, entry, mac_len - 1); mac_out[mac_len - 1] = '\0';
        name_out[0] = '\0'; if (limit_out) *limit_out = 0;
        return;
    }
    size_t mlen = (size_t)(bar1 - entry);
    if (mlen >= mac_len) mlen = mac_len - 1;
    memcpy(mac_out, entry, mlen);
    mac_out[mlen] = '\0';

    const char *bar2 = strchr(bar1 + 1, '|');
    if (bar2) {
        size_t nlen = (size_t)(bar2 - (bar1 + 1));
        if (nlen >= name_len) nlen = name_len - 1;
        memcpy(name_out, bar1 + 1, nlen);
        name_out[nlen] = '\0';
        if (limit_out) *limit_out = atoi(bar2 + 1);
    } else {
        strncpy(name_out, bar1 + 1, name_len - 1);
        name_out[name_len - 1] = '\0';
        if (limit_out) *limit_out = 0;
    }
}

/* kept for the 2 call sites that only ever need mac+name */
static void split_entry(const char *entry, char *mac_out, size_t mac_len, char *name_out, size_t name_len) {
    split_entry3(entry, mac_out, mac_len, name_out, name_len, NULL);
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

static int whitelist_get_limit(const char *mac) {
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    if (strlen(list) == 0) return 0;
    char *copy = strdup(list);
    if (!copy) return 0;
    char *tok = strtok(copy, ",");
    int limit = 0;
    while (tok) {
        while (*tok == ' ') tok++;
        char m[20], n[64]; int lim = 0;
        split_entry3(tok, m, sizeof(m), n, sizeof(n), &lim);
        if (mac_str_eq(m, mac)) { limit = lim; break; }
        tok = strtok(NULL, ",");
    }
    free(copy);
    return limit;
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
        char m[20], n[64]; int lim2;
        split_entry3(tok, m, sizeof(m), n, sizeof(n), &lim2);
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

/* Adds or renames an entry (limit_kbps 0 = unlimited). Returns false when the list is full. */
static bool whitelist_add(const char *mac, const char *name, int limit_kbps) {
    whitelist_remove(mac);
    char clean[48];
    size_t j = 0;
    for (size_t i = 0; name && name[i] && j < sizeof(clean) - 1; i++) {
        clean[j++] = (name[i] == ',' || name[i] == '|') ? ' ' : name[i];
    }
    clean[j] = '\0';
    if (limit_kbps < 0) limit_kbps = 0;
    if (limit_kbps > 100000) limit_kbps = 100000;

    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    char entry[112];
    snprintf(entry, sizeof(entry), "%.20s|%.47s|%d", mac, clean, limit_kbps);
    if (strlen(list) + strlen(entry) + 2 >= WL_BUF) return false;
    if (strlen(list) == 0) {
        whitelist_set(entry);
    } else {
        char buf[WL_BUF + 144];
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

/* ---- Guest packet filter: unauthorized clients keep local access (DHCP,
 * ARP, the admin/tools page at the repeater's own IP) but every packet bound
 * OUTSIDE the local subnet - i.e. anything that would be NAT'd to the
 * internet - is dropped before it ever reaches the IP stack. Whitelisted
 * clients are untouched and behave exactly as before. This only runs when
 * "guest_filter" is on AND the whitelist is not empty (an empty whitelist
 * still means "allow everyone fully", unchanged from earlier versions). */

/* ---- usage tracking + rate limit helpers ---- */

static void usage_key(const uint8_t mac[6], char *out, size_t n) {
    snprintf(out, n, "u%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void usage_load(usage_entry_t *e) {
    char key[16]; usage_key(e->mac, key, sizeof(key));
    char v[96] = "";
    nvs_get_string(key, v, sizeof(v), "");
    unsigned long long tb = 0; unsigned wb = 0; int wn = 0; unsigned mb = 0; int mn = 0;
    if (sscanf(v, "%llu,%u,%d,%u,%d", &tb, &wb, &wn, &mb, &mn) == 5) {
        e->total_bytes = tb; e->week_bytes = wb; e->week_no = wn; e->month_bytes = mb; e->month_no = mn;
    }
}

static void usage_save(usage_entry_t *e) {
    char key[16]; usage_key(e->mac, key, sizeof(key));
    char v[96];
    snprintf(v, sizeof(v), "%llu,%" PRIu32 ",%d,%" PRIu32 ",%d",
         (unsigned long long)e->total_bytes,
         e->week_bytes,
         e->week_no,
         e->month_bytes,
         e->month_no);
    nvs_set_string(key, v);
}

/* whole-repeater download totals - persisted under a fixed NVS key */
static void down_usage_load(void) {
    char v[96] = "";
    nvs_get_string("down_total", v, sizeof(v), "");
    unsigned long long tb = 0; unsigned wb = 0; int wn = 0; unsigned mb = 0; int mn = 0;
    if (sscanf(v, "%llu,%u,%d,%u,%d", &tb, &wb, &wn, &mb, &mn) == 5) {
        s_down_usage.total_bytes = tb; s_down_usage.week_bytes = wb; s_down_usage.week_no = wn;
        s_down_usage.month_bytes = mb; s_down_usage.month_no = mn;
    }
}

static void down_usage_save(void) {
    char v[96];
    snprintf(v, sizeof(v), "%llu,%u,%d,%u,%d", (unsigned long long)s_down_usage.total_bytes,
             s_down_usage.week_bytes, s_down_usage.week_no, s_down_usage.month_bytes, s_down_usage.month_no);
    nvs_set_string("down_total", v);
}

/* finds a tracked device, creating a new slot (loaded from NVS) the first time it is seen */
static usage_entry_t *usage_find_or_add(const uint8_t mac[6]) {
    for (int i = 0; i < TRACK_MAX; i++) {
        if (s_usage[i].used && memcmp(s_usage[i].mac, mac, 6) == 0) return &s_usage[i];
    }
    for (int i = 0; i < TRACK_MAX; i++) {
        if (!s_usage[i].used) {
            memset(&s_usage[i], 0, sizeof(usage_entry_t));
            memcpy(s_usage[i].mac, mac, 6);
            s_usage[i].used = true;
            usage_load(&s_usage[i]);
            return &s_usage[i];
        }
    }
    return NULL; /* table full - that device just won't be tracked */
}

/* call once/minute or so: rolls week/month counters over using real time (needs SNTP) */
static void usage_housekeeping(void) {
    time_t now = time(NULL);
    if (now < 1700000000) return; /* clock not synced yet - do not reset anything on guesswork */
    int week_no = (int)(now / 604800);
    struct tm tmv; localtime_r(&now, &tmv);
    int month_no = tmv.tm_year * 12 + tmv.tm_mon;
    bool any_dirty = false;
    for (int i = 0; i < TRACK_MAX; i++) {
        if (!s_usage[i].used) continue;
        bool dirty = false;
        if (s_usage[i].week_no != week_no) { s_usage[i].week_bytes = 0; s_usage[i].week_no = week_no; dirty = true; }
        if (s_usage[i].month_no != month_no) { s_usage[i].month_bytes = 0; s_usage[i].month_no = month_no; dirty = true; }
        if (dirty) { usage_save(&s_usage[i]); any_dirty = true; }
    }
    if (s_down_usage.week_no != week_no) { s_down_usage.week_bytes = 0; s_down_usage.week_no = week_no; any_dirty = true; }
    if (s_down_usage.month_no != month_no) { s_down_usage.month_bytes = 0; s_down_usage.month_no = month_no; any_dirty = true; }
    if (any_dirty) down_usage_save();
}

static void usage_persist_all(void) {
    for (int i = 0; i < TRACK_MAX; i++) {
        if (s_usage[i].used) usage_save(&s_usage[i]);
    }
    down_usage_save();
}

static void update_subnet_cache(void) {
    char ip[16];
    get_setting("ap_ip", ip, sizeof(ip));
    s_subnet_mask = esp_ip4addr_aton("255.255.255.0");
    s_subnet_base = esp_ip4addr_aton(ip) & s_subnet_mask;
}

static esp_err_t ap_rx_filter(void *buffer, uint16_t len, void *eb) {
    if (!s_guest_filter_on || len < 14) {
        esp_netif_receive(s_ap_netif, buffer, len, eb);
        return ESP_OK;
    }
    uint8_t *f = (uint8_t *)buffer;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", f[6], f[7], f[8], f[9], f[10], f[11]);
    if (whitelist_contains(mac)) {
        usage_entry_t *ue = usage_find_or_add(f + 6);
        if (ue) {
            ue->total_bytes += len;
            ue->week_bytes += len;
            ue->month_bytes += len;

            int limit_kbps = whitelist_get_limit(mac);
            if (limit_kbps > 0) {
                int64_t now = esp_timer_get_time();
                if (ue->bucket_last_us == 0) ue->bucket_last_us = now;
                int64_t dt = now - ue->bucket_last_us;
                if (dt > 0) {
                    uint64_t refill = (uint64_t)dt * (uint32_t)limit_kbps * 1000ULL / 8ULL / 1000000ULL;
                    uint32_t cap = (uint32_t)limit_kbps * 1000 / 8;  /* ~1 second burst */
                    uint64_t nt = (uint64_t)ue->bucket_tokens + refill;
                    ue->bucket_tokens = nt > cap ? cap : (uint32_t)nt;
                    ue->bucket_last_us = now;
                }
                if (ue->bucket_tokens < len) {
                    esp_wifi_internal_free_rx_buffer(eb);   /* over the uplink limit - drop this packet */
                    return ESP_OK;
                }
                ue->bucket_tokens -= len;
            }

            int overall_up = get_setting_int("overall_up_kbps", 0, 9999, 0);
            if (overall_up > 0) {
                int64_t now2 = esp_timer_get_time();
                if (s_up_bucket_last_us == 0) s_up_bucket_last_us = now2;
                int64_t dt2 = now2 - s_up_bucket_last_us;
                if (dt2 > 0) {
                    uint64_t refill2 = (uint64_t)dt2 * (uint32_t)overall_up * 1000ULL / 8ULL / 1000000ULL;
                    uint32_t cap2 = (uint32_t)overall_up * 1000 / 8;
                    uint64_t nt2 = (uint64_t)s_up_bucket_tokens + refill2;
                    s_up_bucket_tokens = nt2 > cap2 ? cap2 : (uint32_t)nt2;
                    s_up_bucket_last_us = now2;
                }
                if (s_up_bucket_tokens < len) {
                    esp_wifi_internal_free_rx_buffer(eb);  /* over the shared overall uplink cap */
                    return ESP_OK;
                }
                s_up_bucket_tokens -= len;
            }
        }
        esp_netif_receive(s_ap_netif, buffer, len, eb);
        return ESP_OK;
    }
    uint16_t ethertype = ((uint16_t)f[12] << 8) | f[13];
    bool allow = false;
    if (ethertype == 0x0806) {
        allow = true; /* ARP - keeps local networking alive, never reaches the internet */
    } else if (ethertype == 0x0800 && len >= 34) {
        uint32_t dest;
        memcpy(&dest, f + 30, 4); /* IPv4 destination address */
        if (dest == 0xFFFFFFFFu || (dest & s_subnet_mask) == s_subnet_base) allow = true;
    }
    if (allow) {
        esp_netif_receive(s_ap_netif, buffer, len, eb);
    } else {
        esp_wifi_internal_free_rx_buffer(eb);
    }
    return ESP_OK;
}

/* ---- Whole-repeater download counter + optional overall download cap.
 * Registered on the STA (upstream) interface. Unlike ap_rx_filter, this
 * ALWAYS forwards every packet unchanged when no cap is set (default) - it
 * only ever drops packets when the admin has explicitly set a download cap
 * greater than 0, so normal internet sharing is completely unaffected
 * unless this is turned on. Must be re-armed after every STA (re)connect,
 * because ESP-IDF's own networking glue re-registers its own default
 * receive callback each time the STA reconnects. */
static esp_err_t sta_rx_counter(void *buffer, uint16_t len, void *eb) {
    s_down_usage.total_bytes += len;
    s_down_usage.week_bytes += len;
    s_down_usage.month_bytes += len;

    int overall_down = get_setting_int("overall_down_kbps", 0, 9999, 0);
    if (overall_down > 0) {
        int64_t now = esp_timer_get_time();
        if (s_down_bucket_last_us == 0) s_down_bucket_last_us = now;
        int64_t dt = now - s_down_bucket_last_us;
        if (dt > 0) {
            uint64_t refill = (uint64_t)dt * (uint32_t)overall_down * 1000ULL / 8ULL / 1000000ULL;
            uint32_t cap = (uint32_t)overall_down * 1000 / 8;
            uint64_t nt = (uint64_t)s_down_bucket_tokens + refill;
            s_down_bucket_tokens = nt > cap ? cap : (uint32_t)nt;
            s_down_bucket_last_us = now;
        }
        if (s_down_bucket_tokens < len) {
            esp_wifi_internal_free_rx_buffer(eb);   /* over the overall download cap */
            return ESP_OK;
        }
        s_down_bucket_tokens -= len;
    }
    esp_netif_receive(s_sta_netif, buffer, len, eb);
    return ESP_OK;
}

/* ---- Deauth / disassoc monitor: passive promiscuous management-frame sniff.
 * Adds counters only, never transmits anything itself. */

static void wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;
    uint8_t *p = pkt->payload;
    uint8_t fc0 = p[0];
    uint8_t ftype = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    if (ftype != 0 || (subtype != 0x0C && subtype != 0x0A)) return; /* mgmt: deauth=0xC, disassoc=0xA */

    uint8_t *addr1 = p + 4, *addr2 = p + 10, *addr3 = p + 16;
    uint8_t self_ap[6] = {0}, self_sta[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, self_ap);
    esp_wifi_get_mac(WIFI_IF_STA, self_sta);
    bool ours = (memcmp(addr1, self_ap, 6) == 0) || (memcmp(addr3, self_ap, 6) == 0) ||
                (memcmp(addr1, self_sta, 6) == 0) || (memcmp(addr3, self_sta, 6) == 0);

    int idx = s_deauth_head;
    s_deauth_ring[idx].t = esp_timer_get_time();
    s_deauth_ring[idx].type = subtype;
    memcpy(s_deauth_ring[idx].src, addr2, 6);
    memcpy(s_deauth_ring[idx].dst, addr1, 6);
    s_deauth_ring[idx].rssi = pkt->rx_ctrl.rssi;
    s_deauth_ring[idx].ours = ours;
    s_deauth_head = (s_deauth_head + 1) % DEAUTH_RING;
    if (s_deauth_count < DEAUTH_RING) s_deauth_count++;
    s_deauth_total++;
}

static void deauth_monitor_apply(bool on) {
    if (on) {
        wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
        esp_wifi_set_promiscuous_filter(&filt);
        esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb);
        esp_wifi_set_promiscuous(true);
    } else {
        esp_wifi_set_promiscuous(false);
    }
}


static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            /* ESP-IDF's own netif glue re-registers its default STA receive
             * callback on every connect, silently overwriting ours - so we
             * must re-arm our download counter/cap right after, every time. */
            esp_err_t re = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, sta_rx_counter);
            ESP_LOGI(TAG, "Download counter armed: %s", esp_err_to_name(re));
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *dc = (wifi_event_sta_disconnected_t *)data;
            s_sta_connected = false;
            strcpy(s_sta_ip, "0.0.0.0");
            int idx = s_discon_head;
            s_discon_ring[idx].t = esp_timer_get_time();
            s_discon_ring[idx].reason = dc ? dc->reason : 0;
            s_discon_head = (s_discon_head + 1) % DISCON_RING;
            if (s_discon_count < DISCON_RING) s_discon_count++;
            s_discon_total++;
            ESP_LOGW(TAG, "Upstream WiFi disconnected (reason %d), retrying...", dc ? dc->reason : 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
            char mac[18];
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     ev->mac[0], ev->mac[1], ev->mac[2], ev->mac[3], ev->mac[4], ev->mac[5]);
            if (!whitelist_contains(mac)) {
                if (s_guest_filter_on) {
                    ESP_LOGI(TAG, "Guest connected (local access only, no internet): %s", mac);
                } else {
                    ESP_LOGW(TAG, "Rejecting non-whitelisted client %s", mac);
                    esp_wifi_deauth_sta(ev->aid);
                }
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
        s_sta_since_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Upstream IP %s - enabling NAT", s_sta_ip);
        esp_netif_napt_enable(s_ap_netif);

        if (!s_time_synced) {
            s_time_synced = true; /* only ever try once per boot */
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP time sync started (needed for weekly/monthly usage totals)");
        }
    }
}

/* AP address + the DNS server that the AP's DHCP server hands to clients.
 * Without an explicit DNS offer the AP advertises itself as DNS, but the ESP32
 * runs no resolver - that is the classic "connected, no internet" symptom. */
static void configure_ap_network(void) {
    update_subnet_cache();
    char ip[16], dns[16];
    get_setting("ap_ip", ip, sizeof(ip));
    get_setting("dns1", dns, sizeof(dns));

    esp_netif_ip_info_t info = { 0 };
    info.ip.addr = esp_ip4addr_aton(ip);
    info.gw.addr = info.ip.addr;
    info.netmask.addr = esp_ip4addr_aton("255.255.255.0");

    esp_netif_dns_info_t dnsinfo = { 0 };
    dnsinfo.ip.u_addr.ip4.addr = esp_ip4addr_aton(dns);
    dnsinfo.ip.type = ESP_IPADDR_TYPE_V4;

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

    char sip[16], sgw[16], smask[16];
    get_setting("sta_static_ip", sip, sizeof(sip));
    get_setting("sta_gateway", sgw, sizeof(sgw));
    get_setting("sta_netmask", smask, sizeof(smask));
    if (sip[0] != '\0' && sgw[0] != '\0') {
        esp_netif_dhcpc_stop(s_sta_netif);
        esp_netif_ip_info_t sinfo = { 0 };
        sinfo.ip.addr = esp_ip4addr_aton(sip);
        sinfo.gw.addr = esp_ip4addr_aton(sgw);
        sinfo.netmask.addr = esp_ip4addr_aton(smask[0] ? smask : "255.255.255.0");
        esp_err_t se = esp_netif_set_ip_info(s_sta_netif, &sinfo);
        ESP_LOGI(TAG, "Static upstream IP %s via gw %s: %s", sip, sgw, esp_err_to_name(se));
    } else {
        ESP_LOGI(TAG, "Upstream IP: automatic (DHCP)");
    }

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

    char gf[4]; get_setting("guest_filter", gf, sizeof(gf));
    s_guest_filter_on = (gf[0] == '1');
    esp_err_t rxe = esp_wifi_internal_reg_rxcb(WIFI_IF_AP, ap_rx_filter);
    ESP_LOGI(TAG, "Guest filter %s (rxcb: %s)", s_guest_filter_on ? "ON" : "OFF", esp_err_to_name(rxe));

    char dm[4]; get_setting("deauth_monitor", dm, sizeof(dm));
    deauth_monitor_apply(dm[0] == '1');

    ESP_LOGI(TAG, "AP up: SSID=%s hostname=%s", ap_ssid, hostname);
}

/* ================= Hardware reset button (BOOT = GPIO0) =================
 * Hold ~8 s -> LED starts blinking fast -> release -> settings erased, reboot.
 * We MUST wait for release before restarting: GPIO0 low at reset means
 * "enter download mode" and the firmware would not start. */

static int tcp_probe(const char *ip, int port, int timeout_ms); /* defined later, used here for speed-history sampling */

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
    uint32_t tick = 0;
    while (1) {
        tick++;
        if (tick % 600 == 0) {          /* every ~60s */
            usage_housekeeping();
            int hrs = get_setting_int("auto_reboot_hours", 0, 168, 0);
            if (hrs > 0 && (esp_timer_get_time() - s_boot_time_us) >= (int64_t)hrs * 3600LL * 1000000LL) {
                ESP_LOGW(TAG, "Scheduled auto-restart (%d h) reached", hrs);
                esp_restart();
            }
        }
        if (tick % 3000 == 0) {         /* every ~5 min */
            usage_persist_all();
        }
        if (tick % 9000 == 0 && s_sta_connected) {   /* every ~15 min */
            int ms = tcp_probe("1.1.1.1", 443, 2000);
            int idx = s_speedhist_head;
            s_speedhist[idx].t = esp_timer_get_time();
            s_speedhist[idx].ms = ms >= 0 ? ms : 0;
            s_speedhist[idx].ok = ms >= 0;
            s_speedhist_head = (s_speedhist_head + 1) % SPEEDHIST_MAX;
            if (s_speedhist_count < SPEEDHIST_MAX) s_speedhist_count++;
        }
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
    cJSON_AddStringToObject(root, "fw", FW_VERSION);
    cJSON_AddBoolToObject(root, "sta_connected", s_sta_connected);
    cJSON_AddStringToObject(root, "sta_ip", s_sta_ip);
    cJSON_AddNumberToObject(root, "uptime_s", (double)((esp_timer_get_time() - s_boot_time_us) / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());

    char v[40];
    nvs_get_string("sta_ssid", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_ssid", v);
    get_setting("ap_ip", v, sizeof(v));
    cJSON_AddStringToObject(root, "ap_ip", v);

    char wlfull[WL_BUF];
    whitelist_get(wlfull, sizeof(wlfull));
    cJSON_AddBoolToObject(root, "wl_active", strlen(wlfull) > 0);

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
            cJSON_AddNumberToObject(c, "rssi", sta_list.sta[i].rssi);
            cJSON_AddBoolToObject(c, "authorized", whitelist_contains(mac));
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
                char m[20], n[64]; int lim = 0;
                split_entry3(tok, m, sizeof(m), n, sizeof(n), &lim);
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "mac", m);
                cJSON_AddStringToObject(e, "name", n);
                cJSON_AddNumberToObject(e, "limit_kbps", lim);
                cJSON_AddItemToArray(wl, e);
                tok = strtok(NULL, ",");
            }
            free(copy);
        }
    }
    cJSON_AddItemToObject(root, "whitelist", wl);

    cJSON_AddNumberToObject(root, "session_uptime_s", s_sta_connected ? (double)((esp_timer_get_time() - s_sta_since_us) / 1000000) : 0);
    cJSON_AddNumberToObject(root, "discon_total", s_discon_total);
    cJSON *dlog = cJSON_CreateArray();
    int dn = s_discon_count;
    int64_t now2 = esp_timer_get_time();
    for (int i = 0; i < dn; i++) {
        int idx = ((s_discon_head - 1 - i) % DISCON_RING + DISCON_RING) % DISCON_RING;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ago_s", (double)((now2 - s_discon_ring[idx].t) / 1000000));
        cJSON_AddNumberToObject(o, "reason", s_discon_ring[idx].reason);
        cJSON_AddItemToArray(dlog, o);
    }
    cJSON_AddItemToObject(root, "discon_log", dlog);
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
            cJSON *limit = cJSON_GetObjectItem(j, "limit_kbps");
            int lim = cJSON_IsNumber(limit) ? limit->valueint : 0;
            if (!whitelist_add(norm, cJSON_IsString(name) ? name->valuestring : "", lim)) {
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

/* ================= Kick / identity ================= */

static esp_err_t kick_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[64];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    bool ok = false;
    const char *err = "invalid data";
    if (j) {
        cJSON *mac = cJSON_GetObjectItem(j, "mac");
        uint8_t m[6];
        if (cJSON_IsString(mac) && parse_mac(mac->valuestring, m)) {
            uint16_t aid = 0;
            if (esp_wifi_ap_get_sta_aid(m, &aid) == ESP_OK && aid > 0) {
                esp_wifi_deauth_sta(aid);
                ok = true;
            } else {
                err = "device is not currently connected";
            }
        } else {
            err = "MAC must look like AA:BB:CC:DD:EE:FF";
        }
        cJSON_Delete(j);
    }
    return send_result(req, ok, ok ? NULL : err);
}

static esp_err_t me_get_handler(httpd_req_t *req) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "admin", check_session(req));
    cJSON_AddStringToObject(r, "fw", FW_VERSION);
    cJSON_AddNumberToObject(r, "reset_count", get_reset_counter());
    return send_json(req, r);
}

static esp_err_t tools_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, tools_html_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t chat_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, chat_html_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================= AI chat (public - no login - opt-in) ================= */

#define AI_RESP_CAP 8192

typedef struct { char *buf; int len; int cap; } http_resp_buf_t;

static esp_err_t chat_http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_resp_buf_t *rb = (http_resp_buf_t *)evt->user_data;
        if (rb && rb->buf && rb->len + evt->data_len < rb->cap - 1) {
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
            rb->buf[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t chat_post_handler(httpd_req_t *req) {
    char en[4]; get_setting("ai_enabled", en, sizeof(en));
    if (en[0] != '1') return send_result(req, false, "AI chat is currently turned off by the repeater admin");

    char api_key[128]; get_setting("ai_api_key", api_key, sizeof(api_key));
    if (api_key[0] == '\0') return send_result(req, false, "AI chat is on, but no API key has been set yet - ask the admin to add one in Advanced Settings");

    char model[40]; get_setting("ai_model", model, sizeof(model));
    char base[128]; get_setting("ai_api_base", base, sizeof(base));

    char *body = malloc(4096);
    if (!body) { httpd_resp_send_500(req); return ESP_OK; }
    read_body(req, body, 4096);
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) return send_result(req, false, "invalid request");

    cJSON *msgs = cJSON_GetObjectItem(in, "messages");
    if (!cJSON_IsArray(msgs) || cJSON_GetArraySize(msgs) == 0) {
        cJSON_Delete(in);
        return send_result(req, false, "invalid request");
    }

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "model", model);
    cJSON_AddItemToObject(payload, "messages", cJSON_Duplicate(msgs, true));
    cJSON_AddNumberToObject(payload, "max_tokens", 500);
    cJSON_Delete(in);
    char *payload_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!payload_str) { httpd_resp_send_500(req); return ESP_OK; }

    http_resp_buf_t rb = { .cap = AI_RESP_CAP, .len = 0 };
    rb.buf = malloc(AI_RESP_CAP);
    if (!rb.buf) { free(payload_str); httpd_resp_send_500(req); return ESP_OK; }
    rb.buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = base,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 25000,
        .event_handler = chat_http_event_handler,
        .user_data = &rb,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth_hdr[160];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_post_field(client, payload_str, strlen(payload_str));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(payload_str);

    if (err != ESP_OK) {
        char e[100]; snprintf(e, sizeof(e), "could not reach the AI service: %s", esp_err_to_name(err));
        free(rb.buf);
        return send_result(req, false, e);
    }
    if (status != 200) {
        char e[150]; snprintf(e, sizeof(e), "AI service returned an error (HTTP %d) - check the API key/model in Advanced Settings", status);
        free(rb.buf);
        return send_result(req, false, e);
    }

    cJSON *resp = cJSON_Parse(rb.buf);
    free(rb.buf);
    if (!resp) return send_result(req, false, "AI service sent an unreadable response");

    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msgobj = first ? cJSON_GetObjectItem(first, "message") : NULL;
    cJSON *content = msgobj ? cJSON_GetObjectItem(msgobj, "content") : NULL;

    cJSON *out = cJSON_CreateObject();
    if (cJSON_IsString(content)) {
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "reply", content->valuestring);
    } else {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "error", "AI service response did not include a reply");
    }
    cJSON_Delete(resp);
    return send_json(req, out);
}

/* ================= WiFi Analyzer (scan) ================= */

static void scan_task(void *arg) {
    wifi_scan_config_t cfg = { .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_err_t e = esp_wifi_scan_start(&cfg, true);
    s_scan_state = (e == ESP_OK) ? 0 : 2;
    vTaskDelete(NULL);
}

static esp_err_t scan_start_post_handler(httpd_req_t *req) {
    if (s_scan_state != 1) {
        s_scan_state = 1;
        xTaskCreate(scan_task, "scan", 4096, NULL, 4, NULL);
    }
    return send_result(req, true, NULL);
}

static const char *auth_name(wifi_auth_mode_t a) {
    switch (a) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "WPA2+";
    }
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    if (s_scan_state == 1) { cJSON *r = cJSON_CreateObject(); cJSON_AddStringToObject(r, "state", "scanning"); return send_json(req, r); }
    if (s_scan_state == 2) { cJSON *r = cJSON_CreateObject(); cJSON_AddStringToObject(r, "state", "error"); cJSON_AddStringToObject(r, "error", "scan failed - try again"); return send_json(req, r); }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 40) num = 40;
    wifi_ap_record_t *recs = num ? calloc(num, sizeof(wifi_ap_record_t)) : NULL;
    if (num && !recs) { httpd_resp_send_500(req); return ESP_OK; }
    if (num) esp_wifi_scan_get_ap_records(&num, recs);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", "done");
    uint8_t cur_ch = 0; wifi_second_chan_t sc;
    esp_wifi_get_channel(&cur_ch, &sc);
    cJSON_AddNumberToObject(root, "cur_ch", cur_ch);

    char apssid[33]; get_setting("ap_ssid", apssid, sizeof(apssid));
    cJSON_AddStringToObject(root, "ap_ssid", apssid);
    uint8_t apmac[6] = {0}; esp_wifi_get_mac(WIFI_IF_AP, apmac);
    char apmacs[18]; snprintf(apmacs, sizeof(apmacs), "%02X:%02X:%02X:%02X:%02X:%02X", apmac[0], apmac[1], apmac[2], apmac[3], apmac[4], apmac[5]);
    cJSON_AddStringToObject(root, "ap_bssid", apmacs);
    if (s_sta_connected) {
        wifi_ap_record_t up;
        if (esp_wifi_sta_get_ap_info(&up) == ESP_OK) {
            char ubssid[18]; snprintf(ubssid, sizeof(ubssid), "%02X:%02X:%02X:%02X:%02X:%02X", up.bssid[0], up.bssid[1], up.bssid[2], up.bssid[3], up.bssid[4], up.bssid[5]);
            cJSON_AddStringToObject(root, "up_bssid", ubssid);
        }
    }

    cJSON *nets = cJSON_CreateArray();
    for (int i = 0; i < num; i++) {
        wifi_ap_record_t *r = &recs[i];
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", (const char *)r->ssid);
        char bssid[18]; snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X", r->bssid[0], r->bssid[1], r->bssid[2], r->bssid[3], r->bssid[4], r->bssid[5]);
        cJSON_AddStringToObject(n, "bssid", bssid);
        cJSON_AddNumberToObject(n, "ch", r->primary);
        cJSON_AddNumberToObject(n, "rssi", r->rssi);
        cJSON_AddStringToObject(n, "auth", auth_name(r->authmode));
        cJSON_AddNumberToObject(n, "bw", r->second != WIFI_SECOND_CHAN_NONE ? 40 : 20);
        cJSON_AddBoolToObject(n, "b", r->phy_11b);
        cJSON_AddBoolToObject(n, "g", r->phy_11g);
        cJSON_AddBoolToObject(n, "n", r->phy_11n);
        cJSON_AddBoolToObject(n, "lr", r->phy_lr);
        cJSON_AddBoolToObject(n, "wps", r->wps);
        if (r->country.cc[0]) { char cc[3] = { r->country.cc[0], r->country.cc[1], 0 }; cJSON_AddStringToObject(n, "cc", cc); }
        cJSON_AddItemToArray(nets, n);
    }
    cJSON_AddItemToObject(root, "nets", nets);
    free(recs);
    return send_json(req, root);
}

/* ================= Deauth monitor readout ================= */

static esp_err_t deauth_get_handler(httpd_req_t *req) {
    char v[4]; get_setting("deauth_monitor", v, sizeof(v));
    bool enabled = (v[0] == '1');
    int thr = get_setting_int("deauth_alert", 1, 50, 10);
    int64_t now = esp_timer_get_time();
    int c10 = 0, b10 = 0, o10 = 0;
    int n = s_deauth_count;

    cJSON *events = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        int idx = ((s_deauth_head - 1 - i) % DEAUTH_RING + DEAUTH_RING) % DEAUTH_RING;
        deauth_evt_t *e = &s_deauth_ring[idx];
        double age = (double)(now - e->t) / 1e6;
        if (age <= 10.0) {
            c10++;
            if (memcmp(e->dst, "\xff\xff\xff\xff\xff\xff", 6) == 0) b10++;
            if (e->ours) o10++;
        }
        if (i < 15) {
            char sm[18], dm[18];
            snprintf(sm, sizeof(sm), "%02X:%02X:%02X:%02X:%02X:%02X", e->src[0], e->src[1], e->src[2], e->src[3], e->src[4], e->src[5]);
            snprintf(dm, sizeof(dm), "%02X:%02X:%02X:%02X:%02X:%02X", e->dst[0], e->dst[1], e->dst[2], e->dst[3], e->dst[4], e->dst[5]);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "age", (int)age);
            cJSON_AddStringToObject(o, "type", e->type == 0x0C ? "deauth" : "disassoc");
            cJSON_AddStringToObject(o, "src", sm);
            cJSON_AddStringToObject(o, "dst", dm);
            cJSON_AddNumberToObject(o, "rssi", e->rssi);
            cJSON_AddBoolToObject(o, "ours", e->ours);
            cJSON_AddItemToArray(events, o);
        }
    }

    const char *level = "ok";
    if (enabled) {
        if (c10 >= thr) level = "attack";
        else if (c10 >= (thr / 3 + 1)) level = "watch";
    }

    uint8_t ch = 0; wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddStringToObject(root, "level", level);
    cJSON_AddNumberToObject(root, "count10", c10);
    cJSON_AddNumberToObject(root, "bcast10", b10);
    cJSON_AddNumberToObject(root, "ours10", o10);
    cJSON_AddBoolToObject(root, "targets_us", o10 > 0);
    cJSON_AddNumberToObject(root, "total", s_deauth_total);
    cJSON_AddNumberToObject(root, "thr", thr);
    cJSON_AddNumberToObject(root, "channel", ch);
    cJSON_AddItemToObject(root, "events", events);
    return send_json(req, root);
}

/* ================= Speed test ================= */

static esp_err_t ping_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "pong", 4);
    return ESP_OK;
}

static esp_err_t speed_get_handler(httpd_req_t *req) {
    char qs[64] = "", kbs[16] = "256";
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        httpd_query_key_value(qs, "kb", kbs, sizeof(kbs));
    }
    int kb = atoi(kbs);
    if (kb <= 0 || kb > 4096) kb = 256;

    static char chunk[16384];
    memset(chunk, 'A', sizeof(chunk));
    httpd_resp_set_type(req, "application/octet-stream");
    int remaining = kb * 1024;
    while (remaining > 0) {
        int n = remaining > (int)sizeof(chunk) ? (int)sizeof(chunk) : remaining;
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) break;
        remaining -= n;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t upspeed_post_handler(httpd_req_t *req) {
    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining);
        if (n <= 0) break;
        remaining -= n;
    }
    return send_result(req, true, NULL);
}

/* ================= Diagnose ================= */

static int tcp_probe(const char *ip, int port, int timeout_ms) {
    int64_t t0 = esp_timer_get_time();
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);
    int r = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);
    if (r != 0) return -1;
    return (int)((esp_timer_get_time() - t0) / 1000);
}

static void diag_task(void *arg) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", s_sta_connected);
    if (s_sta_connected) {
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            cJSON_AddStringToObject(root, "ssid", (const char *)info.ssid);
            cJSON_AddNumberToObject(root, "rssi", info.rssi);
            cJSON_AddNumberToObject(root, "channel", info.primary);
        }
        cJSON_AddStringToObject(root, "ip", s_sta_ip);

        esp_netif_ip_info_t ipi = { 0 };
        esp_netif_get_ip_info(s_sta_netif, &ipi);
        char gw[16]; snprintf(gw, sizeof(gw), IPSTR, IP2STR(&ipi.gw));
        cJSON_AddStringToObject(root, "gw_ip", gw);
        int gwms = tcp_probe(gw, 80, 800);
        cJSON_AddStringToObject(root, "gw", gwms >= 0 ? "ok" : "refused");
        cJSON_AddNumberToObject(root, "gw_ms", gwms >= 0 ? gwms : 0);

        cJSON *inet = cJSON_CreateArray();
        const char *hosts[2] = { "1.1.1.1", "8.8.8.8" };
        for (int i = 0; i < 2; i++) {
            int ms = tcp_probe(hosts[i], 443, 1500);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "host", hosts[i]);
            cJSON_AddBoolToObject(o, "ok", ms >= 0);
            cJSON_AddNumberToObject(o, "ms", ms >= 0 ? ms : 0);
            cJSON_AddItemToArray(inet, o);
        }
        cJSON_AddItemToObject(root, "inet", inet);

        int64_t d0 = esp_timer_get_time();
        struct hostent *he = gethostbyname("google.com");
        int dms = (int)((esp_timer_get_time() - d0) / 1000);
        if (he && he->h_addr_list && he->h_addr_list[0]) {
            uint8_t *a = (uint8_t *)he->h_addr_list[0];
            char dip[16]; snprintf(dip, sizeof(dip), "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
            cJSON_AddBoolToObject(root, "dns_ok", true);
            cJSON_AddStringToObject(root, "dns_ip", dip);
            cJSON_AddNumberToObject(root, "dns_ms", dms);
        } else {
            cJSON_AddBoolToObject(root, "dns_ok", false);
        }
    }
    char dns[16]; get_setting("dns1", dns, sizeof(dns));
    cJSON_AddStringToObject(root, "client_dns", dns);
    cJSON_AddStringToObject(root, "state", "done");

    if (s_diag_result) cJSON_Delete(s_diag_result);
    s_diag_result = root;
    s_diag_state = 2;
    vTaskDelete(NULL);
}

static esp_err_t diag_start_post_handler(httpd_req_t *req) {
    if (s_diag_state != 1) {
        s_diag_state = 1;
        xTaskCreate(diag_task, "diag", 6144, NULL, 4, NULL);
    }
    return send_result(req, true, NULL);
}

static esp_err_t diag_get_handler(httpd_req_t *req) {
    if (s_diag_state != 2 || !s_diag_result) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "state", s_diag_state == 1 ? "running" : "idle");
        return send_json(req, r);
    }
    char *out = cJSON_PrintUnformatted(s_diag_result);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t usage_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "time_synced", time(NULL) >= 1700000000);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < TRACK_MAX; i++) {
        if (!s_usage[i].used) continue;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_usage[i].mac[0], s_usage[i].mac[1], s_usage[i].mac[2], s_usage[i].mac[3], s_usage[i].mac[4], s_usage[i].mac[5]);
        char name[64] = "";
        whitelist_get_name(mac, name, sizeof(name));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", mac);
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddNumberToObject(o, "total_bytes", (double)s_usage[i].total_bytes);
        cJSON_AddNumberToObject(o, "week_bytes", s_usage[i].week_bytes);
        cJSON_AddNumberToObject(o, "month_bytes", s_usage[i].month_bytes);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "devices", arr);

    /* Whole-repeater totals (both directions). This is the only figure here
     * that includes downloads/streaming - per-device figures above are
     * uploads only (see FEATURES.md for why). */
    double up_total = 0, up_week = 0, up_month = 0;
    for (int i = 0; i < TRACK_MAX; i++) {
        if (!s_usage[i].used) continue;
        up_total += (double)s_usage[i].total_bytes;
        up_week += s_usage[i].week_bytes;
        up_month += s_usage[i].month_bytes;
    }
    cJSON *overall = cJSON_CreateObject();
    cJSON_AddNumberToObject(overall, "upload_total_bytes", up_total);
    cJSON_AddNumberToObject(overall, "upload_week_bytes", up_week);
    cJSON_AddNumberToObject(overall, "upload_month_bytes", up_month);
    cJSON_AddNumberToObject(overall, "download_total_bytes", (double)s_down_usage.total_bytes);
    cJSON_AddNumberToObject(overall, "download_week_bytes", s_down_usage.week_bytes);
    cJSON_AddNumberToObject(overall, "download_month_bytes", s_down_usage.month_bytes);
    cJSON_AddItemToObject(root, "overall", overall);

    return send_json(req, root);
}

static esp_err_t speedhistory_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();
    int n = s_speedhist_count;
    for (int i = 0; i < n; i++) {
        int idx = ((s_speedhist_head - 1 - i) % SPEEDHIST_MAX + SPEEDHIST_MAX) % SPEEDHIST_MAX;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ago_s", (double)((now - s_speedhist[idx].t) / 1000000));
        cJSON_AddNumberToObject(o, "ms", s_speedhist[idx].ms);
        cJSON_AddBoolToObject(o, "ok", s_speedhist[idx].ok);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "samples", arr);
    return send_json(req, root);
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
    config.max_uri_handlers = 30;
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
        {.uri = "/api/kick",           .method = HTTP_POST, .handler = kick_post_handler},
        {.uri = "/api/me",             .method = HTTP_GET,  .handler = me_get_handler},
        {.uri = "/tools",              .method = HTTP_GET,  .handler = tools_get_handler},
        {.uri = "/chat",                .method = HTTP_GET,  .handler = chat_get_handler},
        {.uri = "/api/chat",            .method = HTTP_POST, .handler = chat_post_handler},
        {.uri = "/api/scan/start",     .method = HTTP_POST, .handler = scan_start_post_handler},
        {.uri = "/api/scan",           .method = HTTP_GET,  .handler = scan_get_handler},
        {.uri = "/api/deauth",         .method = HTTP_GET,  .handler = deauth_get_handler},
        {.uri = "/api/ping",           .method = HTTP_GET,  .handler = ping_get_handler},
        {.uri = "/api/speed",          .method = HTTP_GET,  .handler = speed_get_handler},
        {.uri = "/api/upspeed",        .method = HTTP_POST, .handler = upspeed_post_handler},
        {.uri = "/api/diag/start",     .method = HTTP_POST, .handler = diag_start_post_handler},
        {.uri = "/api/diag",           .method = HTTP_GET,  .handler = diag_get_handler},
        {.uri = "/api/usage",          .method = HTTP_GET,  .handler = usage_get_handler},
        {.uri = "/api/speedhistory",   .method = HTTP_GET,  .handler = speedhistory_get_handler},
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

    down_usage_load();

    s_boot_time_us = esp_timer_get_time();
    wifi_init();
    start_webserver();
    xTaskCreate(button_task, "reset_btn", 3072, NULL, 5, NULL);
}

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

static void split_entry(const char *entry, char *mac_out, size_t mac_len, char *name_out, size_t name_len);

/* ---- RAM copy of the whitelist, used by the per-packet internet filter ----
 * (reading NVS for every packet would be far too slow) */
#define WL_MAX 64
static uint8_t s_wl_macs[WL_MAX][6];
static int s_wl_count = 0;
static bool s_wl_enabled = false;          /* false = list empty = everyone gets internet */
static portMUX_TYPE s_wl_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_ap_ip_be = 0;            /* AP address, network byte order */

static void whitelist_cache_rebuild(void) {
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    uint8_t macs[WL_MAX][6];
    int n = 0;
    bool enabled = strlen(list) > 0;
    if (enabled) {
        char *copy = strdup(list);
        if (copy) {
            char *tok = strtok(copy, ",");
            while (tok && n < WL_MAX) {
                while (*tok == ' ') tok++;
                char m[20], nm[64];
                split_entry(tok, m, sizeof(m), nm, sizeof(nm));
                uint8_t b[6];
                if (parse_mac(m, b)) { memcpy(macs[n], b, 6); n++; }
                tok = strtok(NULL, ",");
            }
            free(copy);
        }
    }
    portENTER_CRITICAL(&s_wl_mux);
    memcpy(s_wl_macs, macs, sizeof(macs));
    s_wl_count = n;
    s_wl_enabled = enabled;
    portEXIT_CRITICAL(&s_wl_mux);
}

static bool mac_has_internet(const uint8_t *mac) {
    bool ok = false;
    portENTER_CRITICAL(&s_wl_mux);
    if (!s_wl_enabled) {
        ok = true;
    } else {
        for (int i = 0; i < s_wl_count; i++) {
            if (memcmp(s_wl_macs[i], mac, 6) == 0) { ok = true; break; }
        }
    }
    portEXIT_CRITICAL(&s_wl_mux);
    return ok;
}

static void whitelist_set(const char *csv) {
    nvs_set_string("whitelist", csv);
    whitelist_cache_rebuild();
}

/* ---- Internet filter: wraps the AP interface's input function ----
 * Every frame a client sends to the repeater passes through here first.
 * A client that is NOT whitelisted may still: get an IP (DHCP), talk to the
 * repeater itself (dashboard / ping) and use ARP. Everything else (i.e. all
 * traffic that would be routed to the internet, including DNS) is dropped. */
static netif_input_fn s_orig_ap_input = NULL;

static err_t ap_input_filter(struct pbuf *p, struct netif *inp) {
    if (p && p->len >= 34) {
        const uint8_t *f = (const uint8_t *)p->payload;
        uint16_t ethertype = (uint16_t)((f[12] << 8) | f[13]);
        if (ethertype == 0x0800 && !mac_has_internet(f + 6)) {
            uint32_t src, dst;
            memcpy(&src, f + 26, 4);
            memcpy(&dst, f + 30, 4);
            bool allowed = (src == 0) ||                 /* DHCP discover */
                           (dst == s_ap_ip_be) ||        /* the repeater itself */
                           (dst == 0xFFFFFFFFu);         /* broadcast (DHCP) */
            if (!allowed) {
                pbuf_free(p);
                return ERR_OK;
            }
        }
    }
    return s_orig_ap_input(p, inp);
}

static void install_internet_filter(void) {
    struct netif *n = (struct netif *)esp_netif_get_netif_impl(s_ap_netif);
    if (!n) { ESP_LOGE(TAG, "Internet filter: AP netif not found"); return; }
    if (n->input == ap_input_filter) return;
    s_orig_ap_input = n->input;
    n->input = ap_input_filter;
    ESP_LOGI(TAG, "Internet filter installed (whitelist controls internet access)");
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
            if (!mac_has_internet(ev->mac)) {
                ESP_LOGW(TAG, "Client %s connected WITHOUT internet (not whitelisted)", mac);
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
    s_ap_ip_be = info.ip.addr;
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
    esp_err_t e3 = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_

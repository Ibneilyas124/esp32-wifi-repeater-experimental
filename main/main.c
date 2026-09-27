/*
 * ESP32 WiFi Repeater / Internet Sharing Router
 * Board: ESP32 DevKit V1
 * Built by Sarfraz Qureshi
 *
 * Connects to an upstream WiFi network (STA) and re-broadcasts internet
 * access over its own access point (AP) using NAPT (NAT), with:
 *   - Public branded landing page (always visible, even to non-admins)
 *   - Styled login page (session-cookie based, not the plain browser popup)
 *   - Settings only reachable at /admin after a successful login
 *   - MAC whitelist filtering for AP clients
 *   - JSON config import / export
 *   - Connected clients list
 *   - Reboot button
 *
 * Default AP:      SSID "Sarfraz"   Password "Sarfraz1"
 * Default admin:   user  "admin"    Password "Sarfraz1"   (change after first boot!)
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

static const char *TAG = "esp32-repeater";

#define NVS_NS              "repeater"
#define DEFAULT_AP_SSID     "Sarfraz"
#define DEFAULT_AP_PASS     "Sarfraz1"     /* WPA2 requires 8+ chars */
#define DEFAULT_ADMIN_PASS  "Sarfraz1"
#define ADMIN_USER          "admin"
#define BUILDER_NAME         "Sarfraz Qureshi"

static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_sta_connected = false;
static char s_sta_ip[16] = "0.0.0.0";
static int64_t s_boot_time_us = 0;
static char s_session_token[40] = "";

/* ---------------- NVS helpers ---------------- */

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

/* Whitelist stored as a single comma-separated string of MACs, e.g.
 * "AA:BB:CC:DD:EE:FF,11:22:33:44:55:66"  An empty string means "allow all". */

static void whitelist_get(char *out, size_t out_len) {
    nvs_get_string("whitelist", out, out_len, "");
}

static void whitelist_set(const char *csv) {
    nvs_set_string("whitelist", csv);
}

static bool mac_str_eq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static bool whitelist_contains(const char *mac) {
    char list[512];
    whitelist_get(list, sizeof(list));
    if (strlen(list) == 0) return true; /* empty whitelist = allow everyone */
    char *copy = strdup(list);
    char *tok = strtok(copy, ",");
    bool found = false;
    while (tok) {
        while (*tok == ' ') tok++;
        if (mac_str_eq(tok, mac)) { found = true; break; }
        tok = strtok(NULL, ",");
    }
    free(copy);
    return found;
}

static void whitelist_add(const char *mac) {
    char list[512];
    whitelist_get(list, sizeof(list));
    if (strstr(list, mac)) return;
    if (strlen(list) == 0) {
        whitelist_set(mac);
    } else {
        char buf[600];
        snprintf(buf, sizeof(buf), "%s,%s", list, mac);
        whitelist_set(buf);
    }
}

static void whitelist_remove(const char *mac) {
    char list[512];
    whitelist_get(list, sizeof(list));
    char out[512] = "";
    char *copy = strdup(list);
    char *tok = strtok(copy, ",");
    bool first = true;
    while (tok) {
        while (*tok == ' ') tok++;
        if (!mac_str_eq(tok, mac)) {
            if (!first) strncat(out, ",", sizeof(out) - strlen(out) - 1);
            strncat(out, tok, sizeof(out) - strlen(out) - 1);
            first = false;
        }
        tok = strtok(NULL, ",");
    }
    free(copy);
    whitelist_set(out);
}

/* ---------------- Session auth ---------------- */

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
    char cookie[128];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    char needle[64];
    snprintf(needle, sizeof(needle), "session=%s", s_session_token);
    return strstr(cookie, needle) != NULL;
}

/* For fetch()-based API calls: reply 401 JSON so the page JS can redirect. */
static esp_err_t require_session_api(httpd_req_t *req) {
    if (check_session(req)) return ESP_OK;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

/* ---------------- WiFi ---------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_sta_connected = false;
            strcpy(s_sta_ip, "0.0.0.0");
            ESP_LOGW(TAG, "Upstream WiFi disconnected, retrying in 3s...");
            vTaskDelay(pdMS_TO_TICKS(3000));
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
        ESP_LOGI(TAG, "Got upstream IP: %s -- enabling NAT/NAPT", s_sta_ip);
        esp_netif_napt_enable(s_ap_netif);
    }
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    char ap_ssid[33], ap_pass[65], sta_ssid[33], sta_pass[65];
    nvs_get_string("ap_ssid", ap_ssid, sizeof(ap_ssid), DEFAULT_AP_SSID);
    nvs_get_string("ap_pass", ap_pass, sizeof(ap_pass), DEFAULT_AP_PASS);
    nvs_get_string("sta_ssid", sta_ssid, sizeof(sta_ssid), "");
    nvs_get_string("sta_pass", sta_pass, sizeof(sta_pass), "");

    wifi_config_t ap_config = { 0 };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    strncpy((char *)ap_config.ap.password, ap_pass, sizeof(ap_config.ap.password));
    ap_config.ap.max_connection = 8;
    ap_config.ap.authmode = strlen(ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap_config.ap.channel = 1;

    wifi_config_t sta_config = { 0 };
    strncpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, sta_pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* If a password shorter than 8 chars ever ends up saved, fall back to
     * an OPEN network instead of crashing the whole device on boot. */
    if (esp_wifi_set_config(WIFI_IF_AP, &ap_config) != ESP_OK) {
        ESP_LOGW(TAG, "AP password invalid (need 8+ chars) - falling back to OPEN network");
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }

    if (strlen(sta_ssid) > 0) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP up: SSID=%s", ap_ssid);
    if (strlen(sta_ssid) == 0) {
        ESP_LOGW(TAG, "No upstream WiFi configured yet. Log in at / then set it under Upstream WiFi.");
    }
}

/* ---------------- HTML: public landing + login page ---------------- */

static const char LANDING_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Sarfraz Qureshi - WiFi Repeater</title>"
"<style>"
"*{box-sizing:border-box}"
"body{margin:0;min-height:100vh;font-family:sans-serif;"
"background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);"
"display:flex;align-items:center;justify-content:center;padding:20px;color:#eee}"
".wrap{width:100%;max-width:420px}"
".brand{text-align:center;margin-bottom:22px}"
".brand .badge{display:inline-block;background:linear-gradient(135deg,#00e0a0,#00a3ff);"
"width:56px;height:56px;border-radius:16px;line-height:56px;font-size:26px;margin-bottom:10px}"
".brand h1{font-size:1.25em;margin:4px 0 2px;color:#fff}"
".brand p{margin:0;color:#9fd8c8;font-size:.9em}"
"section{background:rgba(20,25,30,.75);backdrop-filter:blur(6px);"
"border:1px solid rgba(255,255,255,.08);border-radius:14px;padding:22px;"
"box-shadow:0 8px 30px rgba(0,0,0,.35)}"
"label{display:block;font-size:.8em;color:#9fb3b0;margin:12px 0 4px}"
"input{width:100%;padding:11px;border-radius:8px;border:1px solid #33474a;"
"background:#0f1a1d;color:#eee;font-size:15px}"
"input:focus{outline:none;border-color:#00e0a0}"
"button{width:100%;margin-top:18px;padding:12px;border:none;border-radius:8px;"
"background:linear-gradient(135deg,#00e0a0,#00a3ff);color:#06201a;font-weight:bold;"
"font-size:15px;cursor:pointer}"
"button:active{opacity:.85}"
"#err{color:#ff8080;font-size:.85em;min-height:18px;margin-top:8px;text-align:center}"
"footer{text-align:center;margin-top:18px;color:#6c8a86;font-size:.78em}"
"</style></head><body>"
"<div class='wrap'>"
"<div class='brand'>"
"<div class='badge'>&#128225;</div>"
"<h1>This WiFi Repeater was built by<br>" BUILDER_NAME "</h1>"
"<p>Secure Admin Access</p>"
"</div>"
"<section>"
"<label>Username</label><input id='u' autocapitalize='off' placeholder='admin'>"
"<label>Password</label><input id='p' type='password' placeholder='Password'>"
"<button onclick='doLogin()'>Login</button>"
"<div id='err'></div>"
"</section>"
"<footer>Firmware crafted with care by " BUILDER_NAME "</footer>"
"</div>"
"<script>"
"async function doLogin(){"
"  const u=document.getElementById('u').value;"
"  const p=document.getElementById('p').value;"
"  const e=document.getElementById('err'); e.textContent='';"
"  try{"
"    const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},"
"      body:JSON.stringify({username:u,password:p})});"
"    const j=await r.json();"
"    if(j.ok){ location.href='/admin'; } else { e.textContent='Invalid username or password'; }"
"  }catch(ex){ e.textContent='Could not reach device'; }"
"}"
"document.getElementById('p').addEventListener('keydown',ev=>{if(ev.key==='Enter')doLogin();});"
"</script></body></html>";

/* ---------------- HTML: admin dashboard (only after login) ---------------- */

static const char ADMIN_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>" BUILDER_NAME " - Admin</title>"
"<style>"
"body{background:#0d0d0d;color:#eee;font-family:sans-serif;margin:0;padding:16px}"
"h1{color:#00e0a0;font-size:1.3em;border-bottom:1px solid #333;padding-bottom:8px}"
"h1 small{display:block;color:#7fa79c;font-size:.55em;font-weight:normal;margin-top:4px}"
"h2{color:#00e0a0;font-size:1em;margin-top:24px}"
"section{background:#161616;border:1px solid #2a2a2a;border-radius:8px;padding:12px;margin-bottom:14px}"
"input,button,textarea{background:#1f1f1f;color:#eee;border:1px solid #333;border-radius:6px;padding:8px;margin:4px 0;width:100%;box-sizing:border-box;font-size:14px}"
"button{background:#00e0a0;color:#0d0d0d;font-weight:bold;cursor:pointer;border:none}"
"button.danger{background:#e04040;color:#fff}"
"button.ghost{background:#222;color:#aaa;border:1px solid #333}"
".row{display:flex;gap:8px}"
"table{width:100%;border-collapse:collapse;font-size:13px}"
"td,th{border-bottom:1px solid #2a2a2a;padding:6px;text-align:left}"
"small{color:#888}"
"</style></head><body>"
"<h1>" BUILDER_NAME " &mdash; WiFi Repeater Admin<small>Logged in as admin</small></h1>"

"<section><h2>Status</h2><div id='status'>Loading...</div></section>"

"<section><h2>Upstream WiFi (network to repeat)</h2>"
"<input id='sta_ssid' placeholder='Upstream SSID'>"
"<input id='sta_pass' placeholder='Upstream Password' type='password'>"
"<button onclick='saveWifi()'>Save &amp; Reboot</button></section>"

"<section><h2>MAC Whitelist</h2><small>Leave empty to allow all devices.</small>"
"<div id='wl'></div>"
"<div class='row'><input id='newmac' placeholder='AA:BB:CC:DD:EE:FF'>"
"<button onclick='addMac()'>Add</button></div></section>"

"<section><h2>Config Backup</h2>"
"<div class='row'><button onclick='exportCfg()'>Export JSON</button></div>"
"<textarea id='importbox' rows='4' placeholder='Paste config JSON here to import'></textarea>"
"<button onclick='importCfg()'>Import &amp; Reboot</button></section>"

"<section><div class='row'>"
"<button class='ghost' onclick='logout()'>Logout</button>"
"<button class='danger' onclick=\"fetch('/api/reboot',{method:'POST'})\">Reboot Device</button>"
"</div></section>"

"<script>"
"function authFail(r){ if(r.status===401){ location.href='/'; return true;} return false;}"
"async function refresh(){"
"  const r=await fetch('/api/status'); if(authFail(r))return; const j=await r.json();"
"  document.getElementById('status').innerHTML="
"    'Upstream: '+(j.sta_connected?('Connected ('+j.sta_ip+')'):'Not connected')+"
"    '<br>Uptime: '+j.uptime_s+'s'+"
"    '<br>Free heap: '+j.free_heap+' bytes'+"
"    '<br><table><tr><th>Client MAC</th></tr>'+j.clients.map(c=>'<tr><td>'+c+'</td></tr>').join('')+'</table>';"
"  document.getElementById('sta_ssid').value=j.sta_ssid||'';"
"  document.getElementById('wl').innerHTML=j.whitelist.map(m=>"
"    '<div class=row><input readonly value=\"'+m+'\"><button class=danger onclick=\"rmMac(\\''+m+'\\')\">X</button></div>').join('')||'<small>No restrictions</small>';"
"}"
"async function saveWifi(){"
"  const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},"
"    body:JSON.stringify({ssid:document.getElementById('sta_ssid').value,password:document.getElementById('sta_pass').value})});"
"  if(authFail(r))return;"
"  alert('Saved. Device is rebooting...');"
"}"
"async function addMac(){"
"  const m=document.getElementById('newmac').value.trim(); if(!m)return;"
"  const r=await fetch('/api/whitelist',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'add',mac:m})});"
"  if(authFail(r))return; refresh();"
"}"
"async function rmMac(m){"
"  const r=await fetch('/api/whitelist',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'remove',mac:m})});"
"  if(authFail(r))return; refresh();"
"}"
"async function exportCfg(){"
"  const r=await fetch('/api/config/export'); if(authFail(r))return; const t=await r.text();"
"  document.getElementById('importbox').value=t;"
"}"
"async function importCfg(){"
"  const t=document.getElementById('importbox').value;"
"  const r=await fetch('/api/config/import',{method:'POST',headers:{'Content-Type':'application/json'},body:t});"
"  if(authFail(r))return;"
"  alert('Imported. Device is rebooting...');"
"}"
"async function logout(){ await fetch('/api/logout',{method:'POST'}); location.href='/'; }"
"refresh(); setInterval(refresh,5000);"
"</script></body></html>";

/* ---------------- HTTP handlers ---------------- */

static esp_err_t root_get_handler(httpd_req_t *req) {
    /* Public page: always visible, no login required. Shows branding + login form. */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, LANDING_HTML, HTTPD_RESP_USE_STRLEN);
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
    httpd_resp_send(req, ADMIN_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len) {
    int total = 0;
    int r;
    while (total < (int)buf_len - 1) {
        r = httpd_req_recv(req, buf + total, buf_len - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';
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
        char admin_pass[65];
        nvs_get_string("admin_pass", admin_pass, sizeof(admin_pass), DEFAULT_ADMIN_PASS);
        if (cJSON_IsString(user) && cJSON_IsString(pass) &&
            strcmp(user->valuestring, ADMIN_USER) == 0 &&
            strcmp(pass->valuestring, admin_pass) == 0) {
            ok = true;
        }
        cJSON_Delete(j);
    }
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        generate_session_token(s_session_token, sizeof(s_session_token));
        char cookie_hdr[80];
        snprintf(cookie_hdr, sizeof(cookie_hdr), "session=%s; Path=/; HttpOnly", s_session_token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_hdr);
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t logout_post_handler(httpd_req_t *req) {
    s_session_token[0] = '\0';
    httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; Max-Age=0");
    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "sta_connected", s_sta_connected);
    cJSON_AddStringToObject(root, "sta_ip", s_sta_ip);
    cJSON_AddNumberToObject(root, "uptime_s", (double)((esp_timer_get_time() - s_boot_time_us) / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());

    char sta_ssid[33];
    nvs_get_string("sta_ssid", sta_ssid, sizeof(sta_ssid), "");
    cJSON_AddStringToObject(root, "sta_ssid", sta_ssid);

    cJSON *clients = cJSON_CreateArray();
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        for (int i = 0; i < sta_list.num; i++) {
            char mac[18];
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                      sta_list.sta[i].mac[0], sta_list.sta[i].mac[1], sta_list.sta[i].mac[2],
                      sta_list.sta[i].mac[3], sta_list.sta[i].mac[4], sta_list.sta[i].mac[5]);
            cJSON_AddItemToArray(clients, cJSON_CreateString(mac));
        }
    }
    cJSON_AddItemToObject(root, "clients", clients);

    cJSON *wl = cJSON_CreateArray();
    char list[512];
    whitelist_get(list, sizeof(list));
    if (strlen(list) > 0) {
        char *copy = strdup(list);
        char *tok = strtok(copy, ",");
        while (tok) {
            while (*tok == ' ') tok++;
            cJSON_AddItemToArray(wl, cJSON_CreateString(tok));
            tok = strtok(NULL, ",");
        }
        free(copy);
    }
    cJSON_AddItemToObject(root, "whitelist", wl);

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t whitelist_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[256];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (j) {
        cJSON *action = cJSON_GetObjectItem(j, "action");
        cJSON *mac = cJSON_GetObjectItem(j, "mac");
        if (cJSON_IsString(action) && cJSON_IsString(mac)) {
            if (strcmp(action->valuestring, "add") == 0) whitelist_add(mac->valuestring);
            else if (strcmp(action->valuestring, "remove") == 0) whitelist_remove(mac->valuestring);
        }
        cJSON_Delete(j);
    }
    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[256];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (j) {
        cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
        cJSON *pass = cJSON_GetObjectItem(j, "password");
        if (cJSON_IsString(ssid)) nvs_set_string("sta_ssid", ssid->valuestring);
        if (cJSON_IsString(pass)) nvs_set_string("sta_pass", pass->valuestring);
        cJSON_Delete(j);
    }
    httpd_resp_send(req, "ok, rebooting", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t config_export_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    char v[128];

    nvs_get_string("ap_ssid", v, sizeof(v), DEFAULT_AP_SSID); cJSON_AddStringToObject(root, "ap_ssid", v);
    nvs_get_string("ap_pass", v, sizeof(v), DEFAULT_AP_PASS); cJSON_AddStringToObject(root, "ap_pass", v);
    nvs_get_string("sta_ssid", v, sizeof(v), ""); cJSON_AddStringToObject(root, "sta_ssid", v);
    nvs_get_string("sta_pass", v, sizeof(v), ""); cJSON_AddStringToObject(root, "sta_pass", v);
    nvs_get_string("admin_pass", v, sizeof(v), DEFAULT_ADMIN_PASS); cJSON_AddStringToObject(root, "admin_pass", v);
    char wl[512];
    whitelist_get(wl, sizeof(wl));
    cJSON_AddStringToObject(root, "whitelist", wl);

    char *out = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t config_import_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[1024];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    const char *keys[] = {"ap_ssid", "ap_pass", "sta_ssid", "sta_pass", "admin_pass", "whitelist"};
    for (int i = 0; i < 6; i++) {
        cJSON *item = cJSON_GetObjectItem(j, keys[i]);
        if (cJSON_IsString(item)) nvs_set_string(keys[i], item->valuestring);
    }
    cJSON_Delete(j);
    httpd_resp_send(req, "ok, rebooting", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    httpd_resp_send(req, "rebooting", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 14;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uris[] = {
            {.uri = "/", .method = HTTP_GET, .handler = root_get_handler},
            {.uri = "/admin", .method = HTTP_GET, .handler = admin_get_handler},
            {.uri = "/api/login", .method = HTTP_POST, .handler = login_post_handler},
            {.uri = "/api/logout", .method = HTTP_POST, .handler = logout_post_handler},
            {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
            {.uri = "/api/whitelist", .method = HTTP_POST, .handler = whitelist_post_handler},
            {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler},
            {.uri = "/api/config/export", .method = HTTP_GET, .handler = config_export_get_handler},
            {.uri = "/api/config/import", .method = HTTP_POST, .handler = config_import_post_handler},
            {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler},
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
            httpd_register_uri_handler(server, &uris[i]);
        }
        ESP_LOGI(TAG, "Web dashboard started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start web server");
    }
}

void app_main(void) {
    s_boot_time_us = esp_timer_get_time();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    start_webserver();
}

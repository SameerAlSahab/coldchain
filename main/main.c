#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "cc.h"

static const char *TAG = "cc";

volatile bool g_sta_up;
char g_sta_ip[16], g_sta_ssid[33];
static esp_timer_handle_t retry_tmr;
static char ai_url[257];


static void creds_load(char *s, size_t sn, char *p, size_t pn) {
    nvs_handle_t h; s[0] = p[0] = 0;
    if (nvs_open("cc", NVS_READONLY, &h) == ESP_OK) { nvs_get_str(h, "ssid", s, &sn); nvs_get_str(h, "pass", p, &pn); nvs_close(h); }
}
static void creds_save(const char *s, const char *p) {
    nvs_handle_t h;
    if (nvs_open("cc", NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, "ssid", s); nvs_set_str(h, "pass", p); nvs_commit(h); nvs_close(h); }
}

static void ai_load(void) {
    nvs_handle_t h; size_t n = sizeof ai_url; ai_url[0] = 0;
    if (nvs_open("cc", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "ai", ai_url, &n) != ESP_OK) ai_url[0] = 0;
        nvs_close(h);
    }
}
static void ai_save(const char *u) {
    nvs_handle_t h;
    if (nvs_open("cc", NVS_READWRITE, &h) == ESP_OK) {
        if (u[0]) nvs_set_str(h, "ai", u); else nvs_erase_key(h, "ai");
        nvs_commit(h); nvs_close(h);
    }
}
bool ai_configured(void) { return ai_url[0] != 0; }


static void sta_apply(const char *s, const char *p) {
    wifi_config_t c = {0};
    strlcpy((char *)c.sta.ssid, s, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, p, sizeof c.sta.password);
    c.sta.threshold.authmode = p[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &c);
}
static void retry_cb(void *a) { if (g_sta_ssid[0]) esp_wifi_connect(); }
static void on_evt(void *a, esp_event_base_t b, int32_t id, void *d) {
    if (b == WIFI_EVENT && id == WIFI_EVENT_STA_START) { if (g_sta_ssid[0]) esp_wifi_connect(); }
    else if (b == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_sta_up = false; g_sta_ip[0] = 0;
        if (g_sta_ssid[0]) esp_timer_start_once(retry_tmr, 5000000);
    } else if (b == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = d;
        snprintf(g_sta_ip, sizeof g_sta_ip, IPSTR, IP2STR(&e->ip_info.ip)); g_sta_up = true;
        ESP_LOGI(TAG, "STA ip %s", g_sta_ip);   /* net_task (net.c) starts SNTP/MQTT when it sees g_sta_up */
    }
}
static void wifi_start(void) {
    esp_netif_init(); esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap(); esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&ic);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL);
    esp_timer_create_args_t ta = {.callback = retry_cb, .name = "retry"}; esp_timer_create(&ta, &retry_tmr);
    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, AP_SSID); strcpy((char *)ap.ap.password, AP_PASS);
    ap.ap.ssid_len = strlen(AP_SSID); ap.ap.max_connection = 4; ap.ap.authmode = WIFI_AUTH_WPA2_PSK; ap.ap.channel = 1;
    char p[65]; creds_load(g_sta_ssid, sizeof g_sta_ssid, p, sizeof p);
    esp_wifi_set_mode(WIFI_MODE_APSTA); esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (g_sta_ssid[0]) sta_apply(g_sta_ssid, p);
    esp_wifi_start();
}


static char rb[8192]; static int rn;
static esp_err_t http_ev(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && rn + e->data_len < (int)sizeof rb - 1) { memcpy(rb + rn, e->data, e->data_len); rn += e->data_len; rb[rn] = 0; }
    return ESP_OK;
}
void cc_urlenc(const char *s, char *o, size_t m) {
    size_t k = 0;
    for (; *s && k + 4 < m; s++) {
        if (isalnum((unsigned char)*s) || strchr("-_.~", *s)) o[k++] = *s; else k += sprintf(o + k, "%%%02X", (unsigned char)*s);
    }
    o[k] = 0;
}


cJSON *ai_query(const char *prompt) {
    cJSON *out = cJSON_CreateObject();
    if (!ai_url[0]) { cJSON_AddBoolToObject(out, "ok", 0); cJSON_AddStringToObject(out, "error", "No AI endpoint set. Add one first."); return out; }
    if (!g_sta_up) { cJSON_AddBoolToObject(out, "ok", 0); cJSON_AddStringToObject(out, "error", "No internet. Connect to a hotspot first."); return out; }
    static char enc[2400], url[2700];
    cc_urlenc(prompt, enc, sizeof enc);
    snprintf(url, sizeof url, "%s%cmessage=%s", ai_url, strchr(ai_url, '?') ? '&' : '?', enc);
    rn = 0; rb[0] = 0;
    esp_http_client_config_t c = {.url = url, .event_handler = http_ev, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 25000, .buffer_size = 2048, .buffer_size_tx = 3072,
        .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"};
    esp_http_client_handle_t h = esp_http_client_init(&c);

    esp_http_client_set_header(h, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(h, "Accept-Language", "en-US,en;q=0.9,bn;q=0.8");

    esp_err_t er = esp_http_client_perform(h); int code = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    cJSON *j = (er == ESP_OK && code == 200) ? cJSON_Parse(rb) : NULL;
    cJSON *r = j ? cJSON_GetObjectItem(j, "response") : NULL;
    if (!cJSON_IsString(r)) {
        char m[80]; snprintf(m, sizeof m, "AI API failed (HTTP %d, %s)", code, esp_err_to_name(er));
        cJSON_AddBoolToObject(out, "ok", 0); cJSON_AddStringToObject(out, "error", m); cJSON_Delete(j); return out;
    }
    cJSON_AddBoolToObject(out, "ok", 1); cJSON_AddStringToObject(out, "text", r->valuestring);
    char *s = strdup(r->valuestring), *a = s ? strchr(s, '{') : NULL, *z = s ? strrchr(s, '}') : NULL;
    if (a && z > a) { *(z + 1) = 0; cJSON *in = cJSON_Parse(a); if (in) cJSON_AddItemToObject(out, "json", in); }
    free(s); cJSON_Delete(j); return out;
}


extern const char idx_s[] asm("_binary_index_html_start");
extern const char idx_e[] asm("_binary_index_html_end");
static esp_err_t send_json(httpd_req_t *r, cJSON *o) {
    char *s = cJSON_PrintUnformatted(o); httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, s ? s : "{}"); free(s); cJSON_Delete(o); return e;
}
static esp_err_t h_root(httpd_req_t *r) { httpd_resp_set_type(r, "text/html"); return httpd_resp_send(r, idx_s, idx_e - idx_s - 1); }
static esp_err_t h_status(httpd_req_t *r) {
    static cc_state_t s; cc_snap(&s);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ap", AP_SSID); cJSON_AddBoolToObject(o, "sta", g_sta_up);
    cJSON_AddStringToObject(o, "ssid", g_sta_ssid); cJSON_AddStringToObject(o, "ip", g_sta_ip);
    cJSON_AddBoolToObject(o, "ai", ai_url[0] != 0);   /* only true/false, never the URL */
    cJSON_AddNumberToObject(o, "up", s.up_s);
    cJSON_AddStringToObject(o, "power", s.pf ? "BATTERY" : "MAINS");
    cJSON_AddBoolToObject(o, "pf", s.pf);
    cJSON_AddNumberToObject(o, "battery", s.batt_pct); cJSON_AddNumberToObject(o, "batt_mv", s.batt_mv);
    cJSON_AddBoolToObject(o, "muted", s.muted); cJSON_AddBoolToObject(o, "mqtt", s.mqtt_up);
    cJSON_AddNumberToObject(o, "q", s.q_len);
    cJSON_AddStringToObject(o, "ai_state", s.ai_state); cJSON_AddStringToObject(o, "ai_msg", s.ai_msg);
    cJSON *a = cJSON_AddArrayToObject(o, "f");
    for (int i = 0; i < 2; i++) {
        const cc_fridge_t *f = &s.f[i]; cJSON *x = cJSON_CreateObject();
        bool fresh = cc_ai_fresh(f, s.up_s);
        cJSON_AddBoolToObject(x, "present", f->present); cJSON_AddBoolToObject(x, "valid", f->valid);
        if (f->present && f->valid) cJSON_AddNumberToObject(x, "t", f->t); else cJSON_AddNullToObject(x, "t");
        cJSON_AddStringToObject(x, "status", cc_st_name(f->st));
        cJSON_AddBoolToObject(x, "door", f->door_open); cJSON_AddNumberToObject(x, "door_open_s", f->door_open_s);
        cJSON_AddBoolToObject(x, "door_alarm", f->door_alarm);
        cJSON_AddBoolToObject(x, "comp", f->comp);
        cJSON_AddNumberToObject(x, "budget_pct", (f->budget * 100 + BUDGET_MAX - 1) / BUDGET_MAX);
        cJSON_AddStringToObject(x, "risk", f->risk); cJSON_AddBoolToObject(x, "discard", f->discard);
        cJSON_AddNumberToObject(x, "health_local", f->health_local); cJSON_AddNumberToObject(x, "pred_local", f->pred_local);
        cJSON_AddBoolToObject(x, "ai_fresh", fresh);
        if (f->ai_ok) { cJSON_AddNumberToObject(x, "health_ai", f->health_ai); cJSON_AddNumberToObject(x, "pred_ai", f->pred_ai); }
        else { cJSON_AddNullToObject(x, "health_ai"); cJSON_AddNullToObject(x, "pred_ai"); }
        cJSON_AddStringToObject(x, "tip", f->tip);
        cJSON_AddItemToArray(a, x);
    }
    return send_json(r, o);
}
static esp_err_t h_cmd(httpd_req_t *r) {
    char buf[80]; int n = httpd_req_recv(r, buf, sizeof buf - 1);
    if (n <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "empty");
    buf[n] = 0; cJSON *j = cJSON_Parse(buf), *c = j ? cJSON_GetObjectItem(j, "cmd") : NULL;
    bool ok = cJSON_IsString(c) && cc_cmd(c->valuestring);
    cJSON_Delete(j);
    cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", ok); return send_json(r, o);
}
static esp_err_t h_scan(httpd_req_t *r) {
    wifi_scan_config_t sc = {0}; uint16_t n = 15; wifi_ap_record_t rec[15];
    esp_wifi_scan_start(&sc, true); esp_wifi_scan_get_ap_records(&n, rec);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "ssid", (char *)rec[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", rec[i].rssi); cJSON_AddBoolToObject(o, "open", rec[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(r, a);
}
static esp_err_t h_wifi(httpd_req_t *r) {
    char buf[200]; int n = httpd_req_recv(r, buf, sizeof buf - 1);
    if (n <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "empty");
    buf[n] = 0; cJSON *j = cJSON_Parse(buf);
    cJSON *s = j ? cJSON_GetObjectItem(j, "ssid") : NULL, *p = j ? cJSON_GetObjectItem(j, "pass") : NULL;
    if (!cJSON_IsString(s) || !s->valuestring[0]) { cJSON_Delete(j); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "ssid"); }
    const char *pw = cJSON_IsString(p) ? p->valuestring : "";
    strlcpy(g_sta_ssid, s->valuestring, sizeof g_sta_ssid); creds_save(g_sta_ssid, pw); sta_apply(g_sta_ssid, pw);
    cJSON_Delete(j); esp_wifi_disconnect(); esp_wifi_connect();
    cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", 1); return send_json(r, o);
}

static esp_err_t h_aiset(httpd_req_t *r) {
    char buf[400];
    if (r->content_len == 0 || r->content_len >= sizeof buf) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "size");
    int n = httpd_req_recv(r, buf, sizeof buf - 1);
    if (n <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "empty");
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf), *u = j ? cJSON_GetObjectItem(j, "url") : NULL;
    if (!cJSON_IsString(u)) { cJSON_Delete(j); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "url"); }
    const char *v = u->valuestring;               /* "" = remove */
    if (v[0] && (strncmp(v, "https://", 8) != 0 || strlen(v) >= sizeof ai_url)) {
        cJSON_Delete(j); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "https only");
    }
    strlcpy(ai_url, v, sizeof ai_url); ai_save(ai_url);
    cJSON_Delete(j);
    cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", 1); return send_json(r, o);
}
static void web_start(void) {
    httpd_handle_t s; httpd_config_t c = HTTPD_DEFAULT_CONFIG(); c.stack_size = 8192; c.max_uri_handlers = 10;
    httpd_start(&s, &c);
    httpd_uri_t u[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root}, {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = h_scan}, {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/cmd", .method = HTTP_POST, .handler = h_cmd},
        {.uri = "/api/aiurl", .method = HTTP_POST, .handler = h_aiset}};
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(s, &u[i]);
}

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);   /* keep URL out of debug logs */
    ai_load();
    core_start();     /* GPIO, OLED, sensing, control, buzzer/LEDs */
    net_start();      /* Telegram, MQTT/SNTP, AI task */
    wifi_start(); web_start();
}

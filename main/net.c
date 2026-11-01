#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "cc.h"
#include "secrets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mqtt_client.h"
#include "mbedtls/md.h"

static const char *TAG = "net";
static char dev[16];
static QueueHandle_t tgq;
static TaskHandle_t ai_th;
static esp_mqtt_client_handle_t mq;
static volatile bool mq_up;


void net_notify(const char *msg) {
    if (!tgq) return;
    char b[120]; strlcpy(b, msg, sizeof b);
    xQueueSend(tgq, b, 0);
}
static void tg_task(void *a) {
    char m[120], full[160];
    static char enc[480], url[700];
    for (;;) {
        xQueueReceive(tgq, m, portMAX_DELAY);
        for (int w = 0; w < 60 && !g_sta_up; w++) vTaskDelay(pdMS_TO_TICKS(1000));
        if (!g_sta_up) continue;
        snprintf(full, sizeof full, "[%s] %s", dev, m);
        cc_urlenc(full, enc, sizeof enc);
        snprintf(url, sizeof url, "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s", TG_TOKEN, TG_CHAT_ID, enc);
        esp_http_client_config_t c = {.url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000};
        esp_http_client_handle_t h = esp_http_client_init(&c);
        esp_err_t e = esp_http_client_perform(h);
        int code = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
        ESP_LOGI(TAG, "telegram: %s HTTP %d", esp_err_to_name(e), code);
    }
}


static void mq_evt(void *a, esp_event_base_t b, int32_t id, void *d) {
    if (id == MQTT_EVENT_CONNECTED) { mq_up = true; cc_mqtt_state(true); }
    else if (id == MQTT_EVENT_DISCONNECTED) { mq_up = false; cc_mqtt_state(false); }
}

static bool synced(void) { return time(NULL) > 1700000000; }


static bool publish_sample(const cc_sample_t *s) {
    static char msg[160], js[360], topic[48];
    char n[9], sig[65] = "";
    uint32_t now_up = up_ms() / 1000;
    bool sy = synced();
    long long ts = sy ? (long long)time(NULL) : (long long)now_up;
    long long st = sy ? ts - (long long)(now_up - s->up_s) : (long long)s->up_s;
    snprintf(n, sizeof n, "%08lx", (unsigned long)esp_random());
    if (HMAC_KEY[0]) {
        snprintf(msg, sizeof msg, "%s|%d|%lu|%lld|%lld|%s|%.2f|%d|%d|%d", dev, s->f, (unsigned long)s->seq, st, ts, n,
                 s->t, s->door, s->comp, s->pw);
        uint8_t mac[32];
        if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY),
                            (const uint8_t *)msg, strlen(msg), mac) != 0) return false;
        for (int i = 0; i < 32; i++) sprintf(sig + 2 * i, "%02x", mac[i]);
    }
    int k = snprintf(js, sizeof js,
        "{\"d\":\"%s\",\"f\":%d,\"seq\":%lu,\"st\":%lld,\"ts\":%lld,\"n\":\"%s\",\"t\":%.2f,\"door\":%d,\"comp\":%d,\"pw\":%d,\"bat\":%d",
        dev, s->f, (unsigned long)s->seq, st, ts, n, s->t, s->door, s->comp, s->pw, s->bat);
    if (sig[0]) k += snprintf(js + k, sizeof js - k, ",\"sig\":\"%s\"", sig);
    snprintf(js + k, sizeof js - k, "}");
    snprintf(topic, sizeof topic, "coldchain/%s/f%d", dev, s->f);
    return esp_mqtt_client_publish(mq, topic, js, 0, 1, 0) >= 0;
}

static void net_task(void *a) {
    bool started = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!started && g_sta_up) {
            started = true;
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            if (MQTT_URI[0]) {
                esp_mqtt_client_config_t mc = {
                    .broker.address.uri = MQTT_URI,
                    .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
                    .session.keepalive = 30,
                    .network.reconnect_timeout_ms = 5000,
                };
                mq = esp_mqtt_client_init(&mc);
                if (mq) {
                    esp_mqtt_client_register_event(mq, ESP_EVENT_ANY_ID, mq_evt, NULL);
                    esp_mqtt_client_start(mq);
                }
            }
        }
        if (!mq || !mq_up) continue;
        for (int k = 0; k < 40; k++) {
            cc_sample_t s;
            if (!cc_ring_peek(&s)) break;
            if (!publish_sample(&s)) break;
            cc_ring_drop();
        }
    }
}


void net_ai_kick(void) { if (ai_th) xTaskNotifyGive(ai_th); }

static bool jnum(cJSON *o, const char *k, double *v) {
    cJSON *x = cJSON_GetObjectItem(o, k);
    if (cJSON_IsNumber(x)) { *v = x->valuedouble; return true; }
    if (cJSON_IsString(x) && x->valuestring[0]) { *v = atof(x->valuestring); return true; }
    return false;
}

static void ai_run(void) {
    static cc_state_t s;
    char pr[1000]; int k = 0;
#define ADD(...) do { if (k < (int)sizeof pr) k += snprintf(pr + k, sizeof pr - k, __VA_ARGS__); } while (0)
    cc_snap(&s);
    ADD("Vaccine cold-chain analyst. Reply ONLY compact JSON, no markdown: "
        "{\"f\":[{\"health\":0-100,\"pred30\":degC,\"risk\":\"low|medium|high\",\"tip\":\"max 50 chars; if risky say which fridge to move vaccines to\"},{same}]}. Safe 2-8C. ");
    for (int i = 0; i < 2; i++) {
        const cc_fridge_t *f = &s.f[i];
        if (!f->present) { ADD("F%d:no sensor. ", i + 1); continue; }
        ADD("F%d: T=%.1f st=%s pred30=%.1f slope=%.2f/min duty=%.0f%% cyc/h=%d budget=%d%% door=%s comp=%s pw=%s. ",
            i + 1, f->t, cc_st_name(f->st), f->pred_local, f->slope, f->duty * 100.0f, f->cph,
            (f->budget * 100 + BUDGET_MAX - 1) / BUDGET_MAX, f->door_open ? "open" : "closed", f->comp ? "on" : "off",
            s.pf ? "battery" : "mains");
    }
#undef ADD
    cc_ai_status("busy", "");
    cJSON *r = ai_query(pr);
    cJSON *ok = cJSON_GetObjectItem(r, "ok");
    cJSON *js = cJSON_GetObjectItem(r, "json");
    cJSON *fa = js ? cJSON_GetObjectItem(js, "f") : NULL;
    int got = 0;
    if (cJSON_IsTrue(ok) && cJSON_IsArray(fa)) {
        for (int i = 0; i < 2 && i < cJSON_GetArraySize(fa); i++) {
            cJSON *it = cJSON_GetArrayItem(fa, i);
            double h, p;
            if (!s.f[i].present || !it || !jnum(it, "health", &h) || !jnum(it, "pred30", &p)) continue;
            if (h < 0) h = 0; if (h > 100) h = 100;
            if (p < -30) p = -30; if (p > 80) p = 80;
            cJSON *rs = cJSON_GetObjectItem(it, "risk"), *tp = cJSON_GetObjectItem(it, "tip");
            char risk[8] = "low", tip[64] = "";
            if (cJSON_IsString(rs)) risk[0] = (char)tolower((unsigned char)rs->valuestring[0]);
            if (risk[0] == 'h') strcpy(risk, "high"); else if (risk[0] == 'm') strcpy(risk, "medium"); else strcpy(risk, "low");
            if (cJSON_IsString(tp)) {
                strlcpy(tip, tp->valuestring, sizeof tip);
                for (char *c = tip; *c; c++) if ((unsigned char)*c < 32) *c = ' ';
            }
            cc_ai_set(i, (int)(h + 0.5), (float)p, risk, tip);
            got++;
        }
    }
    if (got) cc_ai_status("ok", "AI updated");
    else {
        cJSON *er = cJSON_GetObjectItem(r, "error");
        cc_ai_status("err", cJSON_IsString(er) ? er->valuestring : "AI reply not usable; using local values");
    }
    cJSON_Delete(r);
}

static void ai_task(void *a) {
    uint32_t last = 0; bool first = true;
    for (;;) {
        bool kick = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) > 0;
        uint32_t now = up_ms();
        if (!kick && !first && now - last < 120000) continue;
        if (!ai_configured()) { cc_ai_status("off", "No AI endpoint set"); continue; }
        if (!g_sta_up) { cc_ai_status("offline", "No internet; showing local values"); continue; }
        first = false; last = now;
        ai_run();
    }
}

void net_start(void) {
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(dev, sizeof dev, "cc-%02X%02X%02X", mac[3], mac[4], mac[5]);
    if (TG_TOKEN[0] && TG_CHAT_ID[0]) {
        tgq = xQueueCreate(4, 120);
        xTaskCreate(tg_task, "tg", 8192, NULL, 3, NULL);
    }
    xTaskCreate(net_task, "net", 5120, NULL, 4, NULL);
    xTaskCreate(ai_task, "ai", 10240, NULL, 3, &ai_th);
}

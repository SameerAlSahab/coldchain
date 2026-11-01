#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "core";
static cc_state_t S;
static SemaphoreHandle_t mx, rmx, pmx;
#define LK() xSemaphoreTake(mx, portMAX_DELAY)
#define UL() xSemaphoreGive(mx)

static bool nv_get(const char *k, int32_t *v) {
    nvs_handle_t h;
    if (nvs_open("cc", NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_i32(h, k, v); nvs_close(h); return e == ESP_OK;
}
static void nv_set(const char *k, int32_t v) {
    nvs_handle_t h;
    if (nvs_open("cc", NVS_READWRITE, &h) == ESP_OK) { nvs_set_i32(h, k, v); nvs_commit(h); nvs_close(h); }
}


static int sv_b[2] = {-1, -1}, sv_d[2] = {-1, -1};
static void persist(void) {
    int b[2], d[2];
    LK(); for (int i = 0; i < 2; i++) { b[i] = S.f[i].budget; d[i] = S.f[i].discard; } UL();
    xSemaphoreTake(pmx, portMAX_DELAY);
    for (int i = 0; i < 2; i++) {
        if (b[i] != sv_b[i]) { nv_set(i ? "bud1" : "bud0", b[i]); sv_b[i] = b[i]; }
        if (d[i] != sv_d[i]) { nv_set(i ? "dis1" : "dis0", d[i]); sv_d[i] = d[i]; }
    }
    xSemaphoreGive(pmx);
}


#define RING_N 720
static cc_sample_t ring[RING_N];
static int r_head, r_cnt;
static uint32_t seq_next, seq_lim;

static uint32_t get_seq(void) {
    if (seq_next >= seq_lim) { seq_lim += 1000; nv_set("seq", (int32_t)seq_lim); }
    return seq_next++;
}
bool cc_ring_peek(cc_sample_t *o) {
    xSemaphoreTake(rmx, portMAX_DELAY);
    bool ok = r_cnt > 0; if (ok) *o = ring[r_head];
    xSemaphoreGive(rmx); return ok;
}
void cc_ring_drop(void) {
    xSemaphoreTake(rmx, portMAX_DELAY);
    if (r_cnt > 0) { r_head = (r_head + 1) % RING_N; r_cnt--; }
    xSemaphoreGive(rmx);
}
static void push_sample(int i) {
    cc_sample_t s;
    LK();
    const cc_fridge_t *f = &S.f[i];
    if (!f->present || !f->valid) { UL(); return; }
    s.t = f->t; s.door = f->door_open; s.comp = f->comp; s.pw = S.pf; s.bat = (uint8_t)S.batt_pct;
    UL();
    s.f = (uint8_t)i; s.up_s = up_ms() / 1000; s.seq = get_seq();
    xSemaphoreTake(rmx, portMAX_DELAY);
    if (r_cnt == RING_N) { r_head = (r_head + 1) % RING_N; r_cnt--; }
    ring[(r_head + r_cnt) % RING_N] = s; r_cnt++;
    xSemaphoreGive(rmx);
}


static uint8_t rom_slot[2][8];
static bool rom_have[2];
static uint32_t last_ok[2];

static void sens_task(void *a) {
    uint8_t found[2][8];
    int fails = 0; uint32_t last_scan = 0; bool first = true;
    for (;;) {
        uint32_t now = up_ms();
        bool have2 = rom_have[0] && rom_have[1];
        if (first || (!have2 && now - last_scan > 60000) || fails >= 5) {
            first = false; last_scan = now; fails = 0;
            int n = hw_ow_scan(found, 2);
            LK();
            for (int k = 0; k < n; k++) {      /* slots are sticky: fridge numbering never shifts */
                int slot = -1;
                for (int i = 0; i < 2; i++) if (rom_have[i] && !memcmp(rom_slot[i], found[k], 8)) slot = i;
                if (slot < 0) for (int i = 0; i < 2; i++) if (!rom_have[i]) {
                    memcpy(rom_slot[i], found[k], 8); rom_have[i] = true; S.f[i].present = true;
                    ESP_LOGI(TAG, "DS18B20 -> fridge %d", i + 1); break;
                }
            }
            UL();
        }
        if (rom_have[0] || rom_have[1]) {
            if (hw_ow_convert()) {
                int okc = 0;
                for (int i = 0; i < 2; i++) {
                    float t;
                    if (rom_have[i] && hw_ow_read(rom_slot[i], &t)) {
                        LK(); S.f[i].t = t; last_ok[i] = up_ms(); UL(); okc++;
                    }
                }
                fails = okc ? 0 : fails + 1;
            } else fails++;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


static void do_reset(int i) {
    LK(); S.f[i].budget = BUDGET_MAX; S.f[i].discard = false; UL();
    persist(); ESP_LOGI(TAG, "admin reset fridge %d", i + 1);
}
static void toggle_pf(void) { LK(); S.pf = !S.pf; UL(); ESP_LOGW(TAG, "power-fail toggled"); }
static void do_mute(void) {
    LK(); if (S.f[0].door_alarm || S.f[1].door_alarm) S.muted = true; UL();
}
bool cc_cmd(const char *c) {
    if (!strcmp(c, "reset0")) do_reset(0);
    else if (!strcmp(c, "reset1")) do_reset(1);
    else if (!strcmp(c, "pf")) toggle_pf();
    else if (!strcmp(c, "mute")) do_mute();
    else if (!strcmp(c, "ai")) net_ai_kick();
    else return false;
    return true;
}

void cc_snap(cc_state_t *o) {
    LK(); *o = S; UL();
    o->up_s = up_ms() / 1000; o->q_len = r_cnt;
}
void cc_ai_set(int i, int health, float pred, const char *risk, const char *tip) {
    if (i < 0 || i > 1) return;
    LK();
    cc_fridge_t *f = &S.f[i];
    f->ai_ok = true; f->health_ai = health; f->pred_ai = pred; f->ai_up_s = up_ms() / 1000;
    strlcpy(f->risk_ai, risk, sizeof f->risk_ai); strlcpy(f->tip_ai, tip, sizeof f->tip_ai);
    UL();
}
void cc_ai_status(const char *state, const char *msg) {
    LK(); strlcpy(S.ai_state, state, sizeof S.ai_state); strlcpy(S.ai_msg, msg, sizeof S.ai_msg); UL();
}
void cc_mqtt_state(bool up) { LK(); S.mqtt_up = up; UL(); }


static uint32_t off_since[2];

static void comp_ctl(cc_fridge_t *f, int i, uint32_t now) {
    bool want = f->comp;
    if (S.pf || !f->present || !f->valid) want = false;
    else if (f->t >= 7.0f) want = true;
    else if (f->t <= 3.0f) want = false;
    if (want && !f->comp && now - off_since[i] < 60000) want = false;
    if (!want && f->comp) off_since[i] = now;
    f->comp = want; hw_relay(i, want);
}
static int rk(const char *s) { return s[0] == 'h' ? 2 : s[0] == 'm' ? 1 : 0; }
static const char *rname[] = {"low", "medium", "high"};

static void tick1(uint32_t now, const bool *dopen, const uint32_t *dsince) {
    static float mvs = 0;
    int mv = hw_batt_mv();
    mvs = mvs <= 0 ? (float)mv : 0.8f * mvs + 0.2f * (float)mv;
    int pct = (int)((mvs - 3300.0f) * 100.0f / 900.0f);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool alarm_new[2] = {0}, disc_new[2] = {0};

    LK();
    S.batt_mv = (int)mvs; S.batt_pct = pct;
    for (int i = 0; i < 2; i++) {
        cc_fridge_t *f = &S.f[i];
        f->door_open = dopen[i];
        f->door_open_s = dopen[i] ? (now - dsince[i]) / 1000 : 0;
        bool al = f->door_open && f->door_open_s > 60;
        if (al && !f->door_alarm) alarm_new[i] = true;
        f->door_alarm = al;
        f->valid = last_ok[i] != 0 && (now - last_ok[i]) < 30000;
        if (!f->present) { f->st = ST_NOSENSOR; f->at_risk = false; comp_ctl(f, i, now); continue; }
        if (f->valid && !f->discard) {              /* heat budget: 1 unit/s above 8C, 2 above 12C */
            if (f->t > 12.0f) f->budget -= 2; else if (f->t > 8.0f) f->budget -= 1;
            if (f->budget <= 0) { f->budget = 0; f->discard = true; disc_new[i] = true; }
        }
        f->at_risk = !f->discard && f->budget < BUDGET_MAX / 5;
        if (f->discard) f->st = ST_DISCARD;
        else if (!f->valid) f->st = ST_FAULT;
        else {
            float t = f->t;
            if (t >= 2.0f && t <= 8.0f) f->st = ST_SAFE;
            else if ((t > 8.0f && t <= 10.0f) || (t >= 1.0f && t < 2.0f)) f->st = ST_WARN;
            else f->st = ST_BREACH;
        }
        comp_ctl(f, i, now);
    }

    for (int i = 0; i < 2; i++) {
        cc_fridge_t *f = &S.f[i], *o = &S.f[1 - i];
        if (!f->present) { f->risk[0] = f->tip[0] = 0; continue; }
        int lr = (f->st == ST_BREACH || f->st == ST_DISCARD || f->at_risk || f->pred_local > 10.0f) ? 2
               : (f->st == ST_WARN || f->st == ST_FAULT || f->pred_local > 8.0f) ? 1 : 0;
        bool fresh = cc_ai_fresh(f, now / 1000);
        int ar = fresh ? rk(f->risk_ai) : -1;
        int r = ar > lr ? ar : lr;
        strlcpy(f->risk, rname[r], sizeof f->risk);
        if (f->discard) snprintf(f->tip, sizeof f->tip, "DISCARD vaccines, then admin reset");
        else if (fresh && ar >= lr && f->tip_ai[0]) strlcpy(f->tip, f->tip_ai, sizeof f->tip);
        else if (r == 0) f->tip[0] = 0;
        else if (o->present && o->valid && o->st == ST_SAFE && !o->at_risk)
            snprintf(f->tip, sizeof f->tip, "Move vaccines to fridge %d", 2 - i);
        else snprintf(f->tip, sizeof f->tip, "Check fridge now");
    }
    bool any = S.f[0].door_alarm || S.f[1].door_alarm;
    if (!any) S.muted = false;
    UL();

    char m[80];
    for (int i = 0; i < 2; i++) {
        if (alarm_new[i]) { snprintf(m, sizeof m, "ALERT: fridge %d door open >60 s", i + 1); net_notify(m); }
        if (disc_new[i]) { snprintf(m, sizeof m, "ALERT: fridge %d heat budget used up - DISCARD", i + 1); net_notify(m); }
    }
    if (disc_new[0] || disc_new[1]) persist();
}


#define HN 360
typedef struct { float t; uint8_t c; } hent_t;
static hent_t hist[2][HN];
static uint32_t hcnt[2];
static float ema[2], prev_t[2]; static bool ema_ok[2], prev_comp[2];
#define H(i, j) hist[i][(hcnt[i] - (uint32_t)n + (uint32_t)(j)) % HN]

static void tick10(void) {
    float t[2]; bool comp[2], ok[2], door[2];
    LK();
    for (int i = 0; i < 2; i++) { t[i] = S.f[i].t; comp[i] = S.f[i].comp; ok[i] = S.f[i].present && S.f[i].valid; door[i] = S.f[i].door_open; }
    UL();
    for (int i = 0; i < 2; i++) {
        if (!ok[i]) continue;
        if (hcnt[i] > 0 && comp[i] && prev_comp[i] && !door[i]) {
            float rate = (prev_t[i] - t[i]) * 6.0f;          /* C/min */
            if (!ema_ok[i]) { ema[i] = rate; ema_ok[i] = true; } else ema[i] = 0.9f * ema[i] + 0.1f * rate;
        }
        prev_t[i] = t[i]; prev_comp[i] = comp[i];
        hist[i][hcnt[i] % HN].t = t[i]; hist[i][hcnt[i] % HN].c = comp[i]; hcnt[i]++;

        int n = hcnt[i] < HN ? (int)hcnt[i] : HN;
        int on = 0, cyc = 0;
        for (int j = 0; j < n; j++) { on += H(i, j).c; if (j > 0 && H(i, j).c && !H(i, j - 1).c) cyc++; }
        float duty = (float)on / (float)n;
        float cph = cyc / (n * 10.0f / 3600.0f);
        float slope = 0, pred = t[i];
        int m = n < 60 ? n : 60;
        if (m >= 12) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int k = 0; k < m; k++) { double x = k / 6.0, y = H(i, n - m + k).t; sx += x; sy += y; sxx += x * x; sxy += x * y; }
            double den = m * sxx - sx * sx;
            if (den > 1e-9) {
                slope = (float)((m * sxy - sx * sy) / den);
                float fit = (float)(sy / m + slope * ((m - 1) / 6.0 - sx / m));
                pred = fit + slope * 30.0f;
            }
        }
        if (pred < -20.0f) pred = -20.0f; if (pred > 60.0f) pred = 60.0f;
        float hl = 100.0f;
        if (n >= 30) {
            float dp = duty > 0.6f ? fminf(40.0f, (duty - 0.6f) / 0.4f * 40.0f) : 0;
            float cp = cph > 8.0f ? fminf(30.0f, (cph - 8.0f) * 5.0f) : 0;
            float rp = (ema_ok[i] && ema[i] < 0.15f) ? fminf(30.0f, (0.15f - ema[i]) / 0.15f * 30.0f) : 0;
            hl -= dp + cp + rp;
        }
        if (hl < 0) hl = 0;
        LK();
        S.f[i].slope = slope; S.f[i].pred_local = pred; S.f[i].duty = duty; S.f[i].cph = (int)(cph + 0.5f);
        S.f[i].health_local = (int)(hl + 0.5f);
        UL();
    }
}


static int prev_page;
static void on_press(int b) {
    LK();
    if (b == 0) { S.page = (S.page + 1) % 3; prev_page = S.page == 2 ? prev_page : S.page; }
    else if (b == 4) { if (S.page == 2) S.page = prev_page; else { prev_page = S.page; S.page = 2; } }
    UL();
    if (b == 2) toggle_pf();
    else if (b == 3) do_mute();
}
static void on_b2_long(void) {
    LK(); int p = S.page; UL();
    if (p < 2) do_reset(p);
}
static void btn_poll(uint32_t now) {
    static bool st[5]; static uint8_t cnt[5]; static uint32_t t0[5]; static bool fired;
    for (int b = 0; b < 5; b++) {
        bool p = hw_btn_raw(b);
        if (p != st[b]) {
            if (++cnt[b] >= 3) {
                st[b] = p; cnt[b] = 0;
                if (p) { t0[b] = now; if (b != 1) on_press(b); } else if (b == 1) fired = false;
            }
        } else cnt[b] = 0;
    }
    if (st[1] && !fired && now - t0[1] >= 2000) { fired = true; on_b2_long(); }
}


static void outputs(uint32_t now) {
    int st[2]; bool risk[2], alarm, muted;
    LK();
    for (int i = 0; i < 2; i++) { st[i] = S.f[i].st; risk[i] = S.f[i].at_risk; }
    alarm = S.f[0].door_alarm || S.f[1].door_alarm; muted = S.muted;
    UL();
    bool blink = (now / 500) & 1;
    for (int i = 0; i < 2; i++) {
        bool g = false, r = false;
        switch (st[i]) {
            case ST_SAFE: if (risk[i]) r = blink; else g = true; break;
            case ST_WARN: case ST_FAULT: r = blink; break;
            case ST_BREACH: case ST_DISCARD: r = true; break;
            default: break;
        }
        hw_led(i, g, r);
    }
    uint32_t ph = now % 1500;                      /* two short beeps every 1.5 s */
    hw_buzzer(alarm && !muted && (ph < 150 || (ph >= 300 && ph < 450)));
}


static void core_task(void *a) {
    bool dst[2]; uint32_t dsince[2]; int dcnt[2] = {0, 0}; bool sreq[2] = {false, false}; uint32_t nxt_s[2] = {0, 0};
    uint32_t now = up_ms(), t1 = now, t10 = now, t30 = now;
    for (int i = 0; i < 2; i++) { dst[i] = hw_door_raw(i); dsince[i] = now; }
    LK(); for (int i = 0; i < 2; i++) S.f[i].door_open = dst[i]; UL();
    for (;;) {
        now = up_ms();
        for (int i = 0; i < 2; i++) {
            bool raw = hw_door_raw(i);
            if (raw != dst[i]) {
                if (++dcnt[i] >= 5) {              /* ~100 ms debounce */
                    dst[i] = raw; dcnt[i] = 0; dsince[i] = now; sreq[i] = true;
                    LK(); S.f[i].door_open = raw; S.f[i].door_open_s = 0; UL();
                }
            } else dcnt[i] = 0;
        }
        btn_poll(now);
        if (now - t1 >= 1000) { t1 = now; tick1(now, dst, dsince); }
        if (now - t10 >= 10000) { t10 = now; tick10(); }
        if (now - t30 >= 30000) { t30 = now; persist(); }
        for (int i = 0; i < 2; i++)
            if (sreq[i] || now >= nxt_s[i]) { push_sample(i); sreq[i] = false; nxt_s[i] = now + 5000; }
        outputs(now);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void disp_task(void *a) {
    static cc_state_t s;
    for (;;) { cc_snap(&s); hw_oled_draw(&s); vTaskDelay(pdMS_TO_TICKS(500)); }
}

void core_start(void) {
    mx = xSemaphoreCreateMutex(); rmx = xSemaphoreCreateMutex(); pmx = xSemaphoreCreateMutex();
    memset(&S, 0, sizeof S);
    for (int i = 0; i < 2; i++) {
        S.f[i].budget = BUDGET_MAX; S.f[i].st = ST_NOSENSOR; S.f[i].health_local = 100;
        int32_t v;
        if (nv_get(i ? "bud1" : "bud0", &v) && v >= 0 && v <= BUDGET_MAX) S.f[i].budget = v;
        if (nv_get(i ? "dis1" : "dis0", &v) && v) S.f[i].discard = true;
        if (S.f[i].budget == 0) S.f[i].discard = true;
        sv_b[i] = S.f[i].budget; sv_d[i] = S.f[i].discard;
    }
    strlcpy(S.ai_state, "off", sizeof S.ai_state);
    int32_t sq = 0; nv_get("seq", &sq);
    seq_next = (uint32_t)sq; seq_lim = seq_next + 1000; nv_set("seq", (int32_t)seq_lim);
    hw_init();
    hw_oled_init();
    xTaskCreate(sens_task, "sens", 4096, NULL, 5, NULL);
    xTaskCreate(core_task, "core", 5120, NULL, 6, NULL);
    xTaskCreate(disp_task, "disp", 5120, NULL, 3, NULL);
}

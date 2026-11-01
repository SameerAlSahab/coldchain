#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_timer.h"
#include "cJSON.h"

#define AP_SSID "ewu_innovators"
#define AP_PASS "12341122"


#define PIN_SDA 21
#define PIN_SCL 22
#define PIN_OW 4
#define PIN_DOOR0 32
#define PIN_DOOR1 33
#define PIN_RELAY0 26
#define PIN_RELAY1 27
#define PIN_BUZ 25
#define PIN_G1 16
#define PIN_R1 17
#define PIN_G2 18
#define PIN_R2 19
#define PIN_B1 23
#define PIN_B2 13
#define PIN_B3 14
#define PIN_B4 5
#define PIN_B5 15
#define DOOR_OPEN_LEVEL 1
#define RELAY_ON_LVL 0
#define BUDGET_MAX 3600

static inline uint32_t up_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

typedef enum { ST_SAFE, ST_WARN, ST_BREACH, ST_DISCARD, ST_NOSENSOR, ST_FAULT } cc_status_t;
static inline const char *cc_st_name(cc_status_t s) {
    static const char *n[] = {"SAFE", "WARNING", "BREACH", "DISCARD", "NO SENSOR", "SENSOR ERR"};
    return n[s];
}

typedef struct {
    bool present, valid;
    float t;
    cc_status_t st;
    bool at_risk, discard;
    bool door_open, door_alarm; uint32_t door_open_s;
    bool comp;
    int budget;
    float slope, pred_local, duty; int cph; int health_local;
    bool ai_ok; int health_ai; float pred_ai; char risk_ai[8]; char tip_ai[64]; uint32_t ai_up_s;
    char risk[8]; char tip[64];
} cc_fridge_t;

typedef struct {
    cc_fridge_t f[2];
    bool pf, muted;
    int batt_pct, batt_mv;
    int page;
    bool mqtt_up; int q_len;
    char ai_state[8]; char ai_msg[80];
    uint32_t up_s;
} cc_state_t;

static inline bool cc_ai_fresh(const cc_fridge_t *f, uint32_t up_s) { return f->ai_ok && (up_s - f->ai_up_s) < 600; }

typedef struct { uint32_t seq, up_s; float t; uint8_t f, door, comp, pw, bat; } cc_sample_t;

/* hw.c */
void hw_init(void);
int  hw_ow_scan(uint8_t roms[][8], int max);
bool hw_ow_convert(void);
bool hw_ow_read(const uint8_t rom[8], float *t);
bool hw_door_raw(int i);               /* true = open */
void hw_relay(int i, bool on);
void hw_led(int i, bool green, bool red);
void hw_buzzer(bool on);
bool hw_btn_raw(int b);                /* true = pressed */
int  hw_batt_mv(void);
void hw_oled_init(void);
void hw_oled_draw(const cc_state_t *s);

/* core.c */
void core_start(void);
void cc_snap(cc_state_t *o);
bool cc_cmd(const char *c);
bool cc_ring_peek(cc_sample_t *o);
void cc_ring_drop(void);
void cc_ai_set(int i, int health, float pred, const char *risk, const char *tip);
void cc_ai_status(const char *state, const char *msg);
void cc_mqtt_state(bool up);

/* net.c */
void net_start(void);
void net_notify(const char *msg);
void net_ai_kick(void);

/* main.c */
extern volatile bool g_sta_up;
extern char g_sta_ip[16], g_sta_ssid[33];
cJSON *ai_query(const char *prompt);
bool ai_configured(void);
void cc_urlenc(const char *s, char *o, size_t m);

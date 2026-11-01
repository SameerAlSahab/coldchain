#include <stdio.h>
#include <string.h>
#include "cc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_idf_version.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "u8g2.h"
#include "u8g2_esp32_hal.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
#define BATT_ATTEN ADC_ATTEN_DB_12
#else
#define BATT_ATTEN ADC_ATTEN_DB_11
#endif

static const int door_pin[2] = {PIN_DOOR0, PIN_DOOR1};
static const int relay_pin[2] = {PIN_RELAY0, PIN_RELAY1};
static const int led_g[2] = {PIN_G1, PIN_G2};
static const int led_r[2] = {PIN_R1, PIN_R2};
static const int btn_pin[5] = {PIN_B1, PIN_B2, PIN_B3, PIN_B4, PIN_B5};

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static bool cali_ok;
static u8g2_t u8g2;

static void out_pin(int pin, int lvl) {
    gpio_reset_pin(pin); gpio_set_level(pin, lvl); gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}
static void in_pu(int pin) {
    gpio_reset_pin(pin); gpio_set_direction(pin, GPIO_MODE_INPUT); gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
}

void hw_init(void) {
    for (int i = 0; i < 2; i++) {
        out_pin(relay_pin[i], !RELAY_ON_LVL);
        out_pin(led_g[i], 0); out_pin(led_r[i], 0);
        in_pu(door_pin[i]);
    }
    out_pin(PIN_BUZ, 0);
    for (int b = 0; b < 5; b++) in_pu(btn_pin[b]);
    gpio_reset_pin(PIN_OW);
    gpio_set_direction(PIN_OW, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(PIN_OW, 1);

    adc_oneshot_unit_init_cfg_t uc = {.unit_id = ADC_UNIT_1};
    adc_oneshot_new_unit(&uc, &adc);
    adc_oneshot_chan_cfg_t ch = {.atten = BATT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(adc, ADC_CHANNEL_7, &ch);   /* GPIO35 */
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED

#endif
}

bool hw_door_raw(int i) { return gpio_get_level(door_pin[i]) == DOOR_OPEN_LEVEL; }
void hw_relay(int i, bool on) { gpio_set_level(relay_pin[i], on ? RELAY_ON_LVL : !RELAY_ON_LVL); }
void hw_led(int i, bool g, bool r) { gpio_set_level(led_g[i], g); gpio_set_level(led_r[i], r); }
void hw_buzzer(bool on) { gpio_set_level(PIN_BUZ, on); }
bool hw_btn_raw(int b) { return gpio_get_level(btn_pin[b]) == 0; }

int hw_batt_mv(void) {
    long sum = 0; int n = 0;
    for (int k = 0; k < 16; k++) {
        int raw, mv;
        if (adc_oneshot_read(adc, ADC_CHANNEL_7, &raw) != ESP_OK) continue;
        if (cali_ok && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) sum += mv; else sum += raw * 3100L / 4095;
        n++;
    }
    return n ? (int)(sum / n) * 2 : 0;      /* 100k/100k divider */
}


static portMUX_TYPE ow_mux = portMUX_INITIALIZER_UNLOCKED;
#define OWL() gpio_set_level(PIN_OW, 0)
#define OWH() gpio_set_level(PIN_OW, 1)

static bool ow_reset(void) {
    bool pres;
    portENTER_CRITICAL(&ow_mux);
    OWL(); esp_rom_delay_us(480); OWH(); esp_rom_delay_us(70);
    pres = gpio_get_level(PIN_OW) == 0;
    portEXIT_CRITICAL(&ow_mux);
    esp_rom_delay_us(410);
    return pres;
}
static void ow_wbit(int b) {
    portENTER_CRITICAL(&ow_mux);
    OWL(); esp_rom_delay_us(b ? 6 : 60); OWH(); esp_rom_delay_us(b ? 64 : 10);
    portEXIT_CRITICAL(&ow_mux);
}
static int ow_rbit(void) {
    int r;
    portENTER_CRITICAL(&ow_mux);
    OWL(); esp_rom_delay_us(3); OWH(); esp_rom_delay_us(9);
    r = gpio_get_level(PIN_OW);
    esp_rom_delay_us(55);
    portEXIT_CRITICAL(&ow_mux);
    return r;
}
static void ow_wbyte(uint8_t v) { for (int i = 0; i < 8; i++) { ow_wbit(v & 1); v >>= 1; } }
static uint8_t ow_rbyte(void) { uint8_t v = 0; for (int i = 0; i < 8; i++) v |= (uint8_t)(ow_rbit() << i); return v; }
static uint8_t crc8(const uint8_t *d, int n) {
    uint8_t c = 0;
    while (n--) { uint8_t b = *d++; for (int i = 0; i < 8; i++) { uint8_t m = (c ^ b) & 1; c >>= 1; if (m) c ^= 0x8C; b >>= 1; } }
    return c;
}


int hw_ow_scan(uint8_t roms[][8], int max) {
    uint8_t rom[8] = {0};
    int ld = 0, n = 0; bool last = false;
    while (!last && n < max) {
        if (!ow_reset()) break;
        ow_wbyte(0xF0);
        int last_zero = 0, idx = 0; uint8_t mask = 1; bool fail = false;
        for (int bit = 1; bit <= 64; bit++) {
            int id = ow_rbit(), cmp = ow_rbit(), dir;
            if (id && cmp) { fail = true; break; }
            if (id != cmp) dir = id;
            else {
                dir = (bit < ld) ? ((rom[idx] & mask) ? 1 : 0) : (bit == ld);
                if (!dir) last_zero = bit;
            }
            if (dir) rom[idx] |= mask; else rom[idx] &= (uint8_t)~mask;
            ow_wbit(dir);
            mask <<= 1; if (!mask) { idx++; mask = 1; }
        }
        if (fail) break;
        ld = last_zero; if (ld == 0) last = true;
        if (crc8(rom, 7) == rom[7] && rom[0] == 0x28) memcpy(roms[n++], rom, 8);
    }
    return n;
}


bool hw_ow_convert(void) {
    if (!ow_reset()) return false;
    ow_wbyte(0xCC); ow_wbyte(0x44);
    for (int k = 0; k < 60; k++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (ow_rbit()) return true;
    }
    return false;
}

bool hw_ow_read(const uint8_t rom[8], float *t) {
    if (!ow_reset()) return false;
    ow_wbyte(0x55);
    for (int i = 0; i < 8; i++) ow_wbyte(rom[i]);
    ow_wbyte(0xBE);
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) sp[i] = ow_rbyte();
    if (crc8(sp, 8) != sp[8]) return false;
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    float v = raw / 16.0f;
    if (v > 84.99f && v < 85.01f) return false;
    if (v < -55.0f || v > 125.0f) return false;
    *t = v;
    return true;
}


void hw_oled_init(void) {
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = PIN_SDA;
    hal.bus.i2c.scl = PIN_SCL;
    u8g2_esp32_hal_init(hal);
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
    u8x8_SetI2CAddress(&u8g2.u8x8, 0x3C << 1);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
}

static void draw_fridge(const cc_state_t *s, int i) {
    static const char *sn[] = {"SAFE", "WARN", "BREACH", "DISCARD", "NO SENS", "SENS ERR"};
    const cc_fridge_t *f = &s->f[i];
    char b[40], d[16];
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    snprintf(b, sizeof b, "F%d %s", i + 1, sn[f->st]);
    u8g2_DrawStr(&u8g2, 0, 9, b);
    snprintf(b, sizeof b, "%s %d%%", s->pf ? "BAT" : "MAINS", s->batt_pct);
    u8g2_DrawStr(&u8g2, 128 - 6 * (int)strlen(b), 9, b);
    if (!f->present) { u8g2_DrawStr(&u8g2, 0, 36, "No DS18B20 found"); return; }

    bool ai = cc_ai_fresh(f, s->up_s);
    int hp = ai ? f->health_ai : f->health_local;
    float pr = ai ? f->pred_ai : f->pred_local;
    if (f->valid) snprintf(b, sizeof b, "%.1f", f->t); else snprintf(b, sizeof b, "--.-");
    u8g2_SetFont(&u8g2, u8g2_font_logisoso24_tf);
    u8g2_DrawStr(&u8g2, 0, 40, b);
    int w = u8g2_GetStrWidth(&u8g2, b);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    if (w <= 70) u8g2_DrawStr(&u8g2, w + 2, 22, "\260" "C");
    snprintf(b, sizeof b, "BUD:%d%%", (f->budget * 100 + BUDGET_MAX - 1) / BUDGET_MAX);
    u8g2_DrawStr(&u8g2, 80, 24, b);
    snprintf(b, sizeof b, "HP:%d", hp);
    u8g2_DrawStr(&u8g2, 80, 36, b);
    if (f->door_open) snprintf(d, sizeof d, "OPEN %lus", (unsigned long)f->door_open_s); else snprintf(d, sizeof d, "ok");
    snprintf(b, sizeof b, "CMP:%s D:%s", f->comp ? "ON" : "OFF", d);
    u8g2_DrawStr(&u8g2, 0, 50, b);
    if (f->discard) u8g2_DrawStr(&u8g2, 0, 62, "DISCARD! B2 hold=RST");
    else if (f->door_alarm) u8g2_DrawStr(&u8g2, 0, 62, "DOOR OPEN ALARM");
    else {
        snprintf(b, sizeof b, "30m:%.1f\260" "C%s %s", pr, pr > 8.0f ? "!" : "", ai ? "AI" : "LC");
        u8g2_DrawStr(&u8g2, 0, 62, b);
    }
}

static void draw_net(const cc_state_t *s) {
    char b[40];
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    snprintf(b, sizeof b, "AP:%s", AP_SSID); u8g2_DrawStr(&u8g2, 0, 9, b);
    u8g2_DrawStr(&u8g2, 0, 20, "AP IP:192.168.4.1");
    snprintf(b, sizeof b, "STA:%.16s", g_sta_up ? g_sta_ssid : (g_sta_ssid[0] ? "connecting" : "off"));
    u8g2_DrawStr(&u8g2, 0, 31, b);
    snprintf(b, sizeof b, "IP:%s", g_sta_up ? g_sta_ip : "--");
    u8g2_DrawStr(&u8g2, 0, 42, b);
    snprintf(b, sizeof b, "AI:%s MQTT:%s", s->ai_state, s->mqtt_up ? "up" : "off");
    u8g2_DrawStr(&u8g2, 0, 53, b);
    snprintf(b, sizeof b, "Buf:%d Up:%lus", s->q_len, (unsigned long)s->up_s);
    u8g2_DrawStr(&u8g2, 0, 63, b);
}

void hw_oled_draw(const cc_state_t *s) {
    u8g2_ClearBuffer(&u8g2);
    if (s->page == 2) draw_net(s); else draw_fridge(s, s->page);
    u8g2_SendBuffer(&u8g2);
}

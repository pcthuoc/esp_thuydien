#include "counter.h"
#include "driver/pcnt.h"

// ============================================================
// Pin mapping (logic đảo qua 2N7002 → đếm FALLING)
// ============================================================
#define END1_PIN  47
#define END2_PIN  48

// PCNT units: 0 = END1, 1 = END2
static const pcnt_unit_t units[2] = {PCNT_UNIT_0, PCNT_UNIT_1};
static const int pins[2] = {END1_PIN, END2_PIN};

void counter_init() {
    for (int i = 0; i < 2; i++) {
        pcnt_config_t cfg = {};
        cfg.pulse_gpio_num = pins[i];
        cfg.ctrl_gpio_num  = PCNT_PIN_NOT_USED;
        cfg.channel        = PCNT_CHANNEL_0;
        cfg.unit           = units[i];
        cfg.pos_mode       = PCNT_COUNT_DIS;  // rising  = không đếm
        cfg.neg_mode       = PCNT_COUNT_INC;  // falling = +1 (logic đảo)
        cfg.lctrl_mode     = PCNT_MODE_KEEP;
        cfg.hctrl_mode     = PCNT_MODE_KEEP;
        cfg.counter_h_lim  = 32767;
        cfg.counter_l_lim  = 0;

        pcnt_unit_config(&cfg);

        // Glitch filter ~1µs (APB 80MHz → 80 ticks = 1µs)
        pcnt_set_filter_value(units[i], 80);
        pcnt_filter_enable(units[i]);

        pcnt_counter_pause(units[i]);
        pcnt_counter_clear(units[i]);
        pcnt_counter_resume(units[i]);

        Serial.printf("[COUNTER] CH%d init OK (GPIO%d)\n", i + 1, pins[i]);
    }
}

uint16_t counter_get(uint8_t ch) {
    if (ch >= 2) return 0;
    int16_t count = 0;
    pcnt_get_counter_value(units[ch], &count);
    return (uint16_t)count;
}

void counter_reset(uint8_t ch) {
    if (ch >= 2) return;
    pcnt_counter_pause(units[ch]);
    pcnt_counter_clear(units[ch]);
    pcnt_counter_resume(units[ch]);
    Serial.printf("[COUNTER] CH%d reset\n", ch + 1);
}

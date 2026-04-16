#include "counter.h"
#include "debug_config.h"
#include "driver/pcnt.h"
#include "esp_timer.h"

// ============================================================
// EN1: Quadrature encoder Autonics ENC-1-4-T-24
//   DI1 = IO1 = kênh A  (qua optocoupler PC817, logic đảo)
//   DI2 = IO2 = kênh B  (qua optocoupler PC817, logic đảo)
//   DI3 = IO41 = reset encoder về 0 (qua optocoupler PC817, logic đảo)
// Chế độ X1: đếm trên cạnh A, B dùng xác định chiều
//   Kết quả int16_t: dương = thuận chiều, âm = ngược chiều
// ============================================================
#define EN1_A_PIN    1   // DI1
#define EN1_B_PIN    2   // DI2
#define EN1_RST_PIN  41  // DI3

static const pcnt_unit_t units[1] = {PCNT_UNIT_0};
static volatile bool resetRequested = false;

// ISR reset encoder qua DI3 (debounce 50ms) — chỉ set flag, xử lý ở loop
static void IRAM_ATTR onEncoderReset() {
    static uint32_t lastUs = 0;
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (now - lastUs < 50000) return;  // 50ms debounce
    lastUs = now;
    resetRequested = true;
}

void counter_init() {
    // ---- Channel 0: A là pulse, B là ctrl (xác định chiều) ----
    // Optocoupler đảo logic:
    //   Encoder A HIGH  → IO1 LOW  → falling edge tại IO1 = encoder A rising
    //   Encoder B HIGH  → IO2 LOW  → lctrl_mode áp dụng khi IO2=LOW (B đang HIGH)
    //
    // Quy tắc quadrature: A↑ khi B=HIGH → thuận → count INC
    //   neg_mode (IO1 falling = A↑):
    //     lctrl (IO2=LOW, B=HIGH) = KEEP  → INC (thuận)
    //     hctrl (IO2=HIGH, B=LOW) = REVERSE → DEC (ngược)
    pcnt_config_t ch0 = {};
    ch0.pulse_gpio_num = EN1_A_PIN;
    ch0.ctrl_gpio_num  = EN1_B_PIN;
    ch0.channel        = PCNT_CHANNEL_0;
    ch0.unit           = PCNT_UNIT_0;
    ch0.pos_mode       = PCNT_COUNT_DIS;      // IO1 rising (A falling) → bỏ qua
    ch0.neg_mode       = PCNT_COUNT_INC;      // IO1 falling (A rising)  → đếm
    ch0.lctrl_mode     = PCNT_MODE_KEEP;      // IO2=LOW (B=HIGH) → giữ INC = thuận
    ch0.hctrl_mode     = PCNT_MODE_REVERSE;   // IO2=HIGH (B=LOW) → đổi thành DEC = ngược
    ch0.counter_h_lim  =  32767;
    ch0.counter_l_lim  = -32768;
    pcnt_unit_config(&ch0);

    // Glitch filter ~1µs (APB 80MHz)
    pcnt_set_filter_value(PCNT_UNIT_0, 80);
    pcnt_filter_enable(PCNT_UNIT_0);

    pcnt_counter_pause(PCNT_UNIT_0);
    pcnt_counter_clear(PCNT_UNIT_0);
    pcnt_counter_resume(PCNT_UNIT_0);

    // DI3 (IO41) — reset encoder về 0, FALLING (optocoupler đảo)
    pinMode(EN1_RST_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(EN1_RST_PIN), onEncoderReset, FALLING);

    LOG_IF(LOG_COUNTER, "[COUNTER] EN1 quadrature init OK (A=IO1, B=IO2, RST=IO41)\n");
}

int16_t counter_get(uint8_t ch) {
    if (ch >= 1) return 0;
    int16_t count = 0;
    pcnt_get_counter_value(units[ch], &count);
    return count;
}

void counter_reset(uint8_t ch) {
    if (ch >= 1) return;
    pcnt_counter_pause(units[ch]);
    pcnt_counter_clear(units[ch]);
    pcnt_counter_resume(units[ch]);
    LOG_IF(LOG_COUNTER, "[COUNTER] EN%d reset\n", ch + 1);
}

// Gọi trong loop() — xử lý flag reset từ ISR DI3
void counter_update() {
    if (resetRequested) {
        resetRequested = false;
        counter_reset(0);
        LOG_IF(LOG_COUNTER, "[COUNTER] EN1 reset by DI3\n");
    }
}

void counter_debug_print() {
    int16_t val = counter_get(0);
    Serial.printf("[COUNTER] EN1: %d xung (%s)\n",
                  val,
                  val > 0 ? "thuận" : val < 0 ? "ngược" : "đứng yên");
}

#include "rain_gauge.h"
#include "ntp_rtc.h"
#include <time.h>
#include "esp_timer.h"

// ============================================================
// DI4 — Cảm biến mưa tipping bucket
// PC817 optocoupler, pull-up 10kΩ, logic đảo → đếm FALLING
// (chuyển từ DI1/IO1 sang DI4/IO40 để nhường IO1/IO2 cho encoder)
// ============================================================
#define DI4_PIN  40

static volatile uint32_t pulseCount = 0;
static int lastDay = -1;  // Ngày cuối cùng đã check

// ISR — mỗi tip = 1 xung, debounce 200ms chống nảy tiếp điểm cơ
static void IRAM_ATTR onRainPulse() {
    static uint32_t lastUs = 0;
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (now - lastUs < 200000) return;  // 200ms debounce
    lastUs = now;
    pulseCount++;
}

void rain_init() {
    pinMode(DI4_PIN, INPUT);  // pull-up bên ngoài (R200 10kΩ)
    attachInterrupt(digitalPinToInterrupt(DI4_PIN), onRainPulse, FALLING);

    // Ghi nhớ ngày hiện tại
    struct tm t;
    if (getLocalTime(&t)) {
        lastDay = t.tm_mday;
    }

    Serial.println("[RAIN] DI4/IO40 init OK (cảm biến mưa, auto-reset 0h00)");
}

void rain_update() {
    struct tm t;
    if (!getLocalTime(&t)) return;

    // Sang ngày mới (0h00) → reset
    if (lastDay >= 0 && t.tm_mday != lastDay) {
        Serial.printf("[RAIN] New day -> reset (yesterday: %lu tips)\n", pulseCount);
        noInterrupts();
        pulseCount = 0;
        interrupts();
    }
    lastDay = t.tm_mday;
}

uint32_t rain_get_count() {
    noInterrupts();
    uint32_t cnt = pulseCount;
    interrupts();
    return cnt;
}

void rain_reset() {
    noInterrupts();
    pulseCount = 0;
    interrupts();
    Serial.println("[RAIN] Counter reset (manual)");
}

void rain_debug_print() {
    uint32_t cnt = rain_get_count();
    Serial.printf("[RAIN] DI4/IO40: %lu xung hôm nay\n", cnt);
}

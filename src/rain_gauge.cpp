#include "rain_gauge.h"
#include "ntp_rtc.h"
#include <time.h>

// ============================================================
// DI1 — Cảm biến mưa tipping bucket
// PC817 optocoupler, pull-up 10kΩ, logic đảo → đếm FALLING
// ============================================================
#define DI1_PIN  1

static volatile uint32_t pulseCount = 0;
static int lastDay = -1;  // Ngày cuối cùng đã check

// ISR — mỗi tip = 1 xung
static void IRAM_ATTR onRainPulse() {
    pulseCount++;
}

void rain_init() {
    pinMode(DI1_PIN, INPUT);  // pull-up bên ngoài (R200 10kΩ)
    attachInterrupt(digitalPinToInterrupt(DI1_PIN), onRainPulse, FALLING);

    // Ghi nhớ ngày hiện tại
    struct tm t;
    if (getLocalTime(&t)) {
        lastDay = t.tm_mday;
    }

    Serial.println("[RAIN] DI1 init OK (GPIO1, auto-reset 0h00)");
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

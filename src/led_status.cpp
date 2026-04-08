#include "led_status.h"
#include <Adafruit_NeoPixel.h>

// --- Cấu hình phần cứng ---
#define LED_PIN         42
#define LED_COUNT       1
#define LED_BRIGHTNESS  30   // 0-255, giảm để không chói mắt

// --- Timing (ms) ---
#define BLINK_FAST      200
#define BLINK_SLOW      500
#define BLINK_1S        1000
#define BREATH_PERIOD   2000  // Chu kỳ thở đầy đủ (fade in + fade out)

// --- Màu RGB ---
#define COLOR_YELLOW    255, 180,   0
#define COLOR_BLUE        0,  80, 255
#define COLOR_GREEN       0, 255,   0
#define COLOR_ORANGE    255,  80,   0
#define COLOR_RED       255,   0,   0
#define COLOR_PURPLE    160,   0, 255

static Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
static LedState currentState = LedState::BOOTING;
static unsigned long lastUpdate = 0;
static bool blinkOn = false;
static bool needsRefresh = true;  // Cờ để trạng thái tĩnh chỉ gửi 1 lần

// Flash overlay
static uint8_t flashR, flashG, flashB;
static uint8_t flashSteps = 0;
static unsigned long flashTimer = 0;

// ============================================================
// Hàm nội bộ
// ============================================================

// Đặt màu trực tiếp
static void setColor(uint8_t r, uint8_t g, uint8_t b) {
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
}

// Tắt LED
static void setOff() {
    strip.setPixelColor(0, 0);
    strip.show();
}

// Nhấp nháy: bật/tắt theo interval
static void handleBlink(uint8_t r, uint8_t g, uint8_t b, uint16_t interval) {
    unsigned long now = millis();
    if (now - lastUpdate >= interval) {
        lastUpdate = now;
        blinkOn = !blinkOn;
        if (blinkOn) {
            setColor(r, g, b);
        } else {
            setOff();
        }
    }
}

// Hiệu ứng thở: fade in/out mượt
static void handleBreath(uint8_t r, uint8_t g, uint8_t b) {
    unsigned long phase = millis() % BREATH_PERIOD;
    // 0 → period/2: sáng dần, period/2 → period: tối dần
    float ratio;
    if (phase < BREATH_PERIOD / 2) {
        ratio = (float)phase / (BREATH_PERIOD / 2);
    } else {
        ratio = 1.0f - (float)(phase - BREATH_PERIOD / 2) / (BREATH_PERIOD / 2);
    }
    // Áp dụng gamma đơn giản cho mượt hơn
    ratio = ratio * ratio;
    uint8_t rr = (uint8_t)(r * ratio);
    uint8_t gg = (uint8_t)(g * ratio);
    uint8_t bb = (uint8_t)(b * ratio);
    strip.setPixelColor(0, strip.Color(rr, gg, bb));
    strip.show();
}

// ============================================================
// API công khai
// ============================================================

void led_init() {
    strip.begin();
    strip.setBrightness(LED_BRIGHTNESS);
    setOff();
}

void led_set_state(LedState state) {
    if (state != currentState) {
        currentState = state;
        lastUpdate = 0;
        blinkOn = false;
        needsRefresh = true;
    }
}

LedState led_get_state() {
    return currentState;
}

void led_flash(uint8_t r, uint8_t g, uint8_t b, uint8_t count) {
    flashR = r; flashG = g; flashB = b;
    flashSteps = count * 2;
    flashTimer = millis();
    setColor(r, g, b);
}

void led_update() {
    // Flash overlay — tạm thời override trạng thái
    if (flashSteps > 0) {
        if (millis() - flashTimer >= 100) {
            flashTimer = millis();
            flashSteps--;
            if (flashSteps == 0) {
                needsRefresh = true;
            } else if (flashSteps & 1) {
                setColor(flashR, flashG, flashB);
            } else {
                setOff();
            }
        }
        return;
    }

    switch (currentState) {
        case LedState::BOOTING:
            handleBlink(COLOR_YELLOW, BLINK_FAST);
            break;

        case LedState::WIFI_CONNECTING:
            handleBlink(COLOR_BLUE, BLINK_SLOW);
            break;

        case LedState::MQTT_CONNECTING:
            handleBreath(COLOR_BLUE);
            break;

        case LedState::ONLINE_OK:
            if (needsRefresh) { setColor(COLOR_GREEN); needsRefresh = false; }
            break;

        case LedState::ONLINE_SENSOR_WARN:
            handleBlink(COLOR_GREEN, BLINK_1S);
            break;

        case LedState::OFFLINE_BUFFERING:
            handleBlink(COLOR_ORANGE, BLINK_1S);
            break;

        case LedState::ERROR_CRITICAL:
            if (needsRefresh) { setColor(COLOR_RED); needsRefresh = false; }
            break;

        case LedState::CONFIG_AP_MODE:
            handleBreath(COLOR_PURPLE);
            break;
    }
}

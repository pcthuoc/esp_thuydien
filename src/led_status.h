#pragma once
#include <Arduino.h>

// --- Trạng thái LED (theo thứ tự ưu tiên cao → thấp) ---
enum class LedState : uint8_t {
    BOOTING,            // Vàng nhấp nháy nhanh (200ms)
    WIFI_CONNECTING,    // Xanh dương nhấp nháy chậm (500ms)
    MQTT_CONNECTING,    // Xanh dương thở (fade in/out)
    ONLINE_OK,          // Xanh lá sáng liên tục
    ONLINE_SENSOR_WARN, // Xanh lá nhấp nháy chậm (1s)
    OFFLINE_BUFFERING,  // Cam nhấp nháy chậm (1s)
    ERROR_CRITICAL,     // Đỏ sáng liên tục
    CONFIG_AP_MODE      // Tím thở (fade in/out)
};

void led_init();
void led_set_state(LedState state);
LedState led_get_state();
void led_update();  // Gọi trong loop hoặc task, xử lý hiệu ứng

// Flash tạm thời: count = số lần chớp, 100ms/pha, tự khôi phục trạng thái
void led_flash(uint8_t r, uint8_t g, uint8_t b, uint8_t count);

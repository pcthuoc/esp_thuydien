#pragma once

// ============================================================
// Bật/tắt log từng module — 1 = bật, 0 = tắt
// ============================================================
#define LOG_MBTCP     0   // Modbus TCP
#define LOG_MODBUS    1   // Modbus RTU
#define LOG_ADS       0   // Analog (ADS1115)
#define LOG_DATA      0   // Data collector / publish
#define LOG_MQTT      0   // MQTT client
#define LOG_OTA       0   // OTA update
#define LOG_COUNTER   0   // Encoder / counter
#define LOG_SD        1   // SD card
#define LOG_WEB       0   // Web server
#define LOG_NTP       1   // NTP / RTC

// ============================================================
// Macro helper — dùng thay cho Serial.printf / Serial.println
// ============================================================
#define LOG_IF(enabled, ...)  do { if (enabled) Serial.printf(__VA_ARGS__); } while(0)
#define LOGLN_IF(enabled, s)  do { if (enabled) Serial.println(s); } while(0)

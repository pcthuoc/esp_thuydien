#pragma once
#include <Arduino.h>

// Init: load calc config từ SD, reset accumulators
// use4G: true = backfillTask pin Core 1 (tránh race TinyGsmClient UART)
//        false = WiFi, backfillTask chạy bất kỳ core (không có race UART)
void data_collector_init(bool use4G = false);

// Gọi trong loop() — poll sensor + publish theo chu kỳ
void data_collector_update();

// Reload calc config (gọi khi server gửi config mới qua MQTT)
void data_collector_reload_calc();

// Bật/tắt debug mode (biến động, không lưu SD)
void data_collector_set_debug(bool on);

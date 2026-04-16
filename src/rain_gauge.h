#pragma once
#include <Arduino.h>

// Init DI4 (cảm biến mưa tipping bucket) — GPIO40, optocoupler PC817
void rain_init();

// Gọi trong loop() — tự reset lúc 0h00
void rain_update();

// Lấy số xung tích lũy trong ngày (cộng dồn, tự reset 0h00)
uint32_t rain_get_count();

// Reset thủ công (lệnh từ server)
void rain_reset();

// In trạng thái ra Serial (dùng debug)
void rain_debug_print();

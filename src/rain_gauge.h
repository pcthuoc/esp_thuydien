#pragma once
#include <Arduino.h>

// Init DI1 (cảm biến mưa tipping bucket) — GPIO1, optocoupler PC817
void rain_init();

// Gọi trong loop() — tự reset lúc 0h00
void rain_update();

// Lấy số xung tích lũy trong ngày (cộng dồn, tự reset 0h00)
uint32_t rain_get_count();

// Reset thủ công (lệnh từ server)
void rain_reset();

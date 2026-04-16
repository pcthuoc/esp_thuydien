#pragma once
#include <Arduino.h>

#define COUNTER_CHANNELS  1   // EN1 (EN2 tạm chưa dùng)

// Init PCNT quadrature counter trên DI1=IO1 (A) + DI2=IO2 (B)
// Autonics ENC-1-4-T-24, qua optocoupler PC817 (logic đảo)
void counter_init();

// Lấy giá trị đếm quadrature (signed: dương=thuận, âm=ngược)
int16_t counter_get(uint8_t ch);   // ch: 0 = EN1

// Reset counter về 0
void counter_reset(uint8_t ch);

// Gọi trong loop() — xử lý reset từ DI3 (ISR flag)
void counter_update();

// In trạng thái ra Serial (dùng debug)
void counter_debug_print();

#pragma once
#include <Arduino.h>

#define COUNTER_CHANNELS  2   // END1, END2

// Init PCNT hardware counter trên GPIO47, GPIO48
void counter_init();

// Lấy giá trị đếm (16-bit, 0-65535)
uint16_t counter_get(uint8_t ch);  // ch: 0 = END1, 1 = END2

// Reset counter về 0
void counter_reset(uint8_t ch);

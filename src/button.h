#pragma once
#include <Arduino.h>

#define BUTTON_PIN  45

// Callback types
typedef void (*ButtonClickCallback)();
typedef void (*ButtonLongPressCallback)();

void button_init();
void button_update();  // Gọi trong loop

// Đăng ký callback
void button_on_click(ButtonClickCallback cb);
void button_on_long_press(ButtonLongPressCallback cb);  // Giữ > 3s

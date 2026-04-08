#pragma once
#include <Arduino.h>

// Khởi tạo web server AP mode
// ap_ssid: tên WiFi AP, ap_pass: mật khẩu (>= 8 ký tự)
void webserver_init(const char* ap_ssid, const char* ap_pass);

// Tắt web server và AP
void webserver_stop();

// Kiểm tra AP đang chạy
bool webserver_is_running();

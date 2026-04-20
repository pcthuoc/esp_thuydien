#pragma once
#include <Arduino.h>

// Khởi tạo web server AP mode
// ap_ssid: tên WiFi AP, ap_pass: mật khẩu (>= 8 ký tự)
void webserver_init(const char* ap_ssid, const char* ap_pass);

// Tắt web server và AP
void webserver_stop();

// Kiểm tra AP đang chạy
bool webserver_is_running();

// Callback được gọi sau khi lưu config thành công
// module: "rs485", "tcp", "analog", "encoder", "di", "network", "system"
typedef void (*WebConfigSavedCallback)(const char* module);
void webserver_set_config_saved_callback(WebConfigSavedCallback cb);

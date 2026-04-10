#pragma once
#include <Arduino.h>

// Nhận lệnh OTA — set pending flag, xử lý ở ota_loop()
void ota_start(const String& url, const String& checksum);

// Gọi trong loop() — chạy OTA nếu đang pending
void ota_loop();

// OTA đang chạy?
bool ota_in_progress();

// Trả true 1 lần sau khi reboot từ OTA thành công, rồi xóa flag
bool ota_check_just_updated();

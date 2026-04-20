#ifndef NTP_RTC_H
#define NTP_RTC_H

#include <Arduino.h>

// Init I2C bus (SDA=5, SCL=4) — gọi sớm, dùng chung cho RTC + ADS1115
void i2c_init();

// Init RTC (dự phòng) — đọc DS3231 set clock tạm khi chưa có NTP
bool ntp_rtc_init();

// Sync NTP (cần WiFi) — nguồn thời gian chính, nếu OK ghi vào RTC
bool ntp_rtc_sync_ntp();

// Gọi trong loop() — tự sync NTP định kỳ
void ntp_rtc_update();

// Reset timer để ntp_rtc_update() sync sớm (sau reconnect) — KHÔNG block
void ntp_rtc_force_resync();

// Lấy epoch (seconds since 1970)
time_t ntp_rtc_get_epoch();

// Lấy chuỗi "YYYY-MM-DD HH:MM:SS"
String ntp_rtc_get_datetime();

// Thời gian hợp lệ? (năm > 2024)
bool ntp_rtc_is_valid();

// Ghi UTC hiện tại của system clock vào DS3231 (dùng sau khi sync 4G)
bool ntp_rtc_write_rtc();

#endif

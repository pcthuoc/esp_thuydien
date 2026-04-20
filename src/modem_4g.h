#pragma once
#include <Arduino.h>
#include <Client.h>

// ============================================================
// Modem 4G — A7680C (SIM7600 AT command set) via TinyGSM
// UART: TX = GPIO15 → SIM RXD
//       RX = GPIO16 ← SIM TXD
// ============================================================

// Khởi tạo modem: connect GPRS, sẵn sàng gửi data
// apn : chuỗi APN (vd "v-internet")
// pin : SIM PIN, hoặc nullptr nếu không có
bool modem_4g_init(const char* apn, const char* pin = nullptr);

// Trạng thái kết nối GPRS
// Đọc volatile flag — KHÔNG gọi AT command, safe từ bất kỳ core nào.
// Flag được cập nhật bởi modem_4g_init(), modem_4g_reconnect(), và net4g_task.
bool modem_4g_is_connected();

// Cập nhật GPRS connection flag (gọi từ net4g_task sau khi reconnect thành công/thất bại)
void modem_4g_set_gprs_flag(bool connected);

// Thử reconnect khi mất kết nối (blocking AT — chỉ gọi từ net4g_task Core 0)
bool modem_4g_reconnect();

// Thông tin modem
int    modem_4g_rssi();
String modem_4g_operator();
String modem_4g_ip();

// Trả về TinyGsmClient* — dùng làm transport cho PubSubClient
Client* modem_4g_get_client();

// Sync system clock từ mạng 4G qua AT+CCLK? (không cần WiFi)
// Tự động set system time và log chi tiết để debug
bool modem_4g_sync_time();

// Đăng ký tick callback — được gọi mỗi 1 giây trong các vòng chờ blocking
// Dùng để gọi led_update() từ main trong lúc modem đang chờ
void modem_4g_set_tick_cb(void (*cb)());

// Yêu cầu hủy quá trình init/reconnect đang block (gọi từ task khác)
// Sau khi abort, modem_4g_init() / modem_4g_reconnect() trả về false ngay
void modem_4g_abort();

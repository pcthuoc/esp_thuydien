#pragma once
#include <Arduino.h>
#include <Client.h>

// ============================================================
// Modem 4G — A7680C via TinyGSM (SIM7600 AT set)
// UART: ESP RX = GPIO15 ← SIM TXD
//       ESP TX = GPIO16 → SIM RXD
// ============================================================

// Khởi tạo modem: connect GPRS, sẵn sàng gửi data
// apn : chuỗi APN (vd "v-internet")
// pin : SIM PIN, hoặc nullptr nếu không có
bool modem_4g_init(const char* apn, const char* pin = nullptr);

// Trạng thái kết nối GPRS
bool modem_4g_is_connected();

// Thử reconnect khi mất kết nối
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

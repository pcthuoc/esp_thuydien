#pragma once
#include <Arduino.h>

// ============================================================
// net4g_task — Core 0, priority 2
//
// Owns ALL Serial1 / TinyGsm / PubSubClient operations in 4G mode.
// Core 1 NEVER touches Serial1 after net4g_task_start() is called.
//
// Luồng nội bộ (Core 0):
//   1. GPRS: kiểm tra s_gprsFlag — nếu mất → modem_4g_reconnect()
//   2. MQTT: nếu không connected → mqtt4g.connect()
//   3. mqtt4g.loop() mỗi 10ms — nhận msg, gửi PING keepalive
//   4. publish queue: nhận yêu cầu từ Core 1 → execute → trả kết quả
//   5. time sync: định kỳ gọi modem_4g_sync_time()
//
// Core 1 giao tiếp qua:
//   - net4g_mqtt_connected()  → đọc volatile flag, không AT
//   - net4g_just_connected()  → edge-triggered flag (true 1 lần/kết nối)
//   - net4g_publish()         → post vào queue, chờ kết quả
//   - net4g_poll_rx()         → poll rx queue (non-blocking)
// ============================================================

// Received MQTT message — forwarded from Core 0 → Core 1 via queue
struct MqttRxMsg {
    char         topic[64];
    byte         payload[4096];
    unsigned int len;
};

// Khởi động net4g_task trên Core 0.
// Gọi SAU khi modem_4g_init() thành công.
// Đọc broker/port/deviceId/pass từ /config/network.json.
// rxCb: callback được gọi trên Core 1 khi nhận MQTT message.
void net4g_task_start(
    void (*rxCb)(const char* topic, const byte* payload, unsigned int len));

// True nếu net4g_task đã được khởi động
bool net4g_is_active();

// Đọc volatile flag — không gọi AT command, safe từ bất kỳ core nào
bool net4g_mqtt_connected();

// Trả true MỘT LẦN sau mỗi lần kết nối MQTT thành công (edge-triggered)
bool net4g_just_connected();

// Thread-safe publish — post lên queue, chờ kết quả (tối đa 2s)
// Trả false nếu không connected hoặc timeout
bool net4g_publish(const char* topic, const String& payload, bool retain = false);
bool net4g_publish_large(const char* topic, const String& payload);

// Non-blocking: lấy 1 message từ rx queue (trả false nếu rỗng)
bool net4g_poll_rx(MqttRxMsg* out);

// Yêu cầu dừng reconnect (AP mode)
void net4g_abort();

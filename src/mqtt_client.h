#pragma once
#include <Arduino.h>
#include <Client.h>

// Khởi tạo MQTT client từ config trên SD (/config/network.json)
// Trả false nếu thiếu config
bool mqtt_init();

// Đặt transport client (WiFiClient hoặc TinyGsmClient)
// Phải gọi trước mqtt_init()
void mqtt_set_client(Client* client);

// Chuyển transport (WiFi↔4G hoặc ngược lại) trong khi đang chạy.
// client=nullptr → dùng WiFiClient mặc định.
// MQTT sẽ tự động reconnect trong mqtt_update().
void mqtt_switch_transport(Client* client);

// Gọi trong loop() — xử lý reconnect + keep-alive
void mqtt_update();

// Trạng thái kết nối
bool mqtt_is_connected();

// Publish data lên topic station/{device_id}/data
bool mqtt_publish_data(const String& json);

// Publish status lên topic station/{device_id}/status
bool mqtt_publish_status(const String& json);

// Publish config lên topic station/{device_id}/config
bool mqtt_publish_config(const String& json);

// Set callback khi server gửi config mới (module-aware)
// module: "rs485", "tcp", "analog", "encoder", "di", "network", "system"
void mqtt_set_config_callback(void (*cb)(const char* module));

// [DEPRECATED] callback không có tham số — giữ lại để tương thích
void mqtt_set_config_callback_simple(void (*cb)());

// Gọi mqtt.loop() để giữ kết nối (dùng trong OTA)
void mqtt_keep_alive();

// Forward received message vào callback chain (topic/payload từ net4g rx queue)
// Gọi trên Core 1 từ mqtt_update() — dùng bởi net4g_task_start()
void mqttCallback_public(const char* topic, const byte* payload, unsigned int len);

#pragma once
#include <Arduino.h>

// Khởi tạo MQTT client từ config trên SD (/config/network.json)
// Trả false nếu thiếu config
bool mqtt_init();

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

// Set callback khi server gửi config mới (để reload calc config)
void mqtt_set_config_callback(void (*cb)());

// Gọi mqtt.loop() để giữ kết nối (dùng trong OTA)
void mqtt_keep_alive();

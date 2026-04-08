#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "led_status.h"
#include "sd_card.h"
#include "button.h"
#include "webserver.h"
#include "mqtt_client.h"
#include "ntp_rtc.h"
#include "analog_reader.h"
#include "counter.h"
#include "rain_gauge.h"
#include "modbus_rtu.h"
#include "modbus_tcp.h"
#include "data_collector.h"

// --- WiFi reconnect ---
static bool wifiConfigured = false;    // có config WiFi trên SD
static bool mqttInited     = false;    // đã gọi mqtt_init() thành công
static unsigned long lastWifiCheck = 0;
static const unsigned long WIFI_CHECK_INTERVAL = 10000;  // 10s

// --- WiFi STA auto-connect ---
bool wifi_connect_from_config() {
    String json = sd_read_file("/config/network.json");
    if (json.length() == 0) {
        Serial.println("[WIFI] Không tìm thấy config network");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        Serial.println("[WIFI] Config JSON lỗi");
        return false;
    }

    const char* ssid = doc["wifi_ssid"];
    const char* pass = doc["wifi_pass"];
    if (!ssid || strlen(ssid) == 0) {
        Serial.println("[WIFI] SSID trống");
        return false;
    }

    Serial.printf("[WIFI] Kết nối: %s\n", ssid);
    led_set_state(LedState::WIFI_CONNECTING);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    int timeout = 20; // 20 x 500ms = 10s
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
        delay(500);
        Serial.print(".");
        led_update();
        button_update();
        timeout--;
        // Nếu user bấm giữ nút → vào AP mode, dừng kết nối WiFi
        if (webserver_is_running()) {
            Serial.println("\n[WIFI] AP mode activated, abort WiFi connect");
            WiFi.disconnect();
            return false;
        }
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] OK! IP: %s\n", WiFi.localIP().toString().c_str());
        WiFi.setAutoReconnect(true);  // ESP32 tự reconnect ở tầng thấp
        return true;
    } else {
        Serial.println("[WIFI] Kết nối thất bại");
        WiFi.disconnect();
        return false;
    }
}

// --- Button callbacks ---
void onButtonClick() {
    Serial.println("[BTN] Click!");
}

void onButtonLongPress() {
    Serial.println("[BTN] Long press (3s) -> AP MODE!");
    if (!webserver_is_running()) {
        // Đọc AP config từ SD
        String ap_ssid = "THUYDIEN_CFG";
        String ap_pass = "12345678";

        String json = sd_read_file("/config/system.json");
        if (json.length() > 0) {
            JsonDocument doc;
            if (!deserializeJson(doc, json)) {
                if (doc["ap_ssid"].is<const char*>() && strlen(doc["ap_ssid"]) > 0)
                    ap_ssid = doc["ap_ssid"].as<String>();
                if (doc["ap_pass"].is<const char*>() && strlen(doc["ap_pass"]) >= 8)
                    ap_pass = doc["ap_pass"].as<String>();
            }
        }

        Serial.printf("[AP] SSID: %s\n", ap_ssid.c_str());
        led_set_state(LedState::CONFIG_AP_MODE);
        webserver_init(ap_ssid.c_str(), ap_pass.c_str());
    } else {
        Serial.println("[BTN] Web server already running");
    }
}

void printHelp() {
    Serial.println("=== Test Commands ===");
    Serial.println("--- LED ---");
    Serial.println("  0 - BOOTING (vàng nhấp nháy nhanh)");
    Serial.println("  1 - WIFI_CONNECTING (xanh dương nhấp nháy)");
    Serial.println("  2 - MQTT_CONNECTING (xanh dương thở)");
    Serial.println("  3 - ONLINE_OK (xanh lá sáng)");
    Serial.println("  4 - ONLINE_SENSOR_WARN (xanh lá nhấp nháy)");
    Serial.println("  5 - OFFLINE_BUFFERING (cam nhấp nháy)");
    Serial.println("  6 - ERROR_CRITICAL (đỏ sáng)");
    Serial.println("  7 - CONFIG_AP_MODE (tím thở)");
    Serial.println("--- SD Card ---");
    Serial.println("  s - Kiểm tra SD card");
    Serial.println("  w - Ghi file test");
    Serial.println("  r - Đọc file test");
    Serial.println("  d - Xóa file test");
    Serial.println("---");
    Serial.println("  h - Hiển thị menu này");
    Serial.println("=====================");
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Ngắt WiFi cũ nhưng KHÔNG xóa NVS (cần cho reconnect)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, false);  // disconnect + giữ NVS credentials
    delay(100);

    led_init();
    led_set_state(LedState::BOOTING);

    Serial.println("\n[BOOT] Khởi động...");

    // Init SD card
    if (sd_init()) {
        Serial.println("[BOOT] SD card OK");
    } else {
        Serial.println("[BOOT] SD card FAIL");
    }

    // Init Button
    button_init();
    button_on_click(onButtonClick);
    button_on_long_press(onButtonLongPress);
    Serial.println("[BOOT] Button OK (IO45)");

    // Init I2C bus (dùng chung cho RTC + ADS1115)
    i2c_init();

    // Init RTC dự phòng (set clock tạm, sẽ bị NTP ghi đè khi có mạng)
    if (ntp_rtc_init()) {
        Serial.printf("[BOOT] RTC backup: %s\n", ntp_rtc_get_datetime().c_str());
    } else {
        Serial.println("[BOOT] RTC not available");
    }

    // Init Analog (ADS1115)
    if (analog_init()) {
        Serial.println("[BOOT] Analog OK");
    } else {
        Serial.println("[BOOT] Analog: no ADS found");
    }

    // Init Counter (PCNT)
    counter_init();
    Serial.println("[BOOT] Counter OK (E1=GPIO47, E2=GPIO48)");

    // Init Rain gauge (DI1)
    rain_init();
    Serial.println("[BOOT] Rain gauge OK (DI1=GPIO1)");

    // Auto-connect WiFi từ config đã lưu
    if (wifi_connect_from_config()) {
        wifiConfigured = true;
        Serial.println("[BOOT] WiFi connected!");

        // Sync NTP (nguồn thời gian chính)
        if (ntp_rtc_sync_ntp()) {
            Serial.printf("[BOOT] NTP OK: %s\n", ntp_rtc_get_datetime().c_str());
        }

        // Init MQTT
        if (mqtt_init()) {
            mqttInited = true;
            led_set_state(LedState::MQTT_CONNECTING);
            Serial.println("[BOOT] MQTT init OK, connecting...");
        } else {
            led_set_state(LedState::ONLINE_OK);
            Serial.println("[BOOT] MQTT chưa cấu hình");
        }
    } else {
        // Kiểm tra xem có config nhưng kết nối thất bại hay hoàn toàn không có config
        String netJson = sd_read_file("/config/network.json");
        if (netJson.length() > 0) {
            JsonDocument tmpDoc;
            if (!deserializeJson(tmpDoc, netJson)) {
                const char* ssid = tmpDoc["wifi_ssid"];
                if (ssid && strlen(ssid) > 0) {
                    wifiConfigured = true;  // có config → sẽ thử reconnect
                    Serial.println("[BOOT] WiFi config found, will retry in loop");
                }
            }
        }
        led_set_state(LedState::OFFLINE_BUFFERING);
        Serial.println("[BOOT] WiFi chưa cấu hình hoặc kết nối thất bại");
        Serial.println("[BOOT] Nhấn giữ nút 3s để vào AP mode cấu hình");
    }

    // Init Modbus RTU
    if (modbus_rtu_init()) {
        Serial.printf("[BOOT] Modbus RTU OK (%d channels)\n", modbus_rtu_channel_count());
    } else {
        Serial.println("[BOOT] Modbus RTU: no config");
    }

    // Init Modbus TCP (W5500)
    if (modbus_tcp_init()) {
        Serial.printf("[BOOT] Modbus TCP OK (%d channels)\n", modbus_tcp_channel_count());
    } else {
        Serial.println("[BOOT] Modbus TCP: no config or W5500 not found");
    }

    // MQTT config callback → data_collector reload
    mqtt_set_config_callback(data_collector_reload_calc);

    // Init Data Collector
    data_collector_init();
    Serial.println("[BOOT] Data collector OK");

    printHelp();
}

void loop() {
    led_update();
    button_update();

    // AP mode → dừng mọi hoạt động đọc dữ liệu và gửi
    if (webserver_is_running()) return;

    mqtt_update();
    ntp_rtc_update();
    rain_update();
    data_collector_update();

    // --- WiFi auto-reconnect ---
    if (wifiConfigured && !webserver_is_running()) {
        unsigned long now = millis();
        if (now - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
            lastWifiCheck = now;
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[WIFI] Disconnected! Reconnecting...");
                led_set_state(LedState::WIFI_CONNECTING);
                WiFi.reconnect();

                // Đợi tối đa 10s
                int timeout = 20;
                while (WiFi.status() != WL_CONNECTED && timeout > 0) {
                    delay(500);
                    led_update();
                    button_update();
                    timeout--;
                    if (webserver_is_running()) break;
                }

                if (WiFi.status() == WL_CONNECTED) {
                    Serial.printf("[WIFI] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());

                    // Init MQTT lần đầu nếu chưa
                    if (!mqttInited) {
                        if (mqtt_init()) {
                            mqttInited = true;
                            Serial.println("[WIFI] MQTT init OK after reconnect");
                        }
                    }
                    // NTP sync lại
                    ntp_rtc_sync_ntp();
                    led_set_state(LedState::MQTT_CONNECTING);
                } else {
                    Serial.println("[WIFI] Reconnect failed, will retry...");
                    led_set_state(LedState::OFFLINE_BUFFERING);
                }
            }
        }
    }

    // if (Serial.available()) {
    //     char c = Serial.read();
    //     switch (c) {
    //         // LED test
    //         case '0': led_set_state(LedState::BOOTING);            Serial.println(">> BOOTING");            break;
    //         case '1': led_set_state(LedState::WIFI_CONNECTING);     Serial.println(">> WIFI_CONNECTING");     break;
    //         case '2': led_set_state(LedState::MQTT_CONNECTING);     Serial.println(">> MQTT_CONNECTING");     break;
    //         case '3': led_set_state(LedState::ONLINE_OK);           Serial.println(">> ONLINE_OK");           break;
    //         case '4': led_set_state(LedState::ONLINE_SENSOR_WARN);  Serial.println(">> ONLINE_SENSOR_WARN");  break;
    //         case '5': led_set_state(LedState::OFFLINE_BUFFERING);   Serial.println(">> OFFLINE_BUFFERING");   break;
    //         case '6': led_set_state(LedState::ERROR_CRITICAL);      Serial.println(">> ERROR_CRITICAL");      break;
    //         case '7': led_set_state(LedState::CONFIG_AP_MODE);      Serial.println(">> CONFIG_AP_MODE");      break;

    //         // SD card test
    //         case 's': {
    //             Serial.printf("[SD] Inserted: %s\n", sd_is_inserted() ? "YES" : "NO");
    //             Serial.printf("[SD] Total: %llu MB\n", sd_total_bytes() / (1024 * 1024));
    //             Serial.printf("[SD] Used:  %llu MB\n", sd_used_bytes() / (1024 * 1024));
    //             break;
    //         }
    //         case 'w': {
    //             if (sd_write_file("/test.txt", "Hello from ESP32-S3!\n")) {
    //                 Serial.println("[SD] Write OK: /test.txt");
    //             } else {
    //                 Serial.println("[SD] Write FAILED");
    //             }
    //             break;
    //         }
    //         case 'r': {
    //             String content = sd_read_file("/test.txt");
    //             if (content.length() > 0) {
    //                 Serial.printf("[SD] Read: %s", content.c_str());
    //             } else {
    //                 Serial.println("[SD] Read FAILED or empty");
    //             }
    //             break;
    //         }
    //         case 'd': {
    //             if (sd_remove("/test.txt")) {
    //                 Serial.println("[SD] Deleted /test.txt");
    //             } else {
    //                 Serial.println("[SD] Delete FAILED");
    //             }
    //             break;
    //         }

    //         case 'h': case 'H': printHelp(); break;
    //         default: break;
    //     }
    // }



  }
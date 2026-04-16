#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "version.h"
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
#include "ota_update.h"
#include "error_log.h"
#include "modem_4g.h"

// --- Network mode ---
static bool mode4G         = false;   // true = dùng 4G, false = dùng WiFi
static bool wifiConfigured = false;   // có SSID trong config
static bool modem4gInited  = false;   // modem_4g_init() thành công
static bool mqttInited     = false;   // mqtt_init() đã được gọi

// --- Network check timer ---
static unsigned long lastNetCheck = 0;
static const unsigned long NET_CHECK_INTERVAL = 15000;  // 15s

// --- 4G time sync ---
static unsigned long last4gTimeSync = 0;
static const unsigned long SYNC_4G_OK_INTERVAL   = 3600000UL;
static const unsigned long SYNC_4G_FAIL_INTERVAL  = 60000UL;
static bool timeSynced4g = false;

// --- WiFi connect helper ---
static bool wifi_connect_from_config() {
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

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Ngắt WiFi cũ nhưng KHÔNG xóa NVS (cần cho reconnect)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, false);  // disconnect + giữ NVS credentials
    delay(100);

    led_init();
    led_set_state(LedState::BOOTING);

    // Init SD trước — để err_log ghi được từ đầu
    if (sd_init()) {
        Serial.println("[BOOT] SD card OK");
    } else {
        Serial.println("[BOOT] SD card FAIL");
    }

    Serial.println("\n[BOOT] Khởi động...");
    Serial.println("[BOOT] Firmware version: " FW_VERSION);
    err_log("BOOT", "Device started, fw=" FW_VERSION);

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

    // Đọc config mạng
    String netModeStr = "wifi";
    String simApn = "", simPin = "";
    {
        String nj = sd_read_file("/config/network.json");
        if (nj.length() > 0) {
            JsonDocument nd;
            if (!deserializeJson(nd, nj)) {
                netModeStr = nd["net_mode"] | "wifi";
                simApn     = nd["apn"]      | "";
                simPin     = nd["sim_pin"]  | "";
                const char* s = nd["wifi_ssid"];
                if (s && strlen(s) > 0) wifiConfigured = true;
            }
        }
    }
    mode4G = (netModeStr == "4g");
    Serial.printf("[BOOT] Net mode: %s\n", netModeStr.c_str());

    if (mode4G) {
        // --- 4G only ---
        led_set_state(LedState::WIFI_CONNECTING);
        const char* pin4g = (simPin.length() > 0) ? simPin.c_str() : nullptr;
        if (modem_4g_init(simApn.c_str(), pin4g)) {
            modem4gInited = true;
            timeSynced4g = modem_4g_sync_time();
            if (timeSynced4g) {
                ntp_rtc_write_rtc();
                Serial.printf("[BOOT] 4G time OK: %s\n", ntp_rtc_get_datetime().c_str());
            } else {
                Serial.println("[BOOT] 4G time sync FAILED — dùng RTC dự phòng");
            }
            last4gTimeSync = millis();
            mqtt_set_client(modem_4g_get_client());
            if (mqtt_init()) {
                mqttInited = true;
                led_set_state(LedState::MQTT_CONNECTING);
                Serial.println("[BOOT] MQTT init OK (4G)");
            } else {
                led_set_state(LedState::ONLINE_OK);
                Serial.println("[BOOT] MQTT chưa cấu hình");
            }
        } else {
            led_set_state(LedState::OFFLINE_BUFFERING);
            Serial.println("[BOOT] 4G khởi động thất bại");
        }
    } else {
        // --- WiFi only ---
        if (wifi_connect_from_config()) {
            Serial.println("[BOOT] WiFi connected!");
            ntp_rtc_sync_ntp();
            if (mqtt_init()) {
                mqttInited = true;
                led_set_state(LedState::MQTT_CONNECTING);
                Serial.println("[BOOT] MQTT init OK (WiFi)");
            } else {
                led_set_state(LedState::ONLINE_OK);
                Serial.println("[BOOT] MQTT chưa cấu hình");
            }
        } else {
            led_set_state(LedState::OFFLINE_BUFFERING);
            Serial.println("[BOOT] WiFi thất bại — offline mode");
            Serial.println("[BOOT] Nhấn giữ nút 3s để vào AP mode");
        }
    }

    // Init Modbus RTU
    if (modbus_rtu_init()) {
        Serial.printf("[BOOT] Modbus RTU OK (%d channels)\n", modbus_rtu_channel_count());
    } else {
        Serial.println("[BOOT] Modbus RTU: no config");
    }

    // Init W5500 hardware (luôn gọi, không phụ thuộc config)
    if (w5500_init()) {
        Serial.println("[BOOT] W5500 OK");
    } else {
        Serial.println("[BOOT] W5500 not found");
    }

    // Load Modbus TCP channels từ SD (cần W5500 đã init trước)
    if (modbus_tcp_init()) {
        Serial.printf("[BOOT] Modbus TCP OK (%d channels)\n", modbus_tcp_channel_count());
    } else {
        Serial.println("[BOOT] Modbus TCP: no config");
    }

    // MQTT config callback → data_collector reload
    mqtt_set_config_callback(data_collector_reload_calc);

    // Init Data Collector
    data_collector_init(mode4G);
    Serial.println("[BOOT] Data collector OK");
}

void loop() {
    led_update();
    button_update();
    counter_update();

    // AP mode → ưu tiên LED + dừng mọi hoạt động đọc dữ liệu và gửi
    if (webserver_is_running()) {
        if (led_get_state() != LedState::CONFIG_AP_MODE)
            led_set_state(LedState::CONFIG_AP_MODE);
        return;
    }

    mqtt_update();
    ota_loop();

    // OTA đang chạy → dừng đọc sensor, chỉ giữ MQTT
    if (ota_in_progress()) return;

    // NTP sync định kỳ (WiFi only)
    if (!mode4G) ntp_rtc_update();
    rain_update();
    data_collector_update();

    if (webserver_is_running()) return;

    unsigned long now = millis();
    if (now - lastNetCheck >= NET_CHECK_INTERVAL) {
        lastNetCheck = now;

        if (!mode4G) {
            // --- WiFi reconnect ---
            if (WiFi.status() != WL_CONNECTED && wifiConfigured) {
                Serial.println("[WIFI] Mất kết nối, reconnect...");
                err_log("WIFI", "Disconnected — reconnecting");
                led_set_state(LedState::WIFI_CONNECTING);
                WiFi.reconnect();
                int t = 20;
                while (WiFi.status() != WL_CONNECTED && t-- > 0) {
                    delay(500); led_update(); button_update();
                }
                if (WiFi.status() == WL_CONNECTED) {
                    Serial.printf("[WIFI] OK: %s\n", WiFi.localIP().toString().c_str());
                    err_log("WIFI", "Reconnected");
                    ntp_rtc_sync_ntp();
                    led_set_state(LedState::MQTT_CONNECTING);
                } else {
                    Serial.println("[WIFI] Reconnect thất bại, thử lại sau...");
                    led_set_state(LedState::OFFLINE_BUFFERING);
                }
            }
        } else {
            // --- 4G reconnect ---
            if (modem4gInited && !modem_4g_is_connected()) {
                Serial.println("[4G] Mất kết nối, reconnect...");
                err_log("4G", "Connection lost — reconnecting");
                led_set_state(LedState::WIFI_CONNECTING);
                if (modem_4g_reconnect()) {
                    Serial.println("[4G] Reconnected!");
                    timeSynced4g = modem_4g_sync_time();
                    if (timeSynced4g) ntp_rtc_write_rtc();
                    last4gTimeSync = millis();
                    led_set_state(LedState::MQTT_CONNECTING);
                } else {
                    led_set_state(LedState::OFFLINE_BUFFERING);
                }
            }

            // Periodic 4G time re-sync
            unsigned long syncInterval = timeSynced4g ? SYNC_4G_OK_INTERVAL : SYNC_4G_FAIL_INTERVAL;
            if (millis() - last4gTimeSync >= syncInterval) {
                timeSynced4g = modem_4g_sync_time();
                if (timeSynced4g) ntp_rtc_write_rtc();
                last4gTimeSync = millis();
            }
        }
    }
}
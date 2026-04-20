#include <Arduino.h>
#include <WiFi.h>
#include <lwip/dns.h>
#include <ArduinoJson.h>
#include "version.h"
#include "led_status.h"
#include "sd_card.h"
#include "button.h"
#include "webserver.h"
#include "mqtt_client.h"
#include "net4g_task.h"
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

// --- AP mode request flag (set từ btn_task Core 0, xử lý ở loop() Core 1) ---
static volatile bool s_apModeRequest = false;

// --- WiFi reconnect flag (set từ wifi_task Core 0, xử lý ở loop() Core 1) ---
static volatile bool s_wifiJustReconnected = false;

// --- Network check timer (chỉ dùng cho cần thiết, không block) ---
static unsigned long lastNetCheck = 0;
static const unsigned long NET_CHECK_INTERVAL = 15000;  // 15s

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
        timeout--;
        // Nếu user bấm giữ nút → vào AP mode, dừng kết nối WiFi ngay
        if (s_apModeRequest || webserver_is_running()) {
            Serial.println("\n[WIFI] AP mode requested, abort WiFi connect");
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

// --- Config saved callback (từ web AP mode) ---
static void onWebConfigSaved(const char* module) {
    Serial.printf("[WEB] Config saved: %s → reload...\n", module);
    if (strcmp(module, "rs485") == 0) {
        modbus_rtu_init();
        Serial.println("[WEB] Modbus RTU reloaded");
    } else if (strcmp(module, "tcp") == 0) {
        modbus_tcp_init();
        Serial.println("[WEB] Modbus TCP reloaded");
    } else if (strcmp(module, "analog") == 0) {
        analog_init();
        Serial.println("[WEB] Analog reloaded");
    } else if (strcmp(module, "encoder") == 0 || strcmp(module, "di") == 0) {
        // counter/rain không có reload API, cần restart
        Serial.printf("[WEB] %s: cần restart để áp dụng\n", module);
    }
    // data_collector reload calc (áp dụng cho mọi module)
    data_collector_reload_calc();
}

// --- Button callbacks ---
void onButtonClick() {
    Serial.println("[BTN] Click!");
}

// Callback này chạy trên Core 0 (btn_task, stack 2048 bytes)
// CHỈ set flag + abort modem — KHÔNG làm gì nặng (SD, JSON, WiFi, webserver)
void onButtonLongPress() {
    Serial.println("[BTN] Long press (3s) -> AP MODE requested");
    modem_4g_abort();       // an toàn: chỉ set volatile bool
    net4g_abort();          // dừng net4g reconnect nếu đang chạy
    s_apModeRequest = true; // loop() sẽ xử lý trên Core 1
}

// ============================================================
// wifi_task — Core 0, priority 2 (WiFi mode only)
//
// Phương án A: chuyển WiFi reconnect ra khỏi Core 1.
// WiFi.reconnect() an toàn từ bất kỳ core nào (lwIP thread-safe).
// delay() trong FreeRTOS task = vTaskDelay() → yield, KHÔNG block Core 1.
// ============================================================
static void wifi_reconnect_task(void* pv) {
    for (;;) {
        // Chờ 15s giữa các lần kiểm tra
        vTaskDelay(pdMS_TO_TICKS(15000));

        // 4G mode → task này không làm gì cả
        if (mode4G) continue;

        // AP mode đang chạy → không cần reconnect
        if (s_apModeRequest || webserver_is_running()) continue;
        if (!wifiConfigured) continue;

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WIFI] Mất kết nối, reconnect... (Core 0)");
            err_log("WIFI", "Disconnected — reconnecting");

            WiFi.reconnect();

            // Chờ tối đa 10s — delay() ở đây = vTaskDelay(), Core 1 vẫn chạy bình thường
            int t = 20;
            while (WiFi.status() != WL_CONNECTED && t-- > 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
                if (s_apModeRequest) { WiFi.disconnect(false, false); break; }
            }

            if (WiFi.status() == WL_CONNECTED) {
                // Set DNS (lwIP thread-safe)
                ip_addr_t dns1 = IPADDR4_INIT_BYTES(8, 8, 8, 8);
                ip_addr_t dns2 = IPADDR4_INIT_BYTES(1, 1, 1, 1);
                dns_setserver(0, &dns1);
                dns_setserver(1, &dns2);
                Serial.printf("[WIFI] Reconnected: %s  DNS: 8.8.8.8\n",
                              WiFi.localIP().toString().c_str());
                err_log("WIFI", "Reconnected");
                // Báo Core 1 xử lý NTP resync + LED (không gọi từ đây vì không thread-safe)
                s_wifiJustReconnected = true;
            } else {
                Serial.println("[WIFI] Reconnect thất bại");
                err_log("WIFI", "Reconnect FAILED");
            }
        }
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

    // Chạy button_update() trên Core 0 độc lập — hoạt động ngay cả khi Core 1 đang block
    // trong modem_4g_init() (s_modem.restart() có thể block 10-30s)
    // Priority 4 > modbusTask(3): tránh starvation khi modbusTask busy-wait UART/TCP
    xTaskCreatePinnedToCore(
        [](void*) {
            for (;;) {
                button_update();
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        },
        "btn_task", 2048, nullptr, 4, nullptr, 0  // Core 0, prio 4 > modbusTask(3)
    );

    // wifi_reconnect_task khởi động sau — sẽ tự skip nếu mode4G hoặc !wifiConfigured
    // (mode4G chưa được set ở đây, nhưng task tự check wifiConfigured và WiFi.status())
    xTaskCreatePinnedToCore(
        wifi_reconnect_task, "wifi_task", 4096, nullptr, 2, nullptr, 0
        // Core 0, prio 2 — thấp hơn btn(4) và mb_poll(3); cao hơn loop(1)
    );

    // Đăng ký tick callback cho modem 4G — chỉ cần led_update()
    // (button_update() đã có task riêng)
    modem_4g_set_tick_cb([]() {
        led_update();
    });

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

    // Init Rain gauge (DI4)
    rain_init();
    Serial.println("[BOOT] Rain gauge OK (DI4=GPIO40)");

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
        if (!s_apModeRequest && modem_4g_init(simApn.c_str(), pin4g)) {
            modem4gInited = true;
            // Time sync ban đầu (blocking, chạy 1 lần trong setup — OK)
            bool timeSynced4g = modem_4g_sync_time();
            if (timeSynced4g) {
                ntp_rtc_write_rtc();
                Serial.printf("[BOOT] 4G time OK: %s\n", ntp_rtc_get_datetime().c_str());
            } else {
                Serial.println("[BOOT] 4G time sync FAILED — dùng RTC dự phòng");
            }
            // mqtt_init() chỉ đọc config SD — KHÔNG cần set client cho 4G nữa
            // (net4g_task sẽ tự tạo PubSubClient với TinyGsmClient)
            if (mqtt_init()) {
                mqttInited = true;
                led_set_state(LedState::MQTT_CONNECTING);
                Serial.println("[BOOT] MQTT init OK (4G)");
            } else {
                led_set_state(LedState::ONLINE_OK);
                Serial.println("[BOOT] MQTT chưa cấu hình");
            }
            // Khởi động net4g_task (Core 0, prio 2) — sở hữu Serial1 từ đây trở đi
            // mqttCallback_public: forward received messages về Core 1 qua net4g rx queue
            if (mqttInited) {
                net4g_task_start(mqttCallback_public);
                Serial.println("[BOOT] net4g_task started (Core 0)");
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
    // Ethernet_Generic dùng chung lwIP với WiFi → begin() có thể ghi đè DNS
    // Restore lại DNS sau w5500_init() để MQTT resolve được hostname
    {
        ip_addr_t dns1 = IPADDR4_INIT_BYTES(8, 8, 8, 8);
        ip_addr_t dns2 = IPADDR4_INIT_BYTES(1, 1, 1, 1);
        dns_setserver(0, &dns1);
        dns_setserver(1, &dns2);
    }

    // Load Modbus TCP channels từ SD (cần W5500 đã init trước)
    if (modbus_tcp_init()) {
        Serial.printf("[BOOT] Modbus TCP OK (%d channels)\n", modbus_tcp_channel_count());
    } else {
        Serial.println("[BOOT] Modbus TCP: no config");
    }

    // MQTT config callback → reload modbus + data_collector
    mqtt_set_config_callback(onWebConfigSaved);
    mqtt_set_config_callback_simple(data_collector_reload_calc);

    // Init Data Collector
    data_collector_init(mode4G);
    Serial.println("[BOOT] Data collector OK");
}

void loop() {
    led_update();
    // button_update() chỉ chạy trên Core 0 qua btn_task — KHÔNG gọi ở đây
    counter_update();

    // Xử lý AP mode request (set từ btn_task Core 0, thực hiện ở Core 1 với stack đầy đủ)
    if (s_apModeRequest) {
        s_apModeRequest = false;
        if (!webserver_is_running()) {
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
            webserver_set_config_saved_callback(onWebConfigSaved);
        } else {
            Serial.println("[BTN] Web server already running");
        }
    }

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

    // --- WiFi: xử lý kết quả từ wifi_reconnect_task (Core 0) ---
    // Không blocking — chỉ xử lý LED + NTP trigger khi wifi_task báo reconnected
    if (!mode4G && s_wifiJustReconnected) {
        s_wifiJustReconnected = false;
        ntp_rtc_force_resync();  // reset timer, ntp_rtc_update() sẽ sync sớm
        led_set_state(LedState::MQTT_CONNECTING);
        Serial.println("[WIFI] Reconnect detected → NTP resync triggered");
    }

    // --- LED: cập nhật trạng thái offline nếu mất mạng ---
    unsigned long now = millis();
    if (now - lastNetCheck >= NET_CHECK_INTERVAL) {
        lastNetCheck = now;

        if (!mode4G) {
            // WiFi: reconnect đã được xử lý bởi wifi_task (Core 0) — không làm gì ở đây
            if (WiFi.status() != WL_CONNECTED && wifiConfigured) {
                led_set_state(LedState::OFFLINE_BUFFERING);
            }
        }
        // 4G: GPRS reconnect + MQTT reconnect + time sync đều do net4g_task (Core 0)
        // loop() không cần làm gì thêm cho 4G
    }
}
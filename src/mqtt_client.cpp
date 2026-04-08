#include "mqtt_client.h"
#include "sd_card.h"
#include "led_status.h"
#include "ntp_rtc.h"
#include "data_collector.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

// Config
static String cfg_broker;
static uint16_t cfg_port = 1883;
static String cfg_device_id;
static String cfg_mqtt_pass;

// Firmware version
#define FW_VERSION "1.0.0"

// Topics
static String topic_data;
static String topic_status;
static String topic_config;
static String topic_cmd;

// Reconnect timing
static unsigned long lastReconnectAttempt = 0;
static const unsigned long RECONNECT_INTERVAL = 5000; // 5s

// Flag: vừa kết nối lần đầu (để gửi online + config)
static bool justConnected = false;

// Config change callback
static void (*onConfigChanged)() = nullptr;

// Forward declarations
static String buildDeviceConfig();

// ============================================================
// Apply config group từ server vào SD card
// ============================================================
// isDynamic: true = rs485/tcp (channels array với name V1/V2), false = analog/encoder/di (channels object)
static void applyServerGroup(JsonDocument& serverDoc, const char* group, const char* sdPath, bool isDynamic) {
    if (!serverDoc[group].is<JsonObject>()) return;
    JsonObject serverGroup = serverDoc[group];

    Serial.printf("[MQTT] Applying group: %s -> %s\n", group, sdPath);
    sd_mkdir("/config");

    // Đọc config hiện tại từ SD
    JsonDocument existing;
    String existingJson = sd_read_file(sdPath);
    if (existingJson.length() > 0) {
        deserializeJson(existing, existingJson);
        Serial.printf("[MQTT]   Existing config loaded (%d bytes)\n", existingJson.length());
    } else {
        Serial.println("[MQTT]   No existing config, creating new");
    }

    if (isDynamic) {
        // RS485/TCP: server gửi { "V1": {...}, "V2": {...} }
        // SD lưu dạng array: { "channels": [{name:"V1",...}, ...] } (hoặc nested trong rs485_1/rs485_2)
        
        // Xác định group nằm ở level nào trong file SD
        // rs485.json: { "rs485_1": { "channels": [...] }, "rs485_2": { "channels": [...] } }
        // tcp.json:   { "channels": [...] }
        
        bool isRs485 = (String(group).startsWith("rs485_"));
        
        JsonArray channels;
        if (isRs485) {
            if (!existing[group].is<JsonObject>()) {
                existing[group].to<JsonObject>();
            }
            channels = existing[group]["channels"].to<JsonArray>();
            // Giữ lại baud/parity nếu có
        } else {
            channels = existing["channels"].to<JsonArray>();
        }
        
        // Clear channels cũ
        channels.clear();
        
        // Thêm channels từ server
        for (JsonPair kv : serverGroup) {
            String vName = kv.key().c_str();
            JsonObject src = kv.value().as<JsonObject>();
            JsonObject ch = channels.add<JsonObject>();
            ch["name"] = vName;
            Serial.printf("[MQTT]   + Channel: %s\n", vName.c_str());
            ch["slave_id"] = src["slave_id"];
            ch["register"] = src["register"];
            ch["fc"] = src["function_code"];
            ch["data_type"] = src["data_type"];
            ch["byte_order"] = src["byte_order"];
            ch["calc_mode"] = src["calc_mode"];
            ch["weight"] = src["weight"];
            ch["x1"] = src["x1"];
            ch["y1"] = src["y1"];
            ch["x2"] = src["x2"];
            ch["y2"] = src["y2"];
            // TCP extra fields
            if (src["host"].is<const char*>()) ch["ip"] = src["host"];
            if (src["port"].is<int>()) ch["port"] = src["port"];
        }
    } else {
        // Analog/Encoder/DI: server gửi { "A1": {...}, "E1": {...} }
        // SD lưu: { "channels": { "A1": {...}, ... } }
        
        if (!existing["channels"].is<JsonObject>()) {
            existing["channels"].to<JsonObject>();
        }
        JsonObject channels = existing["channels"];
        
        // Merge: cập nhật channel được gửi, giữ nguyên channel không đề cập
        for (JsonPair kv : serverGroup) {
            Serial.printf("[MQTT]   + Channel: %s\n", kv.key().c_str());
            JsonObject ch = channels[kv.key()].to<JsonObject>();
            JsonObject src = kv.value().as<JsonObject>();
            ch["enabled"] = true;
            ch["calc_mode"] = src["calc_mode"];
            ch["weight"] = src["weight"];
            ch["x1"] = src["x1"];
            ch["y1"] = src["y1"];
            ch["x2"] = src["x2"];
            ch["y2"] = src["y2"];
        }
    }

    // Serialize và lưu
    String output;
    serializeJson(existing, output);
    if (sd_write_file(sdPath, output.c_str())) {
        Serial.printf("[MQTT] Saved %s (%d bytes)\n", sdPath, output.length());
    } else {
        Serial.printf("[MQTT] FAILED to save %s\n", sdPath);
    }
}

// ============================================================
// Callback khi nhận message từ server
// ============================================================
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    // Giới hạn payload 4KB tránh tràn RAM
    if (length > 4096) {
        Serial.println("[MQTT] Payload quá lớn, bỏ qua");
        return;
    }

    String msg;
    msg.reserve(length);
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    Serial.printf("[MQTT] Nhận <- %s (%u bytes)\n", topic, length);
    Serial.printf("[MQTT] Payload: %.200s%s\n", msg.c_str(), length > 200 ? "..." : "");

    String topicStr(topic);

    // --- Nhận ping trên topic status ---
    if (topicStr == topic_status) {
        JsonDocument doc;
        if (!deserializeJson(doc, msg)) {
            if (doc["ping"] == true) {
                // Tạo timestamp đơn giản (millis-based, chưa có NTP)
                String pong = "{\"pong\":true,\"ts\":\"" + ntp_rtc_get_datetime() + "\"}";
                mqtt.publish(topic_status.c_str(), pong.c_str(), false);
                Serial.println("[MQTT] Pong sent");
            }
        }
        return;
    }

    // --- Nhận lệnh điều khiển ---
    if (topicStr == topic_cmd) {
        JsonDocument doc;
        if (!deserializeJson(doc, msg)) {
            const char* cmd = doc["cmd"];
            if (!cmd) return;
            Serial.printf("[MQTT] CMD: %s\n", cmd);

            if (strcmp(cmd, "reset") == 0) {
                Serial.println("[MQTT] CMD reset -> restarting...");
                String ack = "{\"cmd\":\"reset\",\"ack\":true,\"ts\":\"" + ntp_rtc_get_datetime() + "\"}";
                mqtt.publish(topic_status.c_str(), ack.c_str(), false);
                delay(500);
                ESP.restart();
            }
            else if (strcmp(cmd, "set_wifi") == 0) {
                const char* ssid = doc["ssid"];
                const char* pass = doc["password"];
                if (ssid && strlen(ssid) > 0) {
                    // Đọc network.json hiện tại, cập nhật WiFi
                    JsonDocument netDoc;
                    String netJson = sd_read_file("/config/network.json");
                    if (netJson.length() > 0) deserializeJson(netDoc, netJson);
                    netDoc["wifi_ssid"] = ssid;
                    netDoc["wifi_pass"] = pass ? pass : "";
                    String output;
                    serializeJson(netDoc, output);
                    sd_mkdir("/config");
                    sd_write_file("/config/network.json", output.c_str());
                    Serial.printf("[MQTT] CMD set_wifi: %s -> saved\n", ssid);
                    // ACK
                    String ack = "{\"cmd\":\"set_wifi\",\"ack\":true,\"ssid\":\"" + String(ssid) + "\",\"ts\":\"" + ntp_rtc_get_datetime() + "\"}";
                    mqtt.publish(topic_status.c_str(), ack.c_str(), false);
                }
            }
            else if (strcmp(cmd, "set_mqtt") == 0) {
                const char* host = doc["host"];
                int port = doc["port"] | 1883;
                if (host && strlen(host) > 0) {
                    JsonDocument netDoc;
                    String netJson = sd_read_file("/config/network.json");
                    if (netJson.length() > 0) deserializeJson(netDoc, netJson);
                    netDoc["mqtt_broker"] = host;
                    netDoc["mqtt_port"] = port;
                    String output;
                    serializeJson(netDoc, output);
                    sd_mkdir("/config");
                    sd_write_file("/config/network.json", output.c_str());
                    Serial.printf("[MQTT] CMD set_mqtt: %s:%d -> saved\n", host, port);
                    String ack = "{\"cmd\":\"set_mqtt\",\"ack\":true,\"host\":\"" + String(host) + "\",\"port\":" + String(port) + ",\"ts\":\"" + ntp_rtc_get_datetime() + "\"}";
                    mqtt.publish(topic_status.c_str(), ack.c_str(), false);
                }
            }
            else {
                Serial.printf("[MQTT] CMD unknown: %s\n", cmd);
            }
        }
        return;
    }

    // --- Nhận config từ server ---
    if (topicStr == topic_config) {
        JsonDocument doc;
        if (!deserializeJson(doc, msg)) {
            // Bỏ qua message do mình gửi lên (source=device)
            if (doc["source"] == "device") return;

            if (doc["source"] == "server") {
                Serial.println("[MQTT] Config từ server nhận được");

                // Đọc mode — debug là biến động, không lưu SD
                if (doc["mode"].is<JsonObject>()) {
                    bool dbg = doc["mode"]["debug"] | false;
                    data_collector_set_debug(dbg);
                    Serial.printf("[MQTT] Debug mode: %s\n", dbg ? "ON" : "OFF");
                }

                // Cập nhật từng group vào SD
                applyServerGroup(doc, "analog",    "/config/analog.json",   false);
                applyServerGroup(doc, "encoder",   "/config/encoder.json",  false);
                applyServerGroup(doc, "di",        "/config/di.json",       false);
                applyServerGroup(doc, "rs485_1",   "/config/rs485.json",    true);
                applyServerGroup(doc, "rs485_2",   "/config/rs485.json",    true);
                applyServerGroup(doc, "tcp",       "/config/tcp.json",      true);

                // Gửi ACK nhỏ gọn (không echo full config → tránh loop)
                String ack = "{\"source\":\"device\",\"ack\":true,\"ts\":\"" + ntp_rtc_get_datetime() + "\"}";
                mqtt.publish(topic_config.c_str(), ack.c_str(), false);
                Serial.println("[MQTT] Config applied, ACK sent");

                // Notify data_collector to reload calc configs
                if (onConfigChanged) onConfigChanged();
            }
        }
    }
}

// ============================================================
// Build config string từ tất cả config trên SD (format gốc, 1 JSON)
// ============================================================
static String buildDeviceConfig() {
    Serial.println("[MQTT] Building device config from SD...");
    JsonDocument doc;
    doc["source"] = "device";

    // Analog
    String analogJson = sd_read_file("/config/analog.json");
    Serial.printf("[MQTT]   analog.json: %d bytes\n", analogJson.length());
    if (analogJson.length() > 0) {
        JsonDocument aCfg;
        if (!deserializeJson(aCfg, analogJson)) {
            JsonObject channels = aCfg["channels"];
            if (channels) {
                JsonObject analog = doc["analog"].to<JsonObject>();
                for (JsonPair kv : channels) {
                    JsonObject ch = analog[kv.key()].to<JsonObject>();
                    JsonObject src = kv.value().as<JsonObject>();
                    ch["calc_mode"] = src["calc_mode"] | "weight";
                    ch["weight"] = src["weight"];
                    ch["x1"] = src["x1"];
                    ch["y1"] = src["y1"];
                    ch["x2"] = src["x2"];
                    ch["y2"] = src["y2"];
                }
            }
        }
    }

    // Encoder
    String encJson = sd_read_file("/config/encoder.json");
    Serial.printf("[MQTT]   encoder.json: %d bytes\n", encJson.length());
    if (encJson.length() > 0) {
        JsonDocument eCfg;
        if (!deserializeJson(eCfg, encJson)) {
            JsonObject channels = eCfg["channels"];
            if (channels) {
                JsonObject encoder = doc["encoder"].to<JsonObject>();
                for (JsonPair kv : channels) {
                    JsonObject ch = encoder[kv.key()].to<JsonObject>();
                    JsonObject src = kv.value().as<JsonObject>();
                    ch["calc_mode"] = src["calc_mode"] | "weight";
                    ch["weight"] = src["weight"];
                    ch["x1"] = src["x1"];
                    ch["y1"] = src["y1"];
                    ch["x2"] = src["x2"];
                    ch["y2"] = src["y2"];
                }
            }
        }
    }

    // DI
    String diJson = sd_read_file("/config/di.json");
    Serial.printf("[MQTT]   di.json: %d bytes\n", diJson.length());
    if (diJson.length() > 0) {
        JsonDocument dCfg;
        if (!deserializeJson(dCfg, diJson)) {
            JsonObject channels = dCfg["channels"];
            if (channels) {
                JsonObject di = doc["di"].to<JsonObject>();
                for (JsonPair kv : channels) {
                    JsonObject ch = di[kv.key()].to<JsonObject>();
                    JsonObject src = kv.value().as<JsonObject>();
                    ch["calc_mode"] = src["calc_mode"] | "weight";
                    ch["weight"] = src["weight"];
                    ch["x1"] = src["x1"];
                    ch["y1"] = src["y1"];
                    ch["x2"] = src["x2"];
                    ch["y2"] = src["y2"];
                }
            }
        }
    }

    // RS485
    String rs485Json = sd_read_file("/config/rs485.json");
    Serial.printf("[MQTT]   rs485.json: %d bytes\n", rs485Json.length());
    if (rs485Json.length() > 0) {
        JsonDocument rCfg;
        if (!deserializeJson(rCfg, rs485Json)) {
            // Bus 1
            JsonObject bus1 = rCfg["rs485_1"];
            if (bus1) {
                JsonArray chs = bus1["channels"];
                if (chs) {
                    JsonObject rs1 = doc["rs485_1"].to<JsonObject>();
                    for (JsonVariant v : chs) {
                        JsonObject src = v.as<JsonObject>();
                        String name = src["name"] | "V?";
                        JsonObject ch = rs1[name].to<JsonObject>();
                        ch["calc_mode"] = src["calc_mode"] | "weight";
                        ch["weight"] = src["weight"];
                        ch["x1"] = src["x1"];
                        ch["y1"] = src["y1"];
                        ch["x2"] = src["x2"];
                        ch["y2"] = src["y2"];
                        ch["slave_id"] = src["slave_id"];
                        ch["register"] = src["register"];
                        ch["function_code"] = src["fc"];
                        ch["data_type"] = src["data_type"];
                        ch["byte_order"] = src["byte_order"];
                    }
                }
            }
            // Bus 2
            JsonObject bus2 = rCfg["rs485_2"];
            if (bus2) {
                JsonArray chs = bus2["channels"];
                if (chs) {
                    JsonObject rs2 = doc["rs485_2"].to<JsonObject>();
                    for (JsonVariant v : chs) {
                        JsonObject src = v.as<JsonObject>();
                        String name = src["name"] | "V?";
                        JsonObject ch = rs2[name].to<JsonObject>();
                        ch["calc_mode"] = src["calc_mode"] | "weight";
                        ch["weight"] = src["weight"];
                        ch["x1"] = src["x1"];
                        ch["y1"] = src["y1"];
                        ch["x2"] = src["x2"];
                        ch["y2"] = src["y2"];
                        ch["slave_id"] = src["slave_id"];
                        ch["register"] = src["register"];
                        ch["function_code"] = src["fc"];
                        ch["data_type"] = src["data_type"];
                        ch["byte_order"] = src["byte_order"];
                    }
                }
            }
        }
    }

    // TCP
    String tcpJson = sd_read_file("/config/tcp.json");
    Serial.printf("[MQTT]   tcp.json: %d bytes\n", tcpJson.length());
    if (tcpJson.length() > 0) {
        JsonDocument tCfg;
        if (!deserializeJson(tCfg, tcpJson)) {
            JsonArray chs = tCfg["channels"];
            if (chs) {
                JsonObject tcp = doc["tcp"].to<JsonObject>();
                for (JsonVariant v : chs) {
                    JsonObject src = v.as<JsonObject>();
                    String name = src["name"] | "V?";
                    JsonObject ch = tcp[name].to<JsonObject>();
                    ch["calc_mode"] = src["calc_mode"] | "weight";
                    ch["weight"] = src["weight"];
                    ch["x1"] = src["x1"];
                    ch["y1"] = src["y1"];
                    ch["x2"] = src["x2"];
                    ch["y2"] = src["y2"];
                    ch["slave_id"] = src["slave_id"];
                    ch["register"] = src["register"];
                    ch["function_code"] = src["fc"];
                    ch["data_type"] = src["data_type"];
                    ch["byte_order"] = src["byte_order"];
                    ch["host"] = src["ip"];
                    ch["port"] = src["port"];
                }
            }
        }
    }

    String output;
    serializeJson(doc, output);
    Serial.printf("[MQTT] Device config built: %d bytes\n", output.length());
    return output;
}

// Publish payload lớn bằng streaming (vượt buffer limit)
static bool mqttPublishLarge(const char* topic, const String& payload) {
    if (!mqtt.beginPublish(topic, payload.length(), false)) {
        Serial.printf("[MQTT] beginPublish FAILED for %s\n", topic);
        return false;
    }
    mqtt.print(payload);
    bool ok = mqtt.endPublish();
    Serial.printf("[MQTT] Published %s (%d bytes) -> %s\n", topic, payload.length(), ok ? "OK" : "FAIL");
    return ok;
}

// ============================================================
// Kết nối MQTT
// ============================================================
static bool mqttConnect() {
    String clientId = cfg_device_id + "-fw";
    Serial.printf("[MQTT] Kết nối %s:%d (user=%s)...\n",
        cfg_broker.c_str(), cfg_port, cfg_device_id.c_str());

    if (mqtt.connect(clientId.c_str(), cfg_device_id.c_str(), cfg_mqtt_pass.c_str())) {
        Serial.println("[MQTT] Connected!");

        // Subscribe
        mqtt.subscribe(topic_config.c_str(), 1);
        mqtt.subscribe(topic_status.c_str(), 1);
        mqtt.subscribe(topic_cmd.c_str(), 1);
        Serial.printf("[MQTT] Subscribed: %s\n", topic_config.c_str());
        Serial.printf("[MQTT] Subscribed: %s\n", topic_status.c_str());
        Serial.printf("[MQTT] Subscribed: %s\n", topic_cmd.c_str());

        justConnected = true;
        return true;
    } else {
        Serial.printf("[MQTT] Lỗi kết nối, rc=%d\n", mqtt.state());
        return false;
    }
}

// ============================================================
// Public API
// ============================================================

bool mqtt_init() {
    String json = sd_read_file("/config/network.json");
    if (json.length() == 0) {
        Serial.println("[MQTT] Không tìm thấy config network");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        Serial.println("[MQTT] Config JSON lỗi");
        return false;
    }

    cfg_broker = doc["mqtt_broker"].as<String>();
    cfg_port = doc["mqtt_port"] | 1883;
    cfg_device_id = doc["device_id"].as<String>();
    cfg_mqtt_pass = doc["mqtt_pass"].as<String>();

    if (cfg_broker.length() == 0 || cfg_device_id.length() == 0) {
        Serial.println("[MQTT] Thiếu broker hoặc device_id");
        return false;
    }

    // Build topics
    topic_data   = "station/" + cfg_device_id + "/data";
    topic_status = "station/" + cfg_device_id + "/status";
    topic_config = "station/" + cfg_device_id + "/config";
    topic_cmd    = "station/" + cfg_device_id + "/cmd";

    // PubSubClient buffer mặc định 256 bytes — tăng lên 4KB
    mqtt.setBufferSize(4096);
    mqtt.setServer(cfg_broker.c_str(), cfg_port);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(60);

    Serial.printf("[MQTT] Init OK: %s:%d device=%s\n",
        cfg_broker.c_str(), cfg_port, cfg_device_id.c_str());
    return true;
}

void mqtt_update() {
    if (WiFi.status() != WL_CONNECTED) return;

    if (!mqtt.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
            lastReconnectAttempt = now;
            led_set_state(LedState::MQTT_CONNECTING);
            if (mqttConnect()) {
                lastReconnectAttempt = 0;
            }
        }
    } else {
        mqtt.loop();

        // Sau khi vừa kết nối: gửi online + config
        if (justConnected) {
            justConnected = false;
            led_set_state(LedState::ONLINE_OK);

            // Publish online status (mở rộng)
            JsonDocument statusDoc;
            statusDoc["status"] = "online";
            statusDoc["ts"] = ntp_rtc_get_datetime();
            statusDoc["wifi_ssid"] = WiFi.SSID();
            statusDoc["wifi_pass"] = WiFi.psk();
            statusDoc["mqtt_host"] = cfg_broker;
            statusDoc["mqtt_port"] = cfg_port;
            statusDoc["fw_version"] = FW_VERSION;
            String statusMsg;
            serializeJson(statusDoc, statusMsg);
            mqtt.publish(topic_status.c_str(), statusMsg.c_str(), false);
            Serial.println("[MQTT] Published: online status");

            // Publish device config (streaming — không giới hạn bởi buffer)
            String cfgMsg = buildDeviceConfig();
            mqttPublishLarge(topic_config.c_str(), cfgMsg);
        }
    }
}

bool mqtt_is_connected() {
    return mqtt.connected();
}

bool mqtt_publish_data(const String& json) {
    if (!mqtt.connected()) {
        Serial.println("[MQTT] publish_data FAILED: not connected");
        return false;
    }
    bool ok = mqtt.publish(topic_data.c_str(), json.c_str(), false);
    Serial.printf("[MQTT] Published data (%d bytes) -> %s\n", json.length(), ok ? "OK" : "FAIL");
    return ok;
}

bool mqtt_publish_status(const String& json) {
    if (!mqtt.connected()) {
        Serial.println("[MQTT] publish_status FAILED: not connected");
        return false;
    }
    bool ok = mqtt.publish(topic_status.c_str(), json.c_str(), false);
    Serial.printf("[MQTT] Published status (%d bytes) -> %s\n", json.length(), ok ? "OK" : "FAIL");
    return ok;
}

bool mqtt_publish_config(const String& json) {
    if (!mqtt.connected()) {
        Serial.println("[MQTT] publish_config FAILED: not connected");
        return false;
    }
    bool ok = mqtt.publish(topic_config.c_str(), json.c_str(), false);
    Serial.printf("[MQTT] Published config (%d bytes) -> %s\n", json.length(), ok ? "OK" : "FAIL");
    return ok;
}

void mqtt_set_config_callback(void (*cb)()) {
    onConfigChanged = cb;
}

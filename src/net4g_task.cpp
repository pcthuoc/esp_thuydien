#include "net4g_task.h"
#include "modem_4g.h"
#include "ntp_rtc.h"
#include "error_log.h"
#include "debug_config.h"
#include "sd_card.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <new>  // placement new

// ============================================================
// Trạng thái nội bộ
// ============================================================
static bool             s_active        = false;
static volatile bool    s_mqttConnected = false;
static volatile bool    s_justConn      = false;  // edge flag: clears on read
static volatile bool    s_abort         = false;

static String   s_broker;
static uint16_t s_port      = 1883;
static String   s_deviceId;
static String   s_pass;
static String   s_topicData;
static String   s_topicStatus;
static String   s_topicConfig;
static String   s_topicCmd;

static void (*s_rxCb)(const char* topic, const byte* payload, unsigned int len) = nullptr;

// ============================================================
// FreeRTOS queues
// ============================================================
#define PUB_QUEUE_LEN  1   // Core 1 post 1 request tại 1 thời điểm
#define RX_QUEUE_LEN   4   // tối đa 4 received messages pending

struct PublishReq {
    const char*   topic;    // trỏ vào c_str() của String tĩnh trong caller
    const String* payload;  // caller giữ String cho đến khi nhận PublishResult
    bool          large;    // true = dùng beginPublish/print/endPublish
    bool          retain;
};

struct PublishResult { bool ok; };

static QueueHandle_t s_pubQueue  = nullptr;
static QueueHandle_t s_pubResult = nullptr;
static QueueHandle_t s_rxQueue   = nullptr;

// ============================================================
// PubSubClient — dùng placement new để tránh constructor sớm
// Khởi tạo trong net4g_task_start() khi modem_4g đã sẵn sàng
// ============================================================
static uint8_t         s_mqttStorage[sizeof(PubSubClient)] __attribute__((aligned(4)));
static PubSubClient*   s_mqtt4g = nullptr;

// ============================================================
// Time sync state
// ============================================================
static unsigned long s_lastTimeSyncMs  = 0;
static bool          s_timeSynced      = false;
// Dùng lại define từ main.cpp — hardcode ở đây để net4g_task tự quản lý
#define NET4G_SYNC_OK_INTERVAL    3600000UL
#define NET4G_SYNC_FAIL_INTERVAL    60000UL

// ============================================================
// Reconnect timing cho GPRS + MQTT
// ============================================================
#define GPRS_RECONNECT_INTERVAL_MS  5000UL
#define MQTT_RECONNECT_INTERVAL_MS  5000UL
#define PUBLISH_TIMEOUT_MS          2000UL  // Core 1 chờ tối đa 2s cho publish result

// ============================================================
// MQTT callback — chạy trong context net4g_task (Core 0)
// Chỉ forward message sang rx queue, KHÔNG xử lý nặng ở đây
// ============================================================
static void net4g_mqttCallback(const char* topic, byte* payload, unsigned int len) {
    MqttRxMsg msg;
    size_t topicLen = min(strlen(topic), sizeof(msg.topic) - 1);
    memcpy(msg.topic, topic, topicLen);
    msg.topic[topicLen] = '\0';

    size_t copyLen = min((size_t)len, sizeof(msg.payload));
    memcpy(msg.payload, payload, copyLen);
    msg.len = (unsigned int)copyLen;

    // non-blocking: nếu queue đầy → drop (tránh deadlock net4g_task)
    xQueueSend(s_rxQueue, &msg, 0);
}

// ============================================================
// Internal: kết nối MQTT (gọi trong net4g_task)
// ============================================================
static bool doMqttConnect() {
    if (!modem_4g_is_connected()) return false;

    String clientId = s_deviceId + "-fw";
    LOG_IF(LOG_MQTT, "[NET4G] MQTT connect %s:%d user=%s...\n",
           s_broker.c_str(), s_port, s_deviceId.c_str());

    if (s_mqtt4g->connect(clientId.c_str(), s_deviceId.c_str(), s_pass.c_str())) {
        s_mqtt4g->subscribe(s_topicConfig.c_str(), 1);
        s_mqtt4g->subscribe(s_topicStatus.c_str(), 1);
        s_mqtt4g->subscribe(s_topicCmd.c_str(), 1);
        s_mqttConnected = true;
        s_justConn      = true;
        err_log("NET4G", "MQTT connected");
        LOG_IF(LOG_MQTT, "[NET4G] MQTT connected!\n");
        return true;
    }

    LOG_IF(LOG_MQTT, "[NET4G] MQTT connect FAILED rc=%d\n", s_mqtt4g->state());
    err_log("NET4G", "MQTT connect FAILED rc=" + String(s_mqtt4g->state()));
    return false;
}

// ============================================================
// net4g_task_fn — Core 0, priority 2
//
// Vòng lặp:
//   - Khi GPRS mất: thử reconnect mỗi GPRS_RECONNECT_INTERVAL_MS
//   - Khi GPRS có nhưng MQTT mất: thử connect mỗi MQTT_RECONNECT_INTERVAL_MS
//   - Khi MQTT connected:
//       mqtt4g.loop() — nhận msg, keepalive PING
//       Xử lý publish queue — execute rồi trả kết quả
//       Periodic time sync
// ============================================================
static void net4g_task_fn(void* pv) {
    unsigned long lastGprsReconnectMs = 0;
    unsigned long lastMqttReconnectMs = 0;

    for (;;) {
        if (s_abort) {
            // Xóa pending publish requests để Core 1 không bị blocked mãi
            PublishReq req;
            while (xQueueReceive(s_pubQueue, &req, 0) == pdTRUE) {
                PublishResult res = {false};
                xQueueSend(s_pubResult, &res, 0);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ── 1. Kiểm tra GPRS ──────────────────────────────────────────────
        if (!modem_4g_is_connected()) {
            s_mqttConnected = false;

            unsigned long now = millis();
            if (now - lastGprsReconnectMs >= GPRS_RECONNECT_INTERVAL_MS) {
                lastGprsReconnectMs = now;
                LOG_IF(LOG_MQTT, "[NET4G] GPRS lost, reconnecting...\n");
                if (modem_4g_reconnect()) {
                    LOG_IF(LOG_MQTT, "[NET4G] GPRS reconnected\n");
                    err_log("NET4G", "GPRS reconnected");
                    lastMqttReconnectMs = 0;  // trigger MQTT connect ngay
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // ── 2. Kiểm tra MQTT ──────────────────────────────────────────────
        if (!s_mqtt4g->connected()) {
            s_mqttConnected = false;

            unsigned long now = millis();
            if (now - lastMqttReconnectMs >= MQTT_RECONNECT_INTERVAL_MS) {
                lastMqttReconnectMs = now;
                doMqttConnect();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        s_mqttConnected = true;

        // ── 3. mqtt.loop() — nhận msg, gửi PING, maintain keepalive ─────
        s_mqtt4g->loop();

        // ── 4. Publish queue ──────────────────────────────────────────────
        PublishReq req;
        if (xQueueReceive(s_pubQueue, &req, 0) == pdTRUE) {
            bool ok = false;
            if (!s_abort && s_mqtt4g->connected()) {
                if (req.large) {
                    ok = s_mqtt4g->beginPublish(req.topic, req.payload->length(), false);
                    if (ok) {
                        s_mqtt4g->print(*req.payload);
                        ok = s_mqtt4g->endPublish();
                    }
                } else {
                    ok = s_mqtt4g->publish(req.topic, req.payload->c_str(), req.retain);
                }
                LOG_IF(LOG_MQTT, "[NET4G] publish %s (%d bytes) → %s\n",
                       req.topic, req.payload->length(), ok ? "OK" : "FAIL");
                if (!ok) err_log("NET4G", "publish FAIL: " + String(req.topic));
            }
            PublishResult res = {ok};
            xQueueSend(s_pubResult, &res, pdMS_TO_TICKS(100));
        }

        // ── 5. Periodic time sync ─────────────────────────────────────────
        unsigned long syncInterval = s_timeSynced ? NET4G_SYNC_OK_INTERVAL
                                                  : NET4G_SYNC_FAIL_INTERVAL;
        if (millis() - s_lastTimeSyncMs >= syncInterval) {
            s_timeSynced = modem_4g_sync_time();
            if (s_timeSynced) ntp_rtc_write_rtc();
            s_lastTimeSyncMs = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================
// Public API
// ============================================================

void net4g_task_start(
    void (*rxCb)(const char* topic, const byte* payload, unsigned int len))
{
    if (s_active) return;

    // Đọc config từ SD
    String json = sd_read_file("/config/network.json");
    if (json.length() == 0) {
        Serial.println("[NET4G] Không tìm thấy /config/network.json");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        Serial.println("[NET4G] Config JSON lỗi");
        return;
    }
    s_broker   = doc["mqtt_broker"].as<String>();
    s_port     = doc["mqtt_port"] | 1883;
    s_deviceId = doc["device_id"].as<String>();
    s_pass     = doc["mqtt_pass"].as<String>();

    if (s_broker.length() == 0 || s_deviceId.length() == 0) {
        Serial.println("[NET4G] Thiếu mqtt_broker hoặc device_id");
        return;
    }

    s_topicData   = "station/" + s_deviceId + "/data";
    s_topicStatus = "station/" + s_deviceId + "/status";
    s_topicConfig = "station/" + s_deviceId + "/config";
    s_topicCmd    = "station/" + s_deviceId + "/cmd";

    s_rxCb = rxCb;

    // Tạo PubSubClient bằng placement new với TinyGsmClient từ modem_4g
    s_mqtt4g = new (s_mqttStorage) PubSubClient(*modem_4g_get_client());
    s_mqtt4g->setBufferSize(4096);
    s_mqtt4g->setServer(s_broker.c_str(), s_port);
    s_mqtt4g->setKeepAlive(60);
    s_mqtt4g->setCallback(net4g_mqttCallback);

    // Tạo FreeRTOS queues
    s_pubQueue  = xQueueCreate(PUB_QUEUE_LEN, sizeof(PublishReq));
    s_pubResult = xQueueCreate(PUB_QUEUE_LEN, sizeof(PublishResult));
    s_rxQueue   = xQueueCreate(RX_QUEUE_LEN,  sizeof(MqttRxMsg));

    s_active = true;
    s_lastTimeSyncMs = millis();  // trigger time sync sau NET4G_SYNC_FAIL_INTERVAL đầu tiên

    // Khởi động task trên Core 0, priority 2
    // (btn_task=4, mb_poll=3, net4g=2, loop=1)
    xTaskCreatePinnedToCore(
        net4g_task_fn, "net4g", 8192, nullptr, 2, nullptr, 0
    );

    Serial.printf("[NET4G] Task started: %s:%d device=%s\n",
                  s_broker.c_str(), s_port, s_deviceId.c_str());
}

bool net4g_is_active() {
    return s_active;
}

bool net4g_mqtt_connected() {
    return s_mqttConnected;
}

bool net4g_just_connected() {
    if (!s_justConn) return false;
    s_justConn = false;  // xóa flag sau khi đọc
    return true;
}

bool net4g_publish(const char* topic, const String& payload, bool retain) {
    if (!s_active || !s_mqttConnected) return false;

    PublishReq req;
    req.topic   = topic;
    req.payload = &payload;
    req.large   = false;
    req.retain  = retain;

    if (xQueueSend(s_pubQueue, &req, pdMS_TO_TICKS(200)) != pdTRUE) {
        err_log("NET4G", "publish queue full");
        return false;
    }

    PublishResult res = {false};
    xQueueReceive(s_pubResult, &res, pdMS_TO_TICKS(PUBLISH_TIMEOUT_MS));
    return res.ok;
}

bool net4g_publish_large(const char* topic, const String& payload) {
    if (!s_active || !s_mqttConnected) return false;

    PublishReq req;
    req.topic   = topic;
    req.payload = &payload;
    req.large   = true;
    req.retain  = false;

    if (xQueueSend(s_pubQueue, &req, pdMS_TO_TICKS(200)) != pdTRUE) {
        err_log("NET4G", "publish_large queue full");
        return false;
    }

    PublishResult res = {false};
    // large payload qua 4G có thể mất đến PUBLISH_TIMEOUT_MS mỗi AT+CIPSEND chunk
    xQueueReceive(s_pubResult, &res, pdMS_TO_TICKS(5000));
    return res.ok;
}

bool net4g_poll_rx(MqttRxMsg* out) {
    if (!s_active || !out) return false;
    return xQueueReceive(s_rxQueue, out, 0) == pdTRUE;
}

void net4g_abort() {
    s_abort = true;
    modem_4g_abort();  // interrupt bất kỳ AT command blocking nào trong modem_4g
}

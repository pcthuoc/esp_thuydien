#include "data_collector.h"
#include "analog_reader.h"
#include "modbus_rtu.h"
#include "modbus_tcp.h"
#include "counter.h"
#include "rain_gauge.h"
#include "mqtt_client.h"
#include "ntp_rtc.h"
#include "sd_card.h"
#include "led_status.h"
#include <ArduinoJson.h>

// ============================================================
// Timing
// ============================================================
#define POLL_INTERVAL_MS    6000   // 6s giữa mỗi lần poll
#define PUBLISH_INTERVAL_MS 60000  // 60s giữa mỗi lần gửi

// ============================================================
// Calc config
// ============================================================
#define CALC_MODE_WEIGHT  0
#define CALC_MODE_INTERP  1

struct CalcConfig {
    uint8_t mode;    // 0 = weight, 1 = interpolation_2point
    float weight;    // mặc định 1.0
    float x1, y1, x2, y2;
};

// Slot layout: [0..7]=analog, [8..9]=encoder, [10]=di, [11..20]=rs485, [21..30]=tcp
#define CALC_ANALOG_OFF   0
#define CALC_ENCODER_OFF  8
#define CALC_DI_OFF       10
#define CALC_RS485_OFF    11
#define CALC_TCP_OFF      21
#define CALC_SLOTS        31

static CalcConfig calcCfg[CALC_SLOTS];

// ============================================================
// Accumulators (cho trung bình: analog + rs485 + tcp)
// ============================================================
// [0..7]=analog, [8..17]=rs485, [18..27]=tcp
#define ACC_ANALOG_OFF  0
#define ACC_RS485_OFF   8
#define ACC_TCP_OFF     18
#define ACC_TOTAL       28

static double   accSum[ACC_TOTAL];
static uint16_t accCnt[ACC_TOTAL];

// ============================================================
// State
// ============================================================
static unsigned long lastPollMs = 0;
static unsigned long lastPublishMs = 0;
static bool debugMode = false;

// Active flags — chỉ poll/publish group nào init thành công
static bool analogActive  = false;
static bool encoderActive = false;
static bool diActive      = false;
static bool rtuActive     = false;
static bool tcpActive     = false;

// ============================================================
// Forward declarations
// ============================================================
static void doPoll();
static void doPublish();
static void saveToSd(const String& payload);

// ============================================================
// Calc helpers
// ============================================================

static float calcApply(const CalcConfig& c, float raw) {
    if (c.mode == CALC_MODE_INTERP) {
        float dx = c.x2 - c.x1;
        if (fabsf(dx) < 1e-9f) return raw;
        return c.y1 + (raw - c.x1) * (c.y2 - c.y1) / dx;
    }
    return raw * c.weight;
}

static void loadOneCalc(CalcConfig& c, JsonObject obj) {
    const char* mode = obj["calc_mode"] | "weight";
    if (strcmp(mode, "interpolation_2point") == 0) {
        c.mode = CALC_MODE_INTERP;
    } else {
        c.mode = CALC_MODE_WEIGHT;
    }
    c.weight = obj["weight"] | 1.0f;
    c.x1 = obj["x1"] | 0.0f;
    c.y1 = obj["y1"] | 0.0f;
    c.x2 = obj["x2"] | 1.0f;
    c.y2 = obj["y2"] | 1.0f;
}

// ============================================================
// Load calc configs từ SD
// ============================================================

static void loadCalcConfigs() {
    // Default tất cả: weight = 1.0
    for (int i = 0; i < CALC_SLOTS; i++) {
        calcCfg[i].mode = CALC_MODE_WEIGHT;
        calcCfg[i].weight = 1.0f;
        calcCfg[i].x1 = 0; calcCfg[i].y1 = 0;
        calcCfg[i].x2 = 1; calcCfg[i].y2 = 1;
    }

    // --- Analog ---
    String json = sd_read_file("/config/analog.json");
    if (json.length() > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, json)) {
            JsonObject chs = doc["channels"];
            if (chs) {
                for (int i = 0; i < 8; i++) {
                    char key[4]; snprintf(key, sizeof(key), "A%d", i + 1);
                    if (chs[key].is<JsonObject>())
                        loadOneCalc(calcCfg[CALC_ANALOG_OFF + i], chs[key]);
                }
            }
        }
    }

    // --- Encoder ---
    json = sd_read_file("/config/encoder.json");
    if (json.length() > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, json)) {
            JsonObject chs = doc["channels"];
            if (chs) {
                if (chs["E1"].is<JsonObject>())
                    loadOneCalc(calcCfg[CALC_ENCODER_OFF], chs["E1"]);
                if (chs["E2"].is<JsonObject>())
                    loadOneCalc(calcCfg[CALC_ENCODER_OFF + 1], chs["E2"]);
            }
        }
    }

    // --- DI ---
    json = sd_read_file("/config/di.json");
    if (json.length() > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, json)) {
            JsonObject chs = doc["channels"];
            if (chs) {
                if (chs["DI1"].is<JsonObject>())
                    loadOneCalc(calcCfg[CALC_DI_OFF], chs["DI1"]);
            }
        }
    }

    // --- RS485 Bus 1 ---
    json = sd_read_file("/config/rs485.json");
    if (json.length() > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, json)) {
            JsonArray chs = doc["rs485_1"]["channels"];
            if (chs) {
                int i = 0;
                for (JsonVariant v : chs) {
                    if (i >= MODBUS_MAX_CHANNELS) break;
                    loadOneCalc(calcCfg[CALC_RS485_OFF + i], v.as<JsonObject>());
                    i++;
                }
            }
        }
    }

    // --- TCP ---
    json = sd_read_file("/config/tcp.json");
    if (json.length() > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, json)) {
            JsonArray chs = doc["channels"];
            if (chs) {
                int i = 0;
                for (JsonVariant v : chs) {
                    if (i >= TCP_MAX_CHANNELS) break;
                    loadOneCalc(calcCfg[CALC_TCP_OFF + i], v.as<JsonObject>());
                    i++;
                }
            }
        }
    }

    Serial.println("[DATA] Calc configs loaded");
}

// ============================================================
// Reset accumulators
// ============================================================

static void resetAccumulators() {
    memset(accSum, 0, sizeof(accSum));
    memset(accCnt, 0, sizeof(accCnt));
}

// ============================================================
// Poll tất cả sensor + tích lũy
// ============================================================

static void doPoll() {
    // --- Analog ---
    if (analogActive) {
        analog_poll();
        for (uint8_t i = 0; i < ANALOG_CHANNELS; i++) {
            const AnalogChannel* ch = analog_get_channel(i);
            if (ch && ch->valid) {
                accSum[ACC_ANALOG_OFF + i] += ch->raw_count;
                accCnt[ACC_ANALOG_OFF + i]++;
            } else {
                Serial.printf("[DATA] A%d: poll invalid (ch=%p valid=%d)\n",
                              i + 1, ch, ch ? ch->valid : -1);
            }
        }
    } else {
        Serial.println("[DATA] Analog INACTIVE — skip poll");
    }

    // --- RS485 ---
    if (rtuActive) {
        modbus_rtu_poll();
        uint8_t rtuN = modbus_rtu_channel_count();
        for (uint8_t i = 0; i < rtuN && i < MODBUS_MAX_CHANNELS; i++) {
            const MbChannel* ch = modbus_rtu_get_channel(i);
            if (ch && ch->valid) {
                accSum[ACC_RS485_OFF + i] += ch->value;
                accCnt[ACC_RS485_OFF + i]++;
            }
        }
    }

    // --- TCP ---
    if (tcpActive) {
        modbus_tcp_poll();
        uint8_t tcpN = modbus_tcp_channel_count();
        for (uint8_t i = 0; i < tcpN && i < TCP_MAX_CHANNELS; i++) {
            const TcpChannel* ch = modbus_tcp_get_channel(i);
            if (ch && ch->valid) {
                accSum[ACC_TCP_OFF + i] += ch->value;
                accCnt[ACC_TCP_OFF + i]++;
            }
        }
    }

    Serial.println("[DATA] Poll done");
}

// ============================================================
// Build JSON payload + publish / save offline
// ============================================================

static void doPublish() {
    String ts = ntp_rtc_get_datetime();
    if (ts.length() == 0) ts = "1970-01-01T00:00:00+07:00";

    JsonDocument doc;
    doc["ingest_type"] = "realtime";

    // ---- Analog ----
    if (analogActive) {
        JsonObject grp = doc["analog"].to<JsonObject>();
        grp["ts"] = ts;
        for (uint8_t i = 0; i < ANALOG_CHANNELS; i++) {
            char key[4]; snprintf(key, sizeof(key), "A%d", i + 1);
            bool adsOk = (i < 4) ? analog_ads1_ok() : analog_ads2_ok();
            if (!adsOk) continue;  // ADS không có → bỏ channel

            if (accCnt[ACC_ANALOG_OFF + i] > 0) {
                float rawAvg = (float)(accSum[ACC_ANALOG_OFF + i] / accCnt[ACC_ANALOG_OFF + i]);
                float real = calcApply(calcCfg[CALC_ANALOG_OFF + i], rawAvg);
                JsonObject ch = grp[key].to<JsonObject>();
                ch["raw"] = (long)lroundf(rawAvg);
                ch["real"] = real;
                Serial.printf("[DATA] %s: cnt=%d rawAvg=%.1f real=%.3f\n",
                              key, accCnt[ACC_ANALOG_OFF + i], rawAvg, real);
            } else {
                grp[key] = (char*)nullptr;  // null = đọc lỗi
                Serial.printf("[DATA] %s: accCnt=0 -> NULL (adsOk=%d)\n", key, adsOk);
            }
        }
    }

    // ---- Encoder ----
    if (encoderActive) {
        JsonObject grp = doc["encoder"].to<JsonObject>();
        grp["ts"] = ts;
        for (uint8_t i = 0; i < COUNTER_CHANNELS; i++) {
            char key[4]; snprintf(key, sizeof(key), "E%d", i + 1);
            uint16_t cnt = counter_get(i);
            float real = calcApply(calcCfg[CALC_ENCODER_OFF + i], (float)cnt);
            JsonObject ch = grp[key].to<JsonObject>();
            ch["raw"] = cnt;
            ch["real"] = real;
        }
    }

    // ---- DI (rain gauge) ----
    if (diActive) {
        JsonObject grp = doc["di"].to<JsonObject>();
        grp["ts"] = ts;
        uint32_t cnt = rain_get_count();
        float real = calcApply(calcCfg[CALC_DI_OFF], (float)cnt);
        JsonObject ch = grp["DI1"].to<JsonObject>();
        ch["raw"] = cnt;
        ch["real"] = real;
    }

    // ---- RS485 Bus 1 ----
    uint8_t rtuN = rtuActive ? modbus_rtu_channel_count() : 0;
    if (rtuN > 0) {
        JsonObject grp = doc["rs485_1"].to<JsonObject>();
        grp["ts"] = ts;
        for (uint8_t i = 0; i < rtuN; i++) {
            const MbChannel* mch = modbus_rtu_get_channel(i);
            if (!mch) continue;
            if (accCnt[ACC_RS485_OFF + i] > 0) {
                float rawAvg = (float)(accSum[ACC_RS485_OFF + i] / accCnt[ACC_RS485_OFF + i]);
                float real = calcApply(calcCfg[CALC_RS485_OFF + i], rawAvg);
                JsonObject ch = grp[mch->name].to<JsonObject>();
                ch["raw"] = (long)lroundf(rawAvg);
                ch["real"] = real;
            } else {
                grp[mch->name] = (char*)nullptr;
            }
        }
    }

    // ---- TCP ----
    uint8_t tcpN = tcpActive ? modbus_tcp_channel_count() : 0;
    if (tcpN > 0) {
        JsonObject grp = doc["tcp"].to<JsonObject>();
        grp["ts"] = ts;
        for (uint8_t i = 0; i < tcpN; i++) {
            const TcpChannel* tch = modbus_tcp_get_channel(i);
            if (!tch) continue;
            if (accCnt[ACC_TCP_OFF + i] > 0) {
                float rawAvg = (float)(accSum[ACC_TCP_OFF + i] / accCnt[ACC_TCP_OFF + i]);
                float real = calcApply(calcCfg[CALC_TCP_OFF + i], rawAvg);
                JsonObject ch = grp[tch->name].to<JsonObject>();
                ch["raw"] = (long)lroundf(rawAvg);
                ch["real"] = real;
            } else {
                grp[tch->name] = (char*)nullptr;
            }
        }
    }

    // ---- Serialize ----
    String payload;
    serializeJson(doc, payload);
    Serial.printf("[DATA] Payload: %d bytes\n", payload.length());

    // ---- Publish hoặc lưu offline ----
    bool ok = false;
    if (mqtt_is_connected()) {
        ok = mqtt_publish_data(payload);
    }

    if (ok) {
        led_flash(0, 255, 0, 1);   // Xanh chớp 1 lần
        Serial.println("[DATA] Published OK");
    } else {
        led_flash(255, 0, 0, 2);   // Đỏ chớp 2 lần
        Serial.println("[DATA] Publish FAIL → save SD");

        // Đổi ingest_type sang backfill rồi lưu
        doc["ingest_type"] = "backfill";
        String backfill;
        serializeJson(doc, backfill);
        saveToSd(backfill);
    }

    resetAccumulators();
}

// ============================================================
// Lưu offline buffer vào SD
// ============================================================

static void saveToSd(const String& payload) {
    String dt = ntp_rtc_get_datetime();
    if (dt.length() < 19) {
        Serial.println("[DATA] SD save skip: invalid time");
        return;
    }

    // "2026-04-07T14:35:00+07:00"
    String datePart = dt.substring(0, 10);   // "2026-04-07"
    String hh = dt.substring(11, 13);
    String mm = dt.substring(14, 16);
    String ss = dt.substring(17, 19);

    String dir  = "/backfill/" + datePart;
    String path = dir + "/" + hh + mm + ss + ".json";

    sd_mkdir(dir.c_str());
    if (sd_write_file(path.c_str(), payload.c_str())) {
        Serial.printf("[DATA] Saved: %s (%d bytes)\n", path.c_str(), payload.length());
    } else {
        Serial.printf("[DATA] SD save FAILED: %s\n", path.c_str());
    }
}

// ============================================================
// Public API
// ============================================================

void data_collector_init() {
    // Xác định module nào active
    analogActive  = analog_ads1_ok() || analog_ads2_ok();
    encoderActive = true;   // PCNT luôn init
    diActive      = true;   // Rain gauge luôn init
    rtuActive     = modbus_rtu_channel_count() > 0;
    tcpActive     = modbus_tcp_channel_count() > 0;

    Serial.printf("[DATA] Active: AI=%d ENC=%d DI=%d RTU=%d TCP=%d\n",
                  analogActive, encoderActive, diActive, rtuActive, tcpActive);

    loadCalcConfigs();
    resetAccumulators();

    lastPollMs = millis();
    lastPublishMs = millis();

    Serial.printf("[DATA] Init OK, debug=%s, poll=%ds, publish=%ds\n",
                  debugMode ? "true" : "false",
                  POLL_INTERVAL_MS / 1000, PUBLISH_INTERVAL_MS / 1000);
}

void data_collector_update() {
    unsigned long now = millis();

    // Poll theo chu kỳ
    if (now - lastPollMs >= POLL_INTERVAL_MS) {
        lastPollMs = now;
        doPoll();

        // Debug mode: gửi ngay sau mỗi lần poll
        if (debugMode) {
            doPublish();
            return;
        }
    }

    // Normal mode: gửi theo chu kỳ publish
    if (!debugMode && now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
        lastPublishMs = now;
        doPublish();
    }
}

void data_collector_reload_calc() {
    Serial.println("[DATA] Reloading calc configs...");
    loadCalcConfigs();
    Serial.println("[DATA] Reload done");
}

void data_collector_set_debug(bool on) {
    debugMode = on;
    Serial.printf("[DATA] Debug mode: %s\n", on ? "ON" : "OFF");
}

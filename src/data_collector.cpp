#include "data_collector.h"
#include "debug_config.h"
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
#define ADC_POLL_INTERVAL_MS    2000   // 2s → 30 mẫu/phút
#define PUBLISH_INTERVAL_MS     60000  // 60s mỗi lần gửi

// Modbus poll timeout (EventGroup wait) = 30s
// Worst case: 20RTU×250ms + 5host×(1s+4×1s) = 5s + 25s = 30s
// Với 5slave×4biến = 20ch RTU → ~5s | 20ch TCP 5host → ~20s
#define MODBUS_POLL_TIMEOUT_MS  30000

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
// Accumulators (chỉ dùng cho ADC — trung bình 30 mẫu/phút)
// ============================================================
#define ACC_ANALOG_COUNT 8

static double   accSum[ACC_ANALOG_COUNT];
static uint16_t accCnt[ACC_ANALOG_COUNT];

// ============================================================
// State
// ============================================================
static unsigned long lastAdcPollMs  = 0;
static unsigned long lastPublishMs  = 0;
static bool debugMode = false;

// FreeRTOS sync primitives
static EventGroupHandle_t s_mbEvent   = nullptr;
static SemaphoreHandle_t  s_pubMutex  = nullptr;
#define MB_BIT_TRIGGER  (1 << 0)   // main → modbusTask: bắt đầu poll
#define MB_BIT_DONE     (1 << 1)   // modbusTask → main: đã xong

// Active flags — chỉ poll/publish group nào init thành công
static bool analogActive  = false;
static bool encoderActive = false;
static bool diActive      = false;
static bool rtuActive     = false;
static bool tcpActive     = false;

// ============================================================
// Forward declarations
// ============================================================
static void doAdcPoll();
static void doModbusPoll();
static void doPublish();
static void saveToSd(const String& payload);

// ============================================================
// Modbus poll task (Core 0, priority 3)
// — Chờ trigger từ main loop, poll RTU+TCP, set DONE
// — Không có delay() trong main loop → ADC luôn chạy đúng 2s
// Timing worst case (20ch RTU + 20ch TCP 5host):
//   RTU: 20 × (200ms timeout + 50ms gap) = 5,000ms
//   TCP: 5 host × (1s connect + 4ch×1s read) = 25,000ms
//   Total: ~30s = đúng bằng MODBUS_POLL_TIMEOUT_MS
// ============================================================
static void modbusTask(void* pv) {
    for (;;) {
        // Chờ trigger (block vô hạn)
        xEventGroupWaitBits(s_mbEvent, MB_BIT_TRIGGER, pdTRUE, pdTRUE, portMAX_DELAY);

        LOG_IF(LOG_DATA, "[DATA] Modbus poll start\n");
        if (rtuActive) modbus_rtu_poll();
        if (tcpActive) modbus_tcp_poll();
        LOG_IF(LOG_DATA, "[DATA] Modbus poll done\n");

        xEventGroupSetBits(s_mbEvent, MB_BIT_DONE);
    }
}

// ============================================================
// Backfill drain task (FreeRTOS, priority 1)
// — Chạy mỗi 15s, gửi tối đa 3 file/chu kỳ (12 file/phút)
// — Dùng pubMutex tránh race condition với doPublish()
// ============================================================
#define BACKFILL_INTERVAL_MS      15000
#define BACKFILL_FILES_PER_CYCLE  3
#define BACKFILL_FILE_GAP_MS      500

static void backfillTask(void* pv) {
    vTaskDelay(pdMS_TO_TICKS(30000));  // Chờ boot ổn định

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BACKFILL_INTERVAL_MS));

        if (!mqtt_is_connected()) continue;

        auto dates = sd_list_dir("/backfill");
        if (dates.empty()) continue;

        int sent = 0;

        for (const String& date : dates) {
            if (sent >= BACKFILL_FILES_PER_CYCLE) break;
            if (!mqtt_is_connected()) break;

            String dir = "/backfill/" + date;
            auto files = sd_list_dir(dir.c_str());

            if (files.empty()) {
                sd_rmdir(dir.c_str());
                continue;
            }

            for (const String& fname : files) {
                if (sent >= BACKFILL_FILES_PER_CYCLE) break;
                if (!mqtt_is_connected()) break;

                String path = dir + "/" + fname;
                String payload = sd_read_file(path.c_str());
                if (payload.length() == 0) { sd_remove(path.c_str()); continue; }

                // Lấy mutex trước khi gửi (doPublish đang giữ mutex thì chờ)
                if (xSemaphoreTake(s_pubMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                    bool ok = mqtt_publish_data(payload);
                    xSemaphoreGive(s_pubMutex);

                    if (ok) {
                        sd_remove(path.c_str());
                        sent++;
                        LOG_IF(LOG_DATA, "[BACKFILL] Sent %d: %s\n", sent, path.c_str());
                        if (sent < BACKFILL_FILES_PER_CYCLE)
                            vTaskDelay(pdMS_TO_TICKS(BACKFILL_FILE_GAP_MS));
                    } else {
                        LOGLN_IF(LOG_DATA, "[BACKFILL] Publish FAIL, abort cycle");
                        goto next_cycle;
                    }
                }
            }

            if (sd_list_dir(dir.c_str()).empty()) sd_rmdir(dir.c_str());
        }

        next_cycle:
        if (sent > 0)
            LOG_IF(LOG_DATA, "[BACKFILL] Cycle done: %d file(s) sent\n", sent);
    }
}

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
    String json;
    if (sd_exists("/config/analog.json")) json = sd_read_file("/config/analog.json");
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
    json = "";
    if (sd_exists("/config/encoder.json")) json = sd_read_file("/config/encoder.json");
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
    json = "";
    if (sd_exists("/config/di.json")) json = sd_read_file("/config/di.json");
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
    json = "";
    if (sd_exists("/config/rs485.json")) json = sd_read_file("/config/rs485.json");
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
    json = "";
    if (sd_exists("/config/tcp.json")) json = sd_read_file("/config/tcp.json");
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

    LOGLN_IF(LOG_DATA, "[DATA] Calc configs loaded");
}

// ============================================================
// Reset accumulators
// ============================================================

static void resetAccumulators() {
    memset(accSum, 0, sizeof(accSum));
    memset(accCnt, 0, sizeof(accCnt));
}

// ============================================================
// Poll ADC — mỗi 2s, tích lũy cho trung bình
// ============================================================

static void doAdcPoll() {
    if (!analogActive) return;

    analog_poll();
    for (uint8_t i = 0; i < ANALOG_CHANNELS; i++) {
        const AnalogChannel* ch = analog_get_channel(i);
        if (ch && ch->valid) {
            accSum[i] += ch->raw_count;
            accCnt[i]++;
        } else {
            LOG_IF(LOG_DATA, "[DATA] A%d: poll invalid (ch=%p valid=%d)\n",
                          i + 1, ch, ch ? ch->valid : -1);
        }
    }
}

// ============================================================
// Poll RS485 / TCP — mỗi 10s, chỉ cập nhật cache (không trung bình)
// ============================================================

static void doModbusPoll() {
    if (rtuActive) modbus_rtu_poll();
    if (tcpActive) modbus_tcp_poll();
}

// ============================================================
// Build JSON payload + publish / save offline
// ============================================================

static void doPublish() {
    String ts = ntp_rtc_get_datetime();
    if (ts.length() == 0) ts = "1970-01-01T00:00:00+07:00";

    JsonDocument doc;
    doc["ingest_type"] = "realtime";

    // ---- Analog (trung bình 30 mẫu) ----
    if (analogActive) {
        JsonObject grp = doc["analog"].to<JsonObject>();
        grp["ts"] = ts;
        for (uint8_t i = 0; i < ANALOG_CHANNELS; i++) {
            char key[4]; snprintf(key, sizeof(key), "A%d", i + 1);
            bool adsOk = (i < 4) ? analog_ads1_ok() : analog_ads2_ok();
            if (!adsOk) continue;

            if (accCnt[i] > 0) {
                float rawAvg = (float)(accSum[i] / accCnt[i]);
                float real = calcApply(calcCfg[CALC_ANALOG_OFF + i], rawAvg);
                JsonObject ch = grp[key].to<JsonObject>();
                ch["raw"] = (long)lroundf(rawAvg);
                ch["real"] = real;
                LOG_IF(LOG_DATA, "[DATA] %s: cnt=%d rawAvg=%.1f real=%.3f\n",
                              key, accCnt[i], rawAvg, real);
            } else {
                grp[key] = (char*)nullptr;
                LOG_IF(LOG_DATA, "[DATA] %s: accCnt=0 -> NULL (adsOk=%d)\n", key, adsOk);
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

    // ---- RS485 Bus 1 (giá trị cache từ lần poll gần nhất) ----
    uint8_t rtuN = rtuActive ? modbus_rtu_channel_count() : 0;
    if (rtuN > 0) {
        JsonObject grp = doc["rs485_1"].to<JsonObject>();
        grp["ts"] = ts;
        for (uint8_t i = 0; i < rtuN; i++) {
            const MbChannel* mch = modbus_rtu_get_channel(i);
            if (!mch) continue;
            if (mch->valid) {
                float real = calcApply(calcCfg[CALC_RS485_OFF + i], mch->value);
                JsonObject ch = grp[mch->name].to<JsonObject>();
                ch["raw"] = mch->value;
                ch["real"] = real;
            } else {
                grp[mch->name] = (char*)nullptr;
            }
        }
    }

    // ---- TCP (giá trị cache từ lần poll gần nhất) ----
    uint8_t tcpN = tcpActive ? modbus_tcp_channel_count() : 0;
    if (tcpN > 0) {
        JsonObject grp = doc["tcp"].to<JsonObject>();
        grp["ts"] = ts;
        for (uint8_t i = 0; i < tcpN; i++) {
            const TcpChannel* tch = modbus_tcp_get_channel(i);
            if (!tch) continue;
            if (tch->valid) {
                float real = calcApply(calcCfg[CALC_TCP_OFF + i], tch->value);
                JsonObject ch = grp[tch->name].to<JsonObject>();
                ch["raw"] = tch->value;
                ch["real"] = real;
            } else {
                grp[tch->name] = (char*)nullptr;
            }
        }
    }

    // ---- Serialize ----
    String payload;
    serializeJson(doc, payload);
    LOG_IF(LOG_DATA, "[DATA] Payload: %d bytes\n", payload.length());
    LOG_IF(LOG_DATA, "[DATA] JSON: %s\n", payload.c_str());

    // ---- Publish hoặc lưu offline ----
    bool ok = false;
    if (mqtt_is_connected()) {
        // Lấy mutex — block backfillTask trong khi gửi realtime
        if (xSemaphoreTake(s_pubMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            ok = mqtt_publish_data(payload);
            xSemaphoreGive(s_pubMutex);
        }
    }

    if (ok) {
        led_flash(0, 255, 0, 1);   // Xanh chớp 1 lần
        LOGLN_IF(LOG_DATA, "[DATA] Published OK");
    } else {
        led_flash(255, 0, 0, 2);   // Đỏ chớp 2 lần
        LOGLN_IF(LOG_DATA, "[DATA] Publish FAIL → save SD");

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
        LOGLN_IF(LOG_DATA, "[DATA] SD save skip: invalid time");
        return;
    }

    // "2026-04-07T14:35:00+07:00"
    String datePart = dt.substring(0, 10);
    String path = "/backfill/" + datePart + "/"
                + dt.substring(11, 13) + dt.substring(14, 16) + dt.substring(17, 19) + ".json";

    if (sd_write_file(path.c_str(), payload.c_str())) {
        LOG_IF(LOG_DATA, "[DATA] Saved: %s (%d bytes)\n", path.c_str(), payload.length());
    } else {
        LOG_IF(LOG_DATA, "[DATA] SD save FAILED: %s\n", path.c_str());
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

    LOG_IF(LOG_DATA, "[DATA] Active: AI=%d ENC=%d DI=%d RTU=%d TCP=%d\n",
                  analogActive, encoderActive, diActive, rtuActive, tcpActive);

    loadCalcConfigs();
    resetAccumulators();

    lastAdcPollMs = millis();
    lastPublishMs = millis();

    // Tạo FreeRTOS primitives
    s_mbEvent  = xEventGroupCreate();
    s_pubMutex = xSemaphoreCreateMutex();

    // Modbus poll task — Core 0, priority 3
    // (càng cao hơn loop() để xử lý nhanh khi được trigger)
    xTaskCreatePinnedToCore(modbusTask, "mb_poll", 8192, nullptr, 3, nullptr, 0);

    // Backfill task — bất kỳ core, priority 1
    xTaskCreate(backfillTask, "backfill", 8192, nullptr, 1, nullptr);

    // Trigger lần đầu ngay sau init
    xEventGroupSetBits(s_mbEvent, MB_BIT_TRIGGER);

    LOG_IF(LOG_DATA, "[DATA] Init OK, debug=%s, adc=%ds, publish=%ds\n",
                  debugMode ? "true" : "false",
                  ADC_POLL_INTERVAL_MS / 1000,
                  PUBLISH_INTERVAL_MS / 1000);
}

void data_collector_update() {
    unsigned long now = millis();

    // ADC poll mỗi 2s → 30 mẫu/phút (tích lũy trung bình)
    // Không bị block bởi Modbus vì modbusTask chạy trên Core 0
    if (now - lastAdcPollMs >= ADC_POLL_INTERVAL_MS) {
        lastAdcPollMs = now;
        doAdcPoll();
    }

    // Debug mode: gửi ngay sau ADC poll
    if (debugMode && (now - lastAdcPollMs < 50)) {
        doPublish();
        return;
    }

    // Normal mode: publish mỗi 60s
    // Chờ Modbus poll xong trước khi gửi (data luôn fresh)
    if (!debugMode && now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
        lastPublishMs = now;

        // Chờ modbusTask xong (timeout = MODBUS_POLL_TIMEOUT_MS)
        // Nếu timeout: vẫn publish với data cũ (valid flag = false cho kênh lỗi)
        if (rtuActive || tcpActive) {
            xEventGroupWaitBits(s_mbEvent, MB_BIT_DONE,
                                pdTRUE,   // xóa bit sau khi đọc
                                pdTRUE,   // chờ tất cả bit set
                                pdMS_TO_TICKS(MODBUS_POLL_TIMEOUT_MS));
        }

        doPublish();

        // Trigger chu kỳ Modbus mới ngay sau khi publish
        xEventGroupSetBits(s_mbEvent, MB_BIT_TRIGGER);
    }
}

void data_collector_reload_calc() {
    LOGLN_IF(LOG_DATA, "[DATA] Reloading calc configs...");
    loadCalcConfigs();
    LOGLN_IF(LOG_DATA, "[DATA] Reload done");
}

void data_collector_set_debug(bool on) {
    debugMode = on;
    LOG_IF(LOG_DATA, "[DATA] Debug mode: %s\n", on ? "ON" : "OFF");
}

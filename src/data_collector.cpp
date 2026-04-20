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
#include "webserver.h"
#include "error_log.h"
#include <ArduinoJson.h>

// ============================================================
// Timing
// ============================================================
#define ADC_POLL_INTERVAL_MS    4000   // 4s → 15 mẫu/phút
#define PUBLISH_INTERVAL_MS     60000  // 60s mỗi lần gửi

// Modbus poll timeout — thời gian tối đa chờ 1 chu kỳ poll hoàn thành
// modbusTask poll liên tục, DONE set sau mỗi chu kỳ
// Normal mode xEventGroupWaitBits() với timeout này trước khi publish
// RTU: 20 × (300ms timeout + 50ms gap) = 7,000ms
// TCP: 5 host × (500ms connect + 4ch×400ms read) = 10,500ms
// Total worst case: ~17.5s → set 25s để có margin
#define MODBUS_POLL_TIMEOUT_MS  25000

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
// Non-blocking DONE wait: khi đến giờ publish mà poll chưa xong, set flag và tiếp tục loop()
// → mqtt_update(), led_update() vẫn chạy bình thường thay vì bị block 25s
static bool s_waitingForDone = false;
// Trigger timer độc lập cho debug mode (tránh trigger liên tục mỗi 4s)
static unsigned long s_lastDebugTriggerMs = 0;

// FreeRTOS sync primitives
static EventGroupHandle_t s_mbEvent   = nullptr;
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
static void saveToSd(const String& payload, const String& ts);

// ============================================================
// Modbus poll task (Core 0, priority 3)
// — Chờ trigger từ data_collector_update(), poll xong → DONE, rồi block lại
// — Block nhiều (chờ trigger) → net4g_task (priority 2) không bị starve
// — Tại thời điểm publish, modbusTask đang BLOCK → không đụng vào channels[]
//   → không có data race
// ============================================================
static void modbusTask(void* pv) {
    for (;;) {
        // Chờ trigger (block vô hạn) — nhường CPU cho net4g_task khi rảnh
        xEventGroupWaitBits(s_mbEvent, MB_BIT_TRIGGER, pdTRUE, pdTRUE, portMAX_DELAY);

        LOG_IF(LOG_DATA, "[DATA] Modbus poll start\n");
        if (rtuActive) modbus_rtu_poll();
        if (tcpActive) modbus_tcp_poll();
        LOG_IF(LOG_DATA, "[DATA] Modbus poll done\n");

        xEventGroupSetBits(s_mbEvent, MB_BIT_DONE);
        // Không tự re-trigger: chờ data_collector_update() fire trigger sau mỗi publish
        // → an toàn: khi main đọc channels[] thì modbusTask đang block
    }
}

// ============================================================
// Backfill drain — chạy inline trong data_collector_update()
// Không dùng FreeRTOS task → hoàn toàn single-threaded với mqtt_update()
// → không có race condition TinyGsmClient UART, không cần mutex
// — Gửi tối đa 1 file/chu kỳ, gọi mỗi BACKFILL_INTERVAL_MS
// ============================================================
#define BACKFILL_INTERVAL_MS   15000
#define BACKFILL_MAX_DATE_DIRS 3    // Giữ tối đa 3 ngày backfill (~4320 files)
                                    // Nếu offline lâu hơn → xóa ngày cũ nhất để tránh
                                    // sd_list_dir() chậm và SD đầy

static unsigned long lastBackfillMs = 0;

// Stuck-file guard: xóa file backfill nếu fail liên tiếp ≥ 3 lần
// Nguyên nhân: TinyGsmClient AT glitch, WiFiClient drop ngay sau connected() check,
// hay bất kỳ trạng thái nào khiến mqtt_publish_data() fail mãi dù mqtt_is_connected() = true
static String   s_bfFailPath;
static uint8_t  s_bfFailCount = 0;

// Xóa thư mục backfill cũ nhất nếu có quá nhiều ngày
static void pruneBackfillDirs() {
    auto dates = sd_list_dir("/backfill");
    while ((int)dates.size() > BACKFILL_MAX_DATE_DIRS) {
        // dates đã sắp xếp tăng dần → dates[0] là cũ nhất
        String oldest = "/backfill/" + dates[0];
        auto files = sd_list_dir(oldest.c_str());
        for (const String& f : files) {
            sd_remove((oldest + "/" + f).c_str());
        }
        sd_rmdir(oldest.c_str());
        Serial.printf("[BACKFILL] Pruned oldest dir: %s (%d files)\n",
                      oldest.c_str(), (int)files.size());
        dates.erase(dates.begin());
    }
}

static void doBackfillCycle() {
    if (webserver_is_running()) return;
    if (!mqtt_is_connected()) return;

    auto dates = sd_list_dir("/backfill");
    if (dates.empty()) return;

    for (const String& date : dates) {
        String dir = "/backfill/" + date;
        auto files = sd_list_dir(dir.c_str());

        if (files.empty()) {
            sd_rmdir(dir.c_str());
            continue;
        }

        for (const String& fname : files) {
            if (!mqtt_is_connected()) return;

            String path = dir + "/" + fname;
            String payload = sd_read_file(path.c_str());
            if (payload.length() == 0) { sd_remove(path.c_str()); continue; }

            bool ok = mqtt_publish_data(payload);
            if (ok) {
                sd_remove(path.c_str());
                s_bfFailPath  = "";
                s_bfFailCount = 0;
                LOG_IF(LOG_DATA, "[BACKFILL] Sent: %s\n", path.c_str());
                if (sd_list_dir(dir.c_str()).empty()) sd_rmdir(dir.c_str());
            } else {
                LOGLN_IF(LOG_DATA, "[BACKFILL] Publish FAIL, abort");
                err_log("BACKFILL", "Publish FAIL: " + path);
                // Stuck-file guard: cùng path fail ≥ 3 lần liên tiếp → xóa tránh block mãi
                // WiFi: TCP drop sau connected() check | 4G: AT glitch modem buffer
                if (path == s_bfFailPath) {
                    if (++s_bfFailCount >= 3) {
                        Serial.printf("[BACKFILL] Skip stuck file (%d fails): %s\n",
                                      s_bfFailCount, path.c_str());
                        err_log("BACKFILL", "Skip stuck: " + path);
                        sd_remove(path.c_str());
                        s_bfFailPath  = "";
                        s_bfFailCount = 0;
                    }
                } else {
                    s_bfFailPath  = path;
                    s_bfFailCount = 1;
                }
            }
            // Gửi 1 file/chu kỳ, trả quyền về loop() để mqtt_update() chạy
            return;
        }
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
                if (chs["DI4"].is<JsonObject>())
                    loadOneCalc(calcCfg[CALC_DI_OFF], chs["DI4"]);
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
            int16_t cnt = counter_get(i);
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
        JsonObject ch = grp["DI4"].to<JsonObject>();
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
        // mqtt_publish_data() tự guard bằng s_mqttMutex bên trong mqtt_client
        ok = mqtt_publish_data(payload);
    }

    if (ok) {
        led_flash(0, 255, 0, 1);   // Xanh chớp 1 lần
        LOGLN_IF(LOG_DATA, "[DATA] Published OK");
    } else {
        led_flash(255, 0, 0, 2);   // Đỏ chớp 2 lần
        LOGLN_IF(LOG_DATA, "[DATA] Publish FAIL → save SD");
        err_log("MQTT", "Publish FAIL ts=" + ts);

        // Đổi ingest_type sang backfill rồi lưu
        doc["ingest_type"] = "backfill";
        String backfill;
        serializeJson(doc, backfill);
        saveToSd(backfill, ts);
    }

    resetAccumulators();
}

// ============================================================
// Lưu offline buffer vào SD
// ============================================================

static void saveToSd(const String& payload, const String& ts) {
    // Dùng chính ts đã ghi trong payload để đặt tên file
    // → đảm bảo filename và JSON ts luôn khớp, tránh ts nhảy khi backfill
    if (ts.length() < 19 || ts.startsWith("1970")) {
        LOGLN_IF(LOG_DATA, "[DATA] SD save skip: clock not synced");
        return;
    }

    // Giới hạn số ngày backfill trước khi lưu thêm
    pruneBackfillDirs();

    // ts format: "2026-04-07T14:35:00+07:00"
    String datePart = ts.substring(0, 10);
    String path = "/backfill/" + datePart + "/"
                + ts.substring(11, 13) + ts.substring(14, 16) + ts.substring(17, 19) + ".json";

    if (sd_write_file(path.c_str(), payload.c_str())) {
        LOG_IF(LOG_DATA, "[DATA] Saved: %s (%d bytes)\n", path.c_str(), payload.length());
    } else {
        LOG_IF(LOG_DATA, "[DATA] SD save FAILED: %s\n", path.c_str());
        err_log("SD", "Save FAILED: " + path);
    }
}

// ============================================================
// Public API
// ============================================================

void data_collector_init(bool use4G) {
    // Xác định module nào active
    analogActive  = analog_ads1_ok() || analog_ads2_ok();
    encoderActive = true;   // EN1 quadrature (IO1=A, IO2=B)
    diActive      = true;   // Rain gauge (IO40)
    rtuActive     = modbus_rtu_channel_count() > 0;
    tcpActive     = modbus_tcp_channel_count() > 0;

    LOG_IF(LOG_DATA, "[DATA] Active: AI=%d ENC=%d DI=%d RTU=%d TCP=%d\n",
                  analogActive, encoderActive, diActive, rtuActive, tcpActive);

    loadCalcConfigs();
    resetAccumulators();

    lastAdcPollMs  = millis();
    lastPublishMs  = millis();
    lastBackfillMs = millis();  // realtimeImminent guard bảo vệ slot realtime đầu tiên
                               // (millis() + PUBLISH_INTERVAL_MS bị unsigned wrap → fire ngay anyway)

    // Tạo FreeRTOS primitives
    s_mbEvent  = xEventGroupCreate();

    // Modbus poll task — Core 0, priority 2 (= net4g_task)
    // Priority 3 cũ → net4g_task (prio 2) bị starve khi ModbusMaster busy-wait RTU
    // Priority 2: round-robin với net4g → cả hai đều chạy được
    xTaskCreatePinnedToCore(modbusTask, "mb_poll", 8192, nullptr, 2, nullptr, 0);

    // Backfill chạy inline trong data_collector_update() — không cần task riêng

    // Trigger lần đầu ngay sau init — bắt đầu poll cycle 1
    xEventGroupSetBits(s_mbEvent, MB_BIT_TRIGGER);

    LOG_IF(LOG_DATA, "[DATA] Init OK, debug=%s, adc=%ds, publish=%ds\n",
                  debugMode ? "true" : "false",
                  ADC_POLL_INTERVAL_MS / 1000,
                  PUBLISH_INTERVAL_MS / 1000);
}

void data_collector_update() {
    unsigned long now = millis();

    // ADC poll mỗi 4s → 15 mẫu/phút (tích lũy trung bình)
    if (now - lastAdcPollMs >= ADC_POLL_INTERVAL_MS) {
        lastAdcPollMs = now;
        doAdcPoll();
    }

    // ── Debug mode ─────────────────────────────────────────────────────────────
    // Trigger modbus mỗi MODBUS_POLL_TIMEOUT_MS, publish SAU KHI DONE set
    // → đảm bảo modbus data luôn valid trước khi doPublish()
    if (debugMode) {
        if (rtuActive || tcpActive) {
            EventBits_t bits = xEventGroupGetBits(s_mbEvent);
            if (bits & MB_BIT_DONE) {
                // Poll xong → publish ngay, rồi trigger lại
                xEventGroupClearBits(s_mbEvent, MB_BIT_DONE);
                doPublish();
                // Trigger chu kỳ poll mới
                xEventGroupSetBits(s_mbEvent, MB_BIT_TRIGGER);
                s_lastDebugTriggerMs = now;
                LOG_IF(LOG_DATA, "[DATA] Debug: poll done → published, re-trigger\n");
            }
            // DONE chưa set → modbusTask đang poll → không publish, chờ vòng tiếp
        } else {
            // Không có modbus → publish theo ADC (như cũ)
            if (now - lastAdcPollMs < 50) {
                doPublish();
            }
        }
        return;
    }

    // ── Normal mode: publish mỗi PUBLISH_INTERVAL_MS ────────────────────────────
    if (now - lastPublishMs >= PUBLISH_INTERVAL_MS || s_waitingForDone) {

        if (rtuActive || tcpActive) {
            // NON-BLOCKING check DONE — KHÔNG block loop() để mqtt_update() vẫn chạy
            // Nếu DONE chưa set: set flag, return — thử lại vòng lặp tiếp theo (~1ms)
            EventBits_t bits = xEventGroupGetBits(s_mbEvent);
            if (!(bits & MB_BIT_DONE)) {
                if (!s_waitingForDone) {
                    s_waitingForDone = true;
                    LOG_IF(LOG_DATA, "[DATA] Waiting for modbus DONE (non-blocking)...\n");
                }
                return;  // quay lại loop() → mqtt_update() chạy bình thường
            }
            // DONE set: xóa bit, tiến hành publish
            xEventGroupClearBits(s_mbEvent, MB_BIT_DONE);
        }

        // Tại đây modbusTask đang BLOCK (chờ TRIGGER mới) → không đụng channels[] → an toàn
        lastPublishMs = now;
        s_waitingForDone = false;
        doPublish();
        lastBackfillMs = now;

        // Trigger chu kỳ poll mới ngay sau publish
        if (rtuActive || tcpActive) {
            xEventGroupSetBits(s_mbEvent, MB_BIT_TRIGGER);
        }
    }

    // Backfill: chỉ chạy khi "rảnh" — không tranh với cửa sổ realtime sắp đến
    if (now - lastBackfillMs >= BACKFILL_INTERVAL_MS) {
        lastBackfillMs = now;
        unsigned long elapsed = now - lastPublishMs;
        bool realtimeImminent = (elapsed < PUBLISH_INTERVAL_MS) &&
                                ((PUBLISH_INTERVAL_MS - elapsed) <= BACKFILL_INTERVAL_MS);
        if (!realtimeImminent) {
            doBackfillCycle();
        }
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

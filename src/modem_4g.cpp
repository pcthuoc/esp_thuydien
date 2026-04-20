#include "modem_4g.h"
#include "error_log.h"
#include "debug_config.h"
#include <time.h>

// ============================================================
// TinyGSM setup — phải define TRƯỚC khi include
// ============================================================
#define TINY_GSM_MODEM_SIM7600
#define TINY_GSM_RX_BUFFER 2048
#define SerialMon Serial
#define SerialAT  Serial1
#include <TinyGsmClient.h>

// ============================================================
// UART pins (theo schematic A7680C)
// TX = GPIO15 → SIM RXD
// RX = GPIO16 ← SIM TXD
// Không có chân RST phần cứng
// ============================================================
#define MODEM_TX 15
#define MODEM_RX 16
#define MODEM_BAUD 115200

// ============================================================
// State
// ============================================================
static bool             s_initialized  = false;
static volatile bool    s_gprsFlag     = false;  // đọc bởi Core 1 — KHÔNG dùng AT
static TinyGsm          s_modem(SerialAT);
static TinyGsmClient    s_client(s_modem);
static String           s_apn = "";
static void (*s_tick_cb)() = nullptr;
static volatile bool    s_abort = false;

void modem_4g_set_tick_cb(void (*cb)()) {
    s_tick_cb = cb;
}

void modem_4g_abort() {
    s_abort = true;
}

static inline void _tick() {
    if (s_tick_cb) s_tick_cb();
}

// ============================================================
// Public API
// ============================================================

bool modem_4g_init(const char* apn, const char* pin) {
    s_abort = false;  // reset trước mỗi lần init
    s_apn = apn ? apn : "";

    SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(100);

    Serial.println("[4G] Khởi động modem A7680C...");
    s_modem.restart();
    delay(1000);

    String modemInfo = s_modem.getModemInfo();
    Serial.printf("[4G] Modem info: %s\n", modemInfo.c_str());

    if (pin && strlen(pin) > 0) {
        if (!s_modem.simUnlock(pin)) {
            Serial.println("[4G] SIM unlock FAILED");
            err_log("4G", "SIM unlock FAILED");
            return false;
        }
    }

    Serial.print("[4G] Chờ đăng ký mạng");
    for (int i = 0; i < 60; i++) {
        if (s_modem.isNetworkConnected()) break;
        delay(1000);
        _tick();
        Serial.print(".");
    }
    Serial.println();
    if (s_abort) {
        Serial.println("[4G] Init hủy bỏ (AP mode requested)");
        return false;
    }
    if (!s_modem.isNetworkConnected()) {
        Serial.println("[4G] Network TIMEOUT");
        err_log("4G", "Network attach TIMEOUT");
        return false;
    }

    Serial.printf("[4G] Operator: %s  RSSI: %d\n",
        s_modem.getOperator().c_str(), s_modem.getSignalQuality());

    if (!s_modem.gprsConnect(s_apn.c_str())) {
        Serial.println("[4G] GPRS connect FAILED");
        err_log("4G", "GPRS connect FAILED");
        return false;
    }

    Serial.printf("[4G] GPRS OK! IP: %s\n", modem_4g_ip().c_str());
    err_log("4G", "GPRS connected, IP=" + modem_4g_ip());

    s_initialized = true;
    s_gprsFlag    = true;  // init thành công → GPRS sẵn sàng
    return true;
}

bool modem_4g_is_connected() {
    // Đọc flag — không gọi AT command.
    // Flag được set bởi modem_4g_init/reconnect và net4g_task.
    return s_initialized && s_gprsFlag;
}

void modem_4g_set_gprs_flag(bool connected) {
    s_gprsFlag = connected;
}

bool modem_4g_reconnect() {
    s_abort = false;  // reset — abort chỉ có hiệu lực trong 1 lần gọi
    Serial.println("[4G] Reconnecting...");

    s_modem.gprsDisconnect();
    delay(500);

    // Chờ network trước khi reconnect GPRS
    for (int i = 0; i < 30; i++) {
        if (s_modem.isNetworkConnected()) break;
        delay(1000);
        _tick();
    }
    if (s_abort) {
        Serial.println("[4G] Reconnect hủy bỏ (AP mode requested)");
        return false;
    }
    if (!s_modem.isNetworkConnected()) {
        err_log("4G", "Reconnect: no network");
        return false;
    }

    if (!s_modem.gprsConnect(s_apn.c_str())) {
        err_log("4G", "Reconnect: GPRS FAILED");
        s_gprsFlag = false;
        return false;
    }

    s_gprsFlag = true;  // reconnect thành công
    err_log("4G", "Reconnected, IP=" + modem_4g_ip());
    return true;
}

int modem_4g_rssi() {
    return s_modem.getSignalQuality();
}

String modem_4g_operator() {
    return s_modem.getOperator();
}

String modem_4g_ip() {
    return s_modem.getLocalIP();
}

Client* modem_4g_get_client() {
    return &s_client;
}

// ============================================================
// Sync thời gian qua AT+CCLK
// ============================================================
bool modem_4g_sync_time() {
    String cclk = s_modem.getGSMDateTime(DATE_FULL);
    LOG_IF(LOG_4G, "[4G-TIME] AT+CCLK: %s\n", cclk.c_str());

    if (cclk.length() < 17) {
        LOG_IF(LOG_4G, "[4G-TIME] CCLK response quá ngắn\n");
        return false;
    }

    // Format: "YY/MM/DD,HH:MM:SS+TZ"
    int yy, mo, dd, hh, mm, ss;
    if (sscanf(cclk.c_str(), "%d/%d/%d,%d:%d:%d", &yy, &mo, &dd, &hh, &mm, &ss) != 6) {
        LOG_IF(LOG_4G, "[4G-TIME] CCLK parse FAILED\n");
        return false;
    }

    struct tm t = {};
    t.tm_year = 2000 + yy - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = dd;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;

    time_t epoch = mktime(&t);
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    LOG_IF(LOG_4G, "[4G-TIME] Set time: %04d-%02d-%02d %02d:%02d:%02d\n",
        2000+yy, mo, dd, hh, mm, ss);
    return true;
}

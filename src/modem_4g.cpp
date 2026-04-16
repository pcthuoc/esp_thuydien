#include "modem_4g.h"
#include "error_log.h"
#include "debug_config.h"
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// TinyGSM — define trước include (fallback nếu chưa define qua build_flags)
#ifndef TINY_GSM_MODEM_SIM7600
#define TINY_GSM_MODEM_SIM7600
#endif
#include <TinyGsmClient.h>

// ============================================================
// UART pins (theo schematic A7680C)
// ============================================================
#define MODEM_RX_PIN   16   // GPIO16 ← SIM TXD
#define MODEM_TX_PIN   15   // GPIO15 → SIM RXD
#define MODEM_BAUD     115200

// ============================================================
// Objects (static — dùng nội bộ module)
// ============================================================
static HardwareSerial simSerial(2);        // UART2
static TinyGsm        modem(simSerial);
static TinyGsmClient  gsmClient(modem);

static bool             s_initialized = false;
static String           s_apn         = "";   // lưu lại để reconnect
static SemaphoreHandle_t s_atMutex    = nullptr;  // bảo vệ UART AT commands khỏi race condition

// ============================================================
// Public API
// ============================================================

bool modem_4g_init(const char* apn, const char* pin) {
    s_apn = apn ? apn : "";
    if (!s_atMutex) s_atMutex = xSemaphoreCreateMutex();
    simSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(300);

    Serial.printf("[4G] UART2: RX=GPIO%d TX=GPIO%d Baud=%d\n",
                  MODEM_RX_PIN, MODEM_TX_PIN, MODEM_BAUD);
    Serial.println("[4G] Khởi động modem A7680C (TinyGSM/SIM7600)...");

    // Gửi thô "AT\r\n" và đọc lại để kiểm tra UART trước TinyGSM
    Serial.println("[4G] Test raw AT...");
    simSerial.print("AT\r\n");
    delay(500);
    String rawResp = "";
    while (simSerial.available()) rawResp += (char)simSerial.read();
    Serial.printf("[4G] Raw response: '%s'\n", rawResp.c_str());
    if (rawResp.length() == 0) {
        Serial.println("[4G] CẢNH BÁO: Không nhận được gì — có thể sai chân TX/RX hoặc baud rate!");
    }

    // Bỏ qua restart() — không có chân RESET phần cứng, dùng init() trực tiếp
    Serial.println("[4G] Gọi modem.init()...");
    if (!modem.init()) {
        Serial.println("[4G] Không liên lạc được modem! Kiểm tra TX/RX và baud rate.");
        err_log("4G", "Modem init FAILED");
        return false;
    }

    // In thông tin modem
    String model = modem.getModemModel();
    String imei  = modem.getIMEI();
    Serial.printf("[4G] Model: %s | IMEI: %s\n", model.c_str(), imei.c_str());

    // Mở khóa SIM nếu có PIN
    if (pin && strlen(pin) > 0) {
        if (!modem.simUnlock(pin)) {
            Serial.println("[4G] SIM PIN sai!");
            err_log("4G", "SIM PIN incorrect");
            return false;
        }
    }

    // Chờ đăng ký mạng (tối đa 60s)
    Serial.print("[4G] Chờ mạng");
    if (!modem.waitForNetwork(60000L)) {
        Serial.println(" TIMEOUT!");
        err_log("4G", "Network wait TIMEOUT");
        return false;
    }
    Serial.println(" OK");

    // Kết nối GPRS
    Serial.printf("[4G] Kết nối GPRS APN='%s'...\n", apn ? apn : "");
    if (!modem.gprsConnect(apn ? apn : "")) {
        Serial.println("[4G] GPRS kết nối thất bại!");
        err_log("4G", "GPRS connect FAILED");
        return false;
    }

    Serial.printf("[4G] OK! IP: %s\n",    modem_4g_ip().c_str());
    Serial.printf("[4G] Operator: %s\n",  modem_4g_operator().c_str());
    Serial.printf("[4G] RSSI: %d dBm\n",  modem_4g_rssi());
    Serial.printf("[4G] SIM ICCID: %s\n", modem.getSimCCID().c_str());

    // Lấy số điện thoại qua AT+CNUM (dùng TinyGSM sendAT để tránh conflict buffer)
    modem.sendAT(GF("+CNUM"));
    String cnumResp = "";
    if (modem.waitResponse(3000L, cnumResp) == 1 || cnumResp.length() > 0) {
        cnumResp.trim();
        Serial.printf("[4G] Phone Number (AT+CNUM): %s\n", cnumResp.c_str());
    } else {
        Serial.println("[4G] Phone Number: không lấy được (SIM chưa lưu số)");
    }
    err_log("4G", "Connected, IP=" + modem_4g_ip());

    s_initialized = true;
    return true;
}

bool modem_4g_is_connected() {
    return s_initialized && modem.isGprsConnected();
}

bool modem_4g_reconnect() {
    Serial.printf("[4G] Reconnecting (APN='%s')...\n", s_apn.c_str());

    // Disconnect GPRS sạch trước để modem reset state
    modem.gprsDisconnect();
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Kiểm tra đăng ký mạng — nếu mất sóng thì chờ lại (tối đa 30s)
    // Bỏ qua bước này là nguyên nhân chính GPRS reconnect "OK" nhưng drop ngay
    if (!modem.isNetworkConnected()) {
        Serial.println("[4G] Chờ đăng ký mạng...");
        if (!modem.waitForNetwork(30000L)) {
            Serial.println("[4G] Network registration TIMEOUT");
            err_log("4G", "Reconnect FAILED: no network");
            return false;
        }
    }

    if (!modem.gprsConnect(s_apn.c_str())) {
        err_log("4G", "Reconnect FAILED: gprsConnect");
        return false;
    }

    // Lấy IP thực từ modem (bỏ prefix "+IPADDR: " nếu có)
    String ip = modem.getLocalIP();
    ip.trim();
    if (ip.startsWith("+")) {
        int colon = ip.indexOf(": ");
        if (colon >= 0) ip = ip.substring(colon + 2);
        ip.trim();
    }
    err_log("4G", "Reconnected, IP=" + ip);
    return true;
}

int modem_4g_rssi() {
    return modem.getSignalQuality();
}

String modem_4g_operator() {
    return modem.getOperator();
}

String modem_4g_ip() {
    String ip = modem.getLocalIP();
    ip.trim();
    // A7680C trả "+IPADDR: x.x.x.x" thay vì chỉ IP
    if (ip.startsWith("+")) {
        int colon = ip.indexOf(": ");
        if (colon >= 0) ip = ip.substring(colon + 2);
        ip.trim();
    }
    return ip;
}

Client* modem_4g_get_client() {
    return &gsmClient;
}

// ============================================================
// Sync system clock từ AT+CCLK? của modem
// ============================================================
bool modem_4g_sync_time() {
    LOG_IF(LOG_4G, "\n[4G-TIME] === Bắt đầu sync thời gian từ mạng 4G ===\n");

    if (!s_initialized) {
        LOG_IF(LOG_4G, "[4G-TIME] SKIP — modem chưa init\n");
        return false;
    }

    // Lấy AT mutex để tránh xung đột UART với TinyGsmClient đang gửi MQTT
    // (race condition AT command ↔ MQTT packet → garbled response → lid invalid → mất kết nối)
    if (!s_atMutex || xSemaphoreTake(s_atMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        LOG_IF(LOG_4G, "[4G-TIME] SKIP — không lấy được AT mutex (UART bận)\n");
        return false;
    }

    int   year = 0, month = 0, day = 0;
    int   hour = 0, minute = 0, second = 0;
    float timezone = 0.0f;

    LOG_IF(LOG_4G, "[4G-TIME] Gửi AT+CCLK? tới modem...\n");
    bool ok = modem.getNetworkTime(&year, &month, &day,
                                   &hour, &minute, &second, &timezone);

    if (!ok) {
        LOG_IF(LOG_4G, "[4G-TIME] FAILED — modem không trả về thời gian\n");
        LOG_IF(LOG_4G, "[4G-TIME] Có thể: chưa sync với mạng, sóng yếu, hoặc SIM chưa kết nối\n");
        xSemaphoreGive(s_atMutex);
        return false;
    }

    LOG_IF(LOG_4G,
        "[4G-TIME] Raw modem: %04d-%02d-%02d %02d:%02d:%02d  timezone_raw=%.2f\n",
        year, month, day, hour, minute, second, timezone);

    // TinyGSM SIM7600 với board này trả timezone đã là giờ thực (vd: 7.0 = UTC+7)
    // Modem báo giờ local UTC+7, trừ đi timezone để ra UTC
    float tzHours = timezone;
    LOG_IF(LOG_4G, "[4G-TIME] timezone=%.2f giờ → trừ %.0f giờ để ra UTC\n", tzHours, tzHours);

    // Kiểm tra năm hợp lệ
    if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31) {
        LOG_IF(LOG_4G, "[4G-TIME] INVALID date: %04d-%02d-%02d — bỏ qua\n", year, month, day);
        return false;
    }

    // Tạo struct tm từ local time modem báo (đã có TZ offset)
    struct tm localTm = {};
    localTm.tm_year  = year - 1900;
    localTm.tm_mon   = month - 1;
    localTm.tm_mday  = day;
    localTm.tm_hour  = hour;
    localTm.tm_min   = minute;
    localTm.tm_sec   = second;
    localTm.tm_isdst = 0;

    // mktime() trong UTC context → ra epoch của local time
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t localEpoch  = mktime(&localTm);
    long   tzOffsetSec = (long)(tzHours * 3600.0f);
    time_t utcEpoch    = localEpoch - tzOffsetSec;

    LOG_IF(LOG_4G,
        "[4G-TIME] localEpoch=%ld  tzOffsetSec=%ld  utcEpoch=%ld\n",
        (long)localEpoch, tzOffsetSec, (long)utcEpoch);

    // Set system clock (UTC)
    struct timeval tv = { .tv_sec = utcEpoch, .tv_usec = 0 };
    int ret = settimeofday(&tv, NULL);
    LOG_IF(LOG_4G, "[4G-TIME] settimeofday() = %d (%s)\n",
           ret, ret == 0 ? "OK" : "FAILED");

    // Khôi phục TZ Vietnam UTC+7
    setenv("TZ", "UTC-7", 1);
    tzset();

    // Verify: đọc lại system clock và in ra
    struct tm verifyTm;
    getLocalTime(&verifyTm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &verifyTm);
    LOG_IF(LOG_4G, "[4G-TIME] Verify system clock: %s (UTC+7)\n", buf);
    LOG_IF(LOG_4G, "[4G-TIME] === Sync 4G %s ===\n\n", ret == 0 ? "OK" : "FAILED");

    xSemaphoreGive(s_atMutex);
    return (ret == 0);
}

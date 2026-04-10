#include "ntp_rtc.h"
#include <Wire.h>
#include <WiFi.h>
#include <time.h>

// ============================================================
// Config
// ============================================================
#define DS3231_ADDR  0x68
#define I2C_SDA      5
#define I2C_SCL      4

#define NTP_SERVER1  "pool.ntp.org"
#define NTP_SERVER2  "time.google.com"
#define GMT_OFFSET   (7 * 3600)   // UTC+7 Vietnam
#define DST_OFFSET   0

// Sync intervals
static const unsigned long SYNC_OK_INTERVAL   = 3600000;  // 1 giờ sau lần sync thành công
static const unsigned long SYNC_FAIL_INTERVAL  = 60000;    // 1 phút nếu sync thất bại

static bool rtcFound = false;
static bool ntpSynced = false;
static unsigned long lastSyncAttempt = 0;

// ============================================================
// BCD helpers
// ============================================================
static uint8_t bcd2dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec2bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

// ============================================================
// DS3231 low-level
// ============================================================

// Đọc 7 registers (sec, min, hour, dow, date, month, year)
static bool ds3231_read(struct tm* t) {
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;

    Wire.requestFrom((uint8_t)DS3231_ADDR, (uint8_t)7);
    if (Wire.available() < 7) return false;

    t->tm_sec  = bcd2dec(Wire.read() & 0x7F);
    t->tm_min  = bcd2dec(Wire.read());
    t->tm_hour = bcd2dec(Wire.read() & 0x3F);  // 24h mode
    Wire.read();                                 // day of week — bỏ qua
    t->tm_mday = bcd2dec(Wire.read());
    t->tm_mon  = bcd2dec(Wire.read() & 0x1F) - 1;  // tm_mon: 0-11
    t->tm_year = bcd2dec(Wire.read()) + 100;         // tm_year: years since 1900
    t->tm_isdst = 0;
    return true;
}

// Ghi thời gian vào DS3231
static bool ds3231_write(const struct tm* t) {
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(0x00);
    Wire.write(dec2bcd(t->tm_sec));
    Wire.write(dec2bcd(t->tm_min));
    Wire.write(dec2bcd(t->tm_hour));
    Wire.write(0x00);                       // day of week
    Wire.write(dec2bcd(t->tm_mday));
    Wire.write(dec2bcd(t->tm_mon + 1));
    Wire.write(dec2bcd(t->tm_year - 100));
    return Wire.endTransmission() == 0;
}

// Kiểm tra + xóa cờ OSF (Oscillator Stop Flag)
static bool ds3231_checkOsf() {
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(0x0F);  // Status register
    if (Wire.endTransmission() != 0) return false;

    Wire.requestFrom((uint8_t)DS3231_ADDR, (uint8_t)1);
    if (!Wire.available()) return false;

    uint8_t status = Wire.read();
    if (status & 0x80) {
        // OSF đã set → oscillator bị dừng (mất pin?) → xóa cờ
        Wire.beginTransmission(DS3231_ADDR);
        Wire.write(0x0F);
        Wire.write(status & ~0x80);
        Wire.endTransmission();
        Serial.println("[RTC] OSF flag cleared — battery may have died");
        return false;
    }
    return true;  // Oscillator OK
}

// ============================================================
// Public API
// ============================================================

void i2c_init() {
    Wire.begin(I2C_SDA, I2C_SCL);
    Serial.printf("[I2C] Init: SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
}

bool ntp_rtc_init() {
    // Kiểm tra DS3231 có trên bus không
    Wire.beginTransmission(DS3231_ADDR);
    rtcFound = (Wire.endTransmission() == 0);

    if (!rtcFound) {
        Serial.println("[RTC] DS3231 NOT found");
        return false;
    }

    Serial.println("[RTC] DS3231 found (dự phòng)");
    bool osfOk = ds3231_checkOsf();

    // Đọc RTC → set system clock tạm (sẽ bị NTP ghi đè khi có mạng)
    struct tm rtcTime;
    if (ds3231_read(&rtcTime)) {
        // RTC lưu giờ UTC → mktime phải chạy trong context UTC
        // (trước khi configTime được gọi, TZ có thể chưa set đúng)
        setenv("TZ", "UTC0", 1);
        tzset();
        time_t epoch = mktime(&rtcTime);  // UTC struct tm → UTC epoch

        // Đặt timezone Việt Nam UTC+7 (POSIX: UTC-7 = UTC+7)
        setenv("TZ", "UTC-7", 1);
        tzset();

        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);

        // Log giờ local (UTC+7) để kiểm tra
        struct tm localTm;
        getLocalTime(&localTm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &localTm);
        Serial.printf("[RTC] Clock tạm từ RTC: %s (UTC+7)%s\n", buf,
            osfOk ? "" : " (OSF — có thể sai)");
    } else {
        Serial.println("[RTC] Failed to read DS3231");
    }

    return true;
}

bool ntp_rtc_sync_ntp() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NTP] No WiFi, skip");
        return false;
    }

    lastSyncAttempt = millis();
    Serial.println("[NTP] Đang sync...");

    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER1, NTP_SERVER2);

    // Chờ tối đa 5 giây
    struct tm timeinfo;
    int retry = 10;
    while (!getLocalTime(&timeinfo, 500) && retry > 0) {
        retry--;
    }

    if (retry <= 0) {
        Serial.println("[NTP] Sync FAILED — timeout");
        return false;
    }

    ntpSynced = true;

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.printf("[NTP] Synced: %s (UTC+7)\n", buf);

    // Ghi UTC vào RTC (không phải local time)
    if (rtcFound) {
        time_t now;
        time(&now);
        struct tm utcTime;
        gmtime_r(&now, &utcTime);
        if (ds3231_write(&utcTime)) {
            char ubuf[32];
            strftime(ubuf, sizeof(ubuf), "%Y-%m-%d %H:%M:%S", &utcTime);
            Serial.printf("[NTP] -> RTC updated (UTC): %s\n", ubuf);
        } else {
            Serial.println("[NTP] -> RTC write FAILED");
        }
    }

    return true;
}

void ntp_rtc_update() {
    unsigned long now = millis();
    unsigned long interval = ntpSynced ? SYNC_OK_INTERVAL : SYNC_FAIL_INTERVAL;

    if (now - lastSyncAttempt >= interval) {
        ntp_rtc_sync_ntp();
    }
}

time_t ntp_rtc_get_epoch() {
    time_t now;
    time(&now);
    return now;
}

String ntp_rtc_get_datetime() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+07:00", &timeinfo);
        return String(buf);
    }
    return "1970-01-01T00:00:00+07:00";
}

bool ntp_rtc_is_valid() {
    time_t now;
    time(&now);
    struct tm* t = localtime(&now);
    return t->tm_year > 124;  // > 2024
}

#include "ota_update.h"
#include "debug_config.h"
#include "mqtt_client.h"
#include "ntp_rtc.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "mbedtls/sha256.h"

static bool _pending = false;
static bool _running = false;
static String _url;
static String _checksum;

static const char* PREFS_NS  = "ota";
static const char* PREFS_KEY = "updated";

// ============================================================
// Publish OTA progress lên MQTT status topic
// ============================================================
static void publishProgress(const char* status, int progress,
                             const char* error = nullptr,
                             const char* fwVer = nullptr) {
    JsonDocument doc;
    doc["ota_status"]   = status;
    doc["ota_progress"] = progress;
    doc["ts"]           = ntp_rtc_get_datetime();
    if (error) doc["ota_error"]  = error;
    if (fwVer) doc["fw_version"] = fwVer;

    String json;
    serializeJson(doc, json);
    mqtt_publish_status(json);
    mqtt_keep_alive();
    LOG_IF(LOG_OTA, "[OTA] %s %d%%\n", status, progress);
}

// ============================================================
// Thực hiện OTA: download → verify SHA256 → flash → reboot
// ============================================================
static void performOTA() {
    _running = true;
    LOG_IF(LOG_OTA, "[OTA] URL: %s\n", _url.c_str());
    LOG_IF(LOG_OTA, "[OTA] Expected SHA256: %s\n", _checksum.c_str());

    publishProgress("downloading", 0);

    // --- HTTP GET firmware ---
    HTTPClient http;
    http.begin(_url);
    http.setTimeout(60000); // 60s timeout (uint16_t max ~65s)
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        String err = "HTTP " + String(httpCode);
        publishProgress("failed", 0, err.c_str());
        http.end();
        _running = false;
        return;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        publishProgress("failed", 0, "Unknown content length");
        http.end();
        _running = false;
        return;
    }

    LOG_IF(LOG_OTA, "[OTA] Firmware size: %d bytes\n", contentLength);

    // --- Bắt đầu ghi OTA partition ---
    if (!Update.begin(contentLength)) {
        publishProgress("failed", 0, "Not enough space");
        http.end();
        _running = false;
        return;
    }

    // --- Init SHA256 ---
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0); // 0 = SHA-256

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[4096];
    int totalRead        = 0;
    int lastReportedPct  = 0;
    unsigned long lastReportTime = millis();
    unsigned long lastDataTime   = millis();
    unsigned long lastKeepAlive  = millis();

    // --- Download + write + hash từng chunk ---
    while (http.connected() && totalRead < contentLength) {
        size_t avail = stream->available();
        if (avail > 0) {
            int toRead   = (avail < sizeof(buf)) ? avail : sizeof(buf);
            int readBytes = stream->readBytes(buf, toRead);
            if (readBytes > 0) {
                size_t written = Update.write(buf, readBytes);
                if (written != (size_t)readBytes) {
                    Update.abort();
                    publishProgress("failed", totalRead * 100 / contentLength, "Flash write error");
                    mbedtls_sha256_free(&sha_ctx);
                    http.end();
                    _running = false;
                    return;
                }
                mbedtls_sha256_update(&sha_ctx, buf, readBytes);
                totalRead += readBytes;
                lastDataTime = millis();

                // Báo progress mỗi 10% hoặc mỗi 5s
                int pct = (int)((int64_t)totalRead * 99 / contentLength);
                unsigned long now = millis();
                if (pct / 10 > lastReportedPct / 10 || now - lastReportTime > 5000) {
                    publishProgress("downloading", pct);
                    lastReportedPct = pct;
                    lastReportTime  = now;
                }
            }
        } else {
            delay(10);
        }

        // MQTT keep-alive mỗi 2s
        if (millis() - lastKeepAlive > 2000) {
            mqtt_keep_alive();
            lastKeepAlive = millis();
        }

        // Stall timeout: không nhận data trong 30s
        if (millis() - lastDataTime > 30000) {
            Update.abort();
            publishProgress("failed", totalRead * 100 / contentLength, "Download stalled");
            mbedtls_sha256_free(&sha_ctx);
            http.end();
            _running = false;
            return;
        }
    }

    http.end();

    if (totalRead != contentLength) {
        Update.abort();
        publishProgress("failed", totalRead * 100 / contentLength, "Incomplete download");
        mbedtls_sha256_free(&sha_ctx);
        _running = false;
        return;
    }

    LOG_IF(LOG_OTA, "[OTA] Download complete: %d bytes\n", totalRead);

    // --- Verify SHA256 ---
    uint8_t sha_result[32];
    mbedtls_sha256_finish(&sha_ctx, sha_result);
    mbedtls_sha256_free(&sha_ctx);

    char computed[65];
    for (int i = 0; i < 32; i++) {
        sprintf(computed + i * 2, "%02x", sha_result[i]);
    }
    computed[64] = '\0';

    LOG_IF(LOG_OTA, "[OTA] Computed SHA256: %s\n", computed);

    if (_checksum.length() > 0 && !_checksum.equalsIgnoreCase(String(computed))) {
        Update.abort();
        publishProgress("failed", 99, "Checksum mismatch");
        LOGLN_IF(LOG_OTA, "[OTA] ABORT: checksum mismatch!");
        _running = false;
        return;
    }

    // --- Finalize flash ---
    if (!Update.end(true)) {
        publishProgress("failed", 100, "Flash finalize error");
        _running = false;
        return;
    }

    LOGLN_IF(LOG_OTA, "[OTA] Flash OK!");
    publishProgress("flashing", 100);
    delay(500);

    // Lưu flag để sau reboot biết OTA vừa xong
    Preferences prefs;
    prefs.begin(PREFS_NS, false);
    prefs.putBool(PREFS_KEY, true);
    prefs.end();

    publishProgress("rebooting", 100);
    delay(500);

    ESP.restart();
}

// ============================================================
// Public API
// ============================================================

void ota_start(const String& url, const String& checksum) {
    if (_running) {
        LOGLN_IF(LOG_OTA, "[OTA] Already running, ignoring");
        return;
    }
    _url      = url;
    _checksum = checksum;
    _pending  = true;
    LOGLN_IF(LOG_OTA, "[OTA] Update queued");
}

static void otaTask(void* pvParameters) {
    performOTA();
    vTaskDelete(nullptr);
}

void ota_loop() {
    if (_pending && !_running) {
        _pending = false;
        xTaskCreate(otaTask, "ota_task", 32768, nullptr, 5, nullptr);
    }
}

bool ota_in_progress() {
    return _running;
}

bool ota_check_just_updated() {
    Preferences prefs;
    prefs.begin(PREFS_NS, true); // read-only
    bool updated = prefs.getBool(PREFS_KEY, false);
    prefs.end();

    if (updated) {
        // Xóa flag
        prefs.begin(PREFS_NS, false);
        prefs.remove(PREFS_KEY);
        prefs.end();
        LOGLN_IF(LOG_OTA, "[OTA] Post-OTA boot detected");
    }
    return updated;
}

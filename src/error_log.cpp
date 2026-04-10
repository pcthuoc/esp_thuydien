#include "error_log.h"
#include "sd_card.h"
#include "ntp_rtc.h"
#include <time.h>

#define LOG_DIR    "/logs"
#define LOG_MAX    (256 * 1024)   // 256 KB per file

static void ensureDir() {
    if (!sd_exists(LOG_DIR)) {
        sd_mkdir(LOG_DIR);
    }
}

// Build đường dẫn file log theo ngày: /logs/2026-04-09.log
static String getLogPath() {
    time_t now = ntp_rtc_get_epoch();
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "/logs/%04d-%02d-%02d.log",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return String(buf);
}

// Rotate: xóa file nếu quá lớn
static void rotateIfNeeded(const String& path) {
    if (!sd_exists(path.c_str())) return;
    String content = sd_read_file(path.c_str());
    if ((long)content.length() < LOG_MAX) return;

    Serial.printf("[ERRLOG] Rotate: %s (%d bytes)\n", path.c_str(), content.length());
    String oldPath = path.substring(0, path.length() - 4) + "_old.log";
    if (sd_exists(oldPath.c_str())) sd_remove(oldPath.c_str());
    sd_write_file(oldPath.c_str(), content.c_str());
    sd_remove(path.c_str());
}

void err_log(const char* tag, const char* msg) {
    if (!sd_is_inserted()) return;
    ensureDir();

    String path = getLogPath();
    rotateIfNeeded(path);

    String ts = ntp_rtc_get_datetime();  // "YYYY-MM-DD HH:MM:SS"
    String line = "[" + ts + "] [" + tag + "] " + msg + "\n";

    sd_append_file(path.c_str(), line.c_str());
    Serial.printf("[ERRLOG] %s", line.c_str());
}

void err_log(const char* tag, const String& msg) {
    err_log(tag, msg.c_str());
}

void err_log_clear() {
    // Xóa file log hôm nay
    String path = getLogPath();
    if (sd_exists(path.c_str())) sd_remove(path.c_str());
    Serial.println("[ERRLOG] Cleared today's log");
}

long err_log_size() {
    String path = getLogPath();
    if (!sd_exists(path.c_str())) return -1;
    String content = sd_read_file(path.c_str());
    return (long)content.length();
}

#include "error_log.h"
#include "sd_card.h"
#include "ntp_rtc.h"
#include <time.h>

#define LOG_DIR      "/logs"
#define LOG_MAX      (64 * 1024)    // 64 KB per file — rotate sớm hơn, zero heap cost (dùng rename)
#define LOG_MAX_FILES 14            // Giữ tối đa 14 files (~7 ngày × 2 files/ngày)

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

// Rotate: đổi tên file cũ → _old.log, tạo file mới
// KHÔNG đọc nội dung vào RAM — tránh heap spike 256+ KB
static void rotateIfNeeded(const String& path) {
    if (!sd_exists(path.c_str())) return;
    long sz = sd_file_size(path.c_str());
    if (sz < LOG_MAX) return;

    Serial.printf("[ERRLOG] Rotate: %s (%ld bytes)\n", path.c_str(), sz);
    String oldPath = path.substring(0, path.length() - 4) + "_old.log";
    if (sd_exists(oldPath.c_str())) sd_remove(oldPath.c_str());
    sd_rename(path.c_str(), oldPath.c_str());
}

// Xóa log cũ — giữ tối đa LOG_MAX_FILES files trong /logs/
// Files được sắp xếp tăng dần (tên = ngày) → xóa đầu danh sách = xóa cũ nhất
static void cleanupOldLogs() {
    auto files = sd_list_dir(LOG_DIR);
    if ((int)files.size() <= LOG_MAX_FILES) return;

    int toDelete = (int)files.size() - LOG_MAX_FILES;
    Serial.printf("[ERRLOG] Cleanup: %d files, xóa %d file cũ nhất\n",
                  (int)files.size(), toDelete);
    for (int i = 0; i < toDelete; i++) {
        String p = String(LOG_DIR) + "/" + files[i];
        sd_remove(p.c_str());
        Serial.printf("[ERRLOG] Deleted old log: %s\n", p.c_str());
    }
}

// Track ngày cuối cleanup — chỉ chạy 1 lần/ngày
static int s_lastCleanupDay = -1;

void err_log(const char* tag, const char* msg) {
    if (!sd_is_inserted()) return;
    ensureDir();

    String path = getLogPath();
    rotateIfNeeded(path);

    // Cleanup log cũ 1 lần/ngày
    time_t now = ntp_rtc_get_epoch();
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_mday != s_lastCleanupDay) {
        s_lastCleanupDay = t.tm_mday;
        cleanupOldLogs();
    }

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
    return sd_file_size(path.c_str());  // không đọc content vào RAM
}

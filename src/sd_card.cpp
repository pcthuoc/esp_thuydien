#include "sd_card.h"
#include <SD_MMC.h>
#include <FS.h>
#include <algorithm>

bool sd_init() {
    pinMode(SD_DET_PIN, INPUT_PULLUP);

    // Cấu hình pin SDMMC trước khi begin
    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);

    if (!SD_MMC.begin("/sdcard", false)) {  // false = 4-bit mode
        Serial.println("[SD] Mount FAILED");
        return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SD] No card found");
        return false;
    }

    const char* typeStr = "UNKNOWN";
    if (cardType == CARD_MMC)       typeStr = "MMC";
    else if (cardType == CARD_SD)   typeStr = "SD";
    else if (cardType == CARD_SDHC) typeStr = "SDHC";

    Serial.printf("[SD] Card type: %s\n", typeStr);
    Serial.printf("[SD] Total: %llu MB\n", SD_MMC.totalBytes() / (1024 * 1024));
    Serial.printf("[SD] Used:  %llu MB\n", SD_MMC.usedBytes() / (1024 * 1024));

    return true;
}

bool sd_is_inserted() {
    return digitalRead(SD_DET_PIN) == LOW;
}

bool sd_write_file(const char* path, const char* content) {
    // Đảm bảo thư mục cha tồn tại trước khi mở
    String p(path);
    int slash = p.lastIndexOf('/');
    if (slash > 0) sd_mkdir(p.substring(0, slash).c_str());

    File file = SD_MMC.open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("[SD] Failed to open %s for writing\n", path);
        return false;
    }
    bool ok = file.print(content);
    file.close();
    return ok;
}

bool sd_append_file(const char* path, const char* content) {
    // Đảm bảo thư mục cha tồn tại trước khi mở
    String p(path);
    int slash = p.lastIndexOf('/');
    if (slash > 0) sd_mkdir(p.substring(0, slash).c_str());

    File file = SD_MMC.open(path, FILE_APPEND);
    if (!file) {
        Serial.printf("[SD] Failed to open %s for append\n", path);
        return false;
    }
    bool ok = file.print(content);
    file.close();
    return ok;
}

String sd_read_file(const char* path) {
    File file = SD_MMC.open(path, FILE_READ);
    if (!file) {
        Serial.printf("[SD] Failed to open %s for reading\n", path);
        return "";
    }
    String content = file.readString();
    file.close();
    return content;
}

bool sd_exists(const char* path) {
    return SD_MMC.exists(path);
}

bool sd_remove(const char* path) {
    return SD_MMC.remove(path);
}

// Tạo thư mục kể cả các cấp cha chưa tồn tại (giống mkdir -p)
bool sd_mkdir(const char* path) {
    if (SD_MMC.exists(path)) return true;

    // Đi từ gốc, tạo từng cấp một
    String p(path);
    // Bỏ slash đầu
    int start = (p.startsWith("/")) ? 1 : 0;
    int pos = start;
    while (pos <= (int)p.length()) {
        int slash = p.indexOf('/', pos);
        if (slash == -1) slash = p.length();
        String sub = p.substring(0, slash);
        if (sub.length() > 0 && !SD_MMC.exists(sub.c_str())) {
            SD_MMC.mkdir(sub.c_str());
        }
        pos = slash + 1;
    }
    return SD_MMC.exists(path);
}

uint64_t sd_total_bytes() {
    return SD_MMC.totalBytes();
}

uint64_t sd_used_bytes() {
    return SD_MMC.usedBytes();
}

// ============================================================
// Liệt kê tên entry trong thư mục (không đệ quy, đã sắp xếp)
// ============================================================
std::vector<String> sd_list_dir(const char* path) {
    std::vector<String> result;
    if (!SD_MMC.exists(path)) return result;  // không tồn tại → trả về rỗng, không log lỗi
    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) return result;

    File entry = dir.openNextFile();
    while (entry) {
        String fullName = entry.name();
        // entry.name() trả full path từ root SD, lấy phần cuối
        int lastSlash = fullName.lastIndexOf('/');
        String name = (lastSlash >= 0) ? fullName.substring(lastSlash + 1) : fullName;
        if (name.length() > 0) result.push_back(name);
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    std::sort(result.begin(), result.end());
    return result;
}

bool sd_rmdir(const char* path) {
    return SD_MMC.rmdir(path);
}

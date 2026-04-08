#include "sd_card.h"
#include <SD_MMC.h>
#include <FS.h>

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

bool sd_mkdir(const char* path) {
    return SD_MMC.mkdir(path);
}

uint64_t sd_total_bytes() {
    return SD_MMC.totalBytes();
}

uint64_t sd_used_bytes() {
    return SD_MMC.usedBytes();
}

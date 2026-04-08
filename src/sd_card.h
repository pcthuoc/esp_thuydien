#pragma once
#include <Arduino.h>

// --- Pin mapping SDMMC (từ schematic) ---
#define SD_MMC_CLK   11
#define SD_MMC_CMD   12
#define SD_MMC_D0    10
#define SD_MMC_D1     9
#define SD_MMC_D2    14
#define SD_MMC_D3    13
#define SD_DET_PIN   21   // Card detect (LOW = có thẻ)

bool sd_init();
bool sd_is_inserted();
bool sd_write_file(const char* path, const char* content);
bool sd_append_file(const char* path, const char* content);
String sd_read_file(const char* path);
bool sd_exists(const char* path);
bool sd_remove(const char* path);
bool sd_mkdir(const char* path);
uint64_t sd_total_bytes();
uint64_t sd_used_bytes();

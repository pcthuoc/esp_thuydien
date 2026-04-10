#pragma once
#include <Arduino.h>

// Ghi 1 dòng lỗi vào /logs/error.log (tự tạo nếu chưa có)
// Format: [YYYY-MM-DD HH:MM:SS] [TAG] message
void err_log(const char* tag, const char* msg);
void err_log(const char* tag, const String& msg);

// Xóa file log (dùng khi log quá lớn)
void err_log_clear();

// Kích thước file log (bytes), -1 nếu không tồn tại
long err_log_size();

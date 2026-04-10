#pragma once
#include <Arduino.h>

#define MODBUS_MAX_CHANNELS  20  // Tối đa 20 kênh mỗi bus (5 slave × 4 biến)

// --- Data types ---
enum MbDataType : uint8_t {
    MB_INT16 = 0,
    MB_UINT16,
    MB_INT32,
    MB_UINT32,
    MB_FLOAT32
};

// --- Byte order (cho 32-bit) ---
enum MbByteOrder : uint8_t {
    MB_BE = 0,   // AB CD (Big Endian)
    MB_LE,       // CD AB (Little Endian)
    MB_MBE,      // BA DC (Mid-Big Endian / byte-swapped)
    MB_MLE       // DC BA (Mid-Little Endian / byte-swapped)
};

// --- Kết quả đọc 1 channel ---
struct MbChannel {
    char name[8];          // "V1", "V2", ...
    uint8_t slave_id;
    uint16_t reg_addr;
    uint8_t fc;            // 3 = holding, 4 = input
    MbDataType data_type;
    MbByteOrder byte_order;
    float value;           // Giá trị đã convert
    bool valid;            // true nếu đọc thành công
};

// --- Init: đọc config từ SD, khởi tạo UART Bus 1 ---
bool modbus_rtu_init();

// --- Poll: đọc tất cả channels trên Bus 1 ---
void modbus_rtu_poll();

// --- Số channel đang active ---
uint8_t modbus_rtu_channel_count();

// --- Lấy channel theo index ---
const MbChannel* modbus_rtu_get_channel(uint8_t index);

// --- Lấy channel theo tên (VD: "V1") ---
const MbChannel* modbus_rtu_find_channel(const char* name);

#pragma once
#include <Arduino.h>
#include "modbus_rtu.h"  // Dùng chung MbDataType, MbByteOrder

#define TCP_MAX_CHANNELS  20  // Tối đa 20 kênh TCP (5 slave × 4 biến)

struct TcpChannel {
    char name[16];      // "V1", "V2"...
    char host[16];      // IP address "192.168.1.x"
    uint16_t port;      // Modbus TCP port (502)
    uint8_t slave_id;
    uint16_t reg_addr;
    uint8_t fc;         // 3 = holding, 4 = input
    MbDataType data_type;
    MbByteOrder byte_order;
    float value;
    bool valid;
};

// Init W5500 hardware (SPI, DHCP) — gọi độc lập, không cần config
bool w5500_init();

// Load channel config từ SD + kích hoạt poll
bool modbus_tcp_init();

// Poll tất cả channels
void modbus_tcp_poll();

// Số channel active
uint8_t modbus_tcp_channel_count();

// Lấy channel theo index
const TcpChannel* modbus_tcp_get_channel(uint8_t index);

// Lấy channel theo tên
const TcpChannel* modbus_tcp_find_channel(const char* name);

// Ethernet link status
bool modbus_tcp_eth_linked();

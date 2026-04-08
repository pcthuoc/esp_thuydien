#pragma once
#include <Arduino.h>

#define ANALOG_CHANNELS  8  // A1-A4 = voltage (0-10V), A5-A8 = current (0-20mA)

struct AnalogChannel {
    float value;       // Giá trị vật lý: voltage (V) hoặc current (mA)
    float raw_adc;     // Voltage tại chân ADS (mV)
    int16_t raw_count; // ADC count gốc từ readADC_SingleEnded()
    bool valid;        // true nếu đọc thành công
};

// Init ADS1115 (cần gọi sau i2c_init())
bool analog_init();

// Đọc tất cả 8 kênh
void analog_poll();

// Lấy kết quả theo index (0-7 = A1-A8)
const AnalogChannel* analog_get_channel(uint8_t index);

// Kiểm tra ADS có trên bus không
bool analog_ads1_ok();  // 0x48 — voltage
bool analog_ads2_ok();  // 0x49 — current

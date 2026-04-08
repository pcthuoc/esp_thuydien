#include "analog_reader.h"
#include <Adafruit_ADS1X15.h>

// ============================================================
// Hardware config (từ schematic)
// ============================================================

// ADS1115 #1 — Voltage (ADDR = GND → 0x48)
// Input: 0-10V
// Divider: 100kΩ series + 22kΩ shunt
// V_ain = V_in × 22k/(100k+22k) → max 10V = 1.803V (trong range GAIN_ONE ±4.096V)
// V_in = V_ain × (100k + 22k) / 22k = V_ain × 5.5455
#define ADS1_ADDR           0x48
#define VOLTAGE_DIVIDER     5.5455f   // (100k + 22k) / 22k

// ADS1115 #2 — Current (ADDR = VDD → 0x49)
// Input: 0-20mA (24VDC loop) hoặc 4-20mA
// Shunt: 100Ω (1kΩ series protection, negligible với ADS input impedance)
// V_ain = I × 100Ω → max 20mA = 2.0V (trong range GAIN_ONE ±4.096V)
// I(mA) = V_ain(V) / R_shunt × 1000 = V_ain(V) × 10
#define ADS2_ADDR           0x49
#define SHUNT_RESISTANCE    100.0f    // 100Ω

static Adafruit_ADS1115 ads1;  // Voltage (0x48)
static Adafruit_ADS1115 ads2;  // Current (0x49)
static bool ads1Found = false;
static bool ads2Found = false;

static AnalogChannel channels[ANALOG_CHANNELS];

// ============================================================
// Init
// ============================================================

bool analog_init() {
    // Wire đã được init bởi ntp_rtc_init() (SDA=5, SCL=4)
    memset(channels, 0, sizeof(channels));

    // ADS1115 #1 — Voltage
    ads1.setGain(GAIN_ONE);  // ±4.096V (LSB = 0.125mV)
    if (ads1.begin(ADS1_ADDR)) {
        ads1Found = true;
        Serial.println("[ADS] #1 (0x48) Voltage OK");
    } else {
        Serial.println("[ADS] #1 (0x48) Voltage NOT found");
    }

    // ADS1115 #2 — Current
    ads2.setGain(GAIN_ONE);  // ±4.096V
    if (ads2.begin(ADS2_ADDR)) {
        ads2Found = true;
        Serial.println("[ADS] #2 (0x49) Current OK");
    } else {
        Serial.println("[ADS] #2 (0x49) Current NOT found");
    }

    return ads1Found || ads2Found;
}

// ============================================================
// Poll — đọc 8 kênh
// ============================================================

void analog_poll() {
    // A1-A4: ADS1115 #1 (Voltage, 0x48)
    if (ads1Found) {
        for (uint8_t i = 0; i < 4; i++) {
            int16_t raw = ads1.readADC_SingleEnded(i);
            if (raw < 0) {
                channels[i].valid = false;
                Serial.printf("[ADS] A%d: READ ERROR (raw=%d)\n", i + 1, raw);
                continue;
            }

            // Voltage tại chân ADS (mV)
            float v_ain = ads1.computeVolts(raw);

            channels[i].raw_count = raw;
            channels[i].raw_adc = v_ain * 1000.0f;  // mV
            channels[i].value = v_ain * VOLTAGE_DIVIDER;  // Voltage thực (V)
            channels[i].valid = true;
            Serial.printf("[ADS] A%d: raw=%d  adc=%.1fmV  volt=%.3fV\n",
                          i + 1, raw, v_ain * 1000.0f, channels[i].value);
        }
    } else {
        for (uint8_t i = 0; i < 4; i++) {
            channels[i].valid = false;
        }
    }

    // A5-A8: ADS1115 #2 (Current, 0x49)
    if (ads2Found) {
        for (uint8_t i = 0; i < 4; i++) {
            int16_t raw = ads2.readADC_SingleEnded(i);
            if (raw < 0) {
                channels[4 + i].valid = false;
                Serial.printf("[ADS] A%d: READ ERROR (raw=%d)\n", i + 5, raw);
                continue;
            }

            float v_ain = ads2.computeVolts(raw);

            channels[4 + i].raw_count = raw;
            channels[4 + i].raw_adc = v_ain * 1000.0f;  // mV
            channels[4 + i].value = (v_ain / SHUNT_RESISTANCE) * 1000.0f;  // mA
            channels[4 + i].valid = true;
            Serial.printf("[ADS] A%d: raw=%d  adc=%.1fmV  curr=%.3fmA\n",
                          i + 5, raw, v_ain * 1000.0f, channels[4 + i].value);
        }
    } else {
        for (uint8_t i = 0; i < 4; i++) {
            channels[4 + i].valid = false;
        }
    }
}

// ============================================================
// Getters
// ============================================================

const AnalogChannel* analog_get_channel(uint8_t index) {
    if (index >= ANALOG_CHANNELS) return nullptr;
    return &channels[index];
}

bool analog_ads1_ok() { return ads1Found; }
bool analog_ads2_ok() { return ads2Found; }

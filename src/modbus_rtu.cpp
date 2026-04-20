#include "modbus_rtu.h"
#include "sd_card.h"
#include "debug_config.h"
#include <ModbusMaster.h>
#include <ArduinoJson.h>

// ============================================================
// Pin mapping — Bus 1 RS485 RTU
// ============================================================
#define BUS1_RX  17
#define BUS1_TX  18

#define BUS1_SERIAL  Serial2  // Serial1 đã dùng cho modem 4G (GPIO15/16)

// Delay giữa các request Modbus (ms) — tránh slave chưa kịp xử lý
#define MODBUS_REQUEST_DELAY  50

// ============================================================
// Cấu trúc bus
// ============================================================
struct MbBus {
    uint32_t baud;
    uint8_t parity;       // 0=none, 1=even, 2=odd
    MbChannel channels[MODBUS_MAX_CHANNELS];
    uint8_t count;
};

static MbBus bus1;
static ModbusMaster node1;

// ============================================================
// Helpers: parse config string → enum
// ============================================================

static MbDataType parseDataType(const char* s) {
    if (!s) return MB_UINT16;
    if (strcasecmp(s, "INT16") == 0)   return MB_INT16;
    if (strcasecmp(s, "INT32") == 0)   return MB_INT32;
    if (strcasecmp(s, "UINT32") == 0)  return MB_UINT32;
    if (strcasecmp(s, "FLOAT32") == 0) return MB_FLOAT32;
    return MB_UINT16;
}

static MbByteOrder parseByteOrder(const char* s) {
    if (!s) return MB_BE;
    if (strcmp(s, "LE") == 0)  return MB_LE;
    if (strcmp(s, "MBE") == 0) return MB_MBE;
    if (strcmp(s, "MLE") == 0) return MB_MLE;
    return MB_BE;
}

static uint32_t serialConfig(uint8_t parity) {
    if (parity == 1) return SERIAL_8E1;
    if (parity == 2) return SERIAL_8O1;
    return SERIAL_8N1;
}

static uint8_t parseParity(const char* s) {
    if (!s) return 0;
    if (strcmp(s, "even") == 0) return 1;
    if (strcmp(s, "odd") == 0)  return 2;
    return 0;
}

// ============================================================
// Helpers: byte swap & 32-bit combine
// ============================================================

static uint16_t swap16(uint16_t v) {
    return (v >> 8) | (v << 8);
}

static uint32_t combine32(uint16_t regs[2], MbByteOrder order) {
    uint16_t hi, lo;
    switch (order) {
        case MB_BE:  hi = regs[0];        lo = regs[1];        break;
        case MB_LE:  hi = regs[1];        lo = regs[0];        break;
        case MB_MBE: hi = swap16(regs[0]); lo = swap16(regs[1]); break;
        case MB_MLE: hi = swap16(regs[1]); lo = swap16(regs[0]); break;
        default:     hi = regs[0];        lo = regs[1];        break;
    }
    return ((uint32_t)hi << 16) | lo;
}

// ============================================================
// Convert raw registers → float
// ============================================================

static float convertValue(uint16_t regs[2], MbDataType dt, MbByteOrder bo) {
    switch (dt) {
        case MB_INT16:
            return (float)(int16_t)regs[0];

        case MB_UINT16:
            return (float)regs[0];

        case MB_INT32: {
            uint32_t u = combine32(regs, bo);
            return (float)(int32_t)u;
        }
        case MB_UINT32: {
            uint32_t u = combine32(regs, bo);
            return (float)u;
        }
        case MB_FLOAT32: {
            uint32_t u = combine32(regs, bo);
            float f;
            memcpy(&f, &u, sizeof(f));
            return f;
        }
        default:
            return (float)regs[0];
    }
}

// ============================================================
// Đọc config 1 bus từ JSON object
// ============================================================

static void loadBusConfig(MbBus& bus, JsonObject busObj) {
    bus.baud = busObj["baud"] | 9600;
    bus.parity = parseParity(busObj["parity"]);
    bus.count = 0;

    JsonArray channels = busObj["channels"];
    if (!channels) return;

    for (JsonVariant v : channels) {
        if (bus.count >= MODBUS_MAX_CHANNELS) break;

        JsonObject ch = v.as<JsonObject>();
        MbChannel& c = bus.channels[bus.count];

        const char* name = ch["name"] | "V?";
        strncpy(c.name, name, sizeof(c.name) - 1);
        c.name[sizeof(c.name) - 1] = '\0';

        c.slave_id  = ch["slave_id"] | 1;
        c.reg_addr = (uint16_t)(ch["register"] | 0);
        // function_code: support string "03"/"04" (server format) or numeric fc (legacy)
        const char* fcStr = ch["function_code"] | (const char*)nullptr;
        if (fcStr) {
            int n = atoi(fcStr);
            c.fc = (n == 4) ? 4 : 3;
        } else {
            c.fc = ch["fc"] | 3;
        }
        c.data_type  = parseDataType(ch["data_type"]);
        // register_order (server format) or byte_order (legacy)
        const char* regOrder = ch["register_order"] | (const char*)nullptr;
        if (!regOrder) regOrder = ch["byte_order"] | "BE";
        c.byte_order = parseByteOrder(regOrder);
        c.value = 0;
        c.valid = false;

        bus.count++;
        LOG_IF(LOG_MODBUS, "[MODBUS] Bus channel: %s slave=%d reg=%d fc=%d\n",
                      c.name, c.slave_id, c.reg_addr, c.fc);
    }
}

// ============================================================
// TD301D485H-A auto-flow: flush echo sau khi TX
// postTransmission gọi sau khi gửi xong, trước khi đọc response
// ============================================================
static void bus1PostTransmission() {
    BUS1_SERIAL.flush();           // chờ TX hoàn tất
    while (BUS1_SERIAL.available()) BUS1_SERIAL.read();  // xả echo
}

// ============================================================
// Đọc 1 channel bằng ModbusMaster
// ============================================================

static const char* modbusErrStr(uint8_t code) {
    switch (code) {
        case 0xE0: return "RESPONSE_TIMEOUT";
        case 0xE1: return "INVALID_SLAVE_ID";
        case 0xE2: return "INVALID_FUNCTION";
        case 0xE3: return "RESPONSE_TOO_LONG";
        case 0xE4: return "CRC_ERROR";
        case 0x01: return "EX_ILLEGAL_FUNCTION";
        case 0x02: return "EX_ILLEGAL_ADDRESS";
        case 0x03: return "EX_ILLEGAL_VALUE";
        case 0x04: return "EX_SLAVE_FAILURE";
        default:   return "UNKNOWN";
    }
}

static bool readChannel(ModbusMaster& node, HardwareSerial& serial,
                        MbChannel& ch) {
    // Chuyển slave_id trước mỗi lần đọc
    node.begin(ch.slave_id, serial);

    uint16_t qty = (ch.data_type >= MB_INT32) ? 2 : 1;
    uint8_t result;

    LOG_IF(LOG_MODBUS, "[MODBUS] >> %s | slave=%d FC=%02d reg=%d qty=%d\n",
           ch.name, ch.slave_id, ch.fc, ch.reg_addr, qty);

    if (ch.fc == 4) {
        result = node.readInputRegisters(ch.reg_addr, qty);
    } else {
        result = node.readHoldingRegisters(ch.reg_addr, qty);
    }

    if (result != node.ku8MBSuccess) {
        ch.valid = false;
        LOG_IF(LOG_MODBUS, "[MODBUS] !! %s FAIL 0x%02X (%s)\n",
               ch.name, result, modbusErrStr(result));
        return false;
    }

    uint16_t regs[2] = {0, 0};
    for (uint16_t i = 0; i < qty; i++) {
        regs[i] = node.getResponseBuffer(i);
        LOG_IF(LOG_MODBUS, "[MODBUS]    reg[%d] = 0x%04X (%u)\n", i, regs[i], regs[i]);
    }

    ch.value = convertValue(regs, ch.data_type, ch.byte_order);
    ch.valid = true;
    LOG_IF(LOG_MODBUS, "[MODBUS] << %s = %.4f (dt=%d bo=%d)\n",
           ch.name, ch.value, ch.data_type, ch.byte_order);
    return true;
}

// ============================================================
// Public API
// ============================================================

bool modbus_rtu_init() {
    memset(&bus1, 0, sizeof(bus1));

    // Đọc config từ SD
    String json = sd_read_file("/config/rs485.json");
    if (json.length() == 0) {
        LOGLN_IF(LOG_MODBUS, "[MODBUS] Không tìm thấy /config/rs485.json");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        LOGLN_IF(LOG_MODBUS, "[MODBUS] JSON parse lỗi");
        return false;
    }

    // Load config Bus 1
    if (doc["rs485_1"].is<JsonObject>()) {
        loadBusConfig(bus1, doc["rs485_1"]);
    }

    if (bus1.count == 0) {
        LOGLN_IF(LOG_MODBUS, "[MODBUS] Bus1 không có channel");
        return false;
    }

    // Init UART Bus 1
    BUS1_SERIAL.begin(bus1.baud, serialConfig(bus1.parity), BUS1_RX, BUS1_TX);
    node1.begin(1, BUS1_SERIAL);
    // TD301D485H-A auto-flow: flush echo bytes sau TX, trước khi đọc response
    node1.postTransmission(bus1PostTransmission);
    LOG_IF(LOG_MODBUS, "[MODBUS] Bus1 init: %lu 8%c1, %d channels\n",
                  bus1.baud,
                  bus1.parity == 1 ? 'E' : (bus1.parity == 2 ? 'O' : 'N'),
                  bus1.count);
    return true;
}

void modbus_rtu_poll() {
    for (uint8_t i = 0; i < bus1.count; i++) {
        MbChannel& ch = bus1.channels[i];
        bool ok = readChannel(node1, BUS1_SERIAL, ch);
        if (ok) {
            LOG_IF(LOG_MODBUS, "[MODBUS] %s = %.4f\n", ch.name, ch.value);
        }
        delay(MODBUS_REQUEST_DELAY);
    }
}

uint8_t modbus_rtu_channel_count() {
    return bus1.count;
}

const MbChannel* modbus_rtu_get_channel(uint8_t index) {
    if (index >= bus1.count) return nullptr;
    return &bus1.channels[index];
}

const MbChannel* modbus_rtu_find_channel(const char* name) {
    if (!name) return nullptr;
    for (uint8_t i = 0; i < bus1.count; i++) {
        if (strcmp(bus1.channels[i].name, name) == 0) {
            return &bus1.channels[i];
        }
    }
    return nullptr;
}

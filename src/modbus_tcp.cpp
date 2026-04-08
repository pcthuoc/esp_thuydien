#include "modbus_tcp.h"
#include "sd_card.h"
#include <SPI.h>
#include <Ethernet.h>
#include <ArduinoJson.h>

// ============================================================
// W5500 SPI Pins (từ schematic)
// ============================================================
#define ETH_MOSI  36
#define ETH_MISO  38
#define ETH_CLK   37
#define ETH_CS    39
// #define ETH_RST   -1  // TODO: xác nhận GPIO nếu có

// Timeout khi đọc Modbus TCP (ms)
#define TCP_CONNECT_TIMEOUT  1000
#define TCP_READ_TIMEOUT     1000

// ============================================================
// State
// ============================================================
static TcpChannel channels[TCP_MAX_CHANNELS];
static uint8_t channelCount = 0;
static bool ethInitOk = false;
static uint16_t transactionId = 0;

// MAC address cho W5500 (unique trên mạng LAN)
static byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01};

// ============================================================
// Helpers: parse config string → enum (reuse logic từ modbus_rtu)
// ============================================================

static MbDataType parseDataType(const char* s) {
    if (!s) return MB_UINT16;
    if (strcmp(s, "INT16") == 0)   return MB_INT16;
    if (strcmp(s, "INT32") == 0)   return MB_INT32;
    if (strcmp(s, "UINT32") == 0)  return MB_UINT32;
    if (strcmp(s, "FLOAT32") == 0) return MB_FLOAT32;
    return MB_UINT16;
}

static MbByteOrder parseByteOrder(const char* s) {
    if (!s) return MB_BE;
    if (strcmp(s, "LE") == 0)  return MB_LE;
    if (strcmp(s, "MBE") == 0) return MB_MBE;
    if (strcmp(s, "MLE") == 0) return MB_MLE;
    return MB_BE;
}

// ============================================================
// Byte swap & 32-bit combine (giống modbus_rtu)
// ============================================================

static uint16_t swap16(uint16_t v) {
    return (v >> 8) | (v << 8);
}

static uint32_t combine32(uint16_t regs[2], MbByteOrder order) {
    uint16_t hi, lo;
    switch (order) {
        case MB_BE:  hi = regs[0];         lo = regs[1];         break;
        case MB_LE:  hi = regs[1];         lo = regs[0];         break;
        case MB_MBE: hi = swap16(regs[0]); lo = swap16(regs[1]); break;
        case MB_MLE: hi = swap16(regs[1]); lo = swap16(regs[0]); break;
        default:     hi = regs[0];         lo = regs[1];         break;
    }
    return ((uint32_t)hi << 16) | lo;
}

static float convertValue(uint16_t regs[2], MbDataType dt, MbByteOrder bo) {
    switch (dt) {
        case MB_INT16:  return (float)(int16_t)regs[0];
        case MB_UINT16: return (float)regs[0];
        case MB_INT32:  { uint32_t u = combine32(regs, bo); return (float)(int32_t)u; }
        case MB_UINT32: { uint32_t u = combine32(regs, bo); return (float)u; }
        case MB_FLOAT32: {
            uint32_t u = combine32(regs, bo);
            float f;
            memcpy(&f, &u, sizeof(f));
            return f;
        }
        default: return (float)regs[0];
    }
}

// ============================================================
// Modbus TCP protocol: build request & parse response
// ============================================================

// MBAP Header (7 bytes) + PDU (5 bytes for FC03/04 request) = 12 bytes
static bool modbusRequest(EthernetClient& client, TcpChannel& ch) {
    uint16_t qty = (ch.data_type >= MB_INT32) ? 2 : 1;
    transactionId++;

    // Build Modbus TCP ADU
    uint8_t req[12];
    req[0]  = (transactionId >> 8) & 0xFF;  // Transaction ID HI
    req[1]  = transactionId & 0xFF;          // Transaction ID LO
    req[2]  = 0x00;                          // Protocol ID HI
    req[3]  = 0x00;                          // Protocol ID LO
    req[4]  = 0x00;                          // Length HI
    req[5]  = 0x06;                          // Length LO (unit_id + fc + addr + qty = 6)
    req[6]  = ch.slave_id;                   // Unit ID
    req[7]  = ch.fc;                         // Function Code (03 or 04)
    req[8]  = (ch.reg_addr >> 8) & 0xFF;     // Start Address HI
    req[9]  = ch.reg_addr & 0xFF;            // Start Address LO
    req[10] = (qty >> 8) & 0xFF;             // Quantity HI
    req[11] = qty & 0xFF;                    // Quantity LO

    client.write(req, 12);
    client.flush();

    // Wait for response
    unsigned long start = millis();
    while (client.available() < (int)(9 + qty * 2)) {
        if (millis() - start > TCP_READ_TIMEOUT) {
            Serial.printf("[MBTCP] %s timeout\n", ch.name);
            return false;
        }
        delay(1);
    }

    // Read MBAP header (7 bytes)
    uint8_t hdr[7];
    client.read(hdr, 7);

    // Read FC + byte count
    uint8_t fc = client.read();
    if (fc & 0x80) {
        // Error response
        uint8_t errCode = client.read();
        Serial.printf("[MBTCP] %s error fc=0x%02X err=%d\n", ch.name, fc, errCode);
        return false;
    }

    uint8_t byteCount = client.read();
    if (byteCount < qty * 2) return false;

    // Read register data
    uint16_t regs[2] = {0, 0};
    for (uint16_t i = 0; i < qty; i++) {
        uint8_t hi = client.read();
        uint8_t lo = client.read();
        regs[i] = ((uint16_t)hi << 8) | lo;
    }

    ch.value = convertValue(regs, ch.data_type, ch.byte_order);
    ch.valid = true;
    return true;
}

// ============================================================
// Public API
// ============================================================

bool modbus_tcp_init() {
    memset(channels, 0, sizeof(channels));
    channelCount = 0;

    // Đọc config từ SD
    String json = sd_read_file("/config/tcp.json");
    if (json.length() == 0) {
        Serial.println("[MBTCP] Không tìm thấy /config/tcp.json");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        Serial.println("[MBTCP] JSON parse lỗi");
        return false;
    }

    // Load channels
    JsonArray arr = doc["channels"];
    if (!arr) {
        Serial.println("[MBTCP] Không có channels");
        return false;
    }

    for (JsonVariant v : arr) {
        if (channelCount >= TCP_MAX_CHANNELS) break;
        JsonObject ch = v.as<JsonObject>();
        TcpChannel& c = channels[channelCount];

        const char* name = ch["name"] | "V?";
        strncpy(c.name, name, sizeof(c.name) - 1);
        c.name[sizeof(c.name) - 1] = '\0';

        const char* ip = ch["ip"] | "";
        strncpy(c.host, ip, sizeof(c.host) - 1);
        c.host[sizeof(c.host) - 1] = '\0';

        c.port       = ch["port"] | 502;
        c.slave_id   = ch["slave_id"] | 1;
        c.reg_addr   = ch["register"] | 0;
        c.fc         = ch["fc"] | 3;
        c.data_type  = parseDataType(ch["data_type"]);
        c.byte_order = parseByteOrder(ch["byte_order"]);
        c.value = 0;
        c.valid = false;

        channelCount++;
        Serial.printf("[MBTCP] Channel: %s -> %s:%d slave=%d reg=%d\n",
                      c.name, c.host, c.port, c.slave_id, c.reg_addr);
    }

    if (channelCount == 0) {
        Serial.println("[MBTCP] Không có channel nào");
        return false;
    }

    // Init SPI cho W5500
    SPI.begin(ETH_CLK, ETH_MISO, ETH_MOSI, ETH_CS);
    Ethernet.init(ETH_CS);

    // DHCP
    Serial.println("[MBTCP] W5500 init (DHCP)...");
    if (Ethernet.begin(mac) == 0) {
        Serial.println("[MBTCP] DHCP failed, thử static IP...");
        // Static fallback: 192.168.1.200
        IPAddress ip(192, 168, 1, 200);
        IPAddress gateway(192, 168, 1, 1);
        IPAddress subnet(255, 255, 255, 0);
        Ethernet.begin(mac, ip, gateway, gateway, subnet);
    }

    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("[MBTCP] W5500 NOT found!");
        ethInitOk = false;
        return false;
    }

    ethInitOk = true;
    Serial.printf("[MBTCP] W5500 OK, IP: %s, %d channels\n",
                  Ethernet.localIP().toString().c_str(), channelCount);
    return true;
}

void modbus_tcp_poll() {
    if (!ethInitOk || channelCount == 0) return;

    // Check link
    if (Ethernet.linkStatus() == LinkOFF) {
        for (uint8_t i = 0; i < channelCount; i++) channels[i].valid = false;
        return;
    }

    Ethernet.maintain();  // DHCP renew nếu cần

    EthernetClient client;
    client.setConnectionTimeout(TCP_CONNECT_TIMEOUT);

    // Gom channel cùng host:port → 1 connection
    bool done[TCP_MAX_CHANNELS] = {};

    for (uint8_t i = 0; i < channelCount; i++) {
        if (done[i]) continue;

        TcpChannel& first = channels[i];
        if (strlen(first.host) == 0) {
            first.valid = false;
            done[i] = true;
            continue;
        }

        IPAddress ip;
        if (!ip.fromString(first.host)) {
            first.valid = false;
            done[i] = true;
            continue;
        }

        // Connect 1 lần cho host:port này
        if (!client.connect(ip, first.port)) {
            Serial.printf("[MBTCP] Connect FAIL %s:%d\n", first.host, first.port);
            // Mark tất cả channel cùng host:port = invalid
            for (uint8_t j = i; j < channelCount; j++) {
                if (!done[j] && strcmp(channels[j].host, first.host) == 0
                    && channels[j].port == first.port) {
                    channels[j].valid = false;
                    done[j] = true;
                }
            }
            continue;
        }

        // Đọc tất cả channel cùng host:port qua connection này
        for (uint8_t j = i; j < channelCount; j++) {
            if (done[j]) continue;
            if (strcmp(channels[j].host, first.host) != 0 || channels[j].port != first.port)
                continue;

            // Flush socket trước mỗi request — loại bỏ stale data
            while (client.available()) client.read();

            if (!modbusRequest(client, channels[j])) {
                channels[j].valid = false;
            }
            done[j] = true;
        }

        client.stop();
    }
}

uint8_t modbus_tcp_channel_count() {
    return channelCount;
}

const TcpChannel* modbus_tcp_get_channel(uint8_t index) {
    if (index >= channelCount) return nullptr;
    return &channels[index];
}

const TcpChannel* modbus_tcp_find_channel(const char* name) {
    if (!name) return nullptr;
    for (uint8_t i = 0; i < channelCount; i++) {
        if (strcmp(channels[i].name, name) == 0) {
            return &channels[i];
        }
    }
    return nullptr;
}

bool modbus_tcp_eth_linked() {
    return ethInitOk && Ethernet.linkStatus() != LinkOFF;
}

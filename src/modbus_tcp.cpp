#include "modbus_tcp.h"
#include "sd_card.h"
#include "debug_config.h"
#include <SPI.h>
#include <Ethernet_Generic.h>
#include <ArduinoJson.h>

// ============================================================
// W5500 SPI Pins (từ schematic)
// ============================================================
#define ETH_MOSI  35
#define ETH_MISO  37
#define ETH_CLK   36
#define ETH_CS    38

// ETH_RST không nối — W5500 dùng internal power-on reset

// DHCP timeout ngắn để không block boot lâu
#define ETH_DHCP_TIMEOUT_MS   5000   // 5s thử DHCP

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

// SPI bus riêng cho W5500 (dùng HSPI/SPI3, không đụng SPI2 mặc định)
static SPIClass ethSPI(HSPI);

// ============================================================
// Helpers: parse config string → enum (reuse logic từ modbus_rtu)
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
// Helpers: đọc đúng n bytes với timeout (pattern từ demo)
// ============================================================

static bool readBytes(EthernetClient& client, uint8_t* buf, size_t n) {
    uint32_t start = millis();
    size_t got = 0;
    while (got < n) {
        if (millis() - start > (uint32_t)TCP_READ_TIMEOUT) return false;
        if (client.available()) buf[got++] = client.read();
    }
    return true;
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
    req[0]  = (transactionId >> 8) & 0xFF;
    req[1]  = transactionId & 0xFF;
    req[2]  = 0x00;
    req[3]  = 0x00;
    req[4]  = 0x00;
    req[5]  = 0x06;
    req[6]  = ch.slave_id;
    req[7]  = ch.fc;
    req[8]  = (ch.reg_addr >> 8) & 0xFF;
    req[9]  = ch.reg_addr & 0xFF;
    req[10] = (qty >> 8) & 0xFF;
    req[11] = qty & 0xFF;

    LOG_IF(LOG_MBTCP, "[MBTCP] >> %s | txId=%d FC=%02X slave=%d reg=%d qty=%d\n",
           ch.name, transactionId, ch.fc, ch.slave_id, ch.reg_addr, qty);
    LOG_IF(LOG_MBTCP, "[MBTCP]    REQ: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
           req[0],req[1],req[2],req[3],req[4],req[5],req[6],req[7],req[8],req[9],req[10],req[11]);

    client.write(req, 12);

    // Đọc MBAP header (7 bytes)
    uint8_t hdr[7];
    if (!readBytes(client, hdr, 7)) {
        LOG_IF(LOG_MBTCP, "[MBTCP] %s TIMEOUT chờ MBAP header (>%dms)\n", ch.name, TCP_READ_TIMEOUT);
        return false;
    }
    LOG_IF(LOG_MBTCP, "[MBTCP]    HDR: %02X %02X %02X %02X %02X %02X %02X\n",
           hdr[0],hdr[1],hdr[2],hdr[3],hdr[4],hdr[5],hdr[6]);

    // Độ dài PDU từ MBAP Length field (trừ Unit ID)
    uint16_t pduLen = (uint16_t)(hdr[4] << 8 | hdr[5]) - 1;
    LOG_IF(LOG_MBTCP, "[MBTCP]    pduLen=%d (rà= %d bytes)\n", pduLen, pduLen);
    if (pduLen == 0 || pduLen > 255) {
        LOG_IF(LOG_MBTCP, "[MBTCP] %s pduLen không hợp lệ: %d\n", ch.name, pduLen);
        return false;
    }

    uint8_t pdu[255];
    if (!readBytes(client, pdu, pduLen)) {
        LOG_IF(LOG_MBTCP, "[MBTCP] %s TIMEOUT chờ PDU %d bytes (>%dms)\n", ch.name, pduLen, TCP_READ_TIMEOUT);
        return false;
    }
    LOG_IF(LOG_MBTCP, "[MBTCP]    PDU[0]=%02X (FC) PDU[1]=%02X (byteCount)\n", pdu[0], pdu[1]);

    // FC byte
    if (pdu[0] & 0x80) {
        const char* excDesc = "Unknown";
        switch (pdu[1]) {
            case 0x01: excDesc = "Illegal Function";        break;
            case 0x02: excDesc = "Illegal Data Address";    break;
            case 0x03: excDesc = "Illegal Data Value";      break;
            case 0x04: excDesc = "Server Device Failure";   break;
            case 0x06: excDesc = "Server Busy";             break;
        }
        LOG_IF(LOG_MBTCP, "[MBTCP] %s Exception 0x%02X: %s\n", ch.name, pdu[1], excDesc);
        return false;
    }

    uint8_t byteCount = pdu[1];
    if (byteCount < qty * 2) {
        LOG_IF(LOG_MBTCP, "[MBTCP] %s byteCount=%d < cần %d\n", ch.name, byteCount, qty * 2);
        return false;
    }

    // Parse register data từ pdu[2..]
    uint16_t regs[2] = {0, 0};
    for (uint16_t i = 0; i < qty; i++) {
        regs[i] = ((uint16_t)pdu[2 + i * 2] << 8) | pdu[3 + i * 2];
        LOG_IF(LOG_MBTCP, "[MBTCP]    reg[%d] = 0x%04X (%u)\n", i, regs[i], regs[i]);
    }

    ch.value = convertValue(regs, ch.data_type, ch.byte_order);
    ch.valid = true;
    LOG_IF(LOG_MBTCP, "[MBTCP] << %s = %.4f (dt=%d bo=%d)\n", ch.name, ch.value, ch.data_type, ch.byte_order);
    return true;
}

// ============================================================
// Public API
// ============================================================

bool w5500_init() {
    Serial.println("[W5500] Init SPI (HSPI)...");
    pinMode(ETH_CS, OUTPUT);
    digitalWrite(ETH_CS, HIGH);

    // Dùng HSPI (SPI3) riêng — không đụng SPI2 đang dùng cho SD card
    ethSPI.begin(ETH_CLK, ETH_MISO, ETH_MOSI, -1);
    ethSPI.setFrequency(8000000);

    // Chờ W5500 POR xong
    delay(500);

    Ethernet.init(ETH_CS);

    // DHCP với timeout ngắn, truyền SPI instance — chip được detect trong begin()
    Serial.println("[W5500] DHCP...");
    if (Ethernet.begin(mac, &ethSPI, ETH_DHCP_TIMEOUT_MS) == 0) {
        Serial.println("[W5500] DHCP failed, dùng static IP");

        // Đọc static IP từ /config/network.json (SD đã init trước w5500_init)
        // Mặc định nếu không có config
        IPAddress ip(192, 168, 0, 200);
        IPAddress gw(192, 168, 0, 1);
        IPAddress sn(255, 255, 255, 0);

        String json = sd_read_file("/config/network.json");
        if (json.length() > 0) {
            JsonDocument doc;
            if (!deserializeJson(doc, json)) {
                const char* sip = doc["eth_static_ip"] | (const char*)nullptr;
                const char* sgw = doc["eth_gateway"]   | (const char*)nullptr;
                const char* ssn = doc["eth_subnet"]    | (const char*)nullptr;
                IPAddress tmp;
                if (sip && tmp.fromString(sip)) { ip = tmp; Serial.printf("[W5500] eth_static_ip = %s\n", sip); }
                if (sgw && tmp.fromString(sgw)) gw = tmp;
                if (ssn && tmp.fromString(ssn)) sn = tmp;
            }
        }

        Ethernet.begin(mac, ip, gw, gw, sn);    // pCUR_SPI đã lưu từ lần begin() trước
        Serial.printf("[W5500] Static IP: %s\n", Ethernet.localIP().toString().c_str());
    } else {
        Serial.printf("[W5500] DHCP OK: %s\n", Ethernet.localIP().toString().c_str());
    }

    // Kiểm tra chip SAU khi begin() — library mới detect được
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("[W5500] NOT found — kiểm tra SPI pins / nguồn");
        return false;
    }
    Serial.printf("[W5500] Chip OK (status=%d)\n", Ethernet.hardwareStatus());

    ethInitOk = true;
    return true;
}

bool modbus_tcp_init() {
    memset(channels, 0, sizeof(channels));
    channelCount = 0;

    // Đọc config từ SD
    String json = sd_read_file("/config/tcp.json");
    if (json.length() == 0) {
        LOGLN_IF(LOG_MBTCP, "[MBTCP] Không tìm thấy /config/tcp.json");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        LOGLN_IF(LOG_MBTCP, "[MBTCP] JSON parse lỗi");
        return false;
    }

    // Load channels
    JsonArray arr = doc["channels"];
    if (!arr) {
        LOGLN_IF(LOG_MBTCP, "[MBTCP] Không có channels");
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

        channelCount++;
        LOG_IF(LOG_MBTCP, "[MBTCP] Channel: %s -> %s:%d slave=%d reg=%d\n",
                      c.name, c.host, c.port, c.slave_id, c.reg_addr);
    }

    if (channelCount == 0) {
        LOGLN_IF(LOG_MBTCP, "[MBTCP] Không có channel nào");
        return false;
    }

    // W5500 phải đã được init bởi w5500_init() trước
    if (!ethInitOk) {
        Serial.println("[MBTCP] W5500 chưa init — gọi w5500_init() trước");
        return false;
    }

    LOG_IF(LOG_MBTCP, "[MBTCP] %d channels loaded\n", channelCount);
    return true;
}

void modbus_tcp_poll() {
    if (!ethInitOk || channelCount == 0) return;

    // Check link
    if (Ethernet.linkStatus() == LinkOFF) {
        LOGLN_IF(LOG_MBTCP, "[MBTCP] Link OFF, skip poll");
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
            LOG_IF(LOG_MBTCP, "[MBTCP] Channel[%d] %s: no host, skip\n", i, first.name);
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
        LOG_IF(LOG_MBTCP, "[MBTCP] Connecting %s:%d ...\n", first.host, first.port);
        unsigned long t0 = millis();
        if (!client.connect(ip, first.port)) {
            LOG_IF(LOG_MBTCP, "[MBTCP] Connect FAIL %s:%d (%lums)\n", first.host, first.port, millis()-t0);
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

        LOG_IF(LOG_MBTCP, "[MBTCP] Connected %s:%d (%lums)\n", first.host, first.port, millis()-t0);

        // Đọc tất cả channel cùng host:port qua connection này
        for (uint8_t j = i; j < channelCount; j++) {
            if (done[j]) continue;
            if (strcmp(channels[j].host, first.host) != 0 || channels[j].port != first.port)
                continue;

            // Flush socket trước mỗi request — loại bỏ stale data
            while (client.available()) client.read();

            if (!modbusRequest(client, channels[j])) {
                LOG_IF(LOG_MBTCP, "[MBTCP] %s read FAIL (host=%s reg=%d)\n",
                       channels[j].name, channels[j].host, channels[j].reg_addr);
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

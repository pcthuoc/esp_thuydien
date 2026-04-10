/*
 * Modbus TCP Master (Client)
 * Board  : ESP32-S3 DevKit-M1
 * Library: Ethernet_Generic (khoih-prog)
 *
 * Wiring (SPI3 / HSPI):
 *   W5500   ESP32-S3
 *   MOSI -> GPIO 35
 *   MISO -> GPIO 37
 *   SCLK -> GPIO 36
 *   CS   -> GPIO 38
 *   RST  -> 3.3V (10k pull-up, no soft reset)
 */

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet_Generic.h>
#include <esp_task_wdt.h>

// ─── W5500 pins ───────────────────────────────────────────────────────────────
#define W5500_MOSI  35
#define W5500_MISO  37
#define W5500_SCK   36
#define W5500_CS    38

// ─── SPI3 (VSPI / HSPI bus) instance ────────────────────────────────────────
SPIClass mySPI(HSPI);

// ─── Network ──────────────────────────────────────────────────────────────────
byte      mac[]    = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress staticIP (192, 168,   0, 177);  // ESP32 static IP
IPAddress gw       (192, 168,   0,   1);
IPAddress mask     (255, 255, 255,   0);

// ─── Modbus TCP target ────────────────────────────────────────────────────────
IPAddress modbusServerIP(192, 168, 0, 100);  // IP laptop - Ethernet adapter (direct cable)
const uint16_t MODBUS_PORT    = 502;
const uint8_t  UNIT_ID        = 1;          // slave address
const uint16_t START_REGISTER = 0;          // địa chỉ register đầu tiên (0-based)
const uint16_t REG_COUNT      = 5;          // số register cần đọc

// ─── Timing ───────────────────────────────────────────────────────────────────
const uint32_t POLL_INTERVAL_MS = 2000;

// ─── Modbus helpers ───────────────────────────────────────────────────────────
static uint16_t txId = 0;

// Build FC03 Read Holding Registers request (12 bytes)
void buildFC03(uint8_t* buf, uint16_t startReg, uint16_t count)
{
    txId++;
    buf[0]  = txId >> 8;
    buf[1]  = txId & 0xFF;
    buf[2]  = 0x00;            // Protocol ID
    buf[3]  = 0x00;
    buf[4]  = 0x00;            // Length high
    buf[5]  = 0x06;            // Length low (6 bytes follow)
    buf[6]  = UNIT_ID;
    buf[7]  = 0x03;            // FC03
    buf[8]  = startReg >> 8;
    buf[9]  = startReg & 0xFF;
    buf[10] = count >> 8;
    buf[11] = count & 0xFF;
}

// Đọc đúng n bytes từ client với timeout
bool readBytes(EthernetClient& client, uint8_t* buf, size_t n, uint32_t timeoutMs = 1000)
{
    uint32_t start = millis();
    size_t   got   = 0;
    while (got < n) {
        if (millis() - start > timeoutMs) return false;
        if (client.available()) buf[got++] = client.read();
    }
    return true;
}

// Kết nối tới server, gửi FC03, parse và in kết quả
bool readHoldingRegisters()
{
    EthernetClient client;

    if (!client.connect(modbusServerIP, MODBUS_PORT)) {
        Serial.println("[ERR] Cannot connect to Modbus server");
        return false;
    }

    uint8_t req[12];
    buildFC03(req, START_REGISTER, REG_COUNT);
    client.write(req, sizeof(req));

    // MBAP header: Transaction ID(2) + Protocol(2) + Length(2) + Unit ID(1) = 7 bytes
    uint8_t hdr[7];
    if (!readBytes(client, hdr, 7)) {
        Serial.println("[ERR] Timeout reading MBAP header");
        client.stop();
        return false;
    }

    // Số byte còn lại sau Unit ID = PDU
    uint16_t pduLen = (uint16_t)(hdr[4] << 8 | hdr[5]) - 1;

    uint8_t pdu[256];
    if (!readBytes(client, pdu, pduLen)) {
        Serial.println("[ERR] Timeout reading PDU");
        client.stop();
        return false;
    }

    client.stop();

    if (pdu[0] & 0x80) {
        Serial.printf("[ERR] Modbus exception 0x%02X\n", pdu[1]);
        return false;
    }

    uint8_t numRegs = pdu[1] / 2;
    Serial.println("─── Holding Registers ──────────────");
    for (uint8_t i = 0; i < numRegs; i++) {
        uint16_t val = (uint16_t)(pdu[2 + i * 2] << 8 | pdu[3 + i * 2]);
        Serial.printf("  Reg %4d = %5u  (0x%04X)\n", START_REGISTER + i, val, val);
    }
    Serial.println("────────────────────────────────────");
    return true;
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    delay(300);

    Serial.println("\n==== Modbus TCP Master (W5500) ====");

    // Tắt WDT trong setup để tránh reboot khi DHCP timeout dài
    esp_task_wdt_deinit();

    pinMode(W5500_CS, OUTPUT);
    digitalWrite(W5500_CS, HIGH);

    // VSPI = SPI3, dùng HSPI bus trên ESP32-S3
    mySPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, -1);
    mySPI.setFrequency(8000000);

    Ethernet.init(W5500_CS);

    Serial.print("[..] DHCP... ");
    // Truyền &mySPI để library dùng đúng SPI3, không dùng default SPI2
    if (Ethernet.begin(mac, &mySPI, 4000) == 0) {
        Serial.println("failed, dùng static IP");
        // pCUR_SPI đã được set bởi lần begin() trên, static IP giữ nguyên
        Ethernet.begin(mac, staticIP, gw, gw, mask);
    } else {
        Serial.println("OK");
    }

    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("[ERR] W5500 not found! Check wiring.");
        while (true) delay(1000);
    }

    Serial.print("[OK] IP: ");
    Serial.println(Ethernet.localIP());
    Serial.printf("[OK] Sẽ đọc %d register từ %d.%d.%d.%d:%d mỗi %dms\n",
        REG_COUNT,
        modbusServerIP[0], modbusServerIP[1], modbusServerIP[2], modbusServerIP[3],
        MODBUS_PORT, POLL_INTERVAL_MS);
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop()
{
    static uint32_t lastPoll = 0;
    if (millis() - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = millis();
        Ethernet.maintain();
        readHoldingRegisters();
    }
}
# Tiến độ phát triển Firmware — Thủy điện ESP32-S3

> Cập nhật: 07/04/2026 (lần 5)

---

## Đã hoàn thành

### 1. LED trạng thái (WS2812)
- File: `src/led_status.h`, `src/led_status.cpp`
- LED NeoPixel trên chân IO42
- 8 trạng thái: BOOT, SD_OK, SD_ERROR, WIFI_CONNECTING, AP_MODE, MQTT_CONNECTING, ONLINE_OK, ERROR
- Tối ưu `needsRefresh` — chỉ ghi LED khi đổi trạng thái

### 2. Thẻ nhớ SD (SDMMC 4-bit)
- File: `src/sd_card.h`, `src/sd_card.cpp`
- Chân: CLK=11, CMD=12, D0=10, D1=9, D2=14, D3=13, DET=21
- Hàm: `sd_init()`, `sd_read_file()`, `sd_write_file()`, `sd_delete_file()`, `sd_mkdir()`, `sd_list_dir()`
- Dùng cho lưu config JSON và buffer dữ liệu offline

### 3. Nút nhấn (AceButton)
- File: `src/button.h`, `src/button.cpp`
- Nút trên chân IO45
- Click = log debug
- Giữ 3 giây = đọc config AP từ SD → khởi động AP mode + Web server

### 4. Web Server cấu hình (ESPAsyncWebServer v3.6)
- File: `src/webserver.h`, `src/webserver.cpp`
- AP mode với SSID/password/hostname tùy chỉnh từ `/config/ap.json`
- REST API:
  - `GET /api/config/{module}` — đọc config từng module (network, analog, encoder, di, rs485, tcp, mode)
  - `POST /api/config/{module}` — lưu config
  - `GET /api/scan` — quét WiFi (hiển thị kiểu WiFiManager)
  - `POST /api/clear-config` — xóa toàn bộ config trên SD
  - `POST /api/restart` — khởi động lại ESP
- Static file caching (Cache-Control: max-age=3600)
- LittleFS phục vụ frontend, SD_MMC lưu config

### 5. Frontend SPA (Single Page Application)
- File: `data/index.html`, `data/style.css`, `data/app.js`
- 9 trang cấu hình: Network, Analog, Encoder, DI, RS485 Bus 1, RS485 Bus 2, TCP, Mode, Hệ thống
- Giao diện dark theme, responsive
- Quét WiFi với biểu tượng cường độ sóng
- Bảng RS485/TCP có cột "Biến" (V1, V2...) để đặt tên biến
- Config caching phía JS — tránh request lặp khi chuyển tab
- Nút "Xóa toàn bộ cấu hình"

### 6. WiFi STA (kết nối WiFi từ config)
- Đọc `/config/network.json` từ SD khi boot
- Tự động kết nối WiFi STA với SSID/password đã lưu
- `WiFi.disconnect(true, true)` khi boot để xóa NVS cũ — tránh tự phát AP cũ

### 7. MQTT Client (PubSubClient)
- File: `src/mqtt_client.h`, `src/mqtt_client.cpp`
- Kết nối broker từ config SD (`/config/network.json`)
- Buffer nhận: 4096 bytes (đủ cho config lớn từ server)
- keepAlive 60s, auto-reconnect mỗi 5 giây
- Serial log chi tiết toàn bộ quá trình MQTT (payload, publish OK/FAIL, SD read/write)

#### 7.1 — Publish (gửi lên server)
- **Online status (mở rộng)**: gửi khi kết nối, kèm `wifi_ssid`, `mqtt_host`, `mqtt_port`, `fw_version`
- **Device config**: đọc tất cả config từ SD → build 1 JSON đầy đủ (format gốc) → gửi bằng `beginPublish()`/`print()`/`endPublish()` (streaming, không giới hạn kích thước payload)
- **Publish helpers**: `mqtt_publish_data()`, `mqtt_publish_status()`, `mqtt_publish_config()` — có log OK/FAIL
- **Firmware version**: `FW_VERSION` define trong mqtt_client.cpp

#### 7.2 — Subscribe (nhận từ server) — 3 topic
- `station/{id}/config` — config từ server
- `station/{id}/status` — ping/pong
- `station/{id}/cmd` — lệnh điều khiển (MỚI)
- **Ping/Pong**: nhận `{"ping":true}` trên topic status → trả `{"pong":true}`
- **Config từ server**: nhận JSON có `"source":"server"` trên topic config → parse từng group → lưu vào SD → gửi ACK
- **CMD** (mới): nhận lệnh điều khiển từ server qua topic `/cmd`:
  - `reset` — khởi động lại ESP (gửi ACK trước khi restart)
  - `set_wifi` — cập nhật SSID/password vào `/config/network.json`
  - `set_mqtt` — cập nhật broker host/port vào `/config/network.json`
  - Mỗi lệnh đều gửi ACK xác nhận về topic status

#### 7.3 — Đồng bộ config 2 chiều
- **Device → Server**: Mỗi khi boot kết nối MQTT, tự gửi toàn bộ config lên server (1 message JSON đầy đủ, streaming publish)
- **Server → Device**: Nhận config từ server → `applyServerGroup()` lưu vào SD:
  - **Static groups** (analog, encoder, di): merge vào object `channels` — chỉ cập nhật channel server gửi, giữ nguyên channel khác
  - **Dynamic groups** (rs485_1, rs485_2, tcp): ghi lại toàn bộ channels array với V1/V2 + slave_id, register, function_code, data_type, byte_order, host, port
  - **Mode config**: lưu riêng `/config/mode.json`
- Sau khi lưu xong, gửi ACK (không echo full config → tránh server gửi lại tạo vòng lặp)

#### 7.4 — Các vấn đề đã fix
- **Echo loop**: Device echo full config → server gửi lại → lặp vô hạn → fix: gửi ACK nhỏ thay echo
- **Buffer overflow**: Config 2029 bytes gần sát buffer 2048 → fix: tăng buffer lên 4096 + streaming publish cho gửi đi
- **Forward declaration**: `buildDeviceConfig()` gọi trước khi định nghĩa → thêm forward declaration

### 8. Cấu trúc boot sequence (main.cpp)
```
Serial init → WiFi.disconnect(true,true) → LED init → SD init
→ Button init → I2C init (Wire, SDA=5, SCL=4)
→ RTC init (đọc DS3231 → set system clock)
→ Analog init (ADS1115) → Counter init (PCNT) → Rain gauge init (DI1)
→ WiFi STA connect → NTP sync → ghi vào RTC
→ MQTT init (+ set config callback → data_collector_reload_calc)
→ Modbus RTU init → Modbus TCP init (W5500 Ethernet)
→ Data Collector init
→ loop: led_update() + button_update() + mqtt_update() + ntp_rtc_update() + rain_update() + data_collector_update()
```
- Nhấn giữ nút 3s → đọc AP config → khởi động AP + Web Server

### 9. NTP + RTC (DS3231)
- File: `src/ntp_rtc.h`, `src/ntp_rtc.cpp`
- I2C: SDA=GPIO5, SCL=GPIO4, địa chỉ DS3231 = 0x68
- Không dùng thư viện ngoài — đọc/ghi DS3231 trực tiếp qua Wire
- Chiến lược đồng bộ:
  - **Boot**: Đọc RTC → set system clock (có thời gian ngay dù không có WiFi)
  - **WiFi vừa kết nối**: Sync NTP ngay → ghi lại vào RTC
  - **Mỗi 1 giờ**: Re-sync NTP → cập nhật RTC
  - **NTP thất bại**: Retry mỗi 1 phút
  - **Không WiFi**: Dùng RTC (pin battery, drift ~1s/ngày)
- NTP servers: `pool.ntp.org`, `time.google.com`
- Timezone: UTC+7 (Việt Nam)
- Kiểm tra cờ OSF (Oscillator Stop Flag) — cảnh báo nếu pin RTC chết
- API: `ntp_rtc_get_datetime()`, `ntp_rtc_get_epoch()`, `ntp_rtc_is_valid()`
- Output format: ISO 8601 `2026-04-07T14:35:00+07:00`

### 10. Analog Reader (2× ADS1115)
- File: `src/analog_reader.h`, `src/analog_reader.cpp`
- 8 kênh: A1-A4 = voltage 0-10V (ADS1 0x48), A5-A8 = current 0-20mA (ADS2 0x49)
- Divider: 100kΩ/22kΩ → hệ số 5.5455, Shunt: 100Ω
- Struct `AnalogChannel`: `raw_count` (ADC gốc), `raw_adc` (mV), `value` (V/mA), `valid`
- ADC read lỗi (< 0) → mark `valid = false` (không mask bằng 0)
- Phụ thuộc `i2c_init()` (shared I2C bus)

### 11. Counter (PCNT Hardware)
- File: `src/counter.h`, `src/counter.cpp`
- 2 kênh: END1=GPIO47, END2=GPIO48
- PCNT legacy API (`driver/pcnt.h`), đếm FALLING edge (2N7002 NFET logic đảo)
- Glitch filter 80 ticks (~1µs), range 0-32767

### 12. Rain Gauge (DI1)
- File: `src/rain_gauge.h`, `src/rain_gauge.cpp`
- GPIO1, PC817 optocoupler, interrupt FALLING
- Tích lũy xung ngày, auto-reset lúc 0h00 (`rain_update()` check `tm_mday`)
- `rain_get_count()` có `noInterrupts()` guard — tránh race condition ISR
- `rain_reset()` cho lệnh manual từ server

### 13. Modbus RTU (RS485 Bus 1)
- File: `src/modbus_rtu.h`, `src/modbus_rtu.cpp`
- Bus 1: Serial1, RX=GPIO17, TX=GPIO18
- Config từ SD `/config/rs485.json` → group `rs485_1`
- Max 10 channels, FC03 (holding) / FC04 (input)
- 5 data types: INT16, UINT16, INT32, UINT32, FLOAT32
- 4 byte orders: BE, LE, MBE (byte-swapped), MLE
- Channel name V1, V2... (gán bởi server)

### 14. Modbus TCP (W5500 Ethernet)
- File: `src/modbus_tcp.h`, `src/modbus_tcp.cpp`
- SPI: MOSI=GPIO36, MISO=GPIO38, CLK=GPIO37, CS=GPIO39
- DHCP với static fallback 192.168.1.200
- Raw Modbus TCP protocol (MBAP header + PDU), max 10 channels
- Connection grouping: gom channel cùng host:port vào 1 connection
- Socket flush giữa mỗi request — tránh stale data

### 15. Data Collector
- File: `src/data_collector.h`, `src/data_collector.cpp`
- **Poll**: 6s/lần → tích lũy raw vào accumulator
- **Publish**: 60s/lần → trung bình (chia theo số lần đọc OK) → calc_mode → JSON → MQTT
- **Debug mode**: gửi ngay sau mỗi poll (cấu hình từ server)
- **Active flags**: chỉ poll/publish group nào init thành công (AI, ENC, DI, RTU, TCP)
- **Calc mode**:
  - Default: `weight = 1.0` → `real = raw × 1.0`
  - Server gửi config → reload tức thì (qua MQTT callback, không restart)
  - `weight`: `real = raw × weight`
  - `interpolation_2point`: `real = y1 + (raw - x1) × (y2 - y1) / (x2 - x1)`
- **Payload format** (JSON → `station/{id}/data`):
  ```json
  {
    "ingest_type": "realtime",
    "analog": { "ts": "2026-04-07T14:35:00+07:00", "A1": {"raw": 1023, "real": 1023.0} },
    "encoder": { "ts": "...", "E1": {"raw": 1234, "real": 1234.0} },
    "di": { "ts": "...", "DI1": {"raw": 142, "real": 142.0} },
    "rs485_1": { "ts": "...", "V1": {"raw": 4567, "real": 4567.0} },
    "tcp": { "ts": "...", "V1": {"raw": 5500, "real": 5500.0} }
  }
  ```
- **Offline buffer**: MQTT fail → lưu SD `/backfill/YYYY-MM-DD/HHMMSS.json` với `ingest_type: "backfill"`
- **LED feedback**: xanh chớp 1 lần = publish OK, đỏ chớp 2 lần = FAIL

---

## Chưa làm

| Module | Mô tả |
|--------|-------|
| RS485 Bus 2 | Serial2, RX=GPIO3, TX=GPIO46 — chưa implement |
| Gửi bù (backfill) | Đọc file `/backfill/` gửi lại khi online |
| OTA update | Cập nhật firmware qua mạng |
| API sensor data | Endpoint `/api/sensors` hiển thị dữ liệu realtime trên web |

---

## Thư viện sử dụng

| Thư viện | Phiên bản | Mục đích |
|----------|-----------|----------|
| Adafruit NeoPixel | ^1.12.0 | LED WS2812 |
| AceButton | ^1.10.1 | Xử lý nút nhấn |
| ESPAsyncWebServer (mathieucarbou) | ^3.6.0 | Web server async |
| ArduinoJson | ^7.4.1 | Parse/build JSON |
| PubSubClient | ^2.8 | MQTT client |
| ModbusMaster | ^2.0.1 | Modbus RTU master |
| Adafruit ADS1X15 | ^2.5.0 | ADS1115 ADC |
| Ethernet | ^2.0.2 | W5500 Modbus TCP |

---

## Ghi chú kỹ thuật

- **Board**: `esp32-s3-devkitc-1` (hỗ trợ 16MB flash native)
- **Flash**: 16MB, QIO mode
- **RAM**: 320KB SRAM (không có PSRAM — N16R0)
- **Partition**: app0 6.25MB + app1 6.25MB (OTA) + LittleFS 3.375MB
- **SD card**: SDMMC 4-bit, config lưu tại `/config/*.json`
- **Web files**: LittleFS (`data/` folder)
- **USB CDC**: Serial qua USB-Serial/JTAG

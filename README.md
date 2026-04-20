# Hệ thống Giám sát Thủy điện — ESP32-S3

**Firmware:** v1.0.6 | **MCU:** ESP32-S3-WROOM-1-N16 (16MB Flash) | **Platform:** PlatformIO / Arduino

---

## 1. Tổng quan

Thiết bị thu thập dữ liệu đa kênh tại trạm thủy điện. Đọc cảm biến Analog, Encoder, DI, RS485 Modbus RTU và Modbus TCP — gửi về server qua MQTT. Tự động lưu SD khi mất mạng và **gửi bù (backfill) bằng FreeRTOS task riêng** khi có mạng lại.

---

## 2. Phần cứng

### 2.1 MCU

| Thành phần | Mô tả |
|---|---|
| ESP32-S3-WROOM-1-N16 | Dual-core LX7 240MHz, WiFi + BLE 5.0, 16MB Flash, PSRAM 0 |
| MAC | `10:20:ba:38:a7:0c` |
| Flash layout | `0x10000` firmware · `0xC90000` LittleFS |

### 2.2 Đầu vào Analog (8 kênh — 2 × ADS1115, I2C)

| Kênh | IC | I2C Addr | Hardware | Công thức |
|---|---|---|---|---|
| A1–A4 | U103 | `0x48` (ADDR→GND) | Shunt 100Ω — **dòng 4–20mA** | `mA = (V_AIN / 100Ω) × 1000` |
| A5–A8 | U102 | `0x49` (ADDR→VDD) | Bộ chia 100kΩ/22kΩ — **áp 0–10V** | `V = V_AIN × 5.5455` |

- GAIN_ONE (±4.096V), 1 LSB = 0.125mV
- Poll 2s/lần → trung bình 30 mẫu/phút trước khi publish

### 2.3 Encoder (E1, E2) — PCNT

| Kênh | GPIO |
|---|---|
| E1 | 47 |
| E2 | 48 |

### 2.4 Digital Input (DI1) — Rain Gauge

| Kênh | GPIO | Mô tả |
|---|---|---|
| DI1 | 1 | Reed switch — mỗi xung = 1 lần gàu đổ |

### 2.5 RS485 Modbus RTU

| Bus | TX | RX | Giới hạn kênh |
|---|---|---|---|
| Bus 1 (`rs485_1`) | 18 | 17 | **20 kênh** (5 slave × 4 biến) |

### 2.6 Modbus TCP (W5500 SPI)

| Pin | GPIO | Giới hạn kênh |
|---|---|---|
| MOSI/MISO/CLK/CS | 36/38/37/39 | **20 kênh** (5 slave × 4 biến) |

- Gom các kênh cùng host:port → 1 TCP connection → tiết kiệm thời gian poll

### 2.7 SD Card (SD_MMC 4-bit)

| Pin | GPIO |
|---|---|
| CLK/CMD/D0/D1/D2/D3 | 11/12/10/9/14/13 |
| Card Detect | 21 (LOW = có thẻ) |

Mount point: `/sdcard` · Config: `/config/*.json` · Backfill: `/backfill/YYYY-MM-DD/HHMMSS.json`

### 2.8 LED Trạng thái (WS2812 RGB)

| Trạng thái | Màu | Hiệu ứng |
|---|---|---|
| Booting | 🟡 Vàng | Nhấp nháy nhanh |
| WiFi connecting | 🔵 Xanh dương | Nhấp nháy 500ms |
| MQTT connecting | 🔵 Xanh dương | Thở (fade) |
| Online OK | 🟢 Xanh lá | Sáng liên tục |
| Online + sensor lỗi | 🟢 Xanh lá | Nhấp nháy 1s |
| Offline (lưu SD) | 🟠 Cam | Nhấp nháy 1s |
| Lỗi nghiêm trọng | 🔴 Đỏ | Sáng liên tục |
| AP config mode | 🟣 Tím | Thở (fade) |

---

## 3. Kiến trúc phần mềm — Luồng task

```
Core 1 — loop() [priority 1]
─────────────────────────────────────────────────────────────────
  led_update()          <1ms
  button_update()       <1ms
  mqtt_update()         <1ms   ← reconnect + keep-alive
  ota_loop()            <1ms   ← chỉ spawn task khi có OTA
  ntp_rtc_update()      <1ms   ← sync 1h/lần
  rain_update()         <1ms
  data_collector_update()
    ├─ doAdcPoll()  mỗi 2s  → tích lũy trung bình
    └─ doPublish()  mỗi 60s → chờ MB_BIT_DONE → gửi MQTT

Core 0 — modbusTask [priority 3] ← cao hơn loop()
─────────────────────────────────────────────────────────────────
  Chờ MB_BIT_TRIGGER (EventGroup)
    → modbus_rtu_poll() — đọc RTU tất cả kênh
    → modbus_tcp_poll() — đọc TCP tất cả kênh
  Set MB_BIT_DONE → báo main loop publish được

Core bất kỳ — backfillTask [priority 1] ← thấp nhất
─────────────────────────────────────────────────────────────────
  vTaskDelay 15s → kiểm tra MQTT
  Lấy pubMutex → gửi tối đa 3 file → Give mutex
  (không thể chạy cùng lúc doPublish)

Core bất kỳ — ota_task [priority 5] ← cao nhất, stack 32KB
─────────────────────────────────────────────────────────────────
  Chỉ tạo khi nhận lệnh ota_update qua MQTT
  HTTPS download + flash (mbedTLS ECDHE cần stack 32KB)
```

### Đồng bộ quan trọng

| Primitive | Bảo vệ cái gì |
|---|---|
| `s_mbEvent` (EventGroup) | Đồng bộ timing modbus poll ↔ publish |
| `s_pubMutex` (Mutex) | Tránh race condition `mqtt_publish_data()` giữa realtime và backfill |

---

## 4. Tính toán timing

### Chu kỳ 60s — sơ đồ thời gian

```
T=0s     T=2s  T=4s  ...  T=58s  T=60s
 │         │     │           │      │
 ├─TRIGGER─┤     │           │      ├─WaitDone─►─Publish─►─TRIGGER─┐
 │         │     │           │      │                               │
 └─[modbusTask đang poll RTU + TCP]─┘                               │
           │     │           │                                      │
           └─ADC─┴─ADC─...──┴─ADC   ← 30 mẫu không bị block       │
                                                                    │
           ┌────────────────────────────────────────────────────────┘
           T=60s+Tp  (Tp = thời gian poll)
```

### Worst-case timing poll (20 RTU + 20 TCP)

| Thành phần | Kênh | Thời gian |
|---|---|---|
| **RTU** — slave trả lời (~20ms/ch) | 20 ch × (20+50)ms | **1,400ms** |
| **RTU** — slave timeout (200ms/ch) | 20 ch × (200+50)ms | **5,000ms** |
| **TCP** — cùng 1 host | 20 ch × ~10ms | **~200ms** |
| **TCP** — 5 host, reply nhanh | 5 connect + 20 ch read | **~700ms** |
| **TCP** — 5 host, timeout hết | 5×(1s connect + 4×1s read) | **25,000ms** |
| **Total worst** (RTU timeout + TCP timeout) | | **30,000ms** |

→ `MODBUS_POLL_TIMEOUT_MS = 30,000ms` = đúng bằng worst case → publish vẫn chạy, channels có vấn đề sẽ = `null` (valid=false).

### Backfill drain

| Tham số | Giá trị |
|---|---|
| Chu kỳ kiểm tra | 15s |
| File/chu kỳ | 3 |
| Tốc độ drain tối đa | 12 file/phút |
| Delay giữa 2 file | 500ms |
| Khởi động trễ | 30s sau boot |
| 1h offline (60 file) → phục hồi trong | ~5 phút |

### OTA

- Stack 32KB (FreeRTOS task) — bắt buộc do mbedTLS ECDHE cần >28KB stack
- Không ảnh hưởng loop() hoặc sensor polling

---

## 5. Cấu trúc file config (SD)

```
/config/
  network.json    ← WiFi, MQTT broker/port/device_id
  analog.json     ← calc_mode + params cho A1–A8
  encoder.json    ← calc_mode + params cho E1–E2
  di.json         ← calc_mode + params cho DI1
  rs485.json      ← bus1/bus2: baud, parity, channels[]
  tcp.json        ← channels[]: host, port, slave_id, register...

/backfill/
  YYYY-MM-DD/
    HHMMSS.json   ← payload bị lỡ khi offline
```

---

## 6. Giao thức MQTT

| Topic | Hướng | Nội dung |
|---|---|---|
| `station/{id}/data` | Device → Server | Payload đo mỗi 60s |
| `station/{id}/status` | Hai chiều | Online/offline, ping/pong |
| `station/{id}/config` | Hai chiều | Cập nhật config từ server |
| `station/{id}/cmd` | Server → Device | reset, set_wifi, set_mqtt, ota_update |

### Payload data (60s)

```json
{
  "ingest_type": "realtime",
  "analog": { "ts": "...", "A1": {"raw": 12345, "real": 15.2}, ... },
  "encoder": { "ts": "...", "E1": {"raw": 100, "real": 10.0} },
  "di":      { "ts": "...", "DI1": {"raw": 50, "real": 25.0} },
  "rs485_1": { "ts": "...", "V1": {"raw": 220.0, "real": 220.0}, ... },
  "tcp":     { "ts": "...", "T1": {"raw": 1.5, "real": 1.5}, ... }
}
```

- Backfill dùng `"ingest_type": "backfill"` thay vì `"realtime"`
- Kênh lỗi (slave không trả lời) = `null` thay vì object

---

## 7. Flash firmware

```bat
C:\Users\HOME\.platformio\packages\tool-esptoolpy\esptool.py ^
  --chip esp32s3 --port COM11 --baud 921600 ^
  --flash_mode qio --flash_freq 80m --flash_size 16MB ^
  write_flash ^
  0x0000    bootloader.bin ^
  0x8000    partitions.bin ^
  0x10000   firmware.bin ^
  0xC90000  littlefs.bin
```

---

## 8. Trạng thái module

| Module | Trạng thái |
|---|---|
| Web Server cấu hình | ✅ Hoàn thành |
| MQTT Client (pub/sub/cmd/config) | ✅ Hoàn thành |
| Analog Input (ADS1115) | ✅ Hoàn thành |
| Encoder (PCNT) | ✅ Hoàn thành |
| Digital Input / Rain Gauge | ✅ Hoàn thành |
| RS485 Modbus RTU | ✅ Hoàn thành |
| Modbus TCP (W5500) | ✅ Hoàn thành |
| NTP + RTC DS3231 backup | ✅ Hoàn thành |
| SD backfill — lưu offline | ✅ Hoàn thành |
| SD backfill — gửi bù (FreeRTOS task) | ✅ Hoàn thành |
| OTA qua MQTT (HTTPS, 32KB stack) | ✅ Hoàn thành |
| Modbus poll task (Core 0, EventGroup) | ✅ Hoàn thành |
| MQTT publish mutex (thread-safe) | ✅ Hoàn thành |

---

## 9. Changelog

### [2026-04-19] — Phân tích & fix backfill: spin bug, stuck-file, init wrap

#### Phân tích timing WiFi vs 4G

Mỗi lần `mqtt_publish_data()` gọi transport layer sẽ block Core 1 khác nhau tuỳ transport:

| Transport | Blocking time | Nguyên nhân |
|---|---|---|
| **WiFi** (`WiFiClient` → lwIP DMA) | 2–20 ms | Syscall, không chờ ACK (QoS 0) |
| **4G** (`TinyGsmClient` → AT UART 115200) | 150–700 ms | `AT+CIPSEND` + 500 byte × 10bit/115200 = 43ms UART + modem GPRS round-trip |

`modbusTask` chạy Core 0 độc lập → **không bị ảnh hưởng** dù backfill block bao lâu.  
`doAdcPoll()` (period 2000ms): WiFi 20ms = 1% trễ bỏ qua; 4G 700ms = 35% nhưng mẫu không mất, chỉ trễ — chấp nhận được.  
→ Giữ **1 file/cycle** và guard **15s** là đúng cho cả 2 transport. Không thể tăng file/cycle trên 4G.

#### Bug 1 — Spin trong cửa sổ 45–60s (`data_collector.cpp`)

**Nguyên nhân:** `lastBackfillMs = now` chỉ được reset khi `!realtimeImminent`. Khi `realtimeImminent = true` (t ≈ 45–60s), timer không reset → `loop()` re-enter check mỗi ~1ms → **spin ~15 giây** (15.000 lần/chu kỳ).

**Phân tích đủ case:**
- Boot bình thường: `millis() + PUBLISH_INTERVAL_MS` bị **unsigned wrap** → fire ngay t≈0 nhưng `mqtt_is_connected()=false` → safe, vô hại
- WDT/exception reset: giống boot bình thường
- `millis()` wrap (49.7 ngày): unsigned arithmetic tự xử lý đúng, không lỗi
- Debug mode bật: `doPublish(); return` → backfill block không chạy → `lastBackfillMs` không reset qua nhánh normal → khi tắt debug fire ngay → mqtt guard
- **Steady-state t=45–60s**: đây là case spin thực sự

**Fix:** dời `lastBackfillMs = now` ra ngoài, luôn reset trước khi kiểm tra `realtimeImminent`.  
Đồng thời xoá comment `millis() + PUBLISH_INTERVAL_MS` sai (giờ dùng `millis()` thẳng, guard đủ).

#### Bug 2 — Stuck file khi `mqtt_publish_data()` fail liên tiếp (`data_collector.cpp`)

**Nguyên nhân:** khi `ok = false`, file không bị xóa và không có cơ chế skip → nếu cùng 1 file cứ fail mãi → **chặn toàn bộ backfill queue vĩnh viễn**.

**Nguyên nhân file fail liên tiếp (khác với mất mạng bình thường):**
- **WiFi**: TCP drop xảy ra ngay sau `mqtt.connected()` check (race window nhỏ) → PubSubClient chưa kịp phát hiện mất mạng → `publish()` trả `false`
- **4G**: AT glitch / modem UART buffer kẹt state → `AT+CIPSEND` không nhận "SEND OK" → fail mà `mqtt.connected()` vẫn `true`
- Các case trên thường tự phục hồi sau 1–2 chu kỳ reconnect, nhưng nếu không → stuck mãi không có cách thoát

**Cần phân biệt với mất mạng thực:** mất mạng thực → `mqtt.connected()=false` → `doBackfillCycle()` return ngay ở đầu → không stuck, chờ reconnect.

**Fix:** thêm `s_bfFailPath` + `s_bfFailCount`. Cùng 1 path fail **≥ 3 lần liên tiếp** → xóa file + `err_log` → reset counter. Ngưỡng 3 đủ để bỏ qua transient drop nhưng không để stuck mãi.  
Reset counter về 0 ngay khi gửi thành công (path mới hoặc ok=true).

#### `src/data_collector.cpp`
- **Fix Bug 1 — spin guard**: `lastBackfillMs = now` dời ra ngoài `if (!realtimeImminent)` → luôn reset mỗi 15s, không spin
- **Fix init unsigned wrap**: `millis() + PUBLISH_INTERVAL_MS` → `millis()` vì phép cộng wrap và fire ngay anyway; `realtimeImminent` guard đủ bảo vệ slot realtime đầu tiên
- **Fix Bug 2 — stuck-file guard**: thêm `s_bfFailPath` (String) + `s_bfFailCount` (uint8_t); file cùng path fail ≥ 3 lần liên tiếp → `sd_remove()` + `err_log("BACKFILL","Skip stuck: path")` → tránh block queue; reset về 0 khi gửi OK
- **Thêm comment phân tích WiFi vs 4G** trong backfill block của `data_collector_update()` — ghi rõ lý do giữ 1 file/cycle và guard 15s

---

### [2026-04-16] — Ổn định 4G, nút bấm, backfill priority

#### `src/mqtt_client.cpp`
- **`setKeepAlive(30)` → `setKeepAlive(60)`**: tránh broker ngắt kết nối do keepalive timeout khi Modbus poll block tới 30s
- **`justConnected` — kiểm tra return value `mqttPublishLarge()`**: trước đây bỏ qua lỗi publish config → TCP stream bị corrupt → broker ngắt → device kẹt reconnect vô tận (root cause của "4G chỉ gửi 1 lần")
- **`mqtt_publish_data()` — xóa `mqtt.disconnect()` khi publish fail**: trên 4G, disconnect → `AT+CIPCLOSE` → socket stuck TIME_WAIT → subsequent `connect()` fail lặp vô tận; giờ để PubSubClient tự phát hiện qua keepalive

#### `src/data_collector.cpp`
- **Backfill delay khởi động**: `millis() + 30000` → `millis() + PUBLISH_INTERVAL_MS (60s)` — backfill đầu tiên luôn sau realtime đầu tiên, không cướp slot
- **Reset backfill timer sau mỗi `doPublish()`**: `lastBackfillMs = now` — không chạy backfill ngay sau khi vừa gửi realtime
- **Guard `realtimeImminent`**: nếu realtime tiếp theo còn ≤ `BACKFILL_INTERVAL_MS` (15s) nữa → skip backfill lần đó → realtime luôn được ưu tiên

#### `src/main.cpp`
- **`btn_task` priority: 2 → 4** (cao hơn `modbusTask` priority 3): `ModbusMaster` và TCP `readBytes()` dùng busy-wait không có `yield()` → starvation `btn_task` lên tới 20s trong worst‑case poll; giờ `btn_task` preempt ngay
- **`wifi_connect_from_config()` — check `s_apModeRequest`**: vòng lặp chờ WiFi thoát ngay khi long press được nhận, không đợi hết timeout 10s
- **WiFi reconnect trong `loop()` — check `s_apModeRequest`**: tương tự, thoát vòng reconnect ngay khi user bấm nút

#### `src/modbus_tcp.cpp`
- **`readBytes()` — thêm `taskYIELD()`**: khi không có byte TCP sẵn → yield CPU → `btn_task` chạy đúng 10ms tick, không bị block trong suốt TCP_READ_TIMEOUT


## 1. Tổng quan

Thiết bị thu thập dữ liệu đa kênh tại trạm thủy điện, sử dụng ESP32-S3 làm MCU trung tâm.  
Dữ liệu được gửi về server qua giao thức MQTT.

---

## 2. Thành phần phần cứng

### 2.1 — MCU chính

| Thành phần | Mô tả |
|------------|-------|
| **ESP32-S3-DevKitM-1** | Vi điều khiển chính, dual-core Xtensa LX7, WiFi + BLE 5.0 |

### 2.2 — Đầu vào Analog (8 kênh, 2 × ADS1115)

Sử dụng 2 IC **ADS1115** (16-bit ADC) giao tiếp **I2C**, nguồn 3V3, địa chỉ I2C phân biệt qua chân ADDR.

#### U102 — 4 kênh đọc điện áp (A1–A4)

| Thành phần | Giá trị | Mô tả |
|------------|---------|-------|
| ADS1115 (U102) | I2C, 3V3 | ADC 16-bit, 4 kênh single-ended (AIN1_1 → AIN1_4) |
| Bộ chia áp | 100kΩ / 22kΩ (R212–R215 / R216–R219) | Chia áp đầu vào 0–10V xuống dải đọc ADC |
| Connector | DB125-3.81-5P-GN-S (U126) | Terminal block 5 pin đầu vào |
| Tụ lọc | C235 — 100nF | Lọc nguồn VDD |

#### U103 — 4 kênh đọc dòng (A5–A8)

| Thành phần | Giá trị | Mô tả |
|------------|---------|-------|
| ADS1115 (U103) | I2C, 3V3 | ADC 16-bit, 4 kênh single-ended (AIN2_1 → AIN2_4) |
| Điện trở shunt | 100Ω (R224–R227) | Chuyển dòng 0–20mA → điện áp (0–2V) |
| Điện trở bảo vệ | 1kΩ (R220–R223) | Hạn dòng vào chân ADC |
| Connector | DB125-3.81-5P-GN-S (U116) | Terminal block 5 pin đầu vào |
| Tụ lọc | C236 — 100nF | Lọc nguồn VDD |
| Nguồn cảm biến | 24VDC | Cấp nguồn cho cảm biến dòng 4–20mA |

### 2.3 — Đầu vào Encoder (đếm xung)

| Thành phần | Số lượng | Mô tả |
|------------|----------|-------|
| Encoder channel | Tối đa 2 (E1, E2) | Bộ đếm xung — đo lưu lượng, tốc độ quay turbine... |
| Mạch cách ly xung *(nếu cần)* | theo số kênh | Optocoupler / level shifter bảo vệ GPIO |

### 2.4 — Digital Input (cảm biến mưa)

| Thành phần | Số lượng | Mô tả |
|------------|----------|-------|
| DI channel | 1 (DI1) | Cảm biến reed đo lượng mưa (tipping bucket rain gauge) — mỗi xung = 1 lần gàu đổ |
| Mạch cách ly | 1 | Optocoupler / input protection |

### 2.5 — RS485 Modbus RTU (2 bus)

| Thành phần | Số lượng | Mô tả |
|------------|----------|-------|
| UART → RS485 transceiver | 2 | MAX485 / SP3485 / MAX13487 (mỗi bus 1 IC) |
| Bus 1 (`rs485_1`) | 1 | Kết nối các slave Modbus RTU trên bus 1 |
| Bus 2 (`rs485_2`) | 1 | Kết nối các slave Modbus RTU trên bus 2 |
| Điện trở terminal 120Ω | 2 | Kết thúc đường truyền cho mỗi bus |

### 2.6 — Modbus TCP

| Thành phần | Mô tả |
|------------|-------|
| Kết nối Ethernet/WiFi | Giao tiếp Modbus TCP với slave qua mạng IP |
| Module Ethernet *(tùy chọn)* | W5500 / ENC28J60 nếu cần kết nối có dây |

### 2.7 — Kết nối mạng & thời gian

| Thành phần | Mô tả |
|------------|-------|
| WiFi (built-in) | Kết nối internet mặc định qua WiFi |
| NTP Client | Đồng bộ thời gian thực, đảm bảo timestamp chính xác (bắt buộc có timezone) |

### 2.8 — Thẻ nhớ SD (MicroSD)

| Thành phần | Mô tả |
|------------|-------|
| MicroSD card slot | Giao tiếp SPI với ESP32-S3 |
| Chức năng 1 — **Buffer offline** | Khi mất mạng, dữ liệu đo được ghi vào SD. Khi có mạng lại → đọc và gửi bù lên server |
| Chức năng 2 — **Lưu config** | Backup cấu hình (JSON) lên SD, dễ kiểm tra / sao chép giữa các trạm |

### 2.9 — LED trạng thái (WS2812 RGB)

| Thành phần | Mô tả |
|------------|-------|
| WS2812 | 1 LED RGB addressable, điều khiển qua 1 GPIO |

**Bảng trạng thái LED:**

| Ưu tiên | Trạng thái | Màu | Hiệu ứng | Mô tả |
|---------|-----------|-----|-----------|-------|
| 1 | **Đang khởi động** | 🟡 Vàng | Nhấp nháy nhanh (200ms) | Boot, init phần cứng |
| 2 | **WiFi đang kết nối** | 🔵 Xanh dương | Nhấp nháy chậm (500ms) | Chưa có WiFi |
| 3 | **MQTT đang kết nối** | 🔵 Xanh dương | Thở (fade in/out) | Có WiFi, chưa MQTT |
| 4 | **Online — bình thường** | 🟢 Xanh lá | Sáng liên tục | WiFi + MQTT OK, sensor OK |
| 5 | **Online — có cảnh báo sensor** | 🟢 Xanh lá | Nhấp nháy chậm (1s) | MQTT OK nhưng có kênh đọc lỗi (null) |
| 6 | **Offline — đang buffer SD** | 🟠 Cam | Nhấp nháy chậm (1s) | Mất mạng, ghi data vào SD |
| 7 | **Lỗi nghiêm trọng** | 🔴 Đỏ | Sáng liên tục | SD full / init fail / panic |
| 8 | **Chế độ cấu hình (AP)** | 🟣 Tím | Thở (fade in/out) | Web server AP đang hoạt động, chờ kết nối |

> Ưu tiên từ cao → thấp. LED hiển thị trạng thái ưu tiên cao nhất đang xảy ra.

### 2.10 — Nguồn cấp

| Thành phần | Mô tả |
|------------|-------|
| Nguồn DC input | 12V / 24V DC từ tủ điện trạm |
| Module hạ áp → 3.3V/5V | LM2596 / AMS1117 / buck converter cấp cho ESP32 và các module |

---

## 3. Web Server cấu hình (trên ESP32)

ESP32-S3 chạy một **HTTP web server nội bộ** (AP hoặc cùng mạng WiFi), cho phép kỹ thuật viên cấu hình trực tiếp tại trạm qua trình duyệt.

### Các trang cấu hình:

| Trang | Chức năng |
|-------|-----------|
| **Mạng & MQTT** | Cài WiFi SSID/password, MQTT broker/port, device_id, mqtt_password |
| **Quy đổi kênh** | Chọn `calc_mode` (weight / interpolation_2point), nhập tham số cho từng kênh (A1–A8, E1–E2, DI1, RS485, TCP) |
| **RS485 Modbus RTU** | Cấu hình slave_id, register address, data type cho từng kênh trên bus 1 & bus 2 |
| **Modbus TCP** | Thêm / sửa / xóa kênh TCP: IP, port, slave_id, register address, data type |
| **Trạng thái** | Xem giá trị raw/real hiện tại, trạng thái MQTT, uptime, thời gian NTP |

### Quy đổi giá trị (áp dụng cho MỌI kênh)

Mỗi kênh cảm biến (Analog, Encoder, DI, RS485 Modbus RTU, Modbus TCP) đều có **2 chế độ tính** giá trị thực (`real`) từ giá trị thô (`raw`):

| `calc_mode` | Công thức | Tham số |
|-------------|-----------|---------|
| `weight` | `real = raw × weight` | `weight` |
| `interpolation_2point` | `real = y1 + (raw - x1) × (y2 - y1) / (x2 - x1)` | `x1, y1, x2, y2` |

> Config có thể được thiết lập từ **web server** hoặc nhận từ **server qua MQTT**. Cả hai nguồn đều được lưu lại trên thiết bị (NVS/Flash).

---

## 4. Sơ đồ khối

```
                        ┌──────────────┐
   Sensor Analog ──────►│              │
   (4-20mA/0-10V)       │              │         ┌──────────┐
                        │              │◄──WiFi──►│  MQTT    │
   Encoder ────────────►│   ESP32-S3   │         │  Broker  │
                        │              │         └──────────┘
   Digital Input ──────►│              │
                        │              │
   RS485 Bus 1 ◄──────►│  UART0/1/2   │
   (Modbus RTU)         │              │
                        │              │
   RS485 Bus 2 ◄──────►│              │
   (Modbus RTU)         │              │
                        │   TCP/IP     │
   Modbus TCP ◄────────►│   stack      │
                        │              │
                        └──────────────┘
```

---

## 5. Giao thức truyền thông

| Giao thức | Vai trò |
|-----------|---------|
| **MQTT v3.1.1** | Gửi dữ liệu đo, nhận cấu hình, heartbeat (ping/pong) |
| **Modbus RTU** | Đọc sensor / thiết bị qua RS485 |
| **Modbus TCP** | Đọc sensor / thiết bị qua mạng IP |
| **NTP** | Đồng bộ thời gian |

> Chi tiết giao thức MQTT xem [README_FIRMWARE.md](README_FIRMWARE.md)

---

## 6. Trạng thái triển khai

| Module | Trạng thái |
|--------|-----------|
| Khung project PlatformIO | ✅ Đã tạo |
| Web Server cấu hình | ⬜ Chưa bắt đầu |
| MQTT Client | ⬜ Chưa bắt đầu |
| Analog Input | ⬜ Chưa bắt đầu |
| Encoder | ⬜ Chưa bắt đầu |
| Digital Input | ⬜ Chưa bắt đầu |
| RS485 Modbus RTU | ⬜ Chưa bắt đầu |
| Modbus TCP | ⬜ Chưa bắt đầu |
| Config Manager | ⬜ Chưa bắt đầu |
| NTP / Timestamp | ⬜ Chưa bắt đầu |



C:\Users\HOME\.platformio\packages\tool-esptoolpy\esptool.py ^
  --chip esp32s3 --port COM3 --baud 921600 ^
  --flash_mode qio --flash_freq 80m --flash_size 16MB ^
  write_flash ^
  0x0000    bootloader.bin ^
  0x8000    partitions.bin ^
  0x10000   firmware.bin ^
  0xC90000  littlefs.bin
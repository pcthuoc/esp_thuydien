# Hệ thống Giám sát Thủy điện — ESP32-S3

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

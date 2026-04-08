# Web Server cấu hình — ESP32-S3

## 1. Tổng quan

Web server chạy trên ESP32-S3 bằng **ESPAsyncWebServer**, phục vụ giao diện cấu hình qua trình duyệt.  
Kỹ thuật viên truy cập web để cài đặt thông số mà không cần nạp lại firmware.

---

## 2. Chế độ hoạt động

| Chế độ | Khi nào | IP truy cập | Cách kích hoạt |
|--------|---------|-------------|----------------|
| **AP (Access Point)** | Chưa có WiFi hoặc cần cấu hình | `192.168.4.1` | Giữ BUTTON (IO45) > 3s hoặc chưa có WiFi lưu sẵn |
| **STA (Station)** | ESP đã kết nối WiFi | IP do router cấp (DHCP) | *(chưa triển khai — dự phòng giai đoạn sau)* |

- **AP mode**: SSID = `THUYDIEN_{device_id}`, password = `12345678`
- LED chuyển 🟣 Tím thở khi vào AP mode

---

## 3. Các trang Web

### 3.1 — Trang chủ `/`

Tổng quan trạng thái hệ thống:

| Thông tin | Mô tả |
|-----------|-------|
| Device ID | Mã trạm 10 ký tự |
| Trạng thái WiFi | SSID, IP, RSSI |
| Trạng thái MQTT | Connected / Disconnected |
| Thời gian NTP | Thời gian hiện tại + timezone |
| Uptime | Thời gian hoạt động |
| SD Card | Dung lượng dùng / tổng |
| Firmware version | Phiên bản firmware |

---

### 3.2 — Cấu hình WiFi & MQTT `/config/network`

| Field | Kiểu | Mô tả |
|-------|------|-------|
| WiFi SSID | text | Tên mạng WiFi |
| WiFi Password | password | Mật khẩu WiFi |
| MQTT Broker | text | Địa chỉ broker (IP hoặc domain) |
| MQTT Port | number | Port (mặc định `1883`) |
| Device ID | text | Mã trạm 10 ký tự (= MQTT username) |
| MQTT Password | password | Mật khẩu MQTT (do server cấp) |

> Lưu vào file `/sdcard/config/network.json`

---

### 3.3 — Cấu hình Analog `/config/analog`

Bảng 8 kênh (A1–A8), mỗi kênh:

| Field | Kiểu | Mô tả |
|-------|------|-------|
| Bật/tắt | checkbox | Kích hoạt kênh |
| Tên kênh | text (readonly) | A1–A4 (điện áp), A5–A8 (dòng) |
| calc_mode | dropdown | `weight` / `interpolation_2point` |
| weight | number | Hệ số nhân (khi calc_mode = weight) |
| x1, y1 | number | Điểm 1 (khi calc_mode = interpolation_2point) |
| x2, y2 | number | Điểm 2 (khi calc_mode = interpolation_2point) |

> Lưu vào file `/sdcard/config/analog.json`

---

### 3.4 — Cấu hình Encoder `/config/encoder`

Bảng 2 kênh (E1, E2):

| Field | Kiểu | Mô tả |
|-------|------|-------|
| Bật/tắt | checkbox | Kích hoạt kênh |
| calc_mode | dropdown | `weight` / `interpolation_2point` |
| weight | number | Hệ số nhân |
| x1, y1, x2, y2 | number | Tham số nội suy |

> Lưu vào file `/sdcard/config/encoder.json`

---

### 3.5 — Cấu hình Digital Input `/config/di`

1 kênh (DI1 — cảm biến mưa reed):

| Field | Kiểu | Mô tả |
|-------|------|-------|
| Bật/tắt | checkbox | Kích hoạt kênh |
| calc_mode | dropdown | `weight` / `interpolation_2point` |
| weight | number | Hệ số nhân (VD: 0.2 = mỗi xung = 0.2mm mưa) |
| x1, y1, x2, y2 | number | Tham số nội suy |

> Lưu vào file `/sdcard/config/di.json`

---

### 3.6 — Cấu hình RS485 Modbus RTU `/config/rs485`

2 bus (rs485_1, rs485_2), mỗi bus có bảng kênh động:

#### Cấu hình bus

| Field | Kiểu | Mô tả |
|-------|------|-------|
| Baud rate | dropdown | 9600 / 19200 / 38400 / 115200 |
| Parity | dropdown | None / Even / Odd |
| Stop bits | dropdown | 1 / 2 |

#### Bảng kênh (thêm/xóa dòng)

| Field | Kiểu | Mô tả |
|-------|------|-------|
| Slave ID | number | Địa chỉ slave (1–247) |
| Register | number | Địa chỉ thanh ghi |
| Function code | dropdown | 03 (Holding) / 04 (Input) |
| Data type | dropdown | INT16 / UINT16 / INT32 / UINT32 / FLOAT32 |
| Byte order | dropdown | Big Endian / Little Endian / Mid-Big / Mid-Little |
| calc_mode | dropdown | `weight` / `interpolation_2point` |
| weight | number | Hệ số nhân |
| x1, y1, x2, y2 | number | Tham số nội suy |

> Lưu vào file `/sdcard/config/rs485.json`

---

### 3.7 — Cấu hình Modbus TCP `/config/tcp`

Bảng kênh động (thêm/sửa/xóa):

| Field | Kiểu | Mô tả |
|-------|------|-------|
| IP | text | Địa chỉ IP của slave |
| Port | number | Port TCP (mặc định 502) |
| Slave ID | number | Unit ID (1–247) |
| Register | number | Địa chỉ thanh ghi |
| Function code | dropdown | 03 (Holding) / 04 (Input) |
| Data type | dropdown | INT16 / UINT16 / INT32 / UINT32 / FLOAT32 |
| Byte order | dropdown | Big Endian / Little Endian / Mid-Big / Mid-Little |
| calc_mode | dropdown | `weight` / `interpolation_2point` |
| weight | number | Hệ số nhân |
| x1, y1, x2, y2 | number | Tham số nội suy |

> Lưu vào file `/sdcard/config/tcp.json`

---

### 3.8 — Giám sát realtime `/monitor`

Hiển thị dữ liệu đo **cập nhật tự động** (AJAX polling hoặc WebSocket):

| Cột | Mô tả |
|-----|-------|
| Group | analog / encoder / di / rs485_1 / rs485_2 / tcp |
| Channel | Mã kênh (A1, E1, 1:0, ...) |
| Raw | Giá trị thô |
| Real | Giá trị sau quy đổi |
| Trạng thái | OK / Error (null) |
| Timestamp | Thời điểm đọc gần nhất |

---

### 3.9 — Nhật ký & Debug `/logs`

| Thông tin | Mô tả |
|-----------|-------|
| Log MQTT | Kết nối, ngắt, gửi/nhận |
| Log Sensor | Lỗi đọc, giá trị bất thường |
| Log SD buffer | Số bản ghi đang chờ gửi |
| Log hệ thống | Boot, restart reason, free heap |

---

## 4. API endpoints (REST)

Giao diện web gọi API qua AJAX, cũng có thể dùng tool bên ngoài (curl, Postman):

| Method | Endpoint | Mô tả |
|--------|----------|-------|
| GET | `/api/status` | Trạng thái tổng quan (JSON) |
| GET | `/api/config/{module}` | Đọc config module (network, analog, encoder, di, rs485, tcp) |
| POST | `/api/config/{module}` | Lưu config module |
| GET | `/api/monitor` | Dữ liệu realtime tất cả kênh |
| GET | `/api/logs` | Nhật ký gần nhất |
| POST | `/api/restart` | Khởi động lại ESP |
| POST | `/api/factory-reset` | Xóa toàn bộ config, khôi phục mặc định |

---

## 5. Cấu trúc file config trên SD Card

```
/sdcard/
  config/
    network.json        ← WiFi + MQTT
    analog.json         ← 8 kênh analog (A1–A8)
    encoder.json        ← 2 kênh encoder (E1, E2)
    di.json             ← 1 kênh DI (DI1)
    rs485.json          ← 2 bus RS485 + danh sách kênh
    tcp.json            ← Danh sách kênh Modbus TCP
  buffer/
    *.jsonl             ← Data offline chờ gửi (mỗi dòng 1 JSON)
  logs/
    system.log          ← Log hệ thống
```

---

## 6. Công nghệ

| Thành phần | Lựa chọn |
|------------|----------|
| HTTP Server | **ESPAsyncWebServer** |
| Frontend | HTML + CSS + vanilla JS (lưu trên SD hoặc PROGMEM) |
| Data format | JSON (ArduinoJson) |
| Realtime update | AJAX polling (2s) hoặc WebSocket |
| Config storage | SD Card (JSON files) |

---

## 7. Luồng hoạt động

```
[BOOT]
  │
  ├─ Đọc config/network.json từ SD
  │
  ├─ Có WiFi config?
  │   ├─ CÓ  → Kết nối WiFi (STA) → Web server chạy trên IP LAN
  │   └─ KHÔNG → Bật AP mode (192.168.4.1) → LED tím thở
  │
  ├─ Web server khởi động (cả STA và AP đều có)
  │
  └─ Loop:
       ├─ BUTTON giữ > 3s → Chuyển sang AP mode
       ├─ Web request → Xử lý API / phục vụ trang HTML
       └─ WiFi mất → Tự reconnect, nếu fail > 60s → Bật AP
```

---

## 8. Bảo mật cơ bản

| Biện pháp | Mô tả |
|-----------|-------|
| AP có password | `12345678` (có thể đổi) |
| Factory reset cần xác nhận | POST `/api/factory-reset` yêu cầu confirm |
| Input validation | Kiểm tra kiểu dữ liệu, range trước khi lưu |
| Không lưu password trong response | API GET trả `"****"` cho WiFi/MQTT password |

# Hướng dẫn MQTT cho Phần Cứng (Firmware)

> Tài liệu này dành cho lập trình viên phần cứng (ESP32 / STM32 / Linux SBC...).  
> Mô tả đầy đủ những gì cần **gửi lên** (publish) và **nhận về** (subscribe) từ server.

---

## 1. Kết nối MQTT

| Thông số | Giá trị |
|----------|---------|
| Broker   | theo cấu hình dự án |
| Port     | `1883` |
| Protocol | MQTT v3.1.1 |
| QoS      | **1** cho tất cả topic |
| Username | `{device_id}` — mã trạm 10 ký tự |
| Password | `mqtt_password` — do server cấp khi tạo trạm |
| Client ID | tự đặt, duy nhất (VD: `{device_id}-fw`) |
| Clean Session | `true` |

**Ví dụ trạm NXL3_1:**
```
Username : O1SGXJNPFJ
Password : ICgElTolm6FK!qRr
```

---

## 2. Subscribe (nhận từ server)

Sau khi kết nối, phần cứng phải **subscribe 2 topic**:

```
station/{device_id}/config
station/{device_id}/status
```

Thay `{device_id}` bằng username của trạm. VD:
```
station/O1SGXJNPFJ/config
station/O1SGXJNPFJ/status
```

---

## 3. Publish (gửi lên server)

### 3.1 — Gửi dữ liệu đo: `station/{device_id}/data`

**Khi nào gửi:**
- Chế độ **debug=true**: gửi ngay sau mỗi lần đọc xong sensor
- Chế độ **debug=false**: gửi theo chu kỳ cố định (VD: 60s)

**Cấu trúc payload:**

```json
{
  "<group>": {
    "ts": "<ISO 8601 timestamp có timezone>",
    "<channel_code>": { "raw": <số nguyên>, "real": <số thực> },
    "<channel_code>": null
  }
}
```

- Chỉ gửi những group **có dữ liệu**, không cần gửi hết
- `null` = sensor lỗi hoặc không đọc được (server vẫn ghi nhận)
- `ts` là **thời điểm đo thực tế** của thiết bị, bắt buộc có timezone

**Ví dụ đầy đủ:**

```json
{
  "analog": {
    "ts": "2026-04-06T08:30:00+07:00",
    "A1": { "raw": 1023, "real": 12.5 },
    "A2": { "raw": 512,  "real": 6.24 },
    "A3": null
  },
  "encoder": {
    "ts": "2026-04-06T08:30:00+07:00",
    "E1": { "raw": 855, "real": 85.5 }
  },
  "di": {
    "ts": "2026-04-06T08:30:00+07:00",
    "DI1": { "raw": 1, "real": 1.0 }
  },
  "rs485_1": {
    "ts": "2026-04-06T08:30:01+07:00",
    "1:0": { "raw": 4567, "real": 2.45 },
    "1:2": { "raw": 1234, "real": 1.87 }
  },
  "rs485_2": {
    "ts": "2026-04-06T08:30:01+07:00",
    "2:10": { "raw": 999, "real": 99.9 }
  },
  "tcp": {
    "ts": "2026-04-06T08:30:02+07:00",
    "1:1": { "raw": 5500, "real": 3.12 },
    "1:4": { "raw": 2980, "real": 2.98 }
  },
  "iec62056": {
    "ts": "2026-04-06T08:30:03+07:00",
    "1.0.1.8.0": { "raw": 123456, "real": 1234.56 }
  }
}
```

**Bảng group và channel code:**

| Group | Channel code | Mô tả |
|-------|-------------|-------|
| `analog` | `A1` … `A8` | Đầu vào analog 4-20mA / 0-10V |
| `encoder` | `E1`, `E2` | Bộ đếm xung encoder |
| `di` | `DI1` | Digital input |
| `rs485_1` | `{slave_id}:{register}` | Modbus RTU Bus 1. VD: `"1:0"` |
| `rs485_2` | `{slave_id}:{register}` | Modbus RTU Bus 2. VD: `"2:10"` |
| `tcp` | `{slave_id}:{register}` | Modbus TCP. VD: `"1:1"` |
| `iec62056` | `{obis_code}` | Đồng hồ điện IEC 62056. VD: `"1.0.1.8.0"` |

> **Lưu ý:** server sẽ bỏ qua group không nằm trong danh sách trên.

---

### 3.2 — Báo trạng thái: `station/{device_id}/status`

#### Khi khởi động / kết nối lại → gửi ngay:

```json
{
  "status": "online",
  "ts": "2026-04-06T08:30:00+07:00"
}
```

#### Khi nhận được `{"ping": true}` từ server → phản hồi ngay:

```json
{
  "pong": true,
  "ts": "2026-04-06T08:30:05+07:00"
}
```

---

### 3.3 — Báo config hiện tại: `station/{device_id}/config`

Phần cứng **publish config của mình** lên server trong 2 trường hợp:

1. **Sau khi boot / reconnect** — gửi toàn bộ config đang dùng
2. **Sau khi nhận config từ server** — echo lại để xác nhận đã áp dụng

```json
{
  "source": "device",
  "analog": {
    "A1": {
      "calc_mode": "weight",
      "weight": 0.015,
      "x1": null,
      "y1": null,
      "x2": null,
      "y2": null
    }
  },
  "rs485_1": {
    "1:0": {
      "calc_mode": "interpolation_2point",
      "weight": null,
      "x1": 0,
      "y1": 0.0,
      "x2": 4095,
      "y2": 100.0
    }
  }
}
```

> **Quan trọng:** luôn có `"source": "device"`. Server sẽ bỏ qua nếu `source = "server"`.

---

## 4. Nhận từ server

### 4.1 — Nhận config: `station/{device_id}/config`

Server gửi xuống khi admin cập nhật cấu hình trên web UI.

```json
{
  "source": "server",
  "mode": {
    "debug": false
  },
  "analog": {
    "A1": {
      "calc_mode": "weight",
      "weight": 0.012207,
      "x1": null,
      "y1": null,
      "x2": null,
      "y2": null
    },
    "A2": {
      "calc_mode": "interpolation_2point",
      "weight": null,
      "x1": 0,
      "y1": 0.0,
      "x2": 4095,
      "y2": 100.0
    }
  }
}
```

**Phần cứng cần làm:**
1. Kiểm tra `source == "server"` → mới xử lý
2. Đọc `mode.debug` → bật/tắt chế độ gửi liên tục
3. Cập nhật calc config cho từng channel được gửi xuống
4. Giữ nguyên config của channel không được đề cập
5. Echo lại config vừa áp dụng với `source: "device"` (xem mục 3.3)

**Bảng calc_mode:**

| `calc_mode` | Công thức | Dùng `weight` | Dùng `x1/y1/x2/y2` |
|-------------|-----------|:---:|:---:|
| `weight` | `real = raw × weight` | ✅ | ❌ |
| `interpolation_2point` | nội suy tuyến tính giữa 2 điểm `(x1→y1)` và `(x2→y2)` | ❌ | ✅ |

**Công thức nội suy 2 điểm:**
```
real = y1 + (raw - x1) × (y2 - y1) / (x2 - x1)
```

---

### 4.2 — Nhận ping: `station/{device_id}/status`

```json
{ "ping": true }
```

→ Phản hồi ngay (xem mục 3.2).

> **Bỏ qua** message `source: "device"` trên topic config (do chính mình gửi lên bị nhận lại).

---

## 5. Flow kết nối đầy đủ

```
[BOOT]
  │
  ├─ Connect MQTT broker (username=device_id, password=mqtt_password)
  │
  ├─ Subscribe: station/{device_id}/config
  ├─ Subscribe: station/{device_id}/status
  │
  ├─ Publish status online:
  │     station/{device_id}/status  {"status":"online","ts":"..."}
  │
  ├─ Publish config hiện tại:
  │     station/{device_id}/config  {"source":"device","analog":{...}}
  │
  └─ Loop:
        ├─ Đọc sensor xong → Publish data:
        │     station/{device_id}/data  {"analog":{...},"rs485_1":{...}}
        │
        ├─ Nhận config từ server → áp dụng → echo lại
        │
        └─ Nhận ping → pong ngay
```

---

## 6. Lưu ý quan trọng

| # | Vấn đề | Quy tắc |
|---|--------|---------|
| 1 | Timestamp **bắt buộc có timezone** | `+07:00` hoặc `Z` — không để naive time |
| 2 | Server tính delay = `server_now - ts` | Nếu delay > 5 phút → đánh dấu `is_late` |
| 3 | Sensor lỗi → gửi `null` | Không bỏ qua, server cần biết để cảnh báo |
| 4 | `source` trên config topic | `"device"` khi phần cứng gửi, `"server"` khi server gửi — **phải bỏ qua đúng chiều** tránh loop |
| 5 | QoS 1 | Tất cả publish/subscribe dùng QoS 1 |
| 6 | Client ID | Phải duy nhất, broker sẽ ngắt client cũ nếu trùng |






---

### [2026-04-07] Sai sót trong ví dụ JSON mục 3.1, 3.3 và mục 4.1

#### Lỗi ở mục 3.1 — ví dụ JSON data payload

Ví dụ JSON ở mục 3.1 (và bảng channel code) dùng sai key cho các group động. Bảng đúng:

| Group | Channel code đúng | Channel code SAI (đã ghi) |
|-------|-------------------|--------------------------|
| `rs485_1` | `V1`, `V2`... | ~~`{slave_id}:{register}`~~ |
| `rs485_2` | `V1`, `V2`... | ~~`{slave_id}:{register}`~~ |
| `tcp` | `V1`, `V2`... | ~~`{slave_id}:{register}`~~ |
| `iec62056` | `V1`, `V2`... | ~~`{obis_code}`~~ |

Ví dụ JSON data payload **đúng** cho các group động:

```json
{
  "rs485_1": {
    "ts": "2026-04-07T08:30:01+07:00",
    "V1": { "raw": 4567, "real": 2.45 },
    "V2": { "raw": 1234, "real": 1.87 }
  },
  "rs485_2": {
    "ts": "2026-04-07T08:30:01+07:00",
    "V1": { "raw": 999, "real": 99.9 }
  },
  "tcp": {
    "ts": "2026-04-07T08:30:02+07:00",
    "V1": { "raw": 5500, "real": 3.12 },
    "V2": { "raw": 2980, "real": 2.98 }
  },
  "iec62056": {
    "ts": "2026-04-07T08:30:03+07:00",
    "V1": { "raw": 123456, "real": 1234.56 }
  }
}
```

#### Lỗi ở mục 3.3 — firmware echo config

Ví dụ JSON mục 3.3 dùng `"1:0"` làm key cho `rs485_1`, sai. Phải dùng `V1`, `V2`...:

```json
{
  "source": "device",
  "rs485_1": {
    "V1": {
      "calc_mode": "interpolation_2point",
      "weight": null,
      "x1": 0,
      "y1": 0.0,
      "x2": 4095,
      "y2": 100.0
    }
  }
}
```

#### Thiếu extra fields trong config của kênh động (mục 4.1)

Mục 4.1 chỉ ví dụ group `analog`, thiếu config cho các kênh động. Server thực tế gửi thêm các fields để firmware biết **phải đọc register nào/địa chỉ nào**.

Config server gửi cho **rs485_1 / rs485_2**:
```json
{
  "source": "server",
  "rs485_1": {
    "V1": {
      "calc_mode": "weight",
      "weight": 0.012,
      "x1": null, "y1": null, "x2": null, "y2": null,
      "slave_id": 1,
      "register": 0,
      "function_code": 3,
      "data_type": "int16",
      "byte_order": "big"
    }
  }
}
```

Config server gửi cho **tcp**:
```json
{
  "source": "server",
  "tcp": {
    "V1": {
      "calc_mode": "weight",
      "weight": 0.01,
      "x1": null, "y1": null, "x2": null, "y2": null,
      "slave_id": 1,
      "register": 1,
      "function_code": 3,
      "data_type": "int16",
      "byte_order": "big",
      "host": "192.168.1.100",
      "port": 502
    }
  }
}
```

Config server gửi cho **iec62056**:
```json
{
  "source": "server",
  "iec62056": {
    "V1": {
      "calc_mode": "weight",
      "weight": 0.001,
      "x1": null, "y1": null, "x2": null, "y2": null,
      "obis_code": "1.0.1.8.0",
      "meter_address": 1,
      "baudrate": 9600
    }
  }
}
```

> **Cơ chế đúng**: Server gán `V1`, `V2`... làm channel code, đồng thời gửi kèm thông số vật lý (`slave_id`, `register`, `obis_code`...) để firmware biết cần đọc gì. Firmware đọc xong thì publish data với đúng key `V1`, `V2`...

---

### [2026-04-07] Thêm field `ingest_type` vào data payload

Firmware có thể khai báo kiểu dữ liệu ở **root level** của payload:

```json
{
  "ingest_type": "backfill",
  "analog": {
    "ts": "2026-04-07T06:00:00+07:00",
    "A1": { "raw": 1023, "real": 12.5 }
  }
}
```

| Giá trị | Ý nghĩa | Khi nào dùng |
|---------|---------|-------------|
| `"realtime"` | Thời gian thực | Mặc định, **không cần gửi** |
| `"backfill"` | Bù dữ liệu | Firmware mất mạng, lưu local, gửi lại sau |

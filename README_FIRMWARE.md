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






---

### [2026-04-08] Thêm OTA (Over-The-Air) firmware update

#### Nhận lệnh OTA qua `station/{device_id}/cmd`

Server gửi lệnh OTA khi admin bấm nút cập nhật firmware trên trang Workbench:

```json
{
  "cmd": "ota_update",
  "url": "http://server-domain/media/firmware/O1SGXJNPFJ.bin",
  "checksum": "a1b2c3d4e5f6...sha256_hex_64_ký_tự..."
}
```

| Field | Kiểu | Mô tả |
|-------|------|-------|
| `cmd` | string | Luôn là `"ota_update"` |
| `url` | string | URL tải file firmware `.bin` (HTTP GET) |
| `checksum` | string | SHA256 hex của file firmware — dùng để verify sau khi tải |

#### Firmware cần làm khi nhận `ota_update`

1. **Tải file** từ `url` (HTTP GET)
2. **Verify checksum**: tính SHA256 của file vừa tải, so với `checksum` — nếu sai → báo `failed`
3. **Flash firmware** vào partition OTA
4. **Restart** thiết bị
5. **Sau khi boot lại** — gửi status online với `fw_version` mới

#### Báo tiến trình OTA qua `station/{device_id}/status`

Trong suốt quá trình OTA, firmware **phải gửi cập nhật tiến trình** lên topic status để server theo dõi realtime:

```json
{
  "ota_status": "downloading",
  "ota_progress": 45,
  "ts": "2026-04-08T10:00:05+07:00"
}
```

**Bảng `ota_status`:**

| `ota_status` | Khi nào gửi | `ota_progress` | Fields thêm |
|-------------|-------------|:--------------:|-------------|
| `downloading` | Đang tải file firmware | 0 → 99 (theo % đã tải) | |
| `flashing` | Đang ghi firmware vào flash | 100 | |
| `rebooting` | Sắp restart | 100 | |
| `done` | Boot lại thành công, firmware mới hoạt động | 100 | `fw_version` |
| `failed` | Bất kỳ bước nào lỗi | giữ nguyên giá trị cuối | `ota_error` |

**Payload mẫu cho từng trạng thái:**

Đang tải (cập nhật nhiều lần theo %):
```json
{
  "ota_status": "downloading",
  "ota_progress": 30,
  "ts": "2026-04-08T10:00:03+07:00"
}
```

Đang flash:
```json
{
  "ota_status": "flashing",
  "ota_progress": 100,
  "ts": "2026-04-08T10:00:10+07:00"
}
```

Sắp restart:
```json
{
  "ota_status": "rebooting",
  "ota_progress": 100,
  "ts": "2026-04-08T10:00:12+07:00"
}
```

Thành công (sau khi boot lại):
```json
{
  "ota_status": "done",
  "ota_progress": 100,
  "fw_version": "1.0.3",
  "ts": "2026-04-08T10:00:20+07:00"
}
```

Thất bại (bất kỳ lúc nào):
```json
{
  "ota_status": "failed",
  "ota_progress": 45,
  "ota_error": "Checksum mismatch",
  "ts": "2026-04-08T10:00:08+07:00"
}
```

**Bảng fields:**

| Field | Kiểu | Bắt buộc | Giới hạn | Mô tả |
|-------|------|:--------:|----------|-------|
| `ota_status` | string | ✅ | max 20 ký tự | Trạng thái hiện tại |
| `ota_progress` | int | ✅ | 0–100 | Phần trăm tiến trình |
| `ota_error` | string | ❌ | max 255 ký tự | Mô tả lỗi (chỉ khi `failed`) |
| `fw_version` | string | ❌ | max 32 ký tự | Phiên bản mới (chỉ khi `done`) |
| `ts` | string | ✅ | ISO 8601 + timezone | Thời điểm gửi |

#### Flow OTA đầy đủ

```
[Server gửi cmd]
  │  station/{device_id}/cmd
  │  {"cmd":"ota_update","url":"...","checksum":"..."}
  │
  ▼
[Firmware nhận lệnh]
  │
  ├─ Bắt đầu tải → publish status:
  │    {"ota_status":"downloading","ota_progress":0,"ts":"..."}
  │
  ├─ Tải được 30% → publish:
  │    {"ota_status":"downloading","ota_progress":30,"ts":"..."}
  │
  ├─ Tải xong 100% → verify checksum
  │    ├─ Sai → {"ota_status":"failed","ota_error":"Checksum mismatch","ts":"..."}
  │    └─ Đúng → tiếp tục
  │
  ├─ Flash firmware → publish:
  │    {"ota_status":"flashing","ota_progress":100,"ts":"..."}
  │
  ├─ Restart → publish:
  │    {"ota_status":"rebooting","ota_progress":100,"ts":"..."}
  │
  └─ [REBOOT]
       │
       ├─ Publish status online (như mục 3.2)
       └─ Publish OTA done:
            {"ota_status":"done","ota_progress":100,"fw_version":"1.0.3","ts":"..."}
```

#### Lưu ý OTA

| # | Vấn đề | Quy tắc |
|---|--------|---------|
| 1 | Checksum **bắt buộc verify** | Không flash nếu SHA256 không khớp |
| 2 | Gửi progress thường xuyên | Tối thiểu mỗi 10% hoặc mỗi 5 giây |
| 3 | Timeout tải file | Nên timeout sau 60–120 giây, báo `failed` |
| 4 | Rollback | Nếu firmware mới boot lỗi → rollback về partition cũ, báo `failed` |
| 5 | OTA status gửi trên topic **status** | Cùng topic `station/{device_id}/status`, **không phải** topic cmd |
| 6 | Kết hợp với status online | Sau reboot, 1 message có cả `"status":"online"` và `"ota_status":"done"` là hợp lệ |





---

## Topic 4: `station/{device_id}/cmd`

**Hướng:** Server → Trạm  
**QoS:** 1  
**Mục đích:** Server gửi lệnh điều khiển xuống trạm (VD: reset kênh đếm mưa lúc 0h)

### Payload

```json
{
  "cmd": "reset_di",
  "cmd_id": "550e8400-e29b-41d4-a716-446655440000",
  "channel": "DI1"
}
```

| Trường | Kiểu | Bắt buộc | Mô tả |
|---|---|---|---|
| `cmd` | string | ✓ | Mã lệnh cần thực hiện (xem bảng lệnh bên dưới) |
| `cmd_id` | string (UUID v4) | ✓ | ID duy nhất của lệnh — dùng để gửi lại trong `ack` |
| `channel` | string | Tùy lệnh | Kênh cụ thể cần tác động |

### Bảng lệnh hiện tại

| `cmd` | `channel` | Hành động |
|---|---|---|
| `reset_di` | `DI1` | Reset bộ đếm xung kênh DI1 về 0 (reset lượng mưa ngày) |

### Yêu cầu phần cứng

1. **Subscribe** topic `station/{device_id}/cmd` sau khi kết nối MQTT thành công.
2. Khi nhận được message:
   - Parse JSON, đọc trường `cmd`.
   - Thực hiện hành động tương ứng (VD: `reset_di` → ghi DI1 = 0).
   - Gửi lại ack về topic `station/{device_id}/ack` với đúng `cmd_id` đã nhận.
3. Nếu không gửi ack trong **60 giây**, server sẽ gửi lại lệnh. Server retry tối đa **5 lần**.

---

## Topic 5: `station/{device_id}/ack`

**Hướng:** Trạm → Server  
**QoS:** 1  
**Mục đích:** Trạm xác nhận đã thực hiện xong lệnh từ server

### Payload

```json
{
  "ack": "550e8400-e29b-41d4-a716-446655440000",
  "status": "ok"
}
```

| Trường | Kiểu | Bắt buộc | Mô tả |
|---|---|---|---|
| `ack` | string (UUID v4) | ✓ | Sao chép nguyên `cmd_id` nhận từ lệnh |
| `status` | string | ✓ | Kết quả thực hiện: `"ok"` (hiện tại chỉ chấp nhận `"ok"`) |

### Luồng hoàn chỉnh (cmd → ack)

```
Server                              Trạm (Firmware)
  │                                     │
  │── CMD  topic: .../cmd ──────────────►│
  │   {cmd, cmd_id, channel}             │
  │                                      │  (thực hiện lệnh)
  │◄─ ACK  topic: .../ack ──────────────│
  │   {ack: <cmd_id>, status: "ok"}      │
  │                                     │
  │  [nếu không nhận ack trong 60s]      │
  │── CMD retry (tối đa 5 lần) ─────────►│
```

### Lưu ý quan trọng

- Trường `ack` trong payload phải là **chính xác** giá trị `cmd_id` nhận được — server dùng UUID này để tra cứu.
- Nếu sau 5 lần retry vẫn không có ack, server sẽ tạo cảnh báo (notification) và **không gửi thêm**.
- Trạm chỉ cần **Publish** topic `.../ack`. Server không publish lên topic này.


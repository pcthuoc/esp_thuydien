# BUG TRACKER

---

## BUG #1 — 4G init không hoàn thành trước khi chuyển AP mode

**Ngày phát hiện:** 2026-04-19  
**Mức độ:** Cao  
**Trạng thái:** Chưa sửa

### Mô tả

Khi thiết bị được cấu hình chạy mode 4G nhưng không có SIM hoặc mạng bị lỗi, logic hiện tại chưa chờ 4G init hoàn tất trước khi chuyển sang AP mode. Dẫn đến thiết bị vẫn nhảy vào AP mode trong khi modem chưa kịp xác nhận trạng thái lỗi rõ ràng.

**Luồng đúng mong muốn:**
1. Boot → khởi động 4G
2. 4G init → xác định thành công hoặc thất bại
3. Nếu thất bại → mới chuyển AP mode

### Log minh họa

```
22:38:43.604 -> [BOOT] Net mode: 4g
22:38:43.738 -> [4G] Khởi động modem A7680C...
22:38:51.754 -> [BTN] Long press (3s) -> AP MODE requested       <-- nút bấm giữa chừng init
22:39:18.784 -> [4G] Modem info: ...IMEI: 868508088937670
22:39:18.784 -> [4G] Chờ đăng ký mạng............................................................
22:40:19.156 -> [4G] Init hủy bỏ (AP mode requested)            <-- bị hủy chứ không phải lỗi thật
22:40:19.156 -> [BOOT] 4G khởi động thất bại
```

### Ghi chú

- Cần phân biệt rõ 2 trường hợp: 4G thất bại do lỗi thật vs 4G bị hủy do user yêu cầu AP mode.
- Nếu không có SIM/mạng, thiết bị phải tự detect lỗi và chuyển AP mode mà không cần user bấm nút.

---

## BUG #2 — Nút Scan WiFi trên web đôi khi không hoạt động

**Ngày phát hiện:** 2026-04-19  
**Mức độ:** Trung bình  
**Trạng thái:** Chưa sửa

### Mô tả

Nút scan WiFi trên web UI hoạt động không ổn định. Thi thoảng bấm scan không có kết quả, phải scan nhiều lần, F5 reload trang rồi mới scan lại được.

**Biểu hiện:**
- Bấm Scan → không có danh sách WiFi trả về
- F5 lại trang → scan lần nữa mới có kết quả
- Xảy ra không nhất quán (intermittent)

### Nguyên nhân nghi ngờ

- Race condition giữa request scan và response trả về (scan chưa xong đã query kết quả).
- WebServer không xử lý đúng trạng thái đang quét (busy state).
- Client-side JS gọi lại kết quả quá sớm trước khi ESP32 hoàn thành scan.

### Ghi chú

- Cần thêm cơ chế polling hoặc callback để đảm bảo scan hoàn tất trước khi trả kết quả về web.
- Có thể thêm loading indicator và retry logic phía `app.js`.

---


22:38:43.604 -> [BOOT] Rain gauge OK (DI1=GPIO1)
22:38:43.604 -> [BOOT] Net mode: 4g
22:38:43.738 -> [4G] Khởi động modem A7680C...
22:38:51.754 -> [BTN] Long press (3s) -> AP MODE requested
22:39:18.784 -> [4G] Modem info: Manufacturer: SIMCOM INCORPORATED Model: A7680C-LANS Revision: V11.0.01 IMEI: 868508088937670
22:39:18.784 -> [4G] Chờ đăng ký mạng............................................................
22:40:19.156 -> [4G] Init hủy bỏ (AP mode requested)
22:40:19.156 -> [BOOT] 4G khởi động thất bại
22:40:19.156 -> [102547][E][vfs_api.cpp:105] open(): /sdcard/config/rs485.json does not exist, no permits for creation
22:40:19.156 -> [SD] Failed to open /config/rs485.json for reading
22:40:19.156 -> [MODBUS] Không tìm thấy /config/rs485.json
22:40:19.156 -> [BOOT] Modbus RTU: no config
22:40:19.156 -> [W5500] Init SPI (HSPI)...
22:40:20.253 -> [W5500] Static IP: 192.168.0.200
22:40:20.253 -> [W5500] Chip OK (status=3)
22:40:20.253 -> [BOOT] W5500 OK
22:40:20.253 -> [103628][E][vfs_api.cpp:105] open(): /sdcard/config/tcp.json does not exist, no permits for creation
22:40:20.253 -> [SD] Failed to open /config/tcp.json for reading
22:40:20.253 -> [MBTCP] Không tìm thấy /config/tcp.json
22:40:20.253 -> [BOOT] Modbus TCP: no config
22:40:20.253 -> [DATA] Active: AI=1 ENC=1 DI=1 RTU=0 TCP=0
22:40:20.301 -> [DATA] Calc configs loaded
22:40:20.301 -> [DATA] Modbus poll start
22:40:20.301 -> [DATA] Modbus poll done
22:40:20.301 -> [DATA] Init OK, debug=false, adc=2s, publish=60s
22:40:20.301 -> [BOOT] Data collector OK
22:40:20.301 -> [103662][E][vfs_api.cpp:105] open(): /sdcard/config/system.json does not exist, no permits for creation
22:40:20.301 -> [SD] Failed to open /config/system.json for reading
22:40:20.301 -> [AP] SSID: THUYDIEN_CFG
22:40:20.395 -> [WEB] AP started: THUYDIEN_CFG
22:40:20.395 -> [WEB] IP: 192.168.4.1
22:40:20.441 -> [WEB] Server started on port 80

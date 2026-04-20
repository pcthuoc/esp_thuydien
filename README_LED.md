# Hướng dẫn đèn LED trạng thái

Thiết bị có 1 đèn LED RGB (NeoPixel, GPIO42). Mỗi màu/hiệu ứng biểu thị trạng thái hoạt động của hệ thống.

---

## Bảng màu

| Màu | Hiệu ứng | Trạng thái | Ý nghĩa |
|-----|----------|------------|---------|
| 🟡 **Vàng** | Nhấp nháy nhanh (200ms) | `BOOTING` | Đang khởi động hệ thống |
| 🔵 **Xanh dương** | Nhấp nháy chậm (500ms) | `WIFI_CONNECTING` | Đang kết nối WiFi hoặc khởi động modem 4G |
| 🔵 **Xanh dương** | Thở (fade in/out 2s) | `MQTT_CONNECTING` | WiFi/4G đã có — đang kết nối MQTT broker |
| 🟢 **Xanh lá** | Sáng liên tục | `ONLINE_OK` | Kết nối MQTT ổn định, hoạt động bình thường |
| 🟢 **Xanh lá** | Nhấp nháy chậm (1s) | `ONLINE_SENSOR_WARN` | MQTT OK nhưng có cảnh báo cảm biến |
| 🟠 **Cam** | Nhấp nháy chậm (1s) | `OFFLINE_BUFFERING` | Mất mạng — đang lưu dữ liệu vào SD card |
| 🔴 **Đỏ** | Sáng liên tục | `ERROR_CRITICAL` | Lỗi nghiêm trọng |
| 🟣 **Tím** | Thở (fade in/out 2s) | `CONFIG_AP_MODE` | Đang chạy WiFi AP để cấu hình (bấm giữ nút 3s) |

---

## Tín hiệu nháy tạm thời

Ngoài các trạng thái trên, đèn còn chớp tạm thời để báo kết quả gửi dữ liệu:

| Màu | Số lần chớp | Ý nghĩa |
|-----|-------------|---------|
| 🟢 Xanh lá | 1 lần | Gửi dữ liệu MQTT thành công |
| 🔴 Đỏ | 2 lần | Gửi thất bại — dữ liệu được lưu SD (backfill) |

Sau khi chớp xong, đèn tự quay lại trạng thái trước đó.

---

## Trình tự khởi động bình thường

```
🟡 Vàng nhấp nháy     → Đang boot (vài giây)
🔵 Xanh dương nhấp nháy → Kết nối WiFi / khởi động modem 4G
🔵 Xanh dương thở     → Kết nối MQTT
🟢 Xanh lá liên tục   → Hệ thống hoạt động bình thường
```

---

## Khi mất mạng

```
🟠 Cam nhấp nháy → Mất WiFi/4G, dữ liệu đang được lưu SD
```
Khi mạng có lại → đèn tự chuyển về 🟢 và tự động gửi bù dữ liệu đã lưu.

---

## Vào chế độ cấu hình (AP Mode)

Bấm giữ nút **3 giây** → đèn chuyển sang 🟣 **Tím thở**  
Kết nối WiFi tên `THUYDIEN_CFG` (mật khẩu `12345678`) rồi mở trình duyệt vào `192.168.4.1` để cấu hình.

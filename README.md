# Hệ thống cảnh báo va chạm tại khúc cua khuất tầm nhìn

Hệ thống IoT dùng 3 ESP32, radar HLK-LD2451 và LoRa E32 để cảnh báo xe tại khúc cua, đoạn đèo, lối ra vào khu dân cư hoặc đường nội bộ có tầm nhìn hạn chế. Hệ thống đồng thời thu thập lưu lượng và tốc độ phương tiện, hiển thị trên web theo thời gian thực.

## Mục lục
1. [Nguyên lý hoạt động](#nguyên-lý-hoạt-động)
2. [Cấu trúc thư mục](#cấu-trúc-thư-mục)
3. [Phần cứng](#phần-cứng)
4. [Sơ đồ đấu dây](#sơ-đồ-đấu-dây)
5. [Cài đặt phần mềm](#cài-đặt-phần-mềm)
6. [Cấu hình](#cấu-hình)
7. [Nạp code](#nạp-code)
8. [Giao thức LoRa](#giao-thức-lora)
9. [Web giám sát](#web-giám-sát)
10. [Xử lý sự cố](#xử-lý-sự-cố)
11. [Lưu ý an toàn và hạn chế](#lưu-ý-an-toàn-và-hạn-chế)

## Nguyên lý hoạt động

```
 Hướng A                                          Hướng B
┌──────────────┐        LoRa 433 MHz        ┌──────────────┐
│ ESP1 (trạm A)│◄──────────┐    ┌──────────►│ ESP2 (trạm B)│
│ Radar + LED  │           │    │           │ Radar + LED  │
└──────────────┘      ┌────┴────┴────┐      └──────────────┘
                      │ ESP3 (điều   │
                      │ khiển)       │──► 2 biển cảnh báo
                      └──────┬───────┘
                             │ WiFi + MQTT (1883)
                             ▼
                    broker.hivemq.com ──► Web (WebSocket 8884)
```

| Trạng thái | Điều kiện | Biển cảnh báo |
|---|---|---|
| IDLE | Không có xe | Tắt cả hai |
| A_ONLY | Chỉ có xe từ hướng A | Bật biển ở hướng B |
| B_ONLY | Chỉ có xe từ hướng B | Bật biển ở hướng A |
| BOTH | Xe từ cả hai hướng | Bật cả hai |

1. Trạm A hoặc B phát hiện xe **đang lại gần** trong phạm vi 50 m, rồi gửi trạng thái qua LoRa cho ESP3.
2. ESP3 ghép trạng thái, bật biển cảnh báo ở hướng đối diện và gửi lệnh hiển thị cho màn hình LED P5 của trạm đối diện (hiện tốc độ và dòng chữ chạy "HUONG DOI DIEN CO XE DI NHANH").
3. Xe rời vùng giám sát và trạng thái ổn định **3 giây** thì ESP3 tắt cảnh báo.
4. ESP3 đẩy trạng thái và dữ liệu từng xe lên MQTT để web thống kê.

**Chống rơi gói tin:**
- Mọi bản tin có checksum XOR, bản tin lỗi hoặc cắt dở bị bỏ.
- Bản tin quan trọng (xe xuất hiện, xe rời đi, thống kê từng xe) có số thứ tự, cần ACK và được gửi lại tối đa 8 lần.
- ESP3 luôn ACK nhưng chỉ xử lý bản tin có số thứ tự mới, nên không đếm trùng xe.
- Trạm chờ chân AUX rảnh và nghỉ ngẫu nhiên 5–60 ms trước khi phát để A và B ít đụng nhau.

## Cấu trúc thư mục

> Đổi tên cho khớp với repo của bạn.

```
.
├── esp1_tram_A/esp1_tram_A.ino     # Trạm A: radar + LoRa + màn hình P5
├── esp2_tram_B/esp2_tram_B.ino     # Trạm B: giống ESP1, khác NODE_ID
├── esp3_dieu_khien/esp3_dieu_khien.ino  # Bộ điều khiển: LoRa + WiFi + MQTT + biển báo
├── web/index.html                  # Web giám sát
└── README.md
```

## Phần cứng

| Thiết bị | Số lượng | Ghi chú |
|---|---|---|
| ESP32 DevKit (WROOM) | 3 | Không dùng WROVER cho ESP1/ESP2 vì GPIO16, GPIO17 bị PSRAM chiếm |
| Radar HLK-LD2451 | 2 | Cho ESP1 và ESP2 |
| LoRa E32-433T30D | 3 | Mỗi ESP một module, **bắt buộc gắn ăng-ten** |
| Màn hình LED P5 HUB75 64x32 (4-scan) | 2 | Cho ESP1 và ESP2 |
| Nguồn 5V 3–4A | 2 | Riêng cho màn hình LED |
| Nguồn 5V ≥ 1A | 3 | Cho mỗi ESP32 và E32 |
| Module relay 5V hoặc transistor/MOSFET | 2 | Điều khiển biển cảnh báo ở ESP3 |
| Tụ 470–1000 µF | 3 | Đặt gần chân VCC của E32 |

## Sơ đồ đấu dây

ESP1 và ESP2 đấu giống hệt nhau, chỉ khác code nạp vào.

### ESP1 và ESP2

**Radar HLK-LD2451**

| LD2451 | ESP32 |
|---|---|
| VCC | 5V (thử 3V3 nếu radar chạy ổn) |
| GND | GND |
| TX | GPIO16 |
| RX | GPIO17 |

**LoRa E32-433T30D**

| E32 | ESP32 |
|---|---|
| VCC | 5V |
| GND | GND |
| TX | GPIO26 |
| RX | GPIO25 |
| AUX | GPIO27 |
| M0 | GPIO32 |
| M1 | GPIO33 |

**Màn hình HUB75 (64x32, 4-scan)**

| HUB75 | ESP32 | HUB75 | ESP32 |
|---|---|---|---|
| R1 | GPIO13 | A | GPIO23 |
| G1 | GPIO14 | B | GPIO5 |
| B1 | GPIO21 | C | GPIO4 |
| R2 | GPIO22 | D, E | Không nối |
| G2 | GPIO18 | LAT | GPIO2 |
| B2 | GPIO19 | OE | GPIO15 |
| | | CLK | GPIO12 |

Màn hình cấp 5V riêng, nối GND chung với ESP32.

### ESP3 (bộ điều khiển)

**LoRa E32:** đấu giống ESP1/ESP2.

**Biển cảnh báo (kích mức HIGH, qua relay hoặc transistor)**

| ESP32 | Điều khiển | Sáng khi |
|---|---|---|
| GPIO18 | Biển đặt ở hướng A | Có xe từ hướng B |
| GPIO19 | Biển đặt ở hướng B | Có xe từ hướng A |

Không nối tải trực tiếp vào GPIO. Nguồn biển báo lấy riêng, chỉ chung GND.

## Cài đặt phần mềm

1. Cài **Arduino IDE 2.x**.
2. **File → Preferences**, thêm vào *Additional boards manager URLs*:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Tools → Board → Boards Manager**, cài **esp32** (Espressif Systems).
4. **Tools → Board → esp32 → ESP32 Dev Module** (chọn nhầm Arduino Uno sẽ báo lỗi `'Serial1' was not declared`).
5. Cài thư viện trong **Tools → Manage Libraries**:

| Thư viện | Dùng cho |
|---|---|
| ESP32-HUB75-MatrixPanel-I2S-DMA (mrfaptastic) | ESP1, ESP2 |
| Adafruit GFX Library, Adafruit BusIO | ESP1, ESP2 (tự kéo theo) |
| PubSubClient (Nick O'Leary) | ESP3 |

## Cấu hình

**ESP3** (đầu file):

```cpp
const char* WIFI_SSID = "TEN_WIFI_2.4GHZ";
const char* WIFI_PASS = "MAT_KHAU";
const char* TOPIC_STATE = "iot677/khuccua01/state";
const char* TOPIC_EVENT = "iot677/khuccua01/event";
```

- ESP32 **chỉ hỗ trợ WiFi 2.4 GHz**, không kết nối được mạng chỉ phát 5 GHz.
- Broker công cộng ai cũng đọc và ghi được. **Đổi tiền tố topic** sang chuỗi riêng và đổi giống hệt trong `web/index.html`.
- Đừng đưa mật khẩu WiFi thật lên GitHub.

**ESP1 và ESP2:**

| Hằng số | Mặc định | Ý nghĩa |
|---|---|---|
| `NODE_ID` | `'A'` / `'B'` | Mã trạm, mỗi trạm một giá trị |
| `MAX_DIST_M` | 50 | Chỉ nhận xe trong phạm vi này (m) |
| `MIN_SPEED_KMH` | 5 | Bỏ qua mục tiêu chậm hơn |
| `CONFIRM_HITS` | 2 | Số khung radar liên tiếp để xác nhận có xe (1 nhạy hơn, 3 lọc nhiễu tốt hơn) |
| `HOLD_MS` | 2000 | Mất mục tiêu bao lâu thì coi là xe đã đi |
| `FAST_KMH` | 30 | Từ mức này màn hình hiện màu đỏ "đi nhanh" |

**ESP3:**

| Hằng số | Mặc định | Ý nghĩa |
|---|---|---|
| `CLEAR_CONFIRM_MS` | 3000 | Hết xe ổn định bao lâu thì tắt biển |
| `NODE_TIMEOUT_MS` | 6000 | Mất tín hiệu một trạm quá thời gian này thì coi như không có xe |

Ngưỡng "Xe vượt ngưỡng" trên web (ô **Ngưỡng**, mặc định 30 km/h) nên trùng với `FAST_KMH`.

## Nạp code

| Board | Nạp file |
|---|---|
| ESP32 số 1 | `esp1_tram_A` |
| ESP32 số 2 | `esp2_tram_B` |
| ESP32 số 3 | `esp3_dieu_khien` |

- Nếu nạp bị treo ở `Connecting...`, giữ nút **BOOT** trên board cho đến khi bắt đầu nạp.
- GPIO2, 5, 12, 15 là chân strapping. Nếu board reset liên tục hoặc không nạp được, **rút cáp HUB75** ra, nạp xong cắm lại.
- Mở Serial Monitor tốc độ **115200** để theo dõi.

**Cấu hình E32:** ba module phải cùng địa chỉ, kênh và tốc độ truyền không dây. Code dùng cấu hình mặc định (địa chỉ 0, kênh 0x17, 9600 8N1) và đặt M0 = M1 = LOW (truyền trong suốt). Module đã dùng cho dự án khác có thể mang cấu hình cũ, cần đặt lại về mặc định (đối chiếu datasheet trước khi ghi).

## Giao thức LoRa

Khung bản tin: `$<nội dung>*<CS>\n`, trong đó `CS` là checksum XOR của phần nội dung, viết hex 2 ký tự.

| Loại | Định dạng | Hướng | Cần ACK |
|---|---|---|---|
| `S` | `S,<node>,<có xe>,<tốc độ>,<khoảng cách>,<tổng xe>` | Trạm → ESP3, định kỳ | Không |
| `P` | `P,<node>,<seq>,<có xe>,<tốc độ>,<khoảng cách>,<tổng xe>` | Trạm → ESP3, khi xe xuất hiện hoặc rời đi | Có |
| `V` | `V,<node>,<seq>,<tốc độ đỉnh>,<tổng xe>` | Trạm → ESP3, thống kê từng xe | Có |
| `K` | `K,<node>,<seq>` | ESP3 → trạm | ACK |
| `W` | `W,<signA>,<signB>,<tốc độ A>,<tốc độ B>` | ESP3 → các trạm, lệnh hiển thị | Không (gửi lặp) |

### Topic MQTT

| Topic | Nội dung |
|---|---|
| `.../state` | JSON trạng thái hệ thống (retained), gửi mỗi 2 giây hoặc khi thay đổi |
| `.../event` | JSON từng xe đi qua: `{"n":"A","spd":48,"cnt":12}` |

## Web giám sát

Mở `web/index.html` bằng trình duyệt (cần internet), hoặc đưa lên GitHub Pages để xem từ xa.

Chức năng: trạng thái hệ thống theo thời gian thực, thông tin từng trạm và biển báo, tổng xe, tốc độ trung bình, tốc độ cao nhất, số xe vượt ngưỡng, biểu đồ tốc độ từng xe, biểu đồ lưu lượng 12 giờ gần nhất, bảng xe gần đây, xuất CSV.

Web nhận dữ liệu qua WebSocket `wss://broker.hivemq.com:8884/mqtt`. Thống kê lưu trong `localStorage` của trình duyệt (tối đa 2000 xe) và **chỉ ghi nhận khi trang đang mở**.

## Xử lý sự cố

| Hiện tượng | Nguyên nhân thường gặp | Cách xử lý |
|---|---|---|
| Biên dịch báo `'Serial1' was not declared` | Đang chọn board Arduino Uno | Chọn **ESP32 Dev Module** |
| ESP3 không có dòng `MQTT connected` | WiFi 5 GHz hoặc sai mật khẩu | Dùng SSID 2.4 GHz |
| Web báo "Bộ điều khiển mất kết nối" | ESP3 mất WiFi/MQTT, hoặc topic khác nhau giữa ESP3 và web | Kiểm tra Serial ESP3 và chuỗi topic |
| Trạm in `TX:` nhưng ESP3 không có `RX:` | Sóng LoRa không tới (bản tin `S` không có ACK nên trạm vẫn in `TX:`) | Xem mục bên dưới |
| Trạm in `DROP (khong co ACK)` | ESP3 không nhận được trạm, hoặc trạm không nhận được ACK | Xem mục bên dưới |
| Báo giả khi không có xe | Nhiễu radar | Tăng `CONFIRM_HITS`, đặt radar hướng thẳng dọc trục đường |
| Bỏ sót xe | Ngưỡng quá chặt | Giảm `CONFIRM_HITS` về 1, kiểm tra góc đặt radar |
| Hình trên LED bị xáo trộn dòng | Sai kiểu quét | Đổi `FOUR_SCAN_32PX_HIGH` theo loại panel |
| ESP32 reset khi LoRa phát | Nguồn yếu | Nguồn 5V ≥ 1A, thêm tụ 470–1000 µF gần VCC của E32 |

**Khi LoRa không thông:**
1. Kiểm tra ăng-ten đã gắn chặt.
2. Kiểm tra dây TX/RX không bị đảo, M0 và M1 đã nối thật vào GPIO32 và GPIO33.
3. Quét tay trước radar để trạm gửi bản tin `P`, xem Serial của trạm có `ACK n` không.
4. Hoán đổi module E32 giữa hai ESP để xác định lỗi ở module hay ở dây/board.
5. Đọc cấu hình từng module (lệnh `C1 C1 C1` ở chế độ cấu hình), ba module phải giống nhau.

## Lưu ý an toàn và hạn chế

- **Không bật nguồn E32 khi chưa gắn ăng-ten**, có thể hỏng khuếch đại công suất.
- Radar chỉ đo tốc độ chính xác khi xe chạy thẳng lại gần theo trục radar. Nên đặt cao khoảng 1–1,5 m.
- Hệ thống là **công cụ hỗ trợ cảnh báo**, không thay thế biển báo giao thông, gương cầu lồi hoặc đèn tín hiệu theo quy định.
- Broker MQTT công cộng không bảo mật. Với triển khai thật nên dùng broker riêng có tài khoản.
- Tốc độ và số xe trên web phụ thuộc vào thời gian trang mở. Muốn thống kê liên tục cần lưu dữ liệu ở server hoặc thẻ nhớ.
- Dải 433 MHz và công suất 30 dBm cần tuân thủ quy định tần số tại nơi lắp đặt.

## Giấy phép

Nguyen Ngoc Tu - TUIOT

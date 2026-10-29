# OneCast: kiểm chứng trên máy thật

Ngày: 2026-10-29. Nhánh `feat/onecast-v1` (sau khi gộp Task 18 ở `4af3d5c`).
Máy: Windows 11 Home 10.0.26100, M-Audio M-Track Solo (ASIO "M-Audio M-Track Solo and Duo ASIO", 48 kHz, 128 mẫu), VB-Audio Virtual Cable, tai nghe iBasso-DC03-Pro là đầu ra mặc định của Windows (app không bao giờ mở nó).

Cách đo chung: `OneCast.exe --test-signal --latency-log --minimized` (sine 440 Hz thay cho mic, đi qua toàn bộ chuỗi mặc định) và `OneCastProbe.exe --device "CABLE Output" --sine 440` thu CABLE Output, báo mọi chỗ rớt, nhảy pha hay tụt mức. Script: `.superpowers/sdd/.../soak.ps1` (ghi CPU mỗi 30 s). Probe chạy qua Windows Audio, nên một "capture xrun" của chính probe có thể tự tạo ra một glitch; glitch trùng xrun của probe được ghi riêng.

## Kết quả theo từng bước của Task 15

| Bước | Nội dung | Kết quả |
|---|---|---|
| 1 | Build sạch 4 target Release, `OneCastTests.exe` | PASS, 191 case (sau Task 18) |
| 2 | Render fixture với từng preset, nghe thử | Đã render 5 file (`build/listen/`) và gửi người dùng. Mức ồn ở đoạn lặng: gốc -49.8 dBFS, Nhẹ -57.1, AI -79.8, Phòng ồn -109.4, Phát thanh -53.7 dBFS |
| 3 | Mic thật ASIO M-Track kênh 1, 128 mẫu, ra CABLE Input | PASS: `init: opened type="ASIO" device="M-Audio M-Track Solo and Duo ASIO"`, 48 kHz, ra "CABLE Input (VB-Audio Virtual Cable)" |
| 4 | Soak 30 phút | Xem mục "Độ trễ và rớt tiếng" |
| 5 | Rút và cắm lại M-Track khi đang chạy | PASS: rút ở 22.0 s, `device lost: "M-Audio M-Track Solo and Duo ASIO" (closed)` ở 23.3 s, cắm lại ở 34.1 s, `reopen: type="ASIO" device="M-Audio M-Track Solo and Duo ASIO" 48000 Hz, 128 samples` + `device reconnected` ở 36.2 s; không mở mic nào khác, đầu ra vẫn là CABLE Input |
| 6 | Đường WASAPI với mic Realtek | PASS: `init: opened type="Windows Audio" device="Microphone Array (Realtek(R) Audio)"`, ra CABLE Input, tổng 39.4 ms (khối WASAPI 10 ms); "CABLE Output" không xuất hiện trong log và bị loại khỏi danh sách đầu vào |
| 7 | Đóng gói, chạy từ ZIP | PASS: `dist/OneCast-0.1.0-win64.zip` (OneCast.exe, MSVCP140.dll, VCRUNTIME140.dll, VCRUNTIME140_1.dll, HUONG-DAN.txt, THIRD-PARTY-NOTICES.txt), exe giải nén chạy được, mở M-Track và CABLE Input |
| thêm | Trình cài đặt `-Setup.exe` | PASS: `package.ps1 -MakeSetupExe` ra `OneCast-0.1.0-win64-Setup.exe` (3.7 MB); cài vào `%LOCALAPPDATA%\Programs\OneCast`, tạo lối tắt Start Menu ("OneCast", "Gỡ OneCast") và Desktop, tự mở app. Gỡ cài xóa thư mục chương trình, 3 lối tắt và `%APPDATA%\OneCast`; khóa HKCU Run không bị đụng (không có trước và sau) |
| thêm | Bấm thử giao diện | PASS: ô giá trị núm nhận `9` (+9.0 dB), `99` kẹp về +24.0 dB, `abc` + Esc giữ nguyên, `6 dB` về +6.0 dB. Hộp Tùy chọn (Chạy cùng Windows, Khởi động thu nhỏ, phím tắt Mute Ctrl+Alt+M), menu khay (Hiện OneCast, Tắt tiếng, Thoát) và Thoát đều chạy đúng (người dùng bấm, vì các cửa sổ bật lên tự đóng khi công cụ điều khiển máy chuyển tiêu điểm) |

## Độ trễ và rớt tiếng

Máy này thỉnh thoảng khựng cả hệ thống 10-17 ms (cả luồng ghi lẫn engine âm thanh của Windows cùng dừng), nên endpoint không báo cạn nhưng CABLE vẫn rớt nếu khoảng khựng dài hơn bộ đệm. Đường ra vì vậy tự thích nghi (Task 17, 18):

- Chế độ IAudioClient3 low-latency của Windows, chu kỳ khởi đầu 2 x chu kỳ nhỏ nhất (5.33 ms, bộ đệm 11.7 ms) khi chưa học được gì.
- Mỗi lần luồng ghi bị đánh thức trễ hơn mức bộ đệm che được (sau 1 s đầu), app tăng chu kỳ lên mức che được khoảng khựng đó cộng 20 %, nhớ lại cho lần chạy sau. Hết nấc thì chuyển hẳn sang shared 10 ms.
- Khi đầu vào giao trễ, app ghi phần đang có (short read) thay vì chèn im lặng rồi chờ nạp lại.

| Lần đo | Chế độ | Tổng trễ | Glitch của probe | Bộ đếm của app |
|---|---|---|---|---|
| Soak B, 30 phút | shared 10 ms (kiểu LiveVocal), bộ đệm 22 ms | 48.4 ms | 1 (lúc khởi động) | late 0, starved 0 |
| Soak A, 30 phút | low-latency 2.67 ms, bộ đệm 5.8 ms (Task 17) | 23.2 ms | 15 (nhảy 2-8 ms, một lần rớt 15 ms) | late 23, maxGap 17.4 ms, starved 0 |
| Task 18 bản đầu, 15 phút | tăng 2.67 -> 9 ms sau khoảng khựng 16.4 ms | 48.2 ms (đệm kép) | 1 sau khi tăng (trùng xrun của probe) | risky 2 |
| **Bản cuối, 30 phút** | 5.33 ms, không cần tăng | **28.9-29.5 ms** | **1** (12.8 ms ở 29.6 s, trùng xrun của probe) | late 1 (khởi động), starved 0, resync 0, underrun 2 (khởi động), short 15 |
| Bản cuối, 10 phút ở 9 ms | trạng thái đã tăng (hint 432) | 37.3-46.3 ms | 0 | late 0, short 0 |
| Bản cuối, 10 phút, OBS đang thu CABLE Output | 5.33 ms, không cần tăng | 28.8-29.8 ms | 0 | late 0, maxGap 7.9 ms, starved 0, underrun 0 |

CPU của OneCast: trung bình 6.4 % của một nhân (tối đa 16 %) ở bản cuối. Mục tiêu "< 5 %" của kế hoạch không đạt; con số đo bằng thời gian CPU của tiến trình mỗi 30 s.

Phát hiện: có lúc (khoảng 14:40 hôm đo, khi Discord và OBS cùng mở, có thể đang trong cuộc gọi) VB-Cable đổi nhịp, luồng ghi của OneCast chỉ được đánh thức khoảng mỗi 15 ms ở chu kỳ 5.33 ms, ở mọi lần khởi động. App tự tăng chu kỳ lên khoảng 8.5 ms ngay sau giây đầu, tổng khoảng 37 ms. Tắt Discord/OBS thì hết; mở lại riêng OBS (thu CABLE Output) thì vẫn sạch ở 5.33 ms, nên nguyên nhân cụ thể chưa xác định. Cơ chế tự thích nghi xử lý được cả hai trường hợp.

## Còn lại và giới hạn đã biết

- Độ trễ hiển thị không gồm phần bên trong VB-Cable và phía Discord/OBS.
- Không tự hạ chu kỳ trong một phiên; chu kỳ đã học được nhớ cho các lần chạy sau (sửa tay `outputPeriodHintFrames` trong `%APPDATA%\OneCast\settings.json`, 0 = học lại).
- Một vài lần rớt 12-45 ms trùng với xrun phía thu của probe hoặc với lúc đầu vào khựng; không bộ đệm hợp lý nào che hết được.

# OneCast - Tài liệu thiết kế

Ngày: 2026-10-27 · Trạng thái: chờ duyệt · Dựa trên kiến trúc của `D:\Projects\LiveVocal`

## 1. Mục đích và lý do

OneCast là một app Windows "All-in-One" xử lý giọng nói. App lấy giọng từ mic,
làm sạch và làm mượt rồi đẩy ra **VB-Cable** ("CABLE Input"). Các app khác
(Discord, OBS, Zoom...) chỉ cần chọn mic "CABLE Output" là nhận được giọng đã xử lý.

Khác với LiveVocal:
- Không host VST. Mọi khâu xử lý được viết sẵn trong app.
- Không nghe lại (monitor), không thu âm thanh desktop, không trộn nhạc.

Người dùng chính là chủ dự án. Họ dùng mic dynamic cắm vào M-Audio M-Track Solo
(trong LiveVocal có ghi chú là XM8500 + M-Track Solo; nên đây là mic thu
nhỏ, cần pre-gain). Âm thanh Windows của họ phát ra tai nghe iBasso-DC03-Pro;
OneCast không được đụng tới đường đó.

Thứ tự ưu tiên: (1) không bao giờ vỡ tiếng hay giật/rè, (2) độ trễ thấp nhất có
thể, (3) giọng sạch và mượt, (4) dễ dùng.

**Phán đoán của orchestrator (chưa được xác nhận rõ ràng):** mục đích là giọng
**nói** (Discord, stream) chứ không phải hát. Các giá trị mặc định của EQ,
compressor, gate, de-esser được chỉnh theo giọng nói.

## 2. Mục tiêu

1. **Nguồn vào**, chọn được:
   - **ASIO**: ưu tiên driver "M-Audio M-Track Solo and Duo ASIO". Chọn kênh vào 1,
     kênh 2, hoặc trung bình 1+2. Chọn buffer size.
   - **Windows Audio (WASAPI)**, chỉ mở chiều thu: dành cho mic USB hoặc mọi mic
     Windows nhìn thấy.
2. **Chuỗi xử lý cố định** (mục 6.2). Mỗi khâu có công tắc bật/tắt và 1-3 núm chỉnh.
3. **Đầu ra duy nhất**: WASAPI render ra "CABLE Input (VB-Audio Virtual Cable)",
   stereo (giọng mono được nhân đôi). Chọn được thiết bị render khác nếu muốn
   (ví dụ Voicemeeter Input).
4. **Không vỡ tiếng**: không mẫu nào ra khỏi app vượt -1 dBFS.
5. **Độ trễ thấp nhất có thể**. App hiển thị tổng độ trễ mà nó biết được.
6. **Khử ồn hai chế độ**: AI (RNNoise, khoảng 30 ms) và Nhẹ (lọc phổ, ≤ 6 ms).
   **Mặc định: Nhẹ** (người dùng đổi lại sau khi đo được AI trễ 30 ms). AI vẫn chọn được.
7. Lưu và khôi phục toàn bộ thiết lập. Có preset sẵn và preset người dùng tự lưu.
8. Tự phục hồi khi rút/cắm mic hoặc interface, và khi CABLE Input mất hoặc trở lại.
9. Tiện ích: phím tắt Mute toàn cục, nút Bypass để so sánh A/B, đồng hồ đo kèm đèn
   clip, thu nhỏ xuống khay hệ thống, tuỳ chọn chạy cùng Windows.

## 3. Không làm (non-goals)

- Không nghe lại giọng (không monitor). Không mở chiều phát của thiết bị vào.
- Không đổi thiết bị mặc định của Windows. Không đụng output Windows (iBasso).
- Không host VST hay plugin dưới bất kỳ dạng nào.
- Không thu desktop audio, không trộn nhạc, không auto-tune/pitch, không reverb.
- Chỉ Windows 10/11 x64. Không có bản macOS/Linux.
- Không cài driver. VB-Cable do người dùng tự cài.
- Không có trình cài đặt (installer) bắt buộc: bản portable là đủ, như LiveVocal.

## 4. Coi là thất bại dù vẫn "chạy được" (anti-goals)

- Có tiếng lụp bụp, giật, rè do underrun/overrun hoặc lệch đồng hồ, kể cả sau nhiều giờ chạy.
- Độ trễ tăng dần theo thời gian.
- Có clip/vỡ tiếng ở CABLE Input.
- Khử ồn làm giọng méo kiểu "robot", nuốt chữ, hoặc gate cắt mất đầu/cuối câu.
- Chỉnh núm gây tiếng "lách tách" (zipper noise).
- Cấp phát bộ nhớ, khoá (lock), I/O hay log trong audio thread.
- App bắt chạy quyền admin, hoặc chiếm thiết bị Windows của người dùng.

## 5. Ràng buộc

- **Ngôn ngữ/khung**: JUCE 8.0.5, C++20, CMake, MSVC. Toolchain có sẵn trên máy là
  Visual Studio 18 2026 Community. CMake nằm trong VS:
  `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
- **Phụ thuộc**: copy `JUCE/` và `ASIO_SDK/` từ LiveVocal vào OneCast; cả hai đều
  gitignore (ASIO SDK không được phân phối lại).
- **RNNoise**: vendor mã nguồn chính thức của Xiph (BSD-3), bản phát hành 0.2. Dùng
  tarball phát hành vì nó có sẵn file model `rnnoise_data.c`; repo git thì
  không có. Đặt trong `third_party/rnnoise/`, build thành static lib qua CMake, kèm
  file LICENSE. RNNoise chỉ chạy ở 48 kHz, khung 480 mẫu, mẫu tính theo thang 16-bit
  (±32768).
- **Quy tắc real-time**: audio thread và render thread không cấp phát, không lock,
  không I/O. Tham số đi từ UI sang audio thread bằng `std::atomic` và được làm mượt
  trong audio thread.
- **Phong cách code**: theo LiveVocal (tên gọi, mật độ comment, comment kiểu `///`
  giải thích "vì sao"). Comment code bằng tiếng Anh.
- **Repo**: `D:\Projects\OneCast` là repo git riêng, nhánh `main`, remote `origin` =
  `https://github.com/ShiroTrang/OneCast.git`. **Chỉ push khi người dùng bảo.**
- **Worker**: orchestrator là Opus; các worker triển khai là Claude Sonnet 5.5
  (người dùng yêu cầu).

## 6. Kiến trúc

### 6.1 Tái sử dụng từ LiveVocal

| Thành phần LiveVocal | Trong OneCast |
|---|---|
| `RingBuffer.h` | Giữ nguyên (SPSC lock-free) |
| `DriftCompensatedReader.h` | Giữ nguyên (bù lệch đồng hồ giữa interface và VB-Cable) |
| `WasapiOutputDevice.*` | Giữ, làm đường ra CABLE Input |
| `SafetyLimiter.h` | Giữ `SafetyLimiter` (look-ahead 1 ms, trần -1 dBFS) |
| `DeviceRecovery.h`, `Platform/DeviceHotplug.*` | Giữ, xử lý rút/cắm |
| `UI/Theme.h`, `UI/MeterComponent.*`, `UI/AlertBanner.h` | Giữ, chỉnh lại cho bố cục mới |
| `Diagnostics/StartupLog.h` | Giữ |
| `Session/SessionState.*`, `Session/PresetManager.*` | Viết lại cho bộ tham số mới (giữ ý tưởng: JSON, lưu tự động) |
| `AudioEngine.*` | Viết lại gọn: bỏ desktop, monitor, reverb, duck, sync, plugin chain |
| `tools/package.ps1` | Giữ, đổi tên sản phẩm (gói portable kèm VC++ runtime) |
| `WasapiLoopbackCapture`, `AutoDucker`, `SyncCalibrator`, `CompensationDelay`, `Plugin/*`, `PluginRackPanel`, `SyncCalibrationCard`, `tools/ParamTool.cpp` | Bỏ |

### 6.2 Luồng tín hiệu

```
[ASIO M-Track Solo | WASAPI mic]   JUCE AudioDeviceManager, chỉ thu (0 kênh ra)
   │ callback trên audio thread
   ▼
Chọn kênh: 1 | 2 | trung bình 1+2  → mono        (đo: mức vào thô, đèn clip vào)
   ▼
Pre-gain           mặc định +6 dB, dải -12..+24 dB
   ▼
Lọc cắt trầm (HPF) mặc định 80 Hz, dải 40-200 Hz, dốc 12-18 dB/oct
   ▼
Khử ồn             Tắt | Nhẹ (≤ 6 ms) | AI RNNoise (~30 ms)   [mặc định Nhẹ, mức 100%]
   ▼
Noise gate mềm     downward expander, có hysteresis + hold, 0 ms
   ▼
EQ làm sạch        cắt đục (~250 Hz), nâng rõ (~3-5 kHz), độ sáng (shelf ~10 kHz)
   ▼
Compressor         RMS, soft-knee, attack/release mượt cho giọng nói
   ▼
De-esser           giảm dải 5-9 kHz khi có âm "s/x" (sau compressor vì
                   compressor đẩy sibilance lên)
   ▼
Output gain
   ▼
Limiter            SafetyLimiter, look-ahead 1 ms, trần -1 dBFS (luôn bật, không tắt được)
   ▼
Mute               ramp ~10 ms, không có tiếng "bụp"   (đo: mức ra, giảm gain limiter)
   ▼
RingBuffer → luồng WasapiOutputDevice (DriftCompensatedReader) → CABLE Input, stereo
```

**Bypass (A/B)**: bỏ qua mọi khâu từ HPF tới de-esser. Pre-gain, output gain,
limiter và mute vẫn chạy, nên đầu ra vẫn được chống vỡ tiếng. Khi bật hoặc tắt
Bypass, trễ của khử ồn mất đi hoặc xuất hiện lại; khâu này phải chuyển mượt,
không gây tiếng bụp.

**Thứ tự gate sau khử ồn** là chủ ý: gate làm việc trên tín hiệu đã sạch, nên
ngưỡng đặt thấp được và ít cắt mất đầu/cuối chữ.

### 6.3 Luồng xử lý (thread)

- **Audio thread** (callback ASIO/WASAPI của JUCE): chạy toàn bộ DSP rồi ghi vào
  RingBuffer.
- **Render thread** (WasapiOutputDevice, event-driven, COM MTA, MMCSS "Pro Audio"):
  đọc RingBuffer qua DriftCompensatedReader rồi ghi ra CABLE Input. Backlog giữ nhỏ
  nhất có thể: 1 chu kỳ + 1.5 ms + 1 block engine, nở tạm khi endpoint đòi nhiều hơn
  và co lại sau 5 s. Khi ring thiếu, ghi phần đang có (short read) thay vì chèn im lặng.
  (Task 16-18.)
- **Chu kỳ đường ra tự thích nghi** (Task 17-18): IAudioClient3 low-latency, khởi đầu ở
  2 x chu kỳ nhỏ nhất của endpoint khi chưa học được gì. Lần đầu luồng ghi bị đánh thức
  trễ hơn mức bộ đệm che được (sau 1 s đầu), app tăng chu kỳ lên mức che được khoảng
  khựng đó cộng 20 %, lưu vào `outputPeriodHintFrames`; hết nấc thì dùng shared 10 ms
  (lưu -1). Không tự hạ chu kỳ trong một phiên.
- **Message thread**: UI (timer 30 Hz), watchdog thiết bị và phục hồi, lưu thiết
  lập (debounce), phím tắt toàn cục, khay hệ thống.

### 6.4 Sample rate và khử ồn

- Khi mở thiết bị, app xin 48 kHz. ASIO M-Track hỗ trợ 48 kHz.
- Nếu thiết bị buộc phải chạy ở rate khác (một số mic USB qua WASAPI shared), toàn
  chuỗi vẫn chạy ở rate của thiết bị. Riêng khâu AI thì resample sang 48 kHz và
  ngược lại ngay trong khâu đó; độ trễ báo cáo phải tính cả phần resample.
  (Quyết định của orchestrator.)
- **AI (RNNoise)**: gom khung 480 mẫu qua FIFO, thêm 1440 mẫu trễ (≈ 30 ms ở
  48 kHz: 480 mẫu gom khung + 960 mẫu trễ nội tại của RNNoise 0.2, vốn nhìn trước một
  khung; đo thực tế ở Task 5). "Mức độ" 0-100% trộn tín hiệu khô (đã delay khớp pha) với tín hiệu ướt.
  Có thể dùng xác suất VAD của RNNoise để hỗ trợ gate (builder tự quyết).
- **Nhẹ**: STFT ngắn (gợi ý FFT 256, hop 64, cửa sổ sqrt-Hann), ước lượng nhiễu
  thích ứng (minimum statistics hoặc MCRA), gain kiểu Wiener có làm mượt và mức giảm
  tối thiểu (floor) để không ra tiếng "nước chảy" (musical noise). Trễ ≤ 6 ms.
- Đổi chế độ khử ồn trong lúc chạy phải mượt (crossfade); thay đổi trễ là chấp nhận
  được, miễn không có tiếng bụp.
- Mỗi khâu báo trễ của mình (số mẫu). Engine cộng lại để hiển thị.

### 6.5 Ngân sách độ trễ (ước tính, 48 kHz)

| Khâu | Trễ |
|---|---|
| ASIO buffer 128 mẫu (mặc định) | ≈ 2.7 ms + trễ driver báo |
| Khử ồn AI / Nhẹ / Tắt | ≈ 30 ms / ≤ 6 ms / 0 |
| Limiter | 1 ms |
| Ring + endpoint CABLE Input | ≈ 16 ms ở chu kỳ 5.33 ms; 24-33 ms khi đã tăng lên ≈ 9 ms; ≈ 35 ms ở shared 10 ms |
| Bên trong VB-Cable | theo cài đặt của VB-Cable, app không kiểm soát |

App hiển thị: trễ vào do driver báo + trễ các khâu + backlog đầu ra đo được (ring +
padding của endpoint). Đo thực tế với M-Track, chế độ Nhẹ: khoảng 29 ms ở chu kỳ
5.33 ms, 37-46 ms khi máy hay khựng và chu kỳ đã tăng (Task 15, `docs/verification/`).

### 6.6 Thiết bị: mặc định và phục hồi

- **Lần chạy đầu**: nếu có driver ASIO tên chứa "M-Track" thì dùng ASIO + kênh 1
  (cổng combo XLR của M-Track Solo), buffer 128 mẫu. Nếu không có thì dùng Windows
  Audio với thiết bị thu mặc định.
- **Đầu ra mặc định**: thiết bị render có tên chứa "CABLE Input". Nếu không có thì
  hiện banner hướng dẫn cài VB-Cable, và cho chọn thiết bị render khác.
- **Không bao giờ phát ra thiết bị mặc định của Windows**: `WasapiOutputDevice::start("")`
  của LiveVocal phát ra output mặc định (chính là iBasso). OneCast không bao giờ truyền
  id rỗng. Thiếu CABLE Input thì hiện banner và im lặng.
- **Không tự đổi sang mic khác**: mic đã chọn bị mất thì OneCast không âm thầm chuyển sang
  thiết bị thu mặc định (`AudioDeviceManager` của JUCE mặc định làm vậy). App hiện banner
  và chờ mic đó quay lại. Danh sách nguồn vào loại bỏ mọi endpoint "CABLE Output" để tránh
  vòng hồi tiếp.
- **ASIO chỉ cho một app dùng một lúc**: nếu driver đang bận (ví dụ LiveVocal đang
  mở) thì báo rõ bằng banner, không crash.
- **Phục hồi**: dùng DeviceRecovery + DeviceHotplug của LiveVocal cho thiết bị vào.
  Với đầu ra: luồng render báo lỗi (thiết bị mất) thì thử mở lại khi endpoint trở
  lại. Trong lúc mất thiết bị, UI hiện banner.
- **Đèn clip vào**: khi mẫu thô ≥ -0.1 dBFS (vỡ ở bộ chuyển đổi của phần cứng, phần
  mềm không cứu được), hiện nhắc "Mic vào quá to, vặn nhỏ gain trên M-Track".

### 6.7 Giao diện

Một cửa sổ, dùng theme tối của LiveVocal. **Nhãn tiếng Việt** (quyết định của
orchestrator; Segoe UI hiển thị đủ dấu tiếng Việt).

- **Thanh trên**: Nguồn vào (kiểu: ASIO / Windows Audio; thiết bị; kênh; buffer),
  Đầu ra (thiết bị, mặc định CABLE Input), trạng thái, tổng độ trễ.
- **Hàng thẻ xử lý** theo thứ tự tín hiệu, mỗi thẻ có công tắc, núm, và đồng hồ giảm
  gain nếu có: Pre-gain · Lọc trầm · Khử ồn (chế độ + mức) · Gate (ngưỡng) · EQ (đục /
  rõ / sáng) · Compressor (ngưỡng, tỉ lệ, bù gain) · De-esser (mức) · Đầu ra (gain,
  đồng hồ limiter).
- **Thanh dưới**: đồng hồ Vào/Ra kèm đèn clip, nút **Mute** lớn, nút **Bypass**, chọn
  preset + Lưu preset.
- **Khay hệ thống**: bấm nút đóng (X) thì ẩn xuống khay. Menu khay có: Hiện, Mute,
  Thoát. (Quyết định của orchestrator.)
- **Tuỳ chọn**: chạy cùng Windows (HKCU Run, mặc định tắt), khởi động thu nhỏ, phím
  tắt Mute (mặc định Ctrl+Alt+M, đổi được; tránh Ctrl+Shift+M vì Discord đã dùng).
- **Một phiên bản**: mở app lần hai thì đưa cửa sổ đang chạy lên trước.

### 6.8 Preset và lưu thiết lập

- Thiết lập lưu tự động vào `%APPDATA%\OneCast\settings.json` (thiết bị, mọi tham
  số, tuỳ chọn, phím tắt).
- Preset có sẵn (chỉ đọc), builder tự tinh chỉnh con số:
  - **Tự nhiên** (mặc định): Nhẹ 100%, compressor nhẹ (~3:1), EQ nhẹ.
  - **Phòng ồn**: AI 100%, gate ngưỡng cao hơn.
  - **Phát thanh**: compressor mạnh hơn, nâng rõ nhiều hơn, de-esser mạnh hơn.
- Preset người dùng lưu thành JSON trong `%APPDATA%\OneCast\Presets\`.

## 7. Kiểm thử

- **Unit test DSP** (console app `OneCastTests`, theo kiểu `Tests/DspTests.cpp` của
  LiveVocal, chạy không cần thiết bị):
  - HPF đúng đáp ứng tần số.
  - Gate mở/đóng có hysteresis và không cắt đầu chữ.
  - Đường cong tĩnh của compressor đúng.
  - De-esser giảm 7 kHz nhưng không giảm 1 kHz.
  - Limiter không bao giờ vượt trần với nhiễu, xung, sine 0 dBFS+.
  - Hai chế độ khử ồn: trễ báo cáo khớp trễ đo được (tương quan xung); nhiễu trắng
    và nhiễu hồng giảm đáng kể (ngưỡng cụ thể do builder đặt và ghi lại); tone giống
    giọng không bị méo quá mức.
  - Kết quả không phụ thuộc block size (1..1024).
  - Không có NaN/denormal.
  - Đổi tham số không gây zipper; Bypass, Mute, đổi chế độ không gây bước nhảy.
- **Công cụ render offline** `OneCastRender` (CLI): chạy một file WAV qua chuỗi với
  preset chọn sẵn. Dùng để đo đạc và để người dùng nghe thử trước/sau.
- **Kiểm thử thật trên máy người dùng** (orchestrator chạy):
  - M-Track ASIO → OneCast → CABLE Input; thu lại CABLE Output bằng WASAPI để đo trễ
    đầu-cuối và kiểm tra rớt mẫu.
  - Soak test ≥ 30 phút: không underrun, backlog không tăng dần.
  - CPU thấp: RNNoise khoảng 1-2% một nhân.
  - Rút/cắm M-Track trong lúc chạy.
- **Người dùng kiểm tra cuối**: Discord voice test, OBS.

## 8. Đóng gói

`tools/package.ps1` (lấy từ LiveVocal): thư mục portable + ZIP kèm VC++ runtime
DLLs, cùng `HUONG-DAN.txt` tiếng Việt và `THIRD-PARTY-NOTICES.txt` (JUCE, RNNoise,
ASIO).

## 9. Để builder tự quyết

- Thuật toán cụ thể của từng khâu DSP: loại bộ lọc, detector, hằng số
  attack/release, dải núm hợp lý.
- Chi tiết chế độ Nhẹ (miễn đạt trễ ≤ 6 ms và không có musical noise rõ rệt).
- Cách gói RNNoise và có dùng VAD hay không.
- Cấu trúc class và file, miễn giữ ranh giới: engine / DSP thuần (test được) / UI /
  session / platform.
- Hình thức chi tiết của UI trong khuôn khổ mục 6.7.

## 10. Các quyết định orchestrator tự đưa ra (người dùng nên rà)

1. Mục đích là giọng nói, không phải hát (ảnh hưởng giá trị mặc định).
2. Thứ tự: gate sau khử ồn; de-esser sau compressor.
3. Bypass vẫn giữ pre-gain, output gain, limiter, mute.
4. Buffer ASIO mặc định 128 mẫu, đổi được (64 nếu máy chạy ổn).
5. Khâu AI tự resample khi thiết bị không chạy 48 kHz.
6. Giao diện tiếng Việt. Bấm X thì ẩn xuống khay.
7. Phím tắt Mute mặc định Ctrl+Alt+M.
8. OneCast là repo git riêng, remote về GitHub, chưa push.
9. Copy JUCE 8.0.5 và ASIO SDK từ LiveVocal, không commit.
10. Có thêm công cụ `OneCastRender` để nghe thử và đo đạc.
11. Mic đã chọn bị mất thì báo và chờ nó quay lại, không tự chuyển sang mic mặc định.
    Không bao giờ phát ra output mặc định của Windows.

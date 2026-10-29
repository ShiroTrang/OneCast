#pragma once
#include <JuceHeader.h>

/** Every piece of user-facing copy, in one place.

    The UI is Vietnamese and the sources are compiled with /utf-8. Each string is
    built from a juce::CharPointer_UTF8 literal rather than a plain const char*
    so a byte sequence is always decoded as UTF-8, whatever the compiler's
    execution charset would do. Segoe UI renders the diacritics correctly.

    %1 and %2 are placeholders. Callers fill them with String::replace(), so the
    copy in this table stays exactly the text the product owner specified.
*/
namespace Strings
{
    inline juce::String appName()          { return juce::String (juce::CharPointer_UTF8 ("OneCast")); }
    inline juce::String input()            { return juce::String (juce::CharPointer_UTF8 ("Nguồn vào")); }
    inline juce::String apiType()          { return juce::String (juce::CharPointer_UTF8 ("Kiểu")); }
    inline juce::String device()           { return juce::String (juce::CharPointer_UTF8 ("Thiết bị")); }
    inline juce::String channel()          { return juce::String (juce::CharPointer_UTF8 ("Kênh")); }
    inline juce::String channel1()         { return juce::String (juce::CharPointer_UTF8 ("Kênh 1")); }
    inline juce::String channel2()         { return juce::String (juce::CharPointer_UTF8 ("Kênh 2")); }
    inline juce::String channelMix()       { return juce::String (juce::CharPointer_UTF8 ("Trộn 1+2")); }
    inline juce::String buffer()           { return juce::String (juce::CharPointer_UTF8 ("Buffer")); }
    inline juce::String output()           { return juce::String (juce::CharPointer_UTF8 ("Đầu ra")); }
    inline juce::String latency()          { return juce::String (juce::CharPointer_UTF8 ("Độ trễ ≈ %1 ms")); }
    inline juce::String statusRunning()    { return juce::String (juce::CharPointer_UTF8 ("Đang chạy")); }
    inline juce::String statusStopped()    { return juce::String (juce::CharPointer_UTF8 ("Đã dừng")); }
    inline juce::String preGain()          { return juce::String (juce::CharPointer_UTF8 ("Pre-gain")); }
    inline juce::String hpf()              { return juce::String (juce::CharPointer_UTF8 ("Lọc trầm")); }
    inline juce::String noise()            { return juce::String (juce::CharPointer_UTF8 ("Khử ồn")); }
    inline juce::String noiseOff()         { return juce::String (juce::CharPointer_UTF8 ("Tắt")); }
    inline juce::String noiseLight()       { return juce::String (juce::CharPointer_UTF8 ("Nhẹ")); }
    inline juce::String noiseAi()          { return juce::String (juce::CharPointer_UTF8 ("AI")); }
    inline juce::String amount()           { return juce::String (juce::CharPointer_UTF8 ("Mức")); }
    inline juce::String gate()             { return juce::String (juce::CharPointer_UTF8 ("Gate")); }
    inline juce::String threshold()        { return juce::String (juce::CharPointer_UTF8 ("Ngưỡng")); }
    inline juce::String eq()               { return juce::String (juce::CharPointer_UTF8 ("EQ")); }
    inline juce::String eqMud()            { return juce::String (juce::CharPointer_UTF8 ("Đục")); }
    inline juce::String eqClarity()        { return juce::String (juce::CharPointer_UTF8 ("Rõ")); }
    inline juce::String eqAir()            { return juce::String (juce::CharPointer_UTF8 ("Sáng")); }
    inline juce::String compressor()       { return juce::String (juce::CharPointer_UTF8 ("Compressor")); }
    inline juce::String ratio()            { return juce::String (juce::CharPointer_UTF8 ("Tỉ lệ")); }
    inline juce::String makeup()           { return juce::String (juce::CharPointer_UTF8 ("Bù")); }
    inline juce::String deEsser()          { return juce::String (juce::CharPointer_UTF8 ("De-esser")); }
    inline juce::String outputGain()       { return juce::String (juce::CharPointer_UTF8 ("Gain ra")); }
    inline juce::String meterIn()          { return juce::String (juce::CharPointer_UTF8 ("Vào")); }
    inline juce::String meterOut()         { return juce::String (juce::CharPointer_UTF8 ("Ra")); }
    inline juce::String clip()             { return juce::String (juce::CharPointer_UTF8 ("CLIP")); }
    inline juce::String reduction()        { return juce::String (juce::CharPointer_UTF8 ("Giảm")); }
    inline juce::String mute()             { return juce::String (juce::CharPointer_UTF8 ("MUTE")); }
    inline juce::String muted()            { return juce::String (juce::CharPointer_UTF8 ("ĐANG TẮT TIẾNG")); }
    inline juce::String bypass()           { return juce::String (juce::CharPointer_UTF8 ("Bypass")); }
    inline juce::String preset()           { return juce::String (juce::CharPointer_UTF8 ("Preset")); }
    inline juce::String savePreset()       { return juce::String (juce::CharPointer_UTF8 ("Lưu preset")); }
    inline juce::String deletePreset()     { return juce::String (juce::CharPointer_UTF8 ("Xoá preset")); }
    inline juce::String presetNamePrompt() { return juce::String (juce::CharPointer_UTF8 ("Tên preset:")); }
    inline juce::String presetNameTaken()  { return juce::String (juce::CharPointer_UTF8 ("Tên này trùng với preset có sẵn. Hãy chọn tên khác.")); }
    inline juce::String presetSaveFailed()   { return juce::String (juce::CharPointer_UTF8 ("Không lưu được preset. Kiểm tra quyền ghi của thư mục %APPDATA%\\OneCast\\Presets.")); }
    inline juce::String presetDeleteFailed() { return juce::String (juce::CharPointer_UTF8 ("Không xoá được preset.")); }
    inline juce::String options()          { return juce::String (juce::CharPointer_UTF8 ("Tuỳ chọn")); }
    inline juce::String startWithWindows() { return juce::String (juce::CharPointer_UTF8 ("Chạy cùng Windows")); }
    inline juce::String startMinimized()   { return juce::String (juce::CharPointer_UTF8 ("Khởi động thu nhỏ")); }
    inline juce::String muteHotkey()       { return juce::String (juce::CharPointer_UTF8 ("Phím tắt Mute")); }
    inline juce::String hotkeyInvalid()    { return juce::String (juce::CharPointer_UTF8 ("Phím tắt không hợp lệ hoặc đã bị app khác dùng.")); }
    inline juce::String bannerNoCable()    { return juce::String (juce::CharPointer_UTF8 ("Không tìm thấy CABLE Input. Cài VB-Audio Virtual Cable (vb-audio.com/Cable) rồi mở lại OneCast, hoặc chọn thiết bị đầu ra khác.")); }
    inline juce::String bannerInputLost()  { return juce::String (juce::CharPointer_UTF8 ("Mất kết nối với \"%1\". OneCast sẽ tự kết nối lại khi thiết bị được cắm lại.")); }
    inline juce::String bannerOpenFailed() { return juce::String (juce::CharPointer_UTF8 ("Không mở được \"%1\": %2. Có thể app khác (ví dụ LiveVocal) đang dùng driver này.")); }
    // The engine's English detail goes to the start-up log; the banner gives
    // the user one of these three short reasons instead.
    inline juce::String errorDeviceBusy()  { return juce::String (juce::CharPointer_UTF8 ("thiết bị đang được app khác dùng")); }
    inline juce::String errorNotFound()    { return juce::String (juce::CharPointer_UTF8 ("không tìm thấy thiết bị")); }
    inline juce::String errorUnknown()     { return juce::String (juce::CharPointer_UTF8 ("lỗi không xác định")); }
    inline juce::String bannerOutputLost() { return juce::String (juce::CharPointer_UTF8 ("Mất kết nối với đầu ra \"%1\". Đang chờ thiết bị quay lại.")); }
    inline juce::String bannerInputClip()  { return juce::String (juce::CharPointer_UTF8 ("Mic vào quá to (bị vỡ ngay ở phần cứng). Vặn nhỏ gain trên M-Track.")); }
    inline juce::String dismiss()          { return juce::String (juce::CharPointer_UTF8 ("Đóng")); }
    inline juce::String trayShow()         { return juce::String (juce::CharPointer_UTF8 ("Hiện OneCast")); }
    inline juce::String trayMute()         { return juce::String (juce::CharPointer_UTF8 ("Tắt tiếng")); }
    inline juce::String trayUnmute()       { return juce::String (juce::CharPointer_UTF8 ("Bật tiếng")); }
    inline juce::String trayQuit()         { return juce::String (juce::CharPointer_UTF8 ("Thoát")); }
    inline juce::String trayTooltipMuted() { return juce::String (juce::CharPointer_UTF8 ("OneCast - đang tắt tiếng")); }
}

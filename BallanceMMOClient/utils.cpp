#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <openssl/md5.h>
#include "utility/string_utils.hpp"
#include "utils.h"
#include "text_sprite.h"
#include "dumpfile.h"

namespace {
    LOGFONT system_font_struct_{};
}

utils::utils(IBML* bml) : bml_(bml) {
    SystemParametersInfo(SPI_GETICONTITLELOGFONT, sizeof(system_font_struct_), &system_font_struct_, 0);
}

void utils::md5_from_file(const std::string& path, uint8_t* result) {
    std::ifstream file(path, std::ifstream::binary);
    if (!file.is_open())
        return;
    MD5_CTX md5Context;
    MD5_Init(&md5Context);
    constexpr static size_t SIZE = 1024 * 16;
    auto buf = std::make_unique_for_overwrite<char[]>(SIZE);
    while (file.good()) {
        file.read(buf.get(), SIZE);
        MD5_Update(&md5Context, buf.get(), (size_t)file.gcount());
    }
    MD5_Final(result, &md5Context);
}

// Windows 7 does not have GetDpiForSystem
typedef UINT (WINAPI* GetDpiForSystemPtr) (void);
GetDpiForSystemPtr const get_system_dpi_function = [] {
    auto hMod = GetModuleHandleW(L"user32.dll");
    if (hMod) {
        return (GetDpiForSystemPtr)GetProcAddress(hMod, "GetDpiForSystem");
    }
    return (GetDpiForSystemPtr)nullptr;
}();

uint32_t utils::get_system_dpi() {
    if (get_system_dpi_function)
        return get_system_dpi_function();
    return 96;
}

bool utils::is_foreground_window() {
    return GetForegroundWindow() == get_main_window();
}

void utils::flash_window() {
    FlashWindow(get_main_window(), false);
}

int utils::split_lines(std::string& text, float max_width, float font_size, int font_weight) {
    std::wstringstream ws {bmmo::string_utils::ConvertUtf8ToWide(bmmo::string_utils::get_parsed_string(text))};
    text.clear();
    auto hdc = GetDC(get_main_window());
    LOGFONT font_struct = system_font_struct_;
    font_struct.lfWeight = font_weight;
    HFONT font = CreateFontIndirect(&font_struct);
    SelectObject(hdc, font);
    SIZE sz;
    int max_length{}, line_length, line_count = 0;
    while (!ws.eof()) {
        std::wstring wline; std::getline(ws, wline);
        do {
            line_length = wline.length();
            GetTextExtentExPointW(hdc, wline.c_str(), line_length,
                                  int(max_width * bml_->GetRenderContext()->GetWidth() / 1.44f * 12 / font_size),
                                  &max_length, NULL, &sz);
            text += bmmo::string_utils::ConvertWideToUtf8(wline.substr(0, max_length)) + '\n';
            wline.erase(0, max_length);
            ++line_count;
        } while (max_length < line_length);
    }
    // GetTextExtentPoint32W(hdc, wtext.c_str(), wtext.length(), &sz);
    DeleteObject(font);
    return line_count;
}

const char* utils::get_system_font() {
    return system_font_struct_.lfFaceName;
}

int utils::get_display_font_size(float size) {
    return (int)std::round(bml_->GetRenderContext()->GetHeight() / (768.0f / 119) * size / utils::get_system_dpi());
}

void utils::display_important_notification(std::string text, float font_size, int line_count, int weight, float y_pos) {
    using namespace std::chrono;
    auto current_ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    text = bmmo::string_utils::utf8_to_ansi(text);
    text_sprite notification(std::format("Notification{}", current_ms),
                              text, 0.0f, y_pos - 0.001053f * font_size * line_count);
    notification.sprite_->SetAlignment(CKSPRITETEXT_CENTER);
    notification.sprite_->SetZOrder(65536 + static_cast<int>(current_ms));
    notification.sprite_->SetSize({ 1.0f, 0.2f + 0.00421f * font_size * line_count });
    notification.sprite_->SetFont(get_system_font(), get_display_font_size(font_size), weight, false, false);
    notification.set_visible(true);
    for (int i = 0; i < 15; i += 2) {
        notification.paint(0x11FF1190 + i * 0x11001001);
        std::this_thread::sleep_for(72ms);
    }
    std::this_thread::sleep_for(9s);
    for (int i = 1; i <= 15; i += 2) {
        notification.paint(0xFFFFF19E - i * 0x11100000);
        std::this_thread::sleep_for(120ms);
    }
}

void utils::cleanup_old_crash_dumps() {
    if (!std::filesystem::is_directory(NSDumpFile::DumpPath)) return;
    for (const auto& entry : std::filesystem::directory_iterator(NSDumpFile::DumpPath)) {
        if (entry.is_directory()) continue;
        char dump_ver[32]{}, dump_time_str[32]{};
        std::ignore = std::sscanf(entry.path().filename().string().c_str(), "BMMO_%31[^_]_%31[^.].dmp",
                                  dump_ver, dump_time_str);
        std::stringstream time_stream(dump_time_str); std::tm time_struct;
        time_stream >> std::get_time(&time_struct, "%Y%m%d-%H%M%S");
        if (time_stream.fail()) continue;
        auto time_diff = std::time(nullptr) - std::mktime(&time_struct);
        if (time_diff > 86400ll * 7)
            std::filesystem::remove(entry);
    }
}

std::string utils::pretty_percentage(float value) {
    if (value < 0)
        return "N/A";

    return std::format("{:.2f}%", value * 100.0f);
}

std::string utils::pretty_bytes(float bytes) {
    const char* suffixes[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB" };
    int s = 0; // which suffix to use
    while (bytes >= 1024 && s < 7) {
        ++s;
        bytes /= 1024;
    }

    return std::format("{:.1f}{}", bytes, suffixes[s]);
}

static inline const std::string MICRO_SIGN = bmmo::string_utils::ConvertWideToANSI(bmmo::string_utils::ConvertUtf8ToWide("µ"));
std::string utils::pretty_status(const SteamNetConnectionRealTimeStatus_t& status) {
    std::string s;
    s.reserve(512);
    s += std::format("Ping: {} ms\n", status.m_nPing);
    s += "ConnectionQualityLocal: " + pretty_percentage(status.m_flConnectionQualityLocal) + "\n";
    s += "ConnectionQualityRemote: " + pretty_percentage(status.m_flConnectionQualityRemote) + "\n";
    s += std::format("Tx: {:.3g}pps, ", status.m_flOutPacketsPerSec) + pretty_bytes(status.m_flOutBytesPerSec) + "/s\n";
    s += std::format("Rx: {:.3g}pps, ", status.m_flInPacketsPerSec) + pretty_bytes(status.m_flInBytesPerSec) + "/s\n";
    s += "Est. MaxBandwidth: " + pretty_bytes((float)status.m_nSendRateBytesPerSecond) + "/s\n";
    s += std::format("Queue time: {}{}s\n", status.m_usecQueueTime, MICRO_SIGN);
    s += std::format("\nReliable:            \nPending: {}\nUnacked: {}\n", status.m_cbPendingReliable, status.m_cbSentUnackedReliable);
    s += std::format("\nUnreliable:          \nPending: {}\n", status.m_cbPendingUnreliable);
    return s;
}

float utils::distance_to_line_segment(const VxVector& begin, const VxVector& end, const VxVector& point) {
    const auto line = end - begin;
    const auto l2 = line.SquareMagnitude();
    if (l2 == 0.0f)
        return (point - begin).Magnitude();

    const auto t = std::clamp((point - begin).Dot(line) / l2, 0.0f, 1.0f);

    return (point - (begin + t * line)).Magnitude();
}

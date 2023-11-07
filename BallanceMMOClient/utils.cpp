#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <openssl/md5.h>
#include "utility/string_utils.hpp"
#include "utils.h"
#include "text_sprite.h"
#include "dumpfile.h"

LOGFONT system_font_struct_{};

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

int utils::split_lines(std::string& text, int font_weight) {
    std::wstringstream ws {bmmo::string_utils::ConvertAnsiToWide(bmmo::string_utils::get_parsed_string(text))};
    text.clear();
    auto hdc = GetDC(get_main_window());
    LOGFONT font_struct = system_font_struct_;
    font_struct.lfWeight = font_weight;
    HFONT font = CreateFontIndirect(&font_struct);
    SelectObject(hdc, font);
    SIZE sz;
    int max_length{}, line_length, line_count = 1;
    while (!ws.eof()) {
        std::wstring wline; std::getline(ws, wline);
        do {
            line_length = wline.length();
            GetTextExtentExPointW(hdc, wline.c_str(), line_length, int(680 / 1.44f / 19.0f * 12), &max_length, NULL, &sz);
            text += bmmo::string_utils::ConvertWideToANSI(wline.substr(0, max_length)) + '\n';
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

void utils::display_important_notification(const std::string& text, float font_size, int line_count, int weight) {
    using namespace std::chrono;
    auto current_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    text_sprite notification(std::format("Notification{}", current_ms),
                             text, 0.0f, 0.4f - 0.001053f * font_size * line_count);
    notification.sprite_->SetAlignment(CKSPRITETEXT_CENTER);
    notification.sprite_->SetZOrder(65536 + static_cast<int>(current_ms));
    notification.sprite_->SetSize({ 1.0f, 0.2f + 0.00421f * font_size * line_count });
    notification.sprite_->SetFont(utils::get_system_font(), get_display_font_size(font_size), weight, false, false);
    notification.set_visible(true);
    for (int i = 1; i < 15; ++i) {
        notification.paint(0x11FF1190 + i * 0x11001001);
        std::this_thread::sleep_for(44ms);
    }
    std::this_thread::sleep_for(9s);
    for (int i = 1; i < 15; ++i) {
        notification.paint(0xFFFFF19E - i * 0x11100000);
        std::this_thread::sleep_for(80ms);
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

#ifndef BALLANCEMMOSERVER_STRING_UTILS_HPP
#define BALLANCEMMOSERVER_STRING_UTILS_HPP
#include <vector>
#include <string>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <filesystem>
#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
# endif
#include <Windows.h>
#endif

namespace bmmo::string_utils {
#ifdef _WIN32
    inline std::string ConvertWideToANSI(const std::wstring& wstr) {
        int count = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.length(), NULL, 0, NULL, NULL);
        std::string str(count, 0);
        WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, str.data(), count, NULL, NULL);
        return str;
    }

    inline std::wstring ConvertAnsiToWide(const std::string& str) {
        int count = MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.length(), NULL, 0);
        std::wstring wstr(count, 0);
        MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.length(), wstr.data(), count);
        return wstr;
    }

    inline std::string ConvertWideToUtf8(const std::wstring& wstr) {
        int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wstr.length(), NULL, 0, NULL, NULL);
        std::string str(count, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, str.data(), count, NULL, NULL);
        return str;
    }

    inline std::wstring ConvertUtf8ToWide(const std::string& str) {
        int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), NULL, 0);
        std::wstring wstr(count, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), wstr.data(), count);
        return wstr;
    }

    inline std::string ansi_to_utf8(const std::string& str) {
        return bmmo::string_utils::ConvertWideToUtf8(bmmo::string_utils::ConvertAnsiToWide(str));
    }

    inline std::string utf8_to_ansi(const std::string& str) {
        return bmmo::string_utils::ConvertWideToANSI(bmmo::string_utils::ConvertUtf8ToWide(str));
    }
#endif

    inline std::string to_lower(std::string data) {
        std::transform(data.begin(), data.end(), data.begin(),
            [](unsigned char c){ return std::tolower(c); });
        return data;
    }

    inline std::string join_strings(const std::vector<std::string>& strings, size_t start = 0, const char* delim = " ") {
        if (strings.size() == start) return "";
        static constexpr const size_t MAX_LENGTH = UINT16_MAX;
        std::string str = strings[start];
        start++;
        size_t length = strings.size();
        if (length > start) {
            for (size_t i = start; i < length; i++)
                str.append(delim + strings[i]);
        }
        return str.substr(0, MAX_LENGTH);
    }

    inline std::vector<std::string> split_strings(const std::string& str) {
        std::stringstream ss { str };
        std::vector<std::string> words;
        while (!ss.eof()) {
            std::string s;
            ss >> s;
            words.push_back(s);
        }
        return words;
    }

    inline void hex_chars_from_string(uint8_t* dest, const std::string& src) {
        for (unsigned int i = 0; i < src.length(); i += 2) {
            std::string byte_string = src.substr(i, 2);
            uint8_t byte = (uint8_t) strtol(byte_string.c_str(), NULL, 16);
            dest[i / 2] = byte;
        }
    }

    inline void string_from_hex_chars(std::string& dest, const uint8_t* src, const int length) {
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < length; i++)
            ss << std::setw(2) << (int)src[i];
        dest = ss.str();
    }

    // sanitize chat message (remove control characters)
    inline void sanitize_string(std::string& str) {
        std::replace_if(str.begin(), str.end(),
            [](char c) { return std::iscntrl(c); }, ' ');
    }

    inline static std::string get_build_time_string() {
        std::stringstream input_ss { __DATE__ }, output_ss {};
        std::tm date_struct;
        input_ss >> std::get_time(&date_struct, "%b %e %Y");
        output_ss << std::put_time(&date_struct, "%F ") << __TIME__;
        return output_ss.str();
    }

    // parse line breaks
    inline std::string get_parsed_string(const std::string& text) {
        std::string parsed_text;
        const auto text_length = text.length();
        for (size_t i = 0; i < text_length; ++i) {
            if (text[i] != '\\') {
                parsed_text += text[i];
                continue;
            }
            switch (text[std::min(i + 1, text_length)]) {
                case '\\':
                    if (text[std::min(i + 2, text_length)] != 'n')
                        break;
                    parsed_text += "\\n";
                    i += 2;
                    continue;
                case 'n':
                    parsed_text += '\n';
                    i += 1;
                    continue;
            }
            parsed_text += text[i];
        }
        return parsed_text;
    }

    inline std::string get_ordinal_suffix(uint32_t n) {
        if ((n / 10) % 10 != 1) {
            switch (n % 10) {
                case 1: return "st";
                case 2: return "nd";
                case 3: return "rd";
            }
        }
        return "th";
    };

    inline std::vector<std::string> get_file_matches(const std::string& prefix) {
        decltype(get_file_matches({})) files;
        std::filesystem::path path(prefix), parent_path(path.parent_path());
        if (path.parent_path().empty()) parent_path.assign(".");
        for (const auto& entry : std::filesystem::directory_iterator(parent_path)) {
            std::filesystem::path entry_path = entry.path();
            if (path.parent_path().empty())
                entry_path = entry_path.filename();
            if (!entry_path.string().starts_with(prefix))
                continue;
            files.emplace_back(entry_path.string());
        }
        return files;
    }
}

#endif //BALLANCEMMOSERVER_STRING_UTILS_HPP

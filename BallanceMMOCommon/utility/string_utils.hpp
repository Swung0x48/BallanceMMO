#ifndef BALLANCEMMOSERVER_STRING_UTILS_HPP
#define BALLANCEMMOSERVER_STRING_UTILS_HPP
#include <vector>
#include <string>
#include <algorithm>
#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
# endif
#include <Windows.h>
#endif

namespace bmmo {
    namespace string_utils {
#ifdef _WIN32
        std::string ConvertWideToANSI(const std::wstring& wstr) {
            int count = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.length(), NULL, 0, NULL, NULL);
            std::string str(count, 0);
            WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, str.data(), count, NULL, NULL);
            return str;
        }

        std::wstring ConvertAnsiToWide(const std::string& str) {
            int count = MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.length(), NULL, 0);
            std::wstring wstr(count, 0);
            MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.length(), wstr.data(), count);
            return wstr;
        }

        std::string ConvertWideToUtf8(const std::wstring& wstr) {
            int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wstr.length(), NULL, 0, NULL, NULL);
            std::string str(count, 0);
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, str.data(), count, NULL, NULL);
            return str;
        }

        std::wstring ConvertUtf8ToWide(const std::string& str) {
            int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), NULL, 0);
            std::wstring wstr(count, 0);
            MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), wstr.data(), count);
            return wstr;
        }
#endif

        std::string to_lower(std::string data) {
            std::transform(data.begin(), data.end(), data.begin(),
                [](unsigned char c){ return std::tolower(c); });
            return data;
        }

        std::string join_strings(const std::vector<std::string>& strings, size_t start = 0, const char* delim = " ") {
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
    }
}

#endif //BALLANCEMMOSERVER_STRING_UTILS_HPP

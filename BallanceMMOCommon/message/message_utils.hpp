#ifndef BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
#define BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
# endif
#include <Windows.h>
#endif

namespace bmmo {
    class message_utils {
    public:
#ifdef _WIN32
        static std::string ConvertWideToANSI(const std::wstring& wstr) {
            int count = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.length(), NULL, 0, NULL, NULL);
            std::string str(count, 0);
            WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &str[0], count, NULL, NULL);
            return str;
        }

        static std::wstring ConvertAnsiToWide(const std::string& str) {
            int count = MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.length(), NULL, 0);
            std::wstring wstr(count, 0);
            MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.length(), &wstr[0], count);
            return wstr;
        }

        static std::string ConvertWideToUtf8(const std::wstring& wstr) {
            int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wstr.length(), NULL, 0, NULL, NULL);
            std::string str(count, 0);
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], count, NULL, NULL);
            return str;
        }

        static std::wstring ConvertUtf8ToWide(const std::string& str) {
            int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), NULL, 0);
            std::wstring wstr(count, 0);
            MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), &wstr[0], count);
            return wstr;
        }
#endif

        static void write_string(std::string str, std::stringstream& stream) {
#ifdef _WIN32
            str = ConvertWideToUtf8(ConvertAnsiToWide(str));
#endif
            uint32_t length = str.length();
            stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
            stream.write(str.c_str(), str.length());
        }

        static bool read_string(std::stringstream& stream, std::string& str) {
            uint32_t length = 0;
            stream.read(reinterpret_cast<char*>(&length), sizeof(length));
            if (length > stream.str().length())
                return false;
            str.resize(length);
            stream.read(str.data(), length);
#ifdef _WIN32
            str = ConvertWideToANSI(ConvertUtf8ToWide(str));
#endif
            return true;
        }
    };
}


#endif //BALLANCEMMOSERVER_MESSAGE_UTILS_HPP

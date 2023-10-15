#ifndef BALLANCEMMOSERVER_ANSI_COLORS_HPP
#define BALLANCEMMOSERVER_ANSI_COLORS_HPP
#include <string>
#include <cstring>
#include <cstdint>
#include <limits>

namespace bmmo {
    struct ansi {
    private:
        inline static constexpr int MODIFIER_BEGIN_BIT = 7;
    public:
        enum color {
            Reset = 0,
            Black = 30, Red, Green, Yellow,
            Blue, Magenta, Cyan, White,
            Default = 39,
            BrightBlack = 90, BrightRed, BrightGreen, BrightYellow,
            BrightBlue, BrightMagenta, BrightCyan, BrightWhite,

            Bold = 1 << (MODIFIER_BEGIN_BIT + 1),
            Dim = 1 << (MODIFIER_BEGIN_BIT + 2), // not widely supported
            Italic = 1 << (MODIFIER_BEGIN_BIT + 3),
            Underline = 1 << (MODIFIER_BEGIN_BIT + 4), // supported by conhost
            Blinking = 1 << (MODIFIER_BEGIN_BIT + 5),
            RapidBlinking = 1 << (MODIFIER_BEGIN_BIT + 6), // may or may not work
            Inverse = 1 << (MODIFIER_BEGIN_BIT + 7), // supported by conhost
            Hidden = 1 << (MODIFIER_BEGIN_BIT + 8), // why does this exist
            Strikethrough = 1 << (MODIFIER_BEGIN_BIT + 9),

            // sadly we don't have enough bits
            DoubleUnderline = 1 << (MODIFIER_BEGIN_BIT + 10), // 21
            Overline = 1 << (MODIFIER_BEGIN_BIT + 11), // 53
            Xterm256 = 1 << (MODIFIER_BEGIN_BIT + 12), // 38;2;<color>; supported by conhost

            WhiteInverse = Xterm256 | 251 | Inverse, // default inverse is unintelligible on some terminals
        };

        inline static constexpr const char* RESET = "\033[0m";

        static std::string get_escape_code(int v) {
            int color = v & std::numeric_limits<uint8_t>::max();
            v >>= MODIFIER_BEGIN_BIT;
            char modifiers[32];
            size_t pos = 0;
            for (int i = 1; i <= 9; ++i) {
                v >>= 1;
                if (!(v & 1)) continue;
                modifiers[pos] = ';';
                ++pos;
                modifiers[pos] = '0' + i;
                ++pos;
            }
            static constexpr const char* EXTRA_MODIFIERS[] = { ";21", ";53" };
            for (int i = 0; i < int(sizeof(EXTRA_MODIFIERS) / sizeof(const char*)); ++i) {
                v >>= 1;
                if (!(v & 1)) continue;
                std::strcpy(modifiers + pos, EXTRA_MODIFIERS[i]);
                pos += 3;
            }
            modifiers[pos] = '\0';
            char text[24];
            std::snprintf(text, sizeof(text), (v >> 1) ? "\033[38;5;%d%sm" : "\033[%d%sm", color, modifiers);
            return {text};
        }
    };
}

#endif //BALLANCEMMOSERVER_ANSI_COLORS_HPP
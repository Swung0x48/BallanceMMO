#ifndef BALLANCEMMOSERVER_ANSI_COLORS_HPP
#define BALLANCEMMOSERVER_ANSI_COLORS_HPP
#include <string>
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
            Italic = 1 << (MODIFIER_BEGIN_BIT + 3),
            Underline = 1 << (MODIFIER_BEGIN_BIT + 4),
            Blinking = 1 << (MODIFIER_BEGIN_BIT + 5),
            Inverse = 1 << (MODIFIER_BEGIN_BIT + 7),
            Hidden = 1 << (MODIFIER_BEGIN_BIT + 8),
            Strikethrough = 1 << (MODIFIER_BEGIN_BIT + 9),

            Xterm256 = 1 << (MODIFIER_BEGIN_BIT + 10), // sadly we don't have enough bits

            WhiteInverse = White | Inverse, // default inverse is unintelligible on some terminals
        };

        inline static constexpr const char* RESET = "\033[0m";

        static std::string get_escape_code(int v) {
            int color = v & std::numeric_limits<uint8_t>::max();
            v >>= MODIFIER_BEGIN_BIT;
            char modifiers[16];
            size_t pos = 0;
            for (int i = 1; i <= 9; ++i) {
                v >>= 1;
                if (v & 1) {
                    modifiers[pos] = ';';
                    ++pos;
                    modifiers[pos] = '0' + i;
                    ++pos;
                }
            }
            modifiers[pos] = '\0';
            char text[24];
            std::snprintf(text, sizeof(text), (v >> 1) ? "\033[38;5;%d%sm" : "\033[%d%sm", color, modifiers);
            return {text};
        }
    };
}

#endif //BALLANCEMMOSERVER_ANSI_COLORS_HPP
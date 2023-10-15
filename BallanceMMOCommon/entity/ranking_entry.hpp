#ifndef BALLANCEMMOSERVER_RANKING_ENTRY_HPP
#define BALLANCEMMOSERVER_RANKING_ENTRY_HPP
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace bmmo::ranking_entry {
    struct entry {
        bool cheated{};
        int sr_rank{};
        std::string name;
        std::string formatted_hs_score, formatted_sr_score;

        std::string to_string(int ranking) const {
            char text[128];
            std::snprintf(text, sizeof(text), "<%d> %s%s: %s | %s",
                          ranking, name.c_str(), cheated ? " [C]" : "",
                          formatted_hs_score.c_str(), formatted_sr_score.c_str());
            return {text};
        }
    };

    static constexpr auto hs_sorter = [](const entry& r1, decltype(r1) r2) {
        return atoi(r1.formatted_hs_score.c_str()) >= atoi(r2.formatted_hs_score.c_str());
    };
    static constexpr auto sr_sorter = [](const entry& r1, decltype(r1) r2) {
        return r1.sr_rank < r2.sr_rank;
    };

    typedef std::vector<entry> player_rankings;
    typedef std::unordered_map<std::string, player_rankings> map_rankings;

    static inline void sort_rankings(player_rankings& rankings, bool hs_mode = false) {
        std::sort(rankings.begin(), rankings.end(), hs_mode ? hs_sorter : sr_sorter);
    }

    static inline std::vector<std::string> get_formatted_rankings(
            const player_rankings& rankings, const std::string& map_name, bool hs_mode = false) {
        decltype(get_formatted_rankings({}, {}, {})) texts;
        char header[128];
        std::snprintf(header, sizeof(header), "Ranking info for %s [%s]:",
                map_name.c_str(), hs_mode ? "HS" : "SR");
        texts.emplace_back(header);
        for (size_t i = 0; i < rankings.size(); ++i) {
            const auto& entry = rankings[i];
            int rank = i;
            if (hs_mode) {
                while (rank > 0 && atoi(entry.formatted_hs_score.c_str())
                        == atoi(rankings[rank - 1].formatted_hs_score.c_str()))
                    --rank;
            }
            texts.emplace_back(entry.to_string(rank + 1));
        }
        return texts;
    }
}

#endif //BALLANCEMMOSERVER_RANKING_ENTRY_HPP

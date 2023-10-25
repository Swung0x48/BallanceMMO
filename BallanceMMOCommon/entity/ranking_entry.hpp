#ifndef BALLANCEMMOSERVER_RANKING_ENTRY_HPP
#define BALLANCEMMOSERVER_RANKING_ENTRY_HPP
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <utility>
#include "map.hpp"

namespace bmmo::ranking_entry {
    struct base_entry {
        bool cheated{};
        std::string name;
    };

    struct finish_entry: base_entry {
        level_mode mode = level_mode::Speedrun;
        int sr_ranking{};
        std::string formatted_hs_score, formatted_sr_score;

        std::string to_string(int ranking, level_mode default_mode) const {
            char text[128];
            std::snprintf(text, sizeof(text), "<%d%s> %s%s: %s | %s",
                          ranking,
                          default_mode == mode ? "" : get_level_mode_suffix(mode),
                          cheated ? "[C] " : "", name.c_str(),
                          formatted_hs_score.c_str(), formatted_sr_score.c_str());
            return {text};
        }
    };
    
    struct dnf_entry: base_entry {
        int dnf_sector{};

        std::string to_string(int ranking) const {
            char text[128];
            std::snprintf(text, sizeof(text), "<%d*> %s%s: DNF | Sector %d",
                          ranking, cheated ? "[C] " : "", name.c_str(), dnf_sector);
            return {text};
        }
    };

    static inline bool hs_sorter(const finish_entry& r1, decltype(r1) r2) {
        return atoi(r1.formatted_hs_score.c_str()) >= atoi(r2.formatted_hs_score.c_str());
    };

    static constexpr bool sr_sorter(const finish_entry& r1, decltype(r1) r2) {
        return r1.sr_ranking < r2.sr_ranking;
    };

    static constexpr bool dnf_sorter(const dnf_entry& r1, decltype(r1) r2) {
        return r1.dnf_sector >= r2.dnf_sector;
    };

    typedef std::vector<finish_entry> player_finish_rankings;
    typedef std::vector<dnf_entry> player_dnf_rankings;
    typedef std::pair<player_finish_rankings, player_dnf_rankings> player_rankings;
    typedef std::unordered_map<std::string, player_rankings> map_rankings;

    static inline void sort_rankings(player_rankings& rankings, bool hs_mode = false) {
        std::sort(rankings.first.begin(), rankings.first.end(), hs_mode ? hs_sorter : sr_sorter);
        std::sort(rankings.second.begin(), rankings.second.end(), dnf_sorter);
    }

    static inline std::vector<std::string> get_formatted_rankings(
            const player_rankings& rankings, const std::string& map_name, bool hs_mode = false) {
        decltype(get_formatted_rankings({}, {}, {})) texts;
        char header[128];
        std::snprintf(header, sizeof(header), "Ranking info for %s [%s]:",
                map_name.c_str(), hs_mode ? "HS" : "SR");
        texts.emplace_back(header);
        using lm = level_mode;
        for (size_t i = 0; i < rankings.first.size(); ++i) {
            const auto& entry = rankings.first[i];
            int rank = i;
            if (hs_mode) {
                while (rank > 0 && atoi(entry.formatted_hs_score.c_str())
                        == atoi(rankings.first[rank - 1].formatted_hs_score.c_str()))
                    --rank;
            }
            texts.emplace_back(entry.to_string(rank + 1, hs_mode ? lm::Highscore : lm::Speedrun));
        }
        for (size_t i = 0; i < rankings.second.size(); ++i) {
            const auto& entry = rankings.second[i];
            int rank = i;
            while (rank > 0 && entry.dnf_sector == rankings.second[rank - 1].dnf_sector)
                --rank;
            texts.emplace_back(entry.to_string(rank + 1 + rankings.first.size()));
        }
        char footer[32];
        std::snprintf(footer, sizeof(footer), "%zu Completion(s), %zu DNF(s).", rankings.first.size(), rankings.second.size());
        texts.emplace_back(footer);
        return texts;
    }
}

#endif //BALLANCEMMOSERVER_RANKING_ENTRY_HPP

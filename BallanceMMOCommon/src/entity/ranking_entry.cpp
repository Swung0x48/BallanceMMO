#include <algorithm>
#include <cstdio>
#include "entity/ranking_entry.hpp"
#include "utility/ansi_colors.hpp"
#include "message/message_colors.hpp"

namespace bmmo::ranking_entry {
    std::string finish_entry::to_string(int ranking, level_mode default_mode) const {
        char text[128];
        std::snprintf(text, sizeof(text), "<%d%s> %s%s: %s | %s",
                        ranking,
                        default_mode == mode ? "" : get_level_mode_suffix(mode),
                        cheated ? "[C] " : "", name.c_str(),
                        formatted_hs_score.c_str(), bmmo::get_formatted_time(sr_time).c_str());
        return {text};
    }

    std::string dnf_entry::to_string(int ranking) const {
        char text[128];
        std::snprintf(text, sizeof(text), "<%d*> %s%s: DNF | Sector %d",
                        ranking, cheated ? "[C] " : "", name.c_str(), dnf_sector);
        return {text};
    }

    void sort_rankings(player_rankings& rankings, bool hs_mode) {
        std::sort(rankings.first.begin(), rankings.first.end(), hs_mode ? hs_sorter : sr_sorter);
        std::sort(rankings.second.begin(), rankings.second.end(), dnf_sorter);
    }

    std::vector<std::pair<std::string, int>> get_formatted_rankings(
            const player_rankings& rankings, const std::string& map_name, bool hs_mode) {
        decltype(get_formatted_rankings({}, {}, {})) texts;
        char header[128];
        std::snprintf(header, sizeof(header), "Ranking info for %s [%s]:",
                map_name.c_str(), hs_mode ? "HS" : "SR");
        texts.emplace_back(header, bmmo::ansi::Reset);
        using lm = level_mode;
		const static int finish_color = bmmo::color_code(bmmo::LevelFinishV2),
                dnf_color = bmmo::color_code(bmmo::DidNotFinish);
        for (size_t i = 0; i < rankings.first.size(); ++i) {
            const auto& entry = rankings.first[i];
            int rank = i;
            if (hs_mode) {
                while (rank > 0 && atoi(entry.formatted_hs_score.c_str())
                        == atoi(rankings.first[rank - 1].formatted_hs_score.c_str()))
                    --rank;
            }
            texts.emplace_back(entry.to_string(rank + 1, hs_mode ? lm::Highscore : lm::Speedrun), finish_color);
        }
        for (size_t i = 0; i < rankings.second.size(); ++i) {
            const auto& entry = rankings.second[i];
            int rank = i;
            while (rank > 0 && entry.dnf_sector == rankings.second[rank - 1].dnf_sector)
                --rank;
            texts.emplace_back(entry.to_string(rank + 1 + rankings.first.size()), dnf_color);
        }
        char footer[32];
        std::snprintf(footer, sizeof(footer), "%zu Completion(s), %zu DNF(s).", rankings.first.size(), rankings.second.size());
        texts.emplace_back(footer, bmmo::ansi::Reset);
        return texts;
    }
}

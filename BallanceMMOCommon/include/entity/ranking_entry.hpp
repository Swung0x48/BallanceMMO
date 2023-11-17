#ifndef BALLANCEMMOSERVER_RANKING_ENTRY_HPP
#define BALLANCEMMOSERVER_RANKING_ENTRY_HPP
#include <string>
#include <vector>
#include <unordered_map>
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
        float sr_time{};
        std::string formatted_hs_score;

        std::string to_string(int ranking, level_mode default_mode) const;
    };
    
    struct dnf_entry: base_entry {
        int dnf_sector{};

        std::string to_string(int ranking) const;
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

    void sort_rankings(player_rankings& rankings, bool hs_mode = false);

    std::vector<std::string> get_formatted_rankings(
            const player_rankings& rankings, const std::string& map_name, bool hs_mode = false);
}

#endif //BALLANCEMMOSERVER_RANKING_ENTRY_HPP

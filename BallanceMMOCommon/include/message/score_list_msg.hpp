#ifndef BALLANCEMMOSERVER_SCORE_LIST_MSG_HPP
#define BALLANCEMMOSERVER_SCORE_LIST_MSG_HPP
#include "message.hpp"
#include "../entity/ranking_entry.hpp"

namespace bmmo {
    // compressed score list
    struct score_list_msg: public serializable_message {
        score_list_msg(): serializable_message(bmmo::ScoreList) {}

        struct map map;
        level_mode mode = bmmo::level_mode::Speedrun;
        ranking_entry::player_rankings rankings;

        bool serialize() override {
            return serialize(rankings);
        }

        bool serialize(const ranking_entry::player_rankings& rankings) {
            if (!serializable_message::serialize()) return false;

            message_utils::write_variable(&map, raw);
            message_utils::write_variable(&mode, raw);

            auto size = static_cast<uint16_t>(rankings.first.size());
            message_utils::write_variable(&size, raw);
            for (const auto& entry: rankings.first) {
                message_utils::write_variable(&entry.cheated, raw);
                message_utils::write_string<uint8_t>(entry.name, raw);
                message_utils::write_variable(&entry.mode, raw);
                message_utils::write_variable(&entry.sr_ranking, raw);
                message_utils::write_variable(&entry.sr_time, raw);
                message_utils::write_string<uint8_t>(entry.formatted_hs_score, raw);
            }
            size = static_cast<uint16_t>(rankings.second.size());
            message_utils::write_variable(&size, raw);
            for (const auto& entry: rankings.second) {
                message_utils::write_variable(&entry.cheated, raw);
                message_utils::write_string(entry.name, raw);
                message_utils::write_variable(&entry.dnf_sector, raw);
            }

            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;

            if (!message_utils::read_variable(raw, &map)) return false;
            if (!message_utils::read_variable(raw, &mode)) return false;

            auto size = message_utils::read_variable<uint16_t>(raw);
            rankings.first.resize(size);
            for (auto& entry: rankings.first) {
                if (!message_utils::read_variable(raw, &entry.cheated)) return false;
                if (!message_utils::read_string<uint8_t>(raw, entry.name)) return false;
                if (!message_utils::read_variable(raw, &entry.mode)) return false;
                if (!message_utils::read_variable(raw, &entry.sr_ranking)) return false;
                if (!message_utils::read_variable(raw, &entry.sr_time)) return false;
                if (!message_utils::read_string<uint8_t>(raw, entry.formatted_hs_score)) return false;
            }
            size = message_utils::read_variable<uint16_t>(raw);
            rankings.second.resize(size);
            for (auto& entry: rankings.second) {
                if (!message_utils::read_variable(raw, &entry.cheated)) return false;
                if (!message_utils::read_string<uint8_t>(raw, entry.name)) return false;
                if (!message_utils::read_variable(raw, &entry.dnf_sector)) return false;
            }

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_SCORE_LIST_MSG_HPP
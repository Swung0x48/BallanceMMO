#pragma once

// FULL step LGM (R7 part one): where the retail core host puts its session
// journals, as the core host's own (session/core_host.cpp: platform
// journal_path / journal_prepare and the LOCAL checkpoint cadence), so it no
// longer borrows the legacy black box's statics
// (session_journal_client.hpp client_journal::directory / file_name / prune /
// kCheckpointTicks / kKeepFiles; FULL step LGD deleted that legacy black box).
// The same values and the same file names as the legacy copies had, so the
// journals of older builds share the directory and the pruning.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bmmo::retail::journal_files {
    // The LOCAL checkpoint cadence's ceiling (every 10 s at 66 Hz) and the
    // number of journals kept in the directory.
    inline constexpr uint32_t kCheckpointTicks = 660;
    inline constexpr size_t kKeepFiles = 10;

    // <game>/ModLoader/BMMOJournals: the game runs from <game>/Bin, and the
    // mods' data belongs next to the loader, not next to the executable.
    inline std::filesystem::path directory() {
        std::error_code ec;
        const auto bin = std::filesystem::current_path(ec);
        if (ec) return std::filesystem::path("..") / "ModLoader" / "BMMOJournals";
        return bin.parent_path() / "ModLoader" / "BMMOJournals";
    }

    // session_<id>_level<N>_<UTC yyyymmddhhmmss>_p<own id>.bmjr
    inline std::string file_name(uint32_t session, int32_t level, uint32_t own_player) {
        const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
        gmtime_s(&utc, &seconds);
        char stamp[32] = {};
        std::strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &utc);
        return std::format("session_{}_level{}_{}_p{}.bmjr", session, level, stamp, own_player);
    }

    // Deletes every session_*.bmjr in `directory` beyond the newest `keep`.
    // Nothing here throws (it runs on the journal's writer strand, just
    // before a session's file is created).
    inline void prune(const std::filesystem::path& directory, size_t keep) {
        std::error_code ec;
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
        std::filesystem::directory_iterator it(directory, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
            const auto name = it->path().filename().string();
            if (name.rfind("session_", 0) != 0 || it->path().extension() != ".bmjr") continue;
            files.emplace_back(it->last_write_time(ec), it->path());
            ec.clear();
        }
        if (files.size() <= keep) return;
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = keep; i < files.size(); ++i) std::filesystem::remove(files[i].second, ec);
    }
}

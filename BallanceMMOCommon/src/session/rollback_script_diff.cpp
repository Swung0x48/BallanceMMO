// Step SDF: the end-of-replay script diff of rollback_engine (session/
// rollback.hpp, set_defer_script_diffs; docs/realtime-design.md 'Built (the
// end-of-replay script diff leaves the rewinding frame, step SDF)').  Out of
// line: core_host.cpp is near MSVC's section limit.

#include <session/rollback.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace bmmo::session {

    rollback_engine::script_diff_queue::script_diff_queue() = default;
    rollback_engine::script_diff_queue::~script_diff_queue() = default;
    rollback_engine::script_diff_queue::script_diff_queue(const script_diff_queue&) = default;
    rollback_engine::script_diff_queue& rollback_engine::script_diff_queue::operator=(const script_diff_queue&) = default;
    rollback_engine::script_diff_queue::script_diff_queue(script_diff_queue&&) noexcept = default;
    rollback_engine::script_diff_queue& rollback_engine::script_diff_queue::operator=(script_diff_queue&&) noexcept = default;

    // The last replayed tick's scripts `replayed`, whose bytes differ from
    // the live record `live`: whether they differ outside what the check
    // leaves out, and how.  An empty answer from diff_scripts: the bytes
    // differ only in the record's written-cell section (step RK2, decision
    // Q4: never compared) - not a mismatch.  Without the hook: a logical
    // difference, named by the sizes.
    bool rollback_engine::classify_scripts(const rollback_world& world, const std::vector<uint8_t>& live,
                                           const std::vector<uint8_t>& replayed, bool& logical, std::string& where) {
        logical = true;
        if (!world.diff_scripts) return true;
        logical = false;   // not the written cells (decision Q4)
        where = world.diff_scripts(live, replayed, logical);
        return !where.empty();
    }

    void rollback_engine::count_script_difference(const rollback_world& world, uint32_t tick, bool logical,
                                                  const std::string& where, size_t replayed_size, size_t live_size) {
        if (logical) ++stats_.script_mismatches; else ++stats_.script_float_mismatches;
        if (world.log && (logical ? script_mismatch_logs_ : script_float_logs_)++ < script_log_cap_)
            world.log(std::string("script replay: the scripts re-executed to tick ") + std::to_string(tick)
                      + (logical ? " differ from the live record: " : " differ in float parameters only: ")
                      + (where.empty() ? std::to_string(replayed_size) + " vs " + std::to_string(live_size) + " bytes" : where));
    }

    void rollback_engine::queue_script_diff(const rollback_world& world, uint32_t tick, std::vector<uint8_t>&& live,
                                            const std::vector<uint8_t>& replayed) {
        auto& items = script_diffs_.items;
        if (items.size() >= kScriptDiffQueue) {
            if (world.log && !script_diff_full_logged_)
                world.log("script replay: " + std::to_string(items.size()) + " deferred script diffs at tick " + std::to_string(tick)
                          + ": the oldest (tick " + std::to_string(items.front().tick) + ") classified on the rewinding frame");
            script_diff_full_logged_ = true;
            classify_script_diffs(world, 1, true);
        }
        items.push_back({tick, std::move(live), replayed});
        ++stats_.script_diffs_queued;
        stats_.script_diff_queue_max = std::max<uint64_t>(stats_.script_diff_queue_max, items.size());
    }

    void rollback_engine::classify_script_diffs(const rollback_world& world, size_t most, bool forced) {
        auto& items = script_diffs_.items;
        for (; most && !items.empty(); --most) {
            const script_diff_queue::item item = std::move(items.front());
            items.erase(items.begin());
            // A binding with no hook cannot classify a blob queued with one
            // (classify_scripts would count it logical): unclassified.
            if (!world.diff_scripts) {
                ++stats_.script_diffs_unclassified;
                continue;
            }
            ++stats_.script_diffs_classified;
            if (forced) ++stats_.script_diffs_forced;
            bool logical = true;
            std::string where;
            if (classify_scripts(world, item.live, item.replayed, logical, where))
                count_script_difference(world, item.tick, logical, where, item.replayed.size(), item.live.size());
        }
    }

    size_t rollback_engine::drop_script_diffs() {
        const size_t dropped = script_diffs_.items.size();
        stats_.script_diffs_unclassified += dropped;
        script_diffs_.items.clear();
        return dropped;
    }

}

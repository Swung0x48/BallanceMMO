#pragma once

// Client-side correction of a locally predicted body (the own ball, or a
// shared mechanism) against the server's authoritative snapshot (design
// section 3.3 / 8.5 step 5).  Pure logic: the caller records the body state
// after every local tick, hands in each snapshot, and applies the returned
// adjustments through the physics bridge.  A snapshot for tick T is only
// ever compared with the state recorded at T: the server runs behind the
// client, so comparing with the current state would rewind moving bodies.
//
//   error < ignore_position && < ignore_velocity  -> nothing
//   error < hard_position                          -> spread the difference
//                                                     over `blend_ticks` ticks
//   otherwise                                      -> hard set to the snapshot

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>

#include "../entity/session.hpp"

namespace bmmo::session {
    struct ball_pose {
        uint32_t tick = 0;
        double position[3] = {};
        double rotation[4] = {0.0, 0.0, 0.0, 1.0};
        float linear[3] = {};
        float angular[3] = {};
    };

    struct correction_thresholds {
        double ignore_position = 0.01;   // metres
        double ignore_velocity = 0.05;   // m/s
        double hard_position = 1.0;      // metres: beyond this, hard set
        uint32_t blend_ticks = 8;
        uint32_t history_ticks = 660;    // 10 s of states
    };

    struct correction_step {
        enum class kind { none, blend, hard } action = kind::none;
        // blend: add these to the body every tick for `remaining` ticks
        double delta_position[3] = {};
        float delta_linear[3] = {};
        // hard: the state to write
        ball_pose target{};
    };

    struct correction_stats {
        uint64_t compared = 0, ignored = 0, blended = 0, hard = 0, unmatched = 0, skipped = 0;
        double last_error = 0.0, max_error = 0.0;
    };

    class body_corrector {
    public:
        explicit body_corrector(correction_thresholds thresholds = {}) : thresholds_(thresholds) {}

        void record(const ball_pose& pose) {
            history_.push_back(pose);
            while (history_.size() > thresholds_.history_ticks) history_.pop_front();
        }
        void clear() {
            history_.clear();
            remaining_ = 0;
        }

        // Compares an authoritative pose for its tick with the local history.
        // Returns what to do now; blends continue through next_blend().
        correction_step compare(const ball_pose& authoritative) {
            correction_step step;
            ++stats_.compared;
            if (remaining_ > 0) {
                // A blend is still being applied; comparing now would count the
                // same difference twice.
                ++stats_.skipped;
                return step;
            }
            const ball_pose* local = find(authoritative.tick);
            if (!local) {
                ++stats_.unmatched;
                return step;
            }
            double dp[3], dv[3];
            for (int k = 0; k < 3; ++k) {
                dp[k] = authoritative.position[k] - local->position[k];
                dv[k] = static_cast<double>(authoritative.linear[k]) - local->linear[k];
            }
            const double position_error = std::sqrt(dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2]);
            const double velocity_error = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            stats_.last_error = position_error;
            if (position_error > stats_.max_error) stats_.max_error = position_error;
            if (position_error < thresholds_.ignore_position && velocity_error < thresholds_.ignore_velocity) {
                ++stats_.ignored;
                return step;
            }
            if (position_error >= thresholds_.hard_position) {
                ++stats_.hard;
                step.action = correction_step::kind::hard;
                step.target = authoritative;
                history_.clear();
                remaining_ = 0;
                return step;
            }
            ++stats_.blended;
            step.action = correction_step::kind::blend;
            const uint32_t ticks = thresholds_.blend_ticks == 0 ? 1 : thresholds_.blend_ticks;
            for (int k = 0; k < 3; ++k) {
                blend_position_[k] = dp[k] / ticks;
                blend_linear_[k] = static_cast<float>(dv[k] / ticks);
                step.delta_position[k] = blend_position_[k];
                step.delta_linear[k] = blend_linear_[k];
            }
            remaining_ = ticks;
            // The history is shifted by the same amount so later snapshots are
            // compared against corrected predictions rather than the stale ones.
            for (auto& pose: history_) {
                if (pose.tick <= authoritative.tick) continue;
                for (int k = 0; k < 3; ++k) {
                    pose.position[k] += dp[k];
                    pose.linear[k] += static_cast<float>(dv[k]);
                }
            }
            return step;
        }

        // Pending blend increment for this tick (kind::none when finished).
        correction_step next_blend() {
            correction_step step;
            if (remaining_ == 0) return step;
            --remaining_;
            step.action = correction_step::kind::blend;
            for (int k = 0; k < 3; ++k) {
                step.delta_position[k] = blend_position_[k];
                step.delta_linear[k] = blend_linear_[k];
            }
            return step;
        }

        bool blending() const { return remaining_ > 0; }
        const correction_stats& stats() const { return stats_; }
        size_t history_size() const { return history_.size(); }

    private:
        const ball_pose* find(uint32_t tick) const {
            for (auto it = history_.rbegin(); it != history_.rend(); ++it)
                if (it->tick == tick) return &*it;
            return nullptr;
        }

        correction_thresholds thresholds_;
        std::deque<ball_pose> history_;
        double blend_position_[3] = {};
        float blend_linear_[3] = {};
        uint32_t remaining_ = 0;
        correction_stats stats_;
    };
    using own_ball_corrector = body_corrector;
}

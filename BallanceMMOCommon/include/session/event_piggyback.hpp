#pragma once

// Lifecycle events on the input datagram (protocol 25, FULL step EVI;
// late-player-design.md §3.4 option (b), "As built (step EVI)").
//
// A client's own Physicalize, Unphysicalize, Sector and Trafo events ride
// every input datagram (session_input_msg `events`) from their send until the
// datagram's newest frame passes stamp + 7 - i.e. every datagram that carries
// the stamp's frame (MAX_INPUT_FRAMES = 8) carries the event too, and the
// server takes a datagram's events before its frames (server.cpp
// handle_session_input), so a present frame T always comes with its events:
// an event is late only when every datagram carrying its stamp's frame was
// lost and its reliable copy too (the frame itself is then substituted).  The
// reliable no-Nagle copy still goes (an outage longer than the window), DX8's
// separate unreliable copies of these four types do not; BodyRevived and
// Finish keep the reliable copy plus DX8's repeats (lifecycle.hpp
// kOwnEventRepeats).
//
// An event sent when no datagram can carry its stamp's frame any more (a
// held event released after stamp + 7: R6H's hold-event, or any host hold)
// rides the first eight datagrams after its send instead - the window is
// max(stamp, the first datagram's newest frame) + 7 - so it is never carried
// by fewer datagrams than DX8's seven copies were (as built, step EVI).
//
// Budget (§3.4): a datagram stays <= kInputDatagramBudget bytes.  Over it, the
// OLDEST piggybacked event is left out of that datagram (it stays in the list
// for the next one; its reliable copy travels anyway) and counted.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "../entity/session.hpp"
#include "../message/session_event_msg.hpp"
#include "../message/session_input_msg.hpp"
#include "lifecycle.hpp"

namespace bmmo::session {
    inline constexpr size_t kInputDatagramBudget = 1100;
    // The last newest-frame tick a datagram carrying `stamp`'s frame can have.
    inline constexpr uint32_t kPiggybackWindowTicks = static_cast<uint32_t>(MAX_INPUT_FRAMES) - 1;

    // The own event types that ride the input datagram.
    inline bool event_rides_input(event_type t) {
        return t == event_type::Physicalize || t == event_type::Unphysicalize || t == event_type::Sector
            || t == event_type::Trafo;
    }

    class event_piggyback {
    public:
        struct entry {
            std::string bytes;   // the event's serialized session_event_msg, as its reliable copy
            uint32_t stamp = 0;
            event_type type = event_type::Physicalize;
            bool started = false;   // its window fixed by the first datagram after the send
            uint32_t until = 0;     // the last newest-frame tick of a datagram that carries it
        };
        struct stats {
            uint64_t carried = 0;    // event copies put into datagrams
            uint64_t dropped = 0;    // copies left out of a datagram by the budget
            uint64_t expired = 0;    // entries whose window ended
            size_t max_bytes = 0;    // the largest input datagram assembled (budget check)
        };

        void push(std::string bytes, uint32_t stamp, event_type type) {
            entries_.push_back(entry{std::move(bytes), stamp, type, false, 0});
        }
        // Before a datagram whose newest frame is `newest`: fix the window of
        // the entries sent since the last datagram (until = max(stamp,
        // newest) + 7, signed as every tick comparison) and drop those no
        // datagram from here on carries (newest > until).
        void prune(uint32_t newest) {
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (!it->started) {
                    it->started = true;
                    it->until = (static_cast<int32_t>(newest - it->stamp) > 0 ? newest : it->stamp) + kPiggybackWindowTicks;
                }
                if (static_cast<int32_t>(newest - it->until) > 0) {
                    it = entries_.erase(it);
                    ++stats_.expired;
                } else {
                    ++it;
                }
            }
        }
        // Put the live entries into `msg` (its frames already set), newest
        // last, leaving out the oldest while the datagram would exceed
        // `budget`.  Returns the number left out.
        size_t attach(session_input_msg& msg, size_t budget = kInputDatagramBudget) {
            msg.events.clear();
            size_t first = 0, bytes = 0;
            for (const auto& e: entries_) bytes += e.bytes.size();
            size_t n = entries_.size();
            if (n > MAX_INPUT_EVENTS) {
                for (; first < n - MAX_INPUT_EVENTS; ++first) bytes -= entries_[first].bytes.size();
            }
            while (first < n
                   && session_input_msg::wire_size(msg.frames.size(), bytes, n - first) > budget) {
                bytes -= entries_[first].bytes.size();
                ++first;
            }
            for (size_t i = first; i < n; ++i) msg.events.push_back(entries_[i].bytes);
            stats_.carried += n - first;
            stats_.dropped += first;
            const size_t size = session_input_msg::wire_size(msg.frames.size(), bytes, n - first);
            if (size > stats_.max_bytes) stats_.max_bytes = size;
            return first;
        }
        size_t pending() const { return entries_.size(); }
        void clear() { entries_.clear(); }
        const stats& counters() const { return stats_; }
        void reset_counters() { stats_ = {}; }

    private:
        std::deque<entry> entries_;
        stats stats_;
    };

    // The server's intake of one input datagram (server.cpp
    // handle_session_input): every piggybacked event, in datagram order,
    // through `take_event` (handle_session_event: the events_seen dedupe
    // first - a piggybacked copy and the reliable one are one key - then the
    // guards and the rate limit, then the submit) BEFORE `submit_frames`.
    // Both post to the runner's ONE FIFO command queue (session_runner::post),
    // so the frames never reach the simulation thread ahead of the events
    // they carry; that order is relied on (see the comment at post()).  An event that does
    // not parse is skipped (the datagram's deserialize refused one already).
    template <class TakeEvent, class SubmitFrames>
    void take_input_datagram(const session_input_msg& msg, TakeEvent&& take_event, SubmitFrames&& submit_frames) {
        for (const auto& bytes: msg.events) {
            session_event_msg event;
            event.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!event.deserialize()) continue;
            take_event(event);
        }
        submit_frames();
    }

    // The per-member event rate window of the server's intake (design 9.4,
    // server.cpp handle_session_event): one count per event that passed the
    // dedupe, reset after a second; false when the count is over `limit`
    // (0 = no limit).  `in_window` == limit + 1 marks the first excess.
    inline bool event_rate_admit(uint32_t& in_window, std::chrono::steady_clock::time_point& window_start,
                                 std::chrono::steady_clock::time_point now, uint32_t limit) {
        if (now - window_start > std::chrono::seconds(1)) {
            window_start = now;
            in_window = 0;
        }
        ++in_window;
        return limit == 0 || in_window <= limit;
    }

    // The server's event intake ORDER (server.cpp handle_session_event; the
    // unit test calls this same function): the events_seen dedupe FIRST - a
    // piggybacked copy and the reliable one are one key, the later one a
    // duplicate - and only then the per-second rate window, so the window
    // counts each event once, however many datagrams carried it (as before
    // protocol 25).  `rate_counted` false (a BodyRevived, dropped after the
    // dedupe without counting) skips the window.
    enum class event_intake { duplicate, admitted, rate_limited };
    inline event_intake event_intake_admit(event_seen& seen, const lifecycle_event_key& key, bool rate_counted,
                                           uint32_t& in_window, std::chrono::steady_clock::time_point& window_start,
                                           std::chrono::steady_clock::time_point now, uint32_t limit) {
        if (!seen.first(key)) return event_intake::duplicate;
        if (!rate_counted) return event_intake::admitted;
        return event_rate_admit(in_window, window_start, now, limit) ? event_intake::admitted
                                                                     : event_intake::rate_limited;
    }
}

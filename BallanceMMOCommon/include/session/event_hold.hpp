#pragma once

// The own-event hold, a test tool both hosts run (FULL step R6H; late-player
// design 5.1 C6): the N-th own lifecycle event of a type (1-based) leaves this
// client MS milliseconds after it was made, or never.  The client applies and
// journals the event at once, as it would anyway: only the wire is gated -
// through hooks.filter_event, before the reliable copy and before the repeat
// queue, so the one gate covers every path the event leaves by.  The bytes go
// out whole at the release (session_client_core::release_event), the repeats
// after them.
//
// The headless client's --hold-event TYPE:N:MS|TYPE:N:drop and the retail
// 'session hold-event TYPE:N:MS|TYPE:N:drop|off' are this one helper (the
// "host logic written once" rule, roadmap section 2): the host parses its
// spec with parse_event_hold, calls hold() from hooks.filter_event,
// release_due() on every pass of its loop (headless) or every OnProcess
// (retail), and discard() from hooks.ending.  The log lines and the journal
// notes are the headless client's texts verbatim (its journals do not change).
//
// The wall clock is read lazily, as the headless client always read it: hold()
// asks its clock only when it holds an event (not for one it lets through or
// drops), release_due() only while something is held - the headless inbound
// capture hands a read back by its site (session/clock_site.hpp), so a read at
// another moment would shift every replayed capture.
//
// CK-free; header-only.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "client_core.hpp"

namespace bmmo::session {

    // What to hold: the `nth` own event of `type` (1-based), `ms` late, or
    // never (`drop`).  type < 0: nothing.
    struct event_hold_spec {
        int type = -1;
        uint32_t nth = 0;
        int ms = 0;
        bool drop = false;
        bool armed() const { return type >= 0; }
    };

    // TYPE:N:MS or TYPE:N:drop, TYPE a name (physicalize, unphysicalize,
    // sector, finish, bodyrevived, trafo) or its number.  (The headless
    // client's parse_hold_event, moved here at R6H; `off` is the retail
    // verb's and is not a spec.)
    inline bool parse_event_hold(const std::string& spec, event_hold_spec& out) {
        const size_t a = spec.find(':');
        const size_t b = a == std::string::npos ? std::string::npos : spec.find(':', a + 1);
        if (b == std::string::npos) return false;
        std::string type = spec.substr(0, a);
        for (char& c: type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.type = -1;
        for (int t = 0; t <= 5; ++t)   // 5: trafo (step T3)
            if (type == event_type_name(t) || type == std::to_string(t)) out.type = t;
        if (out.type < 0) return false;
        out.nth = static_cast<uint32_t>(std::max(1, std::atoi(spec.substr(a + 1, b - a - 1).c_str())));
        const std::string what = spec.substr(b + 1);
        out.drop = what == "drop";
        out.ms = out.drop ? 0 : std::atoi(what.c_str());
        return out.drop || out.ms > 0;
    }

    class event_hold {
    public:
        using clock = std::chrono::steady_clock;
        using log_sink = std::function<void(const std::string& line)>;

        // The host's log (headless logf, retail ILogger::Info).
        void set_log(log_sink log) { log_ = std::move(log); }

        // A new spec: the count restarts.  What is held already stays and is
        // released at its time (a disarm only stops further holds).
        void arm(const event_hold_spec& spec) {
            spec_ = spec;
            seen_ = 0;
        }
        void disarm() {
            spec_ = event_hold_spec{};
            seen_ = 0;
        }
        // The count of the held type restarts (the headless anchor; the
        // retail session's start).
        void restart_count() { seen_ = 0; }

        const event_hold_spec& spec() const { return spec_; }
        uint32_t seen() const { return seen_; }
        size_t held() const { return held_.size(); }
        bool pending() const { return !held_.empty(); }
        uint64_t released() const { return released_; }
        uint64_t dropped() const { return dropped_; }

        // hooks.filter_event: send, or hold `event` until now() + ms, or drop
        // it.  `now` is called only when the event is held.
        template <class Now>
        event_send_verdict hold(session_client_core& core, const bmmo::session_event_msg& event, Now&& now) {
            if (spec_.type < 0 || static_cast<int>(event.type) != spec_.type) return event_send_verdict::send;
            if (++seen_ != spec_.nth) return event_send_verdict::send;
            if (spec_.drop) {
                ++dropped_;
                log(format("hold-event: own %s #%u stamped tick %u dropped at tick %u: it never leaves this client",
                           event_type_name(spec_.type), seen_, event.tick, core.current_tick()));
                core.journal().note("hold-event: dropped " + std::string(event_type_name(spec_.type)) + " stamped "
                                    + std::to_string(event.tick));
                return event_send_verdict::drop;
            }
            held_.push_back(held_event{event.raw.str(), spec_.type, event.tick, core.current_tick(),
                                       now() + std::chrono::milliseconds(spec_.ms)});
            log(format("hold-event: own %s #%u stamped tick %u held at tick %u for %d ms",
                       event_type_name(spec_.type), seen_, event.tick, core.current_tick(), spec_.ms));
            core.journal().note("hold-event: held " + std::string(event_type_name(spec_.type)) + " stamped "
                                + std::to_string(event.tick) + " for " + std::to_string(spec_.ms) + " ms");
            return event_send_verdict::hold;
        }

        // Every held event whose time has come goes out (release_event),
        // once the session is assigned; with no session running it is
        // discarded, said aloud (the session's end clears the list through
        // discard(), so this is a safety net).  `now` is called only while
        // something is held.
        template <class Now>
        void release_due(session_client_core& core, Now&& now) {
            if (held_.empty()) return;
            const clock::time_point at = now();
            for (auto it = held_.begin(); it != held_.end();) {
                if (at < it->release) {
                    ++it;
                    continue;
                }
                if (core.session_phase() != client_core_state::phase::running) {
                    log(format("hold-event: own %s stamped tick %u (held at tick %u) discarded: no session running",
                               event_type_name(it->type), it->stamp, it->held_at));
                    it = held_.erase(it);
                    continue;
                }
                if (!core.assigned()) {   // a session but no tick base yet: wait for it
                    ++it;
                    continue;
                }
                core.release_event(it->bytes);
                ++released_;
                log(format("hold-event: own %s stamped tick %u (held at tick %u) released at tick %u",
                           event_type_name(it->type), it->stamp, it->held_at, core.current_tick()));
                core.journal().note("hold-event: released stamped " + std::to_string(it->stamp));
                it = held_.erase(it);
            }
        }

        // hooks.ending (session_client_core::end): what is still held is
        // discarded, said aloud.
        void discard() {
            for (const auto& held: held_)
                log(format("hold-event: own %s stamped tick %u (held at tick %u) discarded: the session ended first",
                           event_type_name(held.type), held.stamp, held.held_at));
            held_.clear();
        }

    private:
        struct held_event {
            std::string bytes;
            int type = -1;
            uint32_t stamp = 0;
            uint32_t held_at = 0;
            clock::time_point release{};
        };

        template <class... A>
        static std::string format(const char* pattern, A... args) {
            char line[256];
            std::snprintf(line, sizeof(line), pattern, args...);
            return line;
        }
        void log(const std::string& line) const {
            if (log_) log_(line);
        }

        event_hold_spec spec_;
        uint32_t seen_ = 0;
        std::vector<held_event> held_;
        uint64_t released_ = 0, dropped_ = 0;
        log_sink log_;
    };

}

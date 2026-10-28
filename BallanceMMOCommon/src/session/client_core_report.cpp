// The shared session-client core (docs/session-client-core-design.md 1.1):
// the status line and the window: line, a harness contract kept
// byte-identical (scripts/arena_map_test.py parses both).  Step E10 moves
// them out of the headless client (session_client.cpp report() and
// report_window() at 6c96154) as status_text and window_text: the text is
// the core's, one format literal each, with the host's own counters
// (host_counters: the pacer's rebases, the replayed manager activations,
// the scripts a restore kept live) at the places they had; the host prints
// it (session_hooks::status / window).  Verbatim but for the state's prefix,
// the host values, the window's phase (core_config::window_shift for
// --window-origin) and the window's four snprintf pieces, which are one
// literal now: they were appended in order with nothing between them, so the
// line is the same text (each piece had a fixed buffer far wider than its
// numbers can fill).

#include "session/client_core.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bmmo::session {

    // A printf-formatted text in a growing buffer (the core's logf without
    // the log): the status line alone runs past 1.5 KB.
    std::string session_client_core::textf(const char* format, ...) {
        std::string text(256, '\0');
        va_list args;
        va_start(args, format);
        va_list again;
        va_copy(again, args);
        const int length = std::vsnprintf(text.data(), text.size(), format, args);
        if (length >= 0 && static_cast<size_t>(length) >= text.size()) {
            text.resize(static_cast<size_t>(length) + 1);
            std::vsnprintf(text.data(), text.size(), format, again);
        }
        va_end(again);
        va_end(args);
        text.resize(length > 0 ? static_cast<size_t>(length) : 0);
        return text;
    }

    // The host's counters for the text: the platform's, or none.
    host_counters session_client_core::host_values() const {
        return platform_.counters ? platform_.counters() : host_counters{};
    }

    // The 99th percentile of the relayed frames' arrival lag, in ticks.
    uint32_t session_client_core::relay_lag_p99() const {
        uint64_t total = 0;
        for (uint32_t n: s.relay_lag_hist_) total += n;
        if (!total) return 0;
        uint64_t seen = 0;
        for (uint32_t b = 0; b < client_core_state::kRelayLagBuckets; ++b) {
            seen += s.relay_lag_hist_[b];
            if (seen * 100 >= total * 99) return b;
        }
        return client_core_state::kRelayLagBuckets - 1;
    }

    // One line per kWindowTicks ticks (late-player design 5.1 C1): what
    // happened since the previous line, up to the end of frame `tick`.
    // lag[P]=L*N,... is the first-arrival lag histogram of source player
    // P's relayed frames (the status line's semantics: ticks already
    // simulated at arrival, early frames not counted); inrew[P] the input
    // rewinds whose correction named P's ball.  Pairs are count/depth in
    // ticks.  scripts/arena_map_test.py event_report() reads it.  "" when
    // `tick` closes no window (and on the first edge, which only sets the
    // base).
    std::string session_client_core::window_text(uint32_t tick, const host_counters& h) {
        using window_base = client_core_state::window_base;
        // A line closes the window on the first frame of the next index:
        // the ticks t with (t - shift) % 60 == 0 (--window-origin).
        const uint32_t index = window_index_of(tick);
        if (s.window_index_ == index) return std::string();
        const bool first = s.window_index_ == 0xFFFFFFFFu;
        s.window_index_ = index;
        window_base now;
        now.inrew = s.input_rewinds_; now.inrew_depth = s.input_rewind_depth_;
        now.life = s.life_rewinds_; now.life_depth = s.life_rewind_depth_;
        now.forced = s.forced_rollbacks_; now.forced_depth = s.forced_rollback_depth_;
        now.snaprb = s.snapshot_rollbacks_; now.snaprb_depth = s.snapshot_rollback_depth_;
        now.snaps = s.snapshots_applied_; now.resyncs_sent = s.resyncs_sent_; now.resyncs_done = s.resyncs_done_;
        now.relay_new = s.relay_new_; now.relay_late = s.relay_late_; now.rebases = h.rebases;
        now.subst_recv = s.subst_recv_; now.subst_diff = s.subst_diff_; now.subst_same = s.subst_same_;
        now.subst_bodiless = s.subst_bodiless_; now.subst_late = s.subst_late_;
        now.substrew = s.subst_rewinds_; now.substrew_depth = s.subst_rewind_depth_;
        now.subst_deferred = s.subst_deferred_; now.subst_unseen = s.subst_unseen_;
        now.subst_subsumed = s.subst_subsumed_; now.subst_saved = s.subst_saved_;
        now.lead_adj = s.lead_adjustments_;
        now.echo_acked = s.own_echo_acked_; now.echo_moved = s.own_echo_moved_;
        now.echo_superseded = s.own_echo_superseded_; now.echo_rejected = s.own_echo_rejected_;
        now.echo_missing = s.own_echo_missing_;
        now.birth_moved = s.own_birth_echo_moved_; now.own_moved = s.own_births_moved_;
        now.own_removed = s.own_births_removed_; now.own_resynced = s.own_moves_resynced_;
        const window_base& was = s.window_base_;
        std::string text;
        if (!first) {
            auto d = [](uint64_t a, uint64_t b) { return static_cast<unsigned long long>(a - b); };
            // After the rollback classes and the relay: the own correction
            // (late-player design 1.3): subst= as on the status line, this
            // window's share, and substrew= the replays it named as
            // count/depth (a rollback class of its own; inrew= counts the
            // replays a remote's frame named), and substco= the coalescing:
            // deferred frames / unseen diffs / waiting replays another
            // rewind carried / amendments that needed no own replay (the
            // status line's subst_coalesce).  Then SessionLead (late-player
            // design 2.4): the adjustments applied in this window (lead_adj,
            // the harness's count), the acknowledged offset so far (lead)
            // and the ticks the slew in flight still has to move (slew,
            // signed).  Then our own events' echoes (protocol 9): moved= the
            // own births the server applied later than stamped (the
            // harness's moved births), echo=acked/moved/superseded/rejected/
            // missing of every type, and ownlife=moved/removed/resynced what
            // this client did with a moved, superseded or rejected birth
            // (late-player design 3.3): moved inside the history, removed
            // from it, or resynced past the reach.
            text = textf("window: tick=%u inrew=%llu/%llu life=%llu/%llu forced=%llu/%llu snaprb=%llu/%llu snaps=%llu "
                         "resyncs=%llu/%llu rebases=%d relay=%llu late=%llu"
                         " subst=%llu/%llu/%llu/%llu/%llu/%llu substrew=%llu/%llu substco=%llu/%llu/%llu/%llu"
                         " lead_adj=%llu lead=%d slew=%d"
                         " moved=%llu echo=%llu/%llu/%llu/%llu/%llu ownlife=%llu/%llu/%llu",
                         tick, d(now.inrew, was.inrew), d(now.inrew_depth, was.inrew_depth), d(now.life, was.life),
                         d(now.life_depth, was.life_depth), d(now.forced, was.forced), d(now.forced_depth, was.forced_depth),
                         d(now.snaprb, was.snaprb), d(now.snaprb_depth, was.snaprb_depth), d(now.snaps, was.snaps),
                         d(now.resyncs_sent, was.resyncs_sent), d(now.resyncs_done, was.resyncs_done),
                         now.rebases - was.rebases, d(now.relay_new, was.relay_new), d(now.relay_late, was.relay_late),
                         d(now.subst_recv, was.subst_recv), d(now.subst_diff, was.subst_diff),
                         d(now.subst_same, was.subst_same), d(now.subst_bodiless, was.subst_bodiless),
                         d(now.substrew, was.substrew), d(now.subst_late, was.subst_late),
                         d(now.substrew, was.substrew), d(now.substrew_depth, was.substrew_depth),
                         d(now.subst_deferred, was.subst_deferred), d(now.subst_unseen, was.subst_unseen),
                         d(now.subst_subsumed, was.subst_subsumed), d(now.subst_saved, was.subst_saved),
                         d(now.lead_adj, was.lead_adj), s.lead_offset_, s.lead_slew_.outstanding(),
                         d(now.birth_moved, was.birth_moved), d(now.echo_acked, was.echo_acked),
                         d(now.echo_moved, was.echo_moved), d(now.echo_superseded, was.echo_superseded),
                         d(now.echo_rejected, was.echo_rejected), d(now.echo_missing, was.echo_missing),
                         d(now.own_moved, was.own_moved),
                         d(now.own_removed, was.own_removed), d(now.own_resynced, was.own_resynced));
            for (const auto& [player, hist]: s.window_lag_) {
                text += " lag[" + std::to_string(player) + "]=";
                bool comma = false;
                for (const auto& [lag, n]: hist) {
                    if (comma) text += ',';
                    text += std::to_string(lag) + '*' + std::to_string(n);
                    comma = true;
                }
            }
            for (const auto& [player, n]: s.window_inrew_src_)
                text += " inrew[" + std::to_string(player) + "]=" + std::to_string(n);
        }
        s.window_base_ = now;
        s.window_lag_.clear();
        s.window_inrew_src_.clear();
        return text;
    }

    std::string session_client_core::status_text(const host_counters& h) const {
        const auto& st = s.corrector_.stats();
        const auto& rs = s.rollback_.stats();
        uint64_t rc = 0, ri = 0, rb = 0, rh = 0;
        for (const auto& [id, remote]: s.remotes_) {
            const auto& rs = remote.corrector.stats();
            rc += rs.compared; ri += rs.ignored; rb += rs.blended; rh += rs.hard;
        }
        double mech_max = 0.0;
        for (const auto& [name, c]: s.mechanism_correctors_) mech_max = std::max(mech_max, c.stats().max_error);
        // Step T3: the trafo inputs' section, only in a session that runs
        // them (every other status line is the text it was): the sets held,
        // own inputs stamped, inputs received, applied live and in replays,
        // late ones rewound, missed ones (a resync), late ones a resync state
        // covered, the effect checks' leaks, own echoes moved and dropped,
        // sets made / failed, inputs that did not run, the reconcile's
        // unmakes and remakes, sets retired, inputs in the log.
        const std::string trafo = !s.trafo_active_ ? std::string() : textf(
            " trafo: sets=%zu sent=%llu recv=%llu applied=%llu replayed=%llu late=%llu trafo_missed=%llu covered=%llu "
            "trafo_original_leaks=%llu echo=%llu/%llu made=%llu/%llu run_failures=%llu reconcile=%llu/%llu retired=%llu log=%zu",
            s.trafo_sets_.size(), static_cast<unsigned long long>(s.trafo_sent_),
            static_cast<unsigned long long>(s.trafo_received_), static_cast<unsigned long long>(s.trafo_applied_),
            static_cast<unsigned long long>(s.trafo_replayed_), static_cast<unsigned long long>(s.trafo_late_),
            static_cast<unsigned long long>(s.trafo_missed_), static_cast<unsigned long long>(s.trafo_covered_),
            static_cast<unsigned long long>(s.trafo_leaks_), static_cast<unsigned long long>(s.trafo_echo_moved_),
            static_cast<unsigned long long>(s.trafo_echo_dropped_), static_cast<unsigned long long>(s.trafo_sets_made_),
            static_cast<unsigned long long>(s.trafo_set_failures_), static_cast<unsigned long long>(s.trafo_run_failures_),
            static_cast<unsigned long long>(s.trafo_unmade_), static_cast<unsigned long long>(s.trafo_remade_),
            static_cast<unsigned long long>(s.trafo_retired_), s.trafo_log_.size());
        // Step G3/G5 (world-agreement-gaps-design.md 2.5, 2.6): the
        // proximity union's section, only in a session under its world
        // option (every other status line is the text it was): place calls
        // live and in replays, the invariant guard (two bodies of a player),
        // the platform's skipped / relatched counts (host counters), and the
        // script wakes no BodyRevived was sent for.
        const std::string probes = !(s.world_options_ & WORLD_PROXIMITY_UNION) ? std::string() : textf(
            " probes: active=%d places=%llu/%llu two_bodies=%llu skipped=%llu relatched=%llu revived_local=%llu",
            s.probes_active_ ? 1 : 0, static_cast<unsigned long long>(s.probe_places_live_),
            static_cast<unsigned long long>(s.probe_places_replayed_), static_cast<unsigned long long>(s.probe_two_bodies_),
            static_cast<unsigned long long>(h.probes_skipped), static_cast<unsigned long long>(h.probes_relatched),
            static_cast<unsigned long long>(s.revived_local_));
        return textf("status: phase=%d session=%u tick=%u assigned=%d held=%d frames=%lld inputs=%llu events=%llu/%llu/%llu echo=%llu/%llu/%llu/%llu/%llu ownlife=%llu/%llu/%llu/%llu/%llu overtaken=%llu remake=%llu/%llu mlives=%llu/%llu/%llu/%llu/%llu olives=%llu/%llu/%llu/%llu/%llu snapshots=%llu/%llu/%llu "
                     "life=%llu/%llu inrew=%llu/%llu bodiless=%llu/%llu forced=%llu/%llu own_phys=%d remotes=%zu remote_inputs=%llu relay=%llu/%llu/%llu/%llu lag=%u/%u subst=%llu/%llu/%llu/%llu/%llu/%llu subst_skip=%llu/%llu/%llu subst_missed=%llu subst_coalesce=%llu/%llu/%llu/%llu acked=%u held_total=%u remote_writes=%llu remote_corr=%llu/%llu/%llu/%llu mechanisms=%zu/%zu mech_blend=%llu mech_hard=%llu mech_max_err=%.4f amend_failures=%llu rebases=%d resyncs=%llu/%llu "
                     "lead_adj=%llu lead_offset=%d slew=%d lead_forced=%llu lead_skip=%llu/%llu/%llu "
                     "rollback: snaps=%llu ok=%llu mism=%llu rb=%llu resim=%llu unmatched=%llu unmatched_run=%d/%llu far=%llu frozen=%llu imp=%llu ct=%llu/%llu sv=%llu/%llu/%llu births=%llu/%llu/%llu+%llu/%llu/%llu/%llu parked=%llu missing=%llu lives=%llu/%llu/%llu/%llu/%llu/%llu deferred=%llu activations=%llu scr=%llu/%llu/%llu/%llu/%llu t_rest=%.0f t_rep=%.0f t_max=%.0f t_cap=%.0f/%.0f max_err=%.4f last=%s "
                     "corrections: compared=%llu ignored=%llu blended=%llu hard=%llu unmatched=%llu last_err=%.4f max_err=%.4f "
                     "sectors: rew=%llu/%llu depth=%llu forgot=%llu births_moved=%llu phase_misses=%llu walk_timeouts=%llu "
                     "resync: restepped=%llu/%llu depth=%llu states=%llu stale=%llu refused=%llu deferred=%llu/%llu "
                     "snaps_dropped=%llu scripts_refused=%llu blobs_refused=%llu destroyed=%llu unexplained=%llu "
                     "unattributed=%llu unmapped=%llu unportable=%llu holds=%llu/%llu hold_max=%.0f window_rew=%llu subsumed=%llu amended=%llu resent=%llu join_wakes_withheld=%llu "
                     "input: reports=%llu starved=%llu (%.0f%%) worst=%u lead=%.1f reach=%u/%u/%zu cap=%u/%.2f/%.3f/%llu wait=%u%s%s",
                     static_cast<int>(s.phase_), s.session_, current_tick(), s.assigned_ ? 1 : 0, s.held_ ? 1 : 0,
                     static_cast<long long>(s.frames_since_anchor_),
                     static_cast<unsigned long long>(s.inputs_sent_), static_cast<unsigned long long>(s.events_sent_),
                     static_cast<unsigned long long>(s.events_received_), static_cast<unsigned long long>(s.event_dups_),
                     // echo=acked/moved/superseded/rejected/missing: our own events as the
                     // server echoed them back (protocol 9).
                     static_cast<unsigned long long>(s.own_echo_acked_), static_cast<unsigned long long>(s.own_echo_moved_),
                     static_cast<unsigned long long>(s.own_echo_superseded_), static_cast<unsigned long long>(s.own_echo_rejected_),
                     static_cast<unsigned long long>(s.own_echo_missing_),
                     // ownlife=moved/removed/resynced/remade/other: own births
                     // moved inside the history, removed from it, resynced past
                     // the reach, the re-makes at an applied tick (live and in
                     // replays), and MOVED echoes of the other types, kept where
                     // stamped (late-player design 3.3).
                     static_cast<unsigned long long>(s.own_births_moved_), static_cast<unsigned long long>(s.own_births_removed_),
                     static_cast<unsigned long long>(s.own_moves_resynced_), static_cast<unsigned long long>(s.own_remakes_),
                     static_cast<unsigned long long>(s.own_moves_other_),
                     // overtaken: older remote births relayed after their death
                     // and put back in front of it (dropped or re-ordered).
                     static_cast<unsigned long long>(s.births_overtaken_),
                     // remake=made/lost (step LB): mirrors made again at their
                     // birth tick inside a replay (or live after it), and those
                     // made again neither in the replay nor live (bodiless).
                     static_cast<unsigned long long>(s.mirror_remakes_), static_cast<unsigned long long>(s.mirror_remakes_lost_),
                     // mlives=revived/failed/settled/kept/window (gap L, L2, step EL7):
                     // mirror lives ended inside a window that the reconcile
                     // made again, those it could not, the window events no
                     // replay applied (made live after the rewind), and the
                     // late births kept on the parking path (the RC stamp
                     // rule's documented class: stamped at or before the
                     // anchor, once per birth), and the players whose window
                     // lives were replayed with no ended A-life (window).
                     static_cast<unsigned long long>(s.mirror_revives_), static_cast<unsigned long long>(s.mirror_revive_failures_),
                     static_cast<unsigned long long>(s.lives_settled_live_), static_cast<unsigned long long>(s.late_births_kept_),
                     static_cast<unsigned long long>(s.window_lives_),
                     // olives=revived/failed/remade/settled/matrices (gap L,
                     // L3, step EL8): own lives revived at a restore, those
                     // that could not be, window lives remade at their stamp,
                     // queued own changes no replay applied (made live after
                     // the rewind), and the dead own entities' matrices
                     // written into replayed ticks (design 9.7 (a)).
                     static_cast<unsigned long long>(s.own_revives_), static_cast<unsigned long long>(s.own_revive_failures_),
                     static_cast<unsigned long long>(s.own_window_remakes_), static_cast<unsigned long long>(s.own_due_settled_),
                     static_cast<unsigned long long>(s.own_matrix_writes_),
                     static_cast<unsigned long long>(s.snapshots_received_),
                     static_cast<unsigned long long>(s.snapshots_applied_), static_cast<unsigned long long>(s.snapshots_stale_),
                     static_cast<unsigned long long>(s.life_rewinds_), static_cast<unsigned long long>(s.life_rewinds_missed_),
                     static_cast<unsigned long long>(s.input_rewinds_), static_cast<unsigned long long>(s.input_rewinds_missed_),
                     static_cast<unsigned long long>(s.relay_bodiless_), static_cast<unsigned long long>(s.relay_birth_frozen_),
                     static_cast<unsigned long long>(s.forced_rollbacks_), static_cast<unsigned long long>(s.forced_rollbacks_missed_),
                     s.own_physicalized_ ? 1 : 0, s.remotes_.size(), static_cast<unsigned long long>(s.remote_inputs_received_),
                     static_cast<unsigned long long>(s.relay_new_), static_cast<unsigned long long>(s.relay_dup_),
                     static_cast<unsigned long long>(s.relay_parked_), static_cast<unsigned long long>(s.relay_late_),
                     s.relay_lag_max_, relay_lag_p99(),
                     // subst=recv/diff/same/bodiless/rew/late (late-player design
                     // 1.3): the notices of our own substituted ticks inside the
                     // reach (recv, of which diff were replayed onto the server's
                     // frame, same matched what we played and bodiless had no
                     // ball - a tick not simulated yet at arrival is sorted when
                     // its live frame is), the replays they named (rew), and the
                     // ticks past the reach or never noticed (late, a resync).
                     // subst_skip=dup/stale_epoch/base: re-deliveries, records of
                     // a datagram numbered from another base, and records no
                     // rewind could reach (the base tick, a resync's snapshot).
                     // subst_missed: own replays rewind_to refused (a resync
                     // each; inrew's second number is the remotes' only).
                     // subst_coalesce=deferred/unseen/subsumed/saved: frames on
                     // which a waiting own replay was put off (own_replay_pacer),
                     // diffs no replay could see (amended, never replayed; also
                     // counted in diff), waiting own replays a snapshot,
                     // lifecycle or forced rewind carried, and amendments that
                     // needed no own replay of their own.  recv
                     // can exceed diff + same + bodiless by the notices held
                     // for a tick not simulated yet and dropped by a resync.
                     // acked/held_total: the newest header of our own epoch (0/0
                     // until one arrives).  recv + late + base per epoch is what
                     // the server's S2 lines count.
                     static_cast<unsigned long long>(s.subst_recv_), static_cast<unsigned long long>(s.subst_diff_),
                     static_cast<unsigned long long>(s.subst_same_), static_cast<unsigned long long>(s.subst_bodiless_),
                     static_cast<unsigned long long>(s.subst_rewinds_),
                     static_cast<unsigned long long>(s.subst_late_),
                     static_cast<unsigned long long>(s.subst_dup_), static_cast<unsigned long long>(s.subst_stale_epoch_),
                     static_cast<unsigned long long>(s.subst_base_),
                     static_cast<unsigned long long>(s.subst_rewinds_missed_),
                     static_cast<unsigned long long>(s.subst_deferred_), static_cast<unsigned long long>(s.subst_unseen_),
                     static_cast<unsigned long long>(s.subst_subsumed_), static_cast<unsigned long long>(s.subst_saved_),
                     held_header_current() ? s.held_header_acked_ : 0u, held_header_current() ? s.held_header_total_ : 0u,
                     static_cast<unsigned long long>(s.remote_writes_), static_cast<unsigned long long>(rc),
                     static_cast<unsigned long long>(ri), static_cast<unsigned long long>(rb), static_cast<unsigned long long>(rh),
                     s.mechanism_tracking_.known(), s.mechanism_tracking_.resolved(),
                     static_cast<unsigned long long>(s.mechanism_blends_), static_cast<unsigned long long>(s.mechanism_hard_), mech_max,
                     static_cast<unsigned long long>(s.amend_failures_), h.rebases,
                     static_cast<unsigned long long>(s.resyncs_sent_), static_cast<unsigned long long>(s.resyncs_done_),
                     // lead_adj: SessionLeads applied; lead_offset: what their
                     // acks applied in total; slew: ticks the one in flight still
                     // moves; lead_forced: live frames that played the held
                     // frame; lead_skip=dup/stale/void: copies, decisions of
                     // another base, and decisions a resync of ours in flight
                     // voids (late-player design 2.4, 2.8).  `lead=` further
                     // on is the measured lead over the server, not this.
                     static_cast<unsigned long long>(s.lead_adjustments_), s.lead_offset_, s.lead_slew_.outstanding(),
                     static_cast<unsigned long long>(s.lead_forced_played_), static_cast<unsigned long long>(s.lead_dups_),
                     static_cast<unsigned long long>(s.lead_stale_), static_cast<unsigned long long>(s.lead_void_),
                     static_cast<unsigned long long>(rs.snapshots), static_cast<unsigned long long>(rs.matched),
                     static_cast<unsigned long long>(rs.mismatched), static_cast<unsigned long long>(rs.rollbacks),
                     static_cast<unsigned long long>(rs.resim_ticks), static_cast<unsigned long long>(rs.unmatched),
                     // unmatched_run=run/resyncs (step B2SW): the snapshots in
                     // a row with no record now, and the resyncs such runs
                     // asked for (design 7.1 decision 1).
                     s.consecutive_unmatched_, static_cast<unsigned long long>(s.unmatched_resyncs_),
                     static_cast<unsigned long long>(rs.too_far), static_cast<unsigned long long>(rs.frozen),
                     static_cast<unsigned long long>(rs.impulses_restored),
                     static_cast<unsigned long long>(rs.contacts_restored), static_cast<unsigned long long>(rs.contact_restore_failures),
                     static_cast<unsigned long long>(rs.savestates_restored), static_cast<unsigned long long>(rs.savestate_failures),
                     static_cast<unsigned long long>(rs.savestate_capture_failures),
                     static_cast<unsigned long long>(rs.births_inserted), static_cast<unsigned long long>(rs.births_inserted_exact),
                     static_cast<unsigned long long>(rs.birth_insert_mismatches),
                     static_cast<unsigned long long>(rs.births_recreated), static_cast<unsigned long long>(rs.births_recreated_exact),
                     static_cast<unsigned long long>(rs.birth_recreate_mismatches), static_cast<unsigned long long>(rs.birth_recreate_failures),
                     // parked: birth items of balls kept parked by design
                     // (keeps_parked; rollback-engine gaps design 2.4).
                     static_cast<unsigned long long>(rs.births_parked_balls),
                     static_cast<unsigned long long>(rs.births_missing),
                     // lives=ended/revived_own/revived_mirror/revived_mech/
                     // remade/unrevivable: gap L (rollback-engine gaps
                     // design 2.3.1, step EL6).
                     static_cast<unsigned long long>(rs.lives_ended), static_cast<unsigned long long>(rs.lives_revived_own),
                     static_cast<unsigned long long>(rs.lives_revived_mirror), static_cast<unsigned long long>(rs.lives_revived_mech),
                     static_cast<unsigned long long>(rs.lives_remade), static_cast<unsigned long long>(rs.lives_unrevivable),
                     static_cast<unsigned long long>(rs.unknown_rechecks_deferred), static_cast<unsigned long long>(h.activations_replayed),
                     static_cast<unsigned long long>(rs.script_restores), static_cast<unsigned long long>(rs.script_restore_failures),
                     static_cast<unsigned long long>(rs.script_mismatches), static_cast<unsigned long long>(rs.script_float_mismatches),
                     static_cast<unsigned long long>(h.script_reset_skips),
                     rs.restore_us / 1000.0, rs.replay_us / 1000.0, rs.rewind_us_max / 1000.0,
                     rs.capture_us / 1000.0, rs.capture_us_max / 1000.0,
                     rs.max_error, rs.last_mismatch.c_str(),
                     static_cast<unsigned long long>(st.compared), static_cast<unsigned long long>(st.ignored),
                     static_cast<unsigned long long>(st.blended), static_cast<unsigned long long>(st.hard),
                     static_cast<unsigned long long>(st.unmatched), st.last_error, st.max_error,
                     // sectors: rew=made/missed (step B1: late or moved Sector
                     // stamps re-derived by a rewind; missed resynced), depth
                     // (their re-simulated ticks), forgot (live passes the
                     // re-derivation did not reproduce at their tick),
                     // births_moved (bodies of those passes not made at their
                     // live birth tick), and the phase check's misses.
                     static_cast<unsigned long long>(s.sector_rewinds_), static_cast<unsigned long long>(s.sector_rewinds_missed_),
                     static_cast<unsigned long long>(s.sector_rewind_depth_),
                     static_cast<unsigned long long>(s.sector_passes_forgotten_),
                     static_cast<unsigned long long>(rs.births_moved), static_cast<unsigned long long>(s.phase_misses_),
                     // walk_timeouts: the level-walk gate opened by its timeout (step S4)
                     static_cast<unsigned long long>(s.sectors_.walk_timeouts()),
                     // resync: (step A3) the exact resyncs re-stepped / done
                     // in all, their re-stepped ticks, the states received,
                     // stale and refused, the applies put off for the body set
                     // (and their frames), the snapshots dropped while one was
                     // pending, the server scripts and blobs refused, the
                     // reconcile's destroys, unexplained and unattributed
                     // bodies, rows with no body, unportable references, and
                     // the resync holds (timed out) with the longest in ms, and
                     // the correction rewinds deeper than max_resim_ticks
                     // inside a re-step window (window_rewind_reach), and the
                     // pending corrections an apply's re-step subsumed, and the
                     // own records amended while an exact resync was pending,
                     // and the old numbering's own frames played again.
                     static_cast<unsigned long long>(s.resyncs_restepped_), static_cast<unsigned long long>(s.resyncs_done_),
                     static_cast<unsigned long long>(s.resync_restep_depth_), static_cast<unsigned long long>(s.resync_states_),
                     static_cast<unsigned long long>(s.resync_states_stale_),
                     static_cast<unsigned long long>(s.resync_states_refused_),
                     static_cast<unsigned long long>(s.resync_deferred_), static_cast<unsigned long long>(s.resync_deferred_ticks_),
                     static_cast<unsigned long long>(s.snapshots_while_resync_),
                     static_cast<unsigned long long>(s.resync_scripts_refused_),
                     static_cast<unsigned long long>(s.resync_blobs_refused_), static_cast<unsigned long long>(s.resync_destroyed_),
                     static_cast<unsigned long long>(s.resync_unexplained_), static_cast<unsigned long long>(s.resync_unattributed_),
                     static_cast<unsigned long long>(s.resync_rows_unmapped_), static_cast<unsigned long long>(s.resync_unportable_),
                     static_cast<unsigned long long>(s.resync_holds_), static_cast<unsigned long long>(s.resync_hold_timeouts_),
                     s.resync_hold_ms_max_, static_cast<unsigned long long>(s.resync_window_rewinds_),
                     static_cast<unsigned long long>(s.resync_rewinds_subsumed_),
                     static_cast<unsigned long long>(s.resync_amends_),
                     static_cast<unsigned long long>(s.resync_resent_),
                     static_cast<unsigned long long>(s.join_wakes_withheld_),   // step A5
                     static_cast<unsigned long long>(s.input_reports_), static_cast<unsigned long long>(s.input_starved_),
                     s.input_reports_ ? 100.0 * static_cast<double>(s.input_starved_) / static_cast<double>(s.input_reports_) : 0.0,
                     s.worst_starved_,
                     s.lead_count_ ? static_cast<double>(s.lead_sum_) / static_cast<double>(s.lead_count_) : 0.0,
                     // reach=now/deepest/history (late-player plan step 12):
                     // the rollback's reach now, the deepest it reached this
                     // session, and the history it keeps.
                     s.rollback_.thresholds().max_resim_ticks,
                     std::max(s.reach_max_, s.rollback_.thresholds().max_resim_ticks), s.rollback_.thresholds().history_ticks,
                     // cap=cap/restore ms/replay ms per tick/rewinds (step
                     // R12: the cap's estimator) and wait= the server's wait
                     // in the newest relay header (protocol 13).
                     s.rollback_.rewind_cost().cap(), s.rollback_.rewind_cost().restore_ms(),
                     s.rollback_.rewind_cost().replay_ms_per_tick(),
                     static_cast<unsigned long long>(s.rollback_.rewind_cost().samples()),
                     static_cast<unsigned>(s.server_wait_),
                     trafo.c_str(), probes.c_str());
    }

}

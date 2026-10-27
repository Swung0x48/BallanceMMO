#pragma once

// The counters of the level's mechanism Sequencer blocks, as the level file has
// them (design 9.26).
//
// A Virtools Sequencer keeps its position in an output parameter (older
// versions: a local parameter), and neither the retail engine nor the
// reimplementation touches it when a script is (re)activated with reset - the
// reset clears activation flags and link delays, nothing else.  The level
// restart every session starts with therefore leaves each mechanism's Sequencer
// wherever the previous play left it.  Level 11's sandbag Swing group fires its
// Sequencer once at Swing.On to create the first of its two opposite
// SetPhysicsForce actuators, so the direction a sack starts swinging in depends
// on how many 1.5 s flips happened before the restart: on a retail client, on
// how long the player had been in the level when the session started; on the
// server, on its boot.  Two clients that entered the level at different moments
// start their sacks in opposite directions, and the rollback engine can never
// reconcile that - the FORCE differs, not the state it keeps restoring
// (measured 2026-09-11: a sandbag correction every 6 ticks for the whole
// session on the client whose counter read 1, none on the one whose counter
// read 0, the server's).
//
// So every side records the counters when the level file is loaded - before any
// of its scripts ran - and writes them back at the session anchor: a session
// starts its mechanisms the way a first play of the level does, on every side.

#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "CKAll.h"

namespace bmmo::game {
    class sequencer_state {
    public:
        struct entry {
            CK_ID id = 0;
            std::string owner;
            int output = 0;
            bool has_output = false;
            int local = 0;
            bool has_local = false;
        };

        // A "Sequencer" block in a script owned by a level object ("P_*": the
        // mechanisms, placeholders and prop balls).  Gameplay and menu scripts
        // are left alone.
        static CKBehavior* mechanism_sequencer(CKObject* object, std::string* owner_name) {
            auto* beh = CKBehavior::Cast(object);
            if (!beh || !beh->GetName() || std::strcmp(beh->GetName(), "Sequencer") != 0) return nullptr;
            CKBeObject* owner = beh->GetOwner();
            const char* name = owner && owner->GetName() ? owner->GetName() : "";
            if (std::strncmp(name, "P_", 2) != 0) return nullptr;
            if (owner_name) *owner_name = name;
            return beh;
        }

        // Records every mechanism Sequencer not recorded yet, with its counter
        // as it is now.  Call on every tick while a level loads (the engine)
        // or from the map-load hook (the retail client): a block is recorded
        // on first sight, before its script ran, and never overwritten.
        // Returns how many were added.
        int capture_new(CKContext* context) {
            if (!context) return 0;
            int added = 0;
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int i = 0; i < count; ++i) {
                std::string owner;
                CKBehavior* beh = mechanism_sequencer(context->GetObject(ids[i]), &owner);
                if (!beh || known(beh->GetID())) continue;
                entry e;
                e.id = beh->GetID();
                e.owner = owner;
                if (CKParameterOut* out = beh->GetOutputParameter(0))
                    e.has_output = out->GetValue(&e.output) == CK_OK;
                if (CKParameterLocal* local = beh->GetLocalParameter(0))
                    e.has_local = local->GetValue(&e.local) == CK_OK;
                if (!e.has_output && !e.has_local) continue;
                entries_.push_back(e);
                ++added;
            }
            return added;
        }

        // Writes the recorded counters back into the blocks that still exist.
        // Call at the session anchor, before the world's first tick.  Returns
        // how many blocks were written.
        int restore(CKContext* context) const {
            if (!context) return 0;
            int written = 0;
            for (const entry& e: entries_) {
                CKBehavior* beh = mechanism_sequencer(context->GetObject(e.id), nullptr);
                if (!beh) continue;
                bool wrote = false;
                if (e.has_output) {
                    if (CKParameterOut* out = beh->GetOutputParameter(0)) {
                        int value = e.output;
                        wrote = out->SetValue(&value, sizeof(value)) == CK_OK || wrote;
                    }
                }
                if (e.has_local) {
                    if (CKParameterLocal* local = beh->GetLocalParameter(0)) {
                        int value = e.local;
                        wrote = local->SetValue(&value, sizeof(value)) == CK_OK || wrote;
                    }
                }
                if (wrote) ++written;
            }
            return written;
        }

        void clear() { entries_.clear(); }
        size_t size() const { return entries_.size(); }

        // "owner=counter, ..." for a log line.
        std::string describe() const {
            std::string out;
            for (const entry& e: entries_) {
                if (!out.empty()) out += ", ";
                out += e.owner;
                out += '=';
                out += std::to_string(e.has_output ? e.output : e.local);
            }
            return out;
        }

    private:
        bool known(CK_ID id) const {
            for (const entry& e: entries_)
                if (e.id == id) return true;
            return false;
        }

        std::vector<entry> entries_;
    };

    // The countdowns of the level's TT Scaleable Proximity blocks, as the level
    // file has them (session-client-core design 6.2, step Q; the R0 finding).
    //
    // A TT Scaleable Proximity measures its distance only every "Last Check"
    // frames: local 1 counts down by one per execution and is set to 1..
    // Maximum Framedelay by the distance at each check; local 0 is the last
    // in/out state (0 out, 1 in; "In" sets it to 2, "unknown", so the next
    // check fires Enter or Exit Range) (TT_Toolbox_RT ScaleableProximity.cpp).
    // "In" does not touch the countdown.  Like a Sequencer's counter, neither
    // CKBehavior::Reset nor the level restart touches them, so the phase of
    // every check after the session anchor is whatever the play before the
    // anchor left: on a retail client, how long it had been in the level; on
    // the server and the headless client, which load the level fresh, what
    // the same load ticks before the anchor left (on the arena 13 of the 24
    // blocks run during those 122 ticks).  Two retail runs of one key record
    // woke P_Modul_01's pusher at 604 and 606 (Last Check 16 and 30 at the
    // anchor), the server at its own phase's tick unless a client's
    // BodyRevived came first.
    //
    // So every side records both locals when the level file is loaded - before
    // any of its scripts ran - and writes them back at the session anchor, as
    // sequencer_state does for the Sequencers: the countdown phase at the
    // anchor is the level file's in every world.  The rule is about the
    // countdown only: which entity a block measures (its ObjectA, rewired on
    // the server) is left alone.
    class proximity_countdown_state {
    public:
        struct entry {
            CK_ID id = 0;
            std::string owner;
            int was_in = 0;
            bool has_was_in = false;
            int last_check = 0;
            bool has_last_check = false;
        };

        // A level object: the placeholders' scripts ("P_", "PC_", "PS_",
        // "PE_", "PR_" owners).  base.cmo's scripts (Gameplay_Tutorial's
        // twelve probes, owned by All_Gameplay, and one of All_Sound's)
        // outlive a level load, so they have no level-file value, and are
        // left alone (census counts and names them).
        static bool level_owner(const char* name) {
            if (!name || name[0] != 'P') return false;
            if (name[1] == '_') return true;
            return name[1] >= 'A' && name[1] <= 'Z' && name[2] == '_';
        }

        static CKBehavior* proximity(CKObject* object) {
            auto* beh = CKBehavior::Cast(object);
            if (!beh) return nullptr;
            const char* prototype = beh->GetPrototypeName();
            if (!prototype || std::strcmp(prototype, "TT Scaleable Proximity") != 0) return nullptr;
            return beh;
        }

        static const char* owner_name_of(CKBehavior* beh) {
            CKBeObject* owner = beh->GetOwner();
            return owner && owner->GetName() ? owner->GetName() : "";
        }

        static CKBehavior* level_proximity(CKObject* object, std::string* owner_name) {
            CKBehavior* beh = proximity(object);
            if (!beh) return nullptr;
            const char* name = owner_name_of(beh);
            if (!level_owner(name)) return nullptr;
            if (owner_name) *owner_name = name;
            return beh;
        }

        // Records every level proximity block not recorded yet, with its
        // locals as they are now.  Called where sequencer_state::capture_new
        // is: on every tick while a level loads (the engine) or on the frames
        // after the map-load hook (the retail client).  Returns how many were
        // added.
        int capture_new(CKContext* context) {
            if (!context) return 0;
            int added = 0;
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int i = 0; i < count; ++i) {
                std::string owner;
                CKBehavior* beh = level_proximity(context->GetObject(ids[i]), &owner);
                if (!beh || known(beh->GetID())) continue;
                entry e;
                e.id = beh->GetID();
                e.owner = owner;
                if (CKParameterLocal* local = beh->GetLocalParameter(0))
                    e.has_was_in = local->GetValue(&e.was_in) == CK_OK;
                if (CKParameterLocal* local = beh->GetLocalParameter(1))
                    e.has_last_check = local->GetValue(&e.last_check) == CK_OK;
                if (!e.has_was_in && !e.has_last_check) continue;
                entries_.push_back(e);
                ++added;
            }
            return added;
        }

        // Writes the recorded locals back into the blocks that still exist.
        // Call at the session anchor, before the world's first tick.  Returns
        // how many blocks were written; `differed` (optional) gets how many of
        // them held another value just before (the arena: 13 on the server and
        // the headless client, whose load ticks run some of them, 14 on a
        // retail client).  `before` (optional) gets "owner=in/last, ..." of
        // the values the differing blocks held just before.
        int restore(CKContext* context, int* differed = nullptr, std::string* before = nullptr) const {
            if (differed) *differed = 0;
            if (before) before->clear();
            if (!context) return 0;
            int written = 0;
            for (const entry& e: entries_) {
                CKBehavior* beh = level_proximity(context->GetObject(e.id), nullptr);
                if (!beh) continue;
                if (before) {
                    int in_now = 0, last_now = 0;
                    const bool has_in = local_value(beh->GetLocalParameter(0), in_now);
                    const bool has_last = local_value(beh->GetLocalParameter(1), last_now);
                    if ((e.has_was_in && (!has_in || in_now != e.was_in))
                        || (e.has_last_check && (!has_last || last_now != e.last_check))) {
                        if (!before->empty()) *before += ", ";
                        *before += e.owner + '=' + (has_in ? std::to_string(in_now) : std::string("-")) + '/'
                            + (has_last ? std::to_string(last_now) : std::string("-"));
                    }
                }
                bool wrote = false, changed = false;
                if (e.has_was_in) wrote = write(beh->GetLocalParameter(0), e.was_in, changed) || wrote;
                if (e.has_last_check) wrote = write(beh->GetLocalParameter(1), e.last_check, changed) || wrote;
                if (wrote) ++written;
                if (changed && differed) ++*differed;
            }
            return written;
        }

        void clear() { entries_.clear(); }
        size_t size() const { return entries_.size(); }

        // The TT Scaleable Proximity blocks the record does not cover, for a
        // warning at the anchor: `unrecorded` level ones (born after the
        // record window - the retail client records only the frames after the
        // map-load hook, the engine on every load tick) and `other_owner` ones
        // whose owner is no level object (base.cmo's, left alone by design;
        // a level object named outside level_owner's pattern would show here;
        // `other_names` (optional) gets their distinct owner names).
        void census(CKContext* context, int& unrecorded, int& other_owner,
                    std::string* other_names = nullptr) const {
            unrecorded = other_owner = 0;
            if (other_names) other_names->clear();
            if (!context) return;
            std::vector<std::pair<std::string, int>> names;
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int i = 0; i < count; ++i) {
                CKBehavior* beh = proximity(context->GetObject(ids[i]));
                if (!beh) continue;
                const char* name = owner_name_of(beh);
                if (level_owner(name)) {
                    if (!known(beh->GetID())) ++unrecorded;
                    continue;
                }
                ++other_owner;
                auto it = names.begin();
                while (it != names.end() && it->first != name) ++it;
                if (it == names.end()) names.emplace_back(name, 1);
                else ++it->second;
            }
            if (!other_names) return;
            for (const auto& [name, n]: names) {
                if (!other_names->empty()) *other_names += ", ";
                *other_names += (name.empty() ? std::string("(no owner)") : name) + " x" + std::to_string(n);
            }
        }

        // "owner=in/last, ..." for a log line.
        std::string describe() const {
            std::string out;
            for (const entry& e: entries_) {
                if (!out.empty()) out += ", ";
                out += e.owner;
                out += '=';
                out += e.has_was_in ? std::to_string(e.was_in) : std::string("-");
                out += '/';
                out += e.has_last_check ? std::to_string(e.last_check) : std::string("-");
            }
            return out;
        }

    private:
        static bool local_value(CKParameterLocal* local, int& value) {
            return local && local->GetValue(&value) == CK_OK;
        }

        static bool write(CKParameterLocal* local, int value, bool& changed) {
            if (!local) return false;
            int now = 0;
            if (local->GetValue(&now) != CK_OK || now != value) changed = true;
            return local->SetValue(&value, sizeof(value)) == CK_OK;
        }

        bool known(CK_ID id) const {
            for (const entry& e: entries_)
                if (e.id == id) return true;
            return false;
        }

        std::vector<entry> entries_;
    };

    // The level file's counters, the host glue written once (step HU1):
    // recorded on every tick while a level loads, restored at the anchor
    // (design 9.26 for the Sequencers, step Q for the proximity countdowns).
    // `log` gets each line without the host's own prefix.
    inline void capture_level_file_counters(CKContext* context, sequencer_state& sequencers,
                                            proximity_countdown_state& proximities,
                                            const std::function<void(const std::string& line)>& log) {
        if (const int recorded = sequencers.capture_new(context); recorded > 0 && log)
            log(std::to_string(recorded) + " mechanism Sequencer counter(s) recorded from the level file ("
                + sequencers.describe() + ")");
        if (const int recorded = proximities.capture_new(context); recorded > 0 && log)
            log(std::to_string(recorded) + " proximity countdown(s) recorded from the level file (" + proximities.describe()
                + ")");
    }

    inline void restore_level_file_counters(CKContext* context, const sequencer_state& sequencers,
                                            const proximity_countdown_state& proximities,
                                            const std::function<void(const std::string& line)>& log) {
        const int sequencers_restored = sequencers.restore(context);
        if (log)
            log(std::to_string(sequencers_restored) + " of " + std::to_string(sequencers.size())
                + " mechanism Sequencer counter(s) restored to the level file's (" + sequencers.describe() + ")");
        int differed = 0;
        const int proximities_restored = proximities.restore(context, &differed);
        if (log)
            log(std::to_string(proximities_restored) + " of " + std::to_string(proximities.size())
                + " proximity countdown(s) restored to the level file's (" + std::to_string(differed) + " differed)");
    }
}

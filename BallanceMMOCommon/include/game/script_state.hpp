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
#include <string>
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
}

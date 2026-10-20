#pragma once

// The retail sector resets, neutralized for a session (design 9.25 N1 and
// 9.28; the retail mod's death_reset_resolve/apply/restore in
// BallanceMMOClient/session/physics_session_client.cpp is the same edit with
// the mod's own logging, and this is the headless client's copy of it).
//
// Gameplay_Ingame runs Gameplay_SectorManager with Reset? = TRUE from two of
// its groups - the whole sector deactivated and re-activated in one frame:
//
//   * BallManager / Deactivate Ball, for one player's death;
//   * Init Ingame / activate Scripts, for a level reset (a lost last life and
//     the ESC menu's restart both take it: "Reset Level" -> Event_handler /
//     reset Level -> Activate Script (Gameplay_Ingame) -> Init Ingame).
//
// The server's world never does either: it activates the union of the
// players' sectors and never resets (physics_world::update_sectors).  A client
// that reset its sector on a death would tear the shared mechanisms down and
// rebuild their joints under the body guard, and then disagree with the server
// by metres for the rest of the session (measured on the mechanism arena: the
// level-end balloon chain 12-20 m off on every snapshot after a death, in a
// run with no rewinds at all).
//
// Each block's "Script" target is emptied for the session: Execute Script
// activates its Out immediately when the script is null (ExecuteScript.cpp),
// so the death chain keeps its shape, its delays and the respawn behind it,
// and Init Ingame's other Activate Scripts still run.  The level-reset one is
// on the level-start path too, so it is emptied only once the sector it would
// activate is up (the client has resolved a mechanism body of its own).  Both
// call sites are found by walking Gameplay_Ingame for the target's name, not
// by group path; the call sites outside this script are left alone
// (Gameplay_Events / activate Sektor is the checkpoint deactivation's, and
// Event_handler's two only ever deactivate and share Level_Init's published
// parameter).

#include <cstring>
#include <string>
#include <vector>

#include "CKAll.h"

namespace bmmo::game {
    class sector_reset_guard {
    public:
        struct call_site {
            CK_ID block = 0;         // the Execute Script block
            std::string chain;       // the group it sits in (Deactivate Ball, activate Scripts)
            CK_ID retail = 0;        // the target it had: Gameplay_SectorManager
            bool deferred = false;   // only once the sector is up (the level-start path)
            bool applied = false;
        };

        // Finds the call sites.  Returns an empty string on success, else why
        // the graph was not understood - and then nothing is touched: half a
        // neutralization is worse than a loud failure.
        std::string resolve(CKContext* context) {
            sites_.clear();
            CKBehavior* ingame = root_script(context, "Gameplay_Ingame");
            if (!ingame) return "Gameplay_Ingame is missing";
            std::string trouble;
            walk(context, ingame, trouble);
            if (!trouble.empty()) { sites_.clear(); return trouble; }
            if (sites_.size() < 2) {
                const auto n = sites_.size();
                sites_.clear();
                return "Gameplay_Ingame runs Gameplay_SectorManager from " + std::to_string(n)
                     + " place(s), expected at least 2 (Deactivate Ball and Init Ingame / activate Scripts)";
            }
            return {};
        }

        // Empties every target (the deferred ones only when `sector_up`) and
        // keeps it empty: a level reset that handed the retail target back has
        // to be caught before the next death runs the chain.  Returns the
        // chains neutralized by this call, for the log.
        std::vector<std::string> apply(CKContext* context, bool sector_up) {
            std::vector<std::string> fresh;
            for (auto& site: sites_) {
                if (!site.block || (site.deferred && !sector_up)) continue;
                CKParameter* param = target_parameter(context, site.block);
                if (!param) continue;
                CK_ID current = 0;
                param->GetValue(&current);
                if (current == 0) { site.applied = true; continue; }     // already neutral
                if (current != site.retail) continue;                     // somebody else's value
                const CK_ID zero = 0;
                param->SetValue(&zero, sizeof(CK_ID));
                if (!site.applied) {
                    site.applied = true;
                    fresh.push_back(site.chain);
                }
            }
            return fresh;
        }

        // Session end: the retail targets back.
        void restore(CKContext* context) {
            for (auto& site: sites_) {
                CKParameter* param = site.block ? target_parameter(context, site.block) : nullptr;
                if (param && site.retail) {
                    CK_ID current = 0;
                    param->GetValue(&current);
                    if (current == 0) param->SetValue(&site.retail, sizeof(CK_ID));
                }
            }
            sites_.clear();
        }

        size_t count() const { return sites_.size(); }
        const std::vector<call_site>& sites() const { return sites_; }

    private:
        std::vector<call_site> sites_;

        static CKBehavior* root_script(CKContext* context, const char* name) {
            if (!context) return nullptr;
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int i = 0; i < count; ++i) {
                auto* beh = CKBehavior::Cast(context->GetObject(ids[i]));
                if (beh && !beh->GetParent() && beh->GetName() && std::strcmp(beh->GetName(), name) == 0) return beh;
            }
            return nullptr;
        }

        // The "Script" input's source parameter of an Execute Script block,
        // when it is the block's own (a CK_ID-sized script parameter).
        static CKParameter* target_parameter(CKContext* context, CK_ID block_id) {
            auto* block = CKBehavior::Cast(context ? context->GetObject(block_id) : nullptr);
            if (!block || !block->IsUsingFunction()) return nullptr;
            const char* prototype = block->GetPrototypeName();
            if (!prototype || std::strcmp(prototype, "Execute Script") != 0) return nullptr;
            CKParameterIn* input = block->GetInputParameter(1);
            if (!input || input->GetGUID() != CKPGUID_SCRIPT) return nullptr;
            CKParameter* param = input->GetRealSource();
            return param && param->GetDataSize() == static_cast<int>(sizeof(CK_ID)) ? param : nullptr;
        }

        // How many behavior inputs read one parameter: emptying a parameter
        // something else reads would take its value away too.
        static int readers(CKContext* context, CKParameter* param) {
            int readers = 0;
            const int behaviors = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int b = 0; b < behaviors; ++b) {
                auto* other = CKBehavior::Cast(context->GetObject(ids[b]));
                const int inputs = other ? other->GetInputParameterCount() : 0;
                for (int k = 0; k < inputs; ++k) {
                    CKParameterIn* in = other->GetInputParameter(k);
                    if (in && in->GetRealSource() == param) ++readers;
                }
            }
            return readers;
        }

        void walk(CKContext* context, CKBehavior* group, std::string& trouble) {
            const int subs = group->GetSubBehaviorCount();
            for (int i = 0; i < subs && trouble.empty(); ++i) {
                CKBehavior* sub = group->GetSubBehavior(i);
                if (!sub) continue;
                if (!sub->IsUsingFunction()) { walk(context, sub, trouble); continue; }
                const char* prototype = sub->GetPrototypeName();
                if (!prototype || std::strcmp(prototype, "Execute Script") != 0) continue;
                auto* target = CKBehavior::Cast(sub->GetInputParameterObject(1));   // "Script"
                const char* name = target ? target->GetName() : nullptr;
                if (!name || std::strcmp(name, "Gameplay_SectorManager") != 0) continue;
                CKParameter* param = target_parameter(context, sub->GetID());
                if (!param) { trouble = "an Execute Script block has no usable Script target parameter"; return; }
                const int n = readers(context, param);
                if (n != 1) {
                    trouble = "the target parameter of " + std::string(group->GetName() ? group->GetName() : "?")
                            + " is read by " + std::to_string(n) + " inputs";
                    return;
                }
                call_site site;
                site.block = sub->GetID();
                site.chain = group->GetName() ? group->GetName() : "?";
                param->GetValue(&site.retail);
                // Everything but the death chain's is on the level-start path.
                site.deferred = site.chain != "Deactivate Ball";
                sites_.push_back(site);
            }
        }
    };
}

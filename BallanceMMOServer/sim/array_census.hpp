#pragma once

// SimTool --array-reads (docs/record-format-gaps-design.md 2.2 and 7.2 S2):
// the array-read census of a world at its anchor, read-only.  Built here at
// step K0; since step RK2 the census is the shared game/array_reads.hpp,
// which every world runs at its anchor (the server, the headless client, the
// retail core host) and which decides the record's written-cell section.
// This file keeps SimTool's name for it and prints its report.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <game/array_reads.hpp>

namespace bmmo::sim {
    struct array_census: bmmo::game::array_reads {
        // The report (array_reads::lines) on stdout: one line per entry
        // (unrecorded and sound always, the rest with `all`), the count per
        // class, the written-cell section, the unrecorded entries grouped
        // and the closing ARRAY-READS line.
        static void print(const result& r, int level, bool client_scope, bool all) {
            for (const std::string& line: lines(r, level, client_scope, all)) std::printf("%s\n", line.c_str());
        }

        // Step F4, --collectible-fixture: the per-world collectible rule's
        // negative fixture (session/collectible_rules.hpp), in the loaded
        // level only.  The first P_Extra_* `Set Cell` (by id) whose Row Index
        // is an `Iterator If` output: "other-writer" renames its root script
        // (a non-P_Extra writer of the column), "other-row" feeds its Row
        // Index from a new constant local (a P_Extra writer of another row).
        // Returns what it did, or "no ..." when the level has no such block.
        static std::string break_collectible_rule(CKContext* context, const std::string& how) {
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            std::vector<CK_ID> sorted(ids, ids + count);
            std::sort(sorted.begin(), sorted.end());
            for (CK_ID id: sorted) {
                auto* block = CKBehavior::Cast(context->GetObject(id));
                if (!block || !block->IsUsingFunction() || !block->GetPrototypeName()
                    || std::strcmp(block->GetPrototypeName(), "Set Cell") != 0)
                    continue;
                CKBehavior* root = block;
                while (root->GetParent()) root = root->GetParent();
                const std::string root_name = root->GetName() ? root->GetName() : "";
                if (root_name.rfind("P_Extra_", 0) != 0) continue;
                CKParameterIn* row = nullptr;
                for (int i = 0; i < block->GetInputParameterCount(); ++i) {
                    CKParameterIn* in = block->GetInputParameter(i);
                    if (in && in->GetName() && std::strcmp(in->GetName(), "Row Index") == 0) row = in;
                }
                CKParameter* source = row ? row->GetRealSource() : nullptr;
                if (!source || source->GetClassID() != CKCID_PARAMETEROUT) continue;
                auto* it = CKBehavior::Cast(source->GetOwner());
                if (!it || !it->GetPrototypeName() || std::strcmp(it->GetPrototypeName(), "Iterator If") != 0) continue;
                const std::string what = root_name + " (id " + std::to_string(static_cast<unsigned>(root->GetID()))
                                         + "), Set Cell id " + std::to_string(static_cast<unsigned>(block->GetID()));
                if (how == "other-writer") {
                    root->SetName(const_cast<CKSTRING>("F4_Fixture_Writer Script"));
                    return "renamed " + what + " to F4_Fixture_Writer Script";
                }
                CKBehavior* graph = block->GetParent() ? block->GetParent() : root;
                CKParameterLocal* local = graph->CreateLocalParameter(const_cast<CKSTRING>("F4 fixture row"), CKPGUID_INT);
                const int zero = 0;
                if (!local || local->SetValue(&zero, sizeof(zero)) != CK_OK || row->SetDirectSource(local) != CK_OK)
                    return "no fixture: the constant row could not be wired into " + what;
                return "row index of " + what + " fed from a new constant local (row 0)";
            }
            return "no P_Extra_* Set Cell fed by an Iterator If in this level";
        }
    };
}

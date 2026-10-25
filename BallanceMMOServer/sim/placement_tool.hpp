#pragma once

// SimTool --list-placement (docs/sector-placement-design.md 7.2 V1, step
// S1): the sector placement's row plan of a level as the world stands at its
// anchor, read-only.  Prints the level builder's Object Copy blocks (the
// chosen PH copier and the ignored ones), the dependency words the chosen one
// passes to CopyObjects, every PH row (sector, type, object, the matrix as %a
// floats and as its 16 words), the plan (one line per row), the same-pose
// rows, the rewritten share rows, the copies by kind and the applied table's
// changed rows.  Step S2 adds every copy's census and fingerprint once the
// copies can be made.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <entity/session.hpp>
#include <game/sector_placement.hpp>
#include <session/placement_plan.hpp>

#include "CKAll.h"

namespace bmmo::sim {

    // The kind of a PH type, for the design's table (1.4): extra, trafo, fan
    // (P_Modul_18), module (any other P_Modul_) or prop.
    inline const char* placement_type_kind(const std::string& type) {
        if (type.rfind("P_Extra_", 0) == 0) return "extra";
        if (type.rfind("P_Trafo_", 0) == 0) return "trafo";
        if (type == "P_Modul_18") return "fan";
        if (type.rfind("P_Modul_", 0) == 0) return "module";
        return "prop";
    }

    // Prints the listing; 0 when exactly one builder copy block is chosen and
    // the plan has no error, 3 otherwise (printed as such).
    inline int list_placement(CKContext* context, int level) {
        std::printf("placement listing: level %d\n", level);
        int result = 0;

        std::string block_error;
        const std::vector<game::builder_copy_block> blocks = game::find_builder_copy_blocks(context, block_error);
        CKBehavior* chosen = nullptr;
        for (const game::builder_copy_block& b: blocks) {
            std::printf("%s copy block: %s/Object Copy (id %u) -> %s\n", b.chosen ? "builder" : "ignored",
                        b.graph.c_str(), static_cast<unsigned>(b.block->GetID()),
                        b.out_target.empty() ? "(nothing)" : b.out_target.c_str());
            if (b.chosen) chosen = b.block;
        }
        if (!block_error.empty()) {
            std::printf("BLOCK ERROR: %s\n", block_error.c_str());
            chosen = nullptr;
            result = 3;
        }
        if (chosen) std::printf("%s", game::describe_copy_dependencies(chosen).c_str());

        std::vector<session::placement_row> rows;
        const std::string read_error = game::read_placement_rows(context, rows);
        if (!read_error.empty()) {
            std::printf("PH ERROR: %s\n", read_error.c_str());
            std::fflush(stdout);
            return 3;
        }
        std::printf("PH rows: %zu\n", rows.size());
        for (const session::placement_row& r: rows) {
            float m[16];
            std::memcpy(m, r.matrix, sizeof(m));
            std::printf("ph %d s%d %s %s [%a,%a,%a,%a][%a,%a,%a,%a][%a,%a,%a,%a][%a,%a,%a,%a] words=", r.row, r.sector,
                        r.type.c_str(), r.object.empty() ? "-" : r.object.c_str(),
                        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            for (int k = 0; k < 16; ++k) std::printf("%s%08x", k ? "," : "", static_cast<unsigned>(r.matrix[k]));
            std::printf("\n");
        }

        const session::placement_plan plan = session::plan_placements(rows, session::MAX_NAME);
        std::map<std::string, int> by_kind;
        size_t longest = 0;
        std::string same_pose, rewritten;
        for (size_t i = 0; i < plan.actions.size(); ++i) {
            const session::placement_action& a = plan.actions[i];
            std::printf("plan %s\n", session::describe_placement_action(a, rows[i].type).c_str());
            if (a.what == session::placement_action::kind::copy) {
                ++by_kind[placement_type_kind(rows[i].type)];
                longest = std::max(longest, a.name.size());
            }
            if (a.what == session::placement_action::kind::share) {
                same_pose += (same_pose.empty() ? "" : ", ") + std::string("r") + std::to_string(a.row) + " = r"
                           + std::to_string(a.holder_row) + " (" + a.source + ")";
                if (a.name != a.source)
                    rewritten += (rewritten.empty() ? "" : ", ") + std::string("r") + std::to_string(a.row) + " "
                               + a.source + " -> " + a.name;
            }
        }
        std::printf("plan copies by kind: extra %d / trafo %d / fan %d / module %d / prop %d\n", by_kind["extra"],
                    by_kind["trafo"], by_kind["fan"], by_kind["module"], by_kind["prop"]);
        std::printf("plan same-pose rows: %s\n", same_pose.empty() ? "-" : same_pose.c_str());
        std::printf("plan rewritten share rows: %s\n", rewritten.empty() ? "-" : rewritten.c_str());
        std::printf("plan longest copy name: %zu (MAX_NAME %zu)\n", longest, session::MAX_NAME);
        const std::vector<session::placement_row> applied = session::placement_applied_rows(rows, plan);
        for (size_t i = 0; i < applied.size(); ++i)
            if (applied[i].object != rows[i].object)
                std::printf("applied row %d: %s -> %s\n", applied[i].row, rows[i].object.c_str(), applied[i].object.c_str());
        // The plan of the applied table must be all keep/share with nothing
        // to rewrite (design 2.2 "Idempotence").
        const session::placement_plan again = session::plan_placements(applied, session::MAX_NAME);
        std::printf("applied table replanned: %zu copies, %zu rewrites%s\n", again.copies(), again.rewrites(),
                    again.error.empty() ? "" : (" ERROR " + again.error).c_str());
        if (!plan.error.empty()) {
            std::printf("PLAN ERROR: %s\n", plan.error.c_str());
            result = 3;
        }
        std::printf("placement: level %d, %zu rows, %zu copies, %zu shared (%zu rewritten)%s\n", level, rows.size(),
                    plan.copies(), plan.shares(), plan.rewrites(), result ? " FAILED" : "");
        std::fflush(stdout);
        return result;
    }
}

#pragma once

// Per-world collectible state (step F4; docs/record-format-gaps-design.md
// "Decided (2026-10-29): PH[*][4] = option (b)" and "As built (step F4)").
//
// The user's rule (2026-10-29): collectibles - the P_Extra_* objects, extra
// points and extra lives - are per-world like the airship: outside the
// rollback and the physics sync, each player has their own copy.  The array
// column that says whether a collectible is still there (the sector
// manager's PH[*][4], "Set?") is therefore neither recorded nor rolled back:
// a replayed sector pass reads the live column, which is the right value for
// objects the replay never runs.
//
// The census (game/array_reads.hpp) grants a read the class "collectible"
// only when the level's own graph proves the rule: every in-play writer
// (every writer outside the level's loading) of the array the read sees is
//
//   - a `Set Cell` of one constant column C, the same C for all of them;
//   - in a root script outside the replay selection;
//   - either a P_Extra_* script writing its own row - the row index is the
//     Index output of an `Iterator If` on the same array whose operator is
//     `equal` and whose reference value is the script's owner, keyed on a
//     column other than C - or Event_handler's level reset (the row index is
//     an `Iterator` over every row of the same array);
//
// and at least one of them is a P_Extra_* script.  A map that breaks the
// rule keeps the read `unrecorded`, and the census logs why.
//
// This header holds the rule on plain descriptors (no CK call), so the unit
// tests can feed it the maps that break it (tests/collectible_rules_test.cpp);
// array_reads.hpp builds the descriptors from the level's graph.

#include <string>
#include <vector>

namespace bmmo::session {
    // Where a Set Cell's row index comes from.
    enum class collectible_row_source {
        other,              // anything below
        constant,           // a constant local
        iterator_all_rows,  // an Iterator over every row (start 0, end -1) of the same array
        iterator_if_owner,  // an Iterator If on the same array: operator equal, reference = the script's owner
    };

    inline const char* collectible_row_name(collectible_row_source s) {
        switch (s) {
            case collectible_row_source::constant: return "a constant";
            case collectible_row_source::iterator_all_rows: return "an Iterator over every row";
            case collectible_row_source::iterator_if_owner: return "an Iterator If on its owner";
            default: return "not proven";
        }
    }

    struct collectible_writer {
        std::string root;        // the writer's root script
        std::string prototype;   // "Set Cell", "Add Row", ...
        int column = -1;         // its constant column (-1: none, or a row-moving writer)
        bool load_time = false;  // Levelinit_* / *_Init: the level's loading, not in play
        bool selected = false;   // its root script is in the replay selection
        collectible_row_source row = collectible_row_source::other;
        int key_column = -1;     // iterator_if_owner: the Iterator If's key column
    };

    inline bool collectible_extra_script(const std::string& root) { return root.rfind("P_Extra_", 0) == 0; }
    inline bool collectible_reset_script(const std::string& root) { return root == "Event_handler"; }

    // The verdict on the writers of one read (the writers of the column it
    // reads, or of the whole array for a whole-row read): "" when the read is
    // per-world collectible state (`column` = the column the writers write),
    // otherwise why not.
    inline std::string collectible_verdict(const std::vector<collectible_writer>& writers, int& column) {
        column = -1;
        bool extra = false, in_play = false;
        for (const collectible_writer& w: writers) {
            if (w.load_time) continue;
            in_play = true;
            const std::string who = w.root + " [" + w.prototype + "] col "
                                    + (w.column < 0 ? std::string("*") : std::to_string(w.column));
            if (w.prototype != "Set Cell" || w.column < 0) return who + " is not a Set Cell of one constant column";
            if (column < 0) column = w.column;
            else if (w.column != column)
                return "in-play writers write columns " + std::to_string(column) + " and " + std::to_string(w.column);
            if (w.selected) return who + " is in the replay selection";
            if (collectible_extra_script(w.root)) {
                if (w.row != collectible_row_source::iterator_if_owner || w.key_column < 0 || w.key_column == w.column)
                    return who + " does not write its own row (row index: " + collectible_row_name(w.row) + ")";
                extra = true;
            } else if (collectible_reset_script(w.root)) {
                if (w.row != collectible_row_source::iterator_all_rows)
                    return who + " is not the level reset (row index: " + collectible_row_name(w.row) + ")";
            } else {
                return who + " is neither a P_Extra_* script's own row nor Event_handler's level reset";
            }
        }
        if (!in_play) return "no in-play writer";
        if (!extra) return "no P_Extra_* writer";
        return std::string();
    }
}

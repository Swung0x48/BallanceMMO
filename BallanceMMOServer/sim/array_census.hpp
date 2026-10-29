#pragma once

// SimTool --array-reads (docs/record-format-gaps-design.md 2.2 and 7.2 S2):
// the array-read census of a world at its anchor, read-only.  Built here at
// step K0; since step RK2 the census is the shared game/array_reads.hpp,
// which every world runs at its anchor (the server, the headless client, the
// retail core host) and which decides the record's written-cell section.
// This file keeps SimTool's name for it and prints its report.

#include <cstdio>
#include <string>

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
    };
}

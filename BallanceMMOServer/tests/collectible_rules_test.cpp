// Step F4 (docs/record-format-gaps-design.md "As built (step F4)"): the
// per-world collectible rule, session/collectible_rules.hpp.  The positive
// case is the writer set the census finds for the sector manager's PH read on
// L1-L13 and the arena; the negative cases are the maps that break the rule
// (no level does: these are its negative fixture beside SimTool's
// --collectible-fixture).

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <session/collectible_rules.hpp>

namespace {
    using bmmo::session::collectible_row_source;
    using bmmo::session::collectible_verdict;
    using bmmo::session::collectible_writer;

    collectible_writer make(const std::string& root, const std::string& prototype, int column, bool load_time,
                            collectible_row_source row = collectible_row_source::other, int key = -1) {
        collectible_writer w;
        w.root = root;
        w.prototype = prototype;
        w.column = column;
        w.load_time = load_time;
        w.row = row;
        w.key_column = key;
        return w;
    }

    // PH's writers on L2: the level's loading, the level reset, the extra points.
    std::vector<collectible_writer> ph_writers() {
        std::vector<collectible_writer> out = {
            make("Levelinit_build", "Add Row", -1, true),
            make("Levelinit_build", "Restore IC", -1, true),
            make("Levelinit_build", "Set Cell", 3, true),
            make("Levelinit_build", "Sort Rows", -1, true),
            make("Event_handler", "Set Cell", 4, false, collectible_row_source::iterator_all_rows),
        };
        for (int i = 0; i < 4; ++i)
            out.push_back(make("P_Extra_Point_MF Script", "Set Cell", 4, false, collectible_row_source::iterator_if_owner, 3));
        return out;
    }

    TEST(CollectibleRules, PhReadIsCollectible) {
        int column = -1;
        EXPECT_EQ(collectible_verdict(ph_writers(), column), "");
        EXPECT_EQ(column, 4);
    }

    TEST(CollectibleRules, ExtraLivesCountToo) {
        auto w = ph_writers();
        w.push_back(make("P_Extra_Life_MF Script", "Set Cell", 4, false, collectible_row_source::iterator_if_owner, 3));
        int column = -1;
        EXPECT_EQ(collectible_verdict(w, column), "");
    }

    TEST(CollectibleRules, NonExtraWriterBreaksIt) {
        auto w = ph_writers();
        w.push_back(make("P_Modul_18_MF Script", "Set Cell", 4, false, collectible_row_source::iterator_if_owner, 3));
        int column = -1;
        const std::string why = collectible_verdict(w, column);
        EXPECT_NE(why.find("P_Modul_18_MF Script [Set Cell] col 4 is neither"), std::string::npos) << why;
    }

    TEST(CollectibleRules, ExtraWriterOfAnotherRowBreaksIt) {
        for (const collectible_row_source row: {collectible_row_source::constant, collectible_row_source::other,
                                                collectible_row_source::iterator_all_rows}) {
            auto w = ph_writers();
            w.back().row = row;
            int column = -1;
            const std::string why = collectible_verdict(w, column);
            EXPECT_NE(why.find("does not write its own row"), std::string::npos) << why;
        }
        auto w = ph_writers();
        w.back().key_column = 4;   // keyed on the column it writes
        int column = -1;
        EXPECT_NE(collectible_verdict(w, column).find("does not write its own row"), std::string::npos);
    }

    TEST(CollectibleRules, AnotherColumnInPlayBreaksIt) {
        auto w = ph_writers();
        w.push_back(make("P_Extra_Point_MF Script", "Set Cell", 2, false, collectible_row_source::iterator_if_owner, 3));
        int column = -1;
        EXPECT_NE(collectible_verdict(w, column).find("write columns 4 and 2"), std::string::npos);
        auto m = ph_writers();
        m.push_back(make("P_Extra_Point_MF Script", "Sort Rows", -1, false));
        EXPECT_NE(collectible_verdict(m, column).find("is not a Set Cell of one constant column"), std::string::npos);
    }

    TEST(CollectibleRules, SelectedWriterBreaksIt) {
        auto w = ph_writers();
        w.back().selected = true;
        int column = -1;
        EXPECT_NE(collectible_verdict(w, column).find("is in the replay selection"), std::string::npos);
    }

    TEST(CollectibleRules, ResetMustCoverEveryRow) {
        auto w = ph_writers();
        w[4].row = collectible_row_source::constant;
        int column = -1;
        EXPECT_NE(collectible_verdict(w, column).find("is not the level reset"), std::string::npos);
    }

    TEST(CollectibleRules, NeedsAnExtraWriter) {
        std::vector<collectible_writer> w;
        for (const auto& x: ph_writers())
            if (x.root.rfind("P_Extra_", 0) != 0) w.push_back(x);
        int column = -1;
        EXPECT_EQ(collectible_verdict(w, column), "no P_Extra_* writer");
        std::vector<collectible_writer> load_only = {make("Levelinit_build", "Set Cell", 3, true)};
        EXPECT_EQ(collectible_verdict(load_only, column), "no in-play writer");
    }
}

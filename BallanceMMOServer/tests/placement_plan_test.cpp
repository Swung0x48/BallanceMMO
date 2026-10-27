// Unit tests for the sector placement's row plan (docs/sector-placement-
// design.md 2.2 and 7.1, step S1): keep / share / copy / none per PH row, the
// same-pose rule (bitwise), the name rules, the plan of the applied table and
// the signature's order-freedom.  The level rows (L1, L8, the arena) are the
// PH tables printed by SimTool --list-placement at the anchor
// (placement_plan_rows.inc, generated from those listings).
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <entity/session.hpp>
#include <session/placement_plan.hpp>

namespace {

using namespace bmmo::session;
using kind = placement_action::kind;

constexpr size_t kMaxName = bmmo::session::MAX_NAME;   // the wire limit the copy names must fit

placement_row make_row(int row, int sector, const std::string& object, float x, const std::string& type = "P_Test") {
    placement_row r;
    r.row = row;
    r.sector = sector;
    r.type = type;
    r.object = object;
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 2.5f, -3.0f, 1};
    std::memcpy(r.matrix, m, sizeof(m));
    return r;
}

struct level_row { int row, sector; const char* type; const char* object; uint32_t words[16]; };

std::vector<placement_row> to_rows(const level_row* data, size_t n) {
    std::vector<placement_row> rows;
    for (size_t i = 0; i < n; ++i) {
        placement_row r;
        r.row = data[i].row;
        r.sector = data[i].sector;
        r.type = data[i].type;
        r.object = data[i].object;
        std::memcpy(r.matrix, data[i].words, sizeof(r.matrix));
        rows.push_back(std::move(r));
    }
    return rows;
}

#include "placement_plan_rows.inc"

TEST(PlacementPlan, FirstRowKeepsLaterPoseCopiesSamePoseShares) {
    // A(p), B(q), C(p): C shares A's placement, the original.
    const std::vector<placement_row> rows = {make_row(0, 1, "X_MF", 1.0f), make_row(1, 2, "X_MF", 2.0f),
                                             make_row(2, 3, "X_MF", 1.0f)};
    const placement_plan plan = plan_placements(rows, kMaxName);
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    ASSERT_EQ(3u, plan.actions.size());
    EXPECT_EQ(kind::keep, plan.actions[0].what);
    EXPECT_EQ("X_MF", plan.actions[0].name);
    EXPECT_EQ(kind::copy, plan.actions[1].what);
    EXPECT_EQ("X_MF", plan.actions[1].source);
    EXPECT_EQ("X_MF_R1", plan.actions[1].name);
    EXPECT_EQ(kind::share, plan.actions[2].what);
    EXPECT_EQ(0, plan.actions[2].holder_row);
    EXPECT_EQ("X_MF", plan.actions[2].name);
    EXPECT_EQ(1u, plan.copies());
    EXPECT_EQ(1u, plan.shares());
    EXPECT_EQ(0u, plan.rewrites());
}

TEST(PlacementPlan, SamePoseAsALaterCopySharesThatCopy) {
    // A(p), B(q), C(q): C shares B's copy and is rewritten onto it.
    const std::vector<placement_row> rows = {make_row(0, 1, "X_MF", 1.0f), make_row(1, 2, "X_MF", 2.0f),
                                             make_row(2, 3, "X_MF", 2.0f)};
    const placement_plan plan = plan_placements(rows, kMaxName);
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    EXPECT_EQ(kind::share, plan.actions[2].what);
    EXPECT_EQ(1, plan.actions[2].holder_row);
    EXPECT_EQ("X_MF", plan.actions[2].source);
    EXPECT_EQ("X_MF_R1", plan.actions[2].name);
    EXPECT_EQ(1u, plan.rewrites());
}

TEST(PlacementPlan, SamePoseIsBitwiseNegativeZeroIsACopy) {
    std::vector<placement_row> rows = {make_row(0, 1, "X_MF", 0.0f), make_row(1, 2, "X_MF", 0.0f)};
    const float negative_zero = -0.0f;
    std::memcpy(&rows[1].matrix[12], &negative_zero, sizeof(float));
    ASSERT_NE(rows[0].matrix[12], rows[1].matrix[12]);
    const placement_plan plan = plan_placements(rows, kMaxName);
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    EXPECT_EQ(kind::copy, plan.actions[1].what);
    EXPECT_EQ("X_MF_R1", plan.actions[1].name);
}

TEST(PlacementPlan, RowsWithoutAnObjectAreNone) {
    const std::vector<placement_row> rows = {make_row(0, 1, "", 1.0f), make_row(1, 2, "", 2.0f)};
    const placement_plan plan = plan_placements(rows, kMaxName);
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    EXPECT_EQ(kind::none, plan.actions[0].what);
    EXPECT_EQ(kind::none, plan.actions[1].what);
    EXPECT_EQ(0u, plan.copies());
}

TEST(PlacementPlan, NameLengthRuleIsTheWires) {
    // Row 7's suffix is "_R7" (3 characters).
    const std::string base61(kMaxName - 3, 'a'), base62(kMaxName - 2, 'a');
    {
        const std::vector<placement_row> rows = {make_row(0, 1, base61, 1.0f), make_row(7, 2, base61, 2.0f)};
        const placement_plan plan = plan_placements(rows, kMaxName);
        EXPECT_TRUE(plan.error.empty()) << plan.error;
        EXPECT_EQ(kMaxName, plan.actions[1].name.size());
    }
    {
        const std::vector<placement_row> rows = {make_row(0, 1, base62, 1.0f), make_row(7, 2, base62, 2.0f)};
        const placement_plan plan = plan_placements(rows, kMaxName);
        EXPECT_FALSE(plan.error.empty());
        EXPECT_EQ(kMaxName + 1, plan.actions[1].name.size());
    }
}

// Step S5 (S2's open bullet a): every host passes the plan the bound step 5
// enforces, game::kPlacementMaxName = min(MAX_NAME, BMMO_PHYSICS_NAME_SIZE -
// 1) = 63, so a 64-character copy root is refused by the CK-free plan
// already instead of passing it and failing the anchor.  (This test does not
// include the CK header that defines the constant; the value is spelled.)
TEST(PlacementPlan, TheHostsBoundIs63) {
    constexpr size_t kHostBound = 63;
    static_assert(kHostBound < kMaxName, "the bridge's body name is the tighter bound");
    const std::string base60(kHostBound - 3, 'a'), base61(kHostBound - 2, 'a');
    {
        const std::vector<placement_row> rows = {make_row(0, 1, base60, 1.0f), make_row(7, 2, base60, 2.0f)};
        const placement_plan plan = plan_placements(rows, kHostBound);
        EXPECT_TRUE(plan.error.empty()) << plan.error;
        EXPECT_EQ(kHostBound, plan.actions[1].name.size());
    }
    {
        const std::vector<placement_row> rows = {make_row(0, 1, base61, 1.0f), make_row(7, 2, base61, 2.0f)};
        EXPECT_FALSE(plan_placements(rows, kHostBound).error.empty()) << "64 characters: refused by the plan";
        EXPECT_TRUE(plan_placements(rows, kMaxName).error.empty()) << "MAX_NAME alone let it through";
    }
}

TEST(PlacementPlan, ACopyNameTheTableAlreadyHoldsIsAnError) {
    const std::vector<placement_row> rows = {make_row(0, 1, "X_MF", 1.0f), make_row(1, 1, "X_MF_R2", 5.0f),
                                             make_row(2, 2, "X_MF", 2.0f)};
    const placement_plan plan = plan_placements(rows, kMaxName);
    EXPECT_NE(std::string::npos, plan.error.find("X_MF_R2")) << plan.error;
}

TEST(PlacementPlan, TheAppliedTablePlansToKeepAndShareOnly) {
    const std::vector<placement_row> rows = {make_row(0, 1, "X_MF", 1.0f), make_row(1, 2, "X_MF", 2.0f),
                                             make_row(2, 3, "X_MF", 2.0f), make_row(3, 3, "Y_MF", 4.0f),
                                             make_row(4, 4, "Y_MF", 4.0f), make_row(5, 4, "X_MF", 3.0f)};
    const placement_plan plan = plan_placements(rows, kMaxName);
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    const std::vector<placement_row> applied = placement_applied_rows(rows, plan);
    EXPECT_EQ("X_MF_R1", applied[2].object);
    EXPECT_EQ("X_MF_R5", applied[5].object);
    const placement_plan again = plan_placements(applied, kMaxName);
    ASSERT_TRUE(again.error.empty()) << again.error;
    EXPECT_EQ(0u, again.copies());
    EXPECT_EQ(0u, again.rewrites());
    for (const placement_action& a: again.actions) {
        EXPECT_TRUE(a.what == kind::keep || a.what == kind::share);
        EXPECT_EQ(a.source, a.name);
    }
}

TEST(PlacementPlan, TheSignatureIgnoresTheOrderOfTheEntityPrints) {
    const std::vector<placement_row> rows = {make_row(0, 1, "X_MF", 1.0f), make_row(1, 2, "X_MF_R1", 2.0f)};
    entity_print root{"X_MF", "", 33, 1, {"Sector", "PH"}};
    entity_print child{"X_Body", "X_Mesh", 33, 0, {"Physicalize"}};
    entity_print root_swapped{"X_MF", "", 33, 1, {"PH", "Sector"}};
    const uint64_t a = placement_signature(rows, {{root, child}, {}});
    const uint64_t b = placement_signature(rows, {{child, root_swapped}, {}});
    EXPECT_EQ(a, b);
    // Any field of a print, a name or a matrix word changes it.
    entity_print other_mesh = child;
    other_mesh.mesh = "Y_Mesh";
    EXPECT_NE(a, placement_signature(rows, {{root, other_mesh}, {}}));
    std::vector<placement_row> moved = rows;
    moved[1].matrix[13] ^= 1u;
    EXPECT_NE(a, placement_signature(moved, {{root, child}, {}}));
    std::vector<placement_row> renamed = rows;
    renamed[1].object = "X_MF";
    EXPECT_NE(a, placement_signature(renamed, {{root, child}, {}}));
}

// The levels (V1's tables, design 1.4).

TEST(PlacementPlanLevels, Level8) {
    const std::vector<placement_row> rows = to_rows(kLevel8Rows, sizeof(kLevel8Rows) / sizeof(kLevel8Rows[0]));
    ASSERT_EQ(86u, rows.size());
    const placement_plan plan = plan_placements(rows, kMaxName);
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    EXPECT_EQ(39u, plan.copies());
    ASSERT_EQ(1u, plan.shares());
    EXPECT_EQ(kind::share, plan.actions[52].what);
    EXPECT_EQ(41, plan.actions[52].holder_row);
    EXPECT_EQ(plan.actions[52].source, plan.actions[52].name);
    EXPECT_EQ(0u, plan.rewrites());
    EXPECT_EQ(kind::copy, plan.actions[15].what);
    EXPECT_EQ("P_Modul_30_MF_R15", plan.actions[15].name);
    const placement_plan again = plan_placements(placement_applied_rows(rows, plan), kMaxName);
    ASSERT_TRUE(again.error.empty()) << again.error;
    EXPECT_EQ(0u, again.copies());
    EXPECT_EQ(0u, again.rewrites());
}

TEST(PlacementPlanLevels, Level1) {
    const std::vector<placement_row> rows = to_rows(kLevel1Rows, sizeof(kLevel1Rows) / sizeof(kLevel1Rows[0]));
    ASSERT_EQ(51u, rows.size());
    const placement_plan plan = plan_placements(rows, kMaxName);
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    EXPECT_EQ(23u, plan.copies());
    EXPECT_EQ(kind::copy, plan.actions[37].what);
    EXPECT_EQ("P_Trafo_Wood_MF", plan.actions[37].source);
    EXPECT_EQ("P_Trafo_Wood_MF_R37", plan.actions[37].name);
    EXPECT_EQ(kind::share, plan.actions[38].what);
    EXPECT_EQ(37, plan.actions[38].holder_row);
    EXPECT_EQ("P_Trafo_Wood_MF", plan.actions[38].source);
    EXPECT_EQ("P_Trafo_Wood_MF_R37", plan.actions[38].name);
    EXPECT_EQ(1u, plan.rewrites());
    const std::vector<placement_row> applied = placement_applied_rows(rows, plan);
    EXPECT_EQ("P_Trafo_Wood_MF_R37", applied[38].object);
    const placement_plan again = plan_placements(applied, kMaxName);
    ASSERT_TRUE(again.error.empty()) << again.error;
    EXPECT_EQ(0u, again.copies());
    EXPECT_EQ(0u, again.rewrites());
}

TEST(PlacementPlanLevels, ArenaHasNothingPooled) {
    const std::vector<placement_row> rows = to_rows(kArenaRows, sizeof(kArenaRows) / sizeof(kArenaRows[0]));
    ASSERT_EQ(25u, rows.size());
    const placement_plan plan = plan_placements(rows, kMaxName);
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    EXPECT_EQ(0u, plan.copies());
    EXPECT_EQ(0u, plan.shares());
}

}  // namespace

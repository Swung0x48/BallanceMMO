// Step OPB (rewind-cost-design R.4 row B, option B1): the written-cell
// columns' memo (game/written_cells_memo.hpp), driven with a fake world: a few
// writer blocks whose pins name arrays, and arrays with a column layout.  The
// CK side (array_reads.hpp cells_key / watch_of) builds the same three key
// parts from the real graph.

#include <game/written_cells_memo.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {
    using bmmo::game::cells_memo_key;
    using bmmo::game::cells_memo_mode;

    struct cell_col {
        uint32_t array = 0;
        int column = -1;
        bool operator==(const cell_col& o) const { return array == o.array && column == o.column; }
    };

    // A fake level: blocks (each writes `column` of the array its pin names)
    // and arrays (column types; a type 1 column is an int one).
    struct fake_world {
        std::vector<uint32_t> scripts{10, 11};
        struct block {
            uint32_t id = 0;
            uint32_t pin_array = 0;   // the array the target pin resolves to now
            int column = 0;
        };
        std::vector<block> blocks{{100, 1, 0}, {101, 2, 1}};
        struct array {
            uint32_t id = 0;
            std::vector<int> types;   // 1: int, 2: string
            std::string name;
        };
        std::vector<array> arrays{{1, {1, 2}, "A"}, {2, {1, 1}, "B"}, {3, {1, 1}, "C"}};
        // Hidden state the probe does not see (the forced stale case).
        bool hidden_extra = false;

        const array* find(uint32_t id) const {
            for (const array& a: arrays)
                if (a.id == id) return &a;
            return nullptr;
        }
        std::vector<cell_col> walk() const {
            std::vector<cell_col> out;
            for (const block& b: blocks) {
                const array* a = find(b.pin_array);
                if (!a || b.column >= static_cast<int>(a->types.size()) || a->types[b.column] != 1) continue;
                out.push_back({a->id, b.column});
            }
            if (hidden_extra) out.push_back({99, 0});
            return out;
        }
    };

    struct watch {
        std::vector<uint32_t> blocks, arrays;
        size_t count_blocks = 0, count_pins = 0, count_dynamic = 0, count_arrays = 0;
    };
    using memo_t = bmmo::game::written_cells_memo<std::vector<cell_col>, watch>;

    bool key_of(const fake_world& w, const watch& at, cells_memo_key& key) {
        key.clear();
        for (uint32_t s: w.scripts) key.scripts.push_back(s);
        for (uint32_t id: at.blocks) {
            const fake_world::block* b = nullptr;
            for (const auto& x: w.blocks)
                if (x.id == id) b = &x;
            if (!b) return false;
            key.pins.push_back(id);
            key.pins.push_back(b->pin_array);
            key.pins.push_back(static_cast<uint32_t>(b->column));
        }
        for (uint32_t id: at.arrays) {
            key.layout.push_back(id);
            const fake_world::array* a = w.find(id);
            if (!a) continue;
            key.layout.push_back(a->types.size());
            for (int t: a->types) key.layout.push_back(static_cast<uint32_t>(t));
            key.names.push_back(a->name);
        }
        return true;
    }

    struct driver {
        fake_world world;
        int walks = 0;
        std::vector<cell_col> get(memo_t& memo) {
            const auto probe = [this](const watch& at, cells_memo_key& key) { return key_of(world, at, key); };
            const auto walk = [this](watch* at, cells_memo_key* key) {
                ++walks;
                std::vector<cell_col> out = world.walk();
                if (at && key) {
                    for (const auto& b: world.blocks) {
                        at->blocks.push_back(b.id);
                        at->arrays.push_back(b.pin_array);
                    }
                    at->count_blocks = at->blocks.size();
                    at->count_pins = at->blocks.size();
                    at->count_dynamic = 1;
                    at->count_arrays = at->arrays.size();
                    key_of(world, *at, *key);
                }
                return out;
            };
            const auto describe = [](const std::vector<cell_col>& v) { return std::to_string(v.size()) + " columns"; };
            return memo.get(probe, walk, describe);
        }
    };
}

TEST(WrittenCellsMemo, HitWalksNothing) {
    memo_t memo(cells_memo_mode::on);
    driver d;
    const std::vector<cell_col> first = d.get(memo);
    EXPECT_EQ(d.walks, 1);
    EXPECT_EQ(first, d.world.walk());
    for (int i = 0; i < 5; ++i) EXPECT_EQ(d.get(memo), first);
    EXPECT_EQ(d.walks, 1);
    EXPECT_EQ(memo.stats().hits, 5u);
    EXPECT_EQ(memo.stats().first, 1u);
    EXPECT_EQ(memo.stats().blocks, 2u);
    EXPECT_EQ(memo.stats().dynamic_pins, 1u);
    EXPECT_EQ(memo.stats().columns, first.size());
}

TEST(WrittenCellsMemo, RepointedPinWalksAgain) {
    memo_t memo(cells_memo_mode::on);
    driver d;
    d.get(memo);
    d.world.blocks[0].pin_array = 3;   // a script (or the host's rewire) repoints the pin to C
    const std::vector<cell_col> after = d.get(memo);
    EXPECT_EQ(d.walks, 2);
    EXPECT_EQ(after, d.world.walk());
    EXPECT_EQ(after.front().array, 3u);
    EXPECT_EQ(memo.stats().changed_pins, 1u);
    // ... and the new key holds: the next bind is a hit.
    d.get(memo);
    EXPECT_EQ(d.walks, 2);
}

TEST(WrittenCellsMemo, ChangedColumnLayoutWalksAgain) {
    memo_t memo(cells_memo_mode::on);
    driver d;
    const std::vector<cell_col> before = d.get(memo);
    ASSERT_EQ(before.size(), 2u);
    d.world.arrays[1].types[1] = 2;   // B's column 1 becomes a string column: out of the section
    const std::vector<cell_col> after = d.get(memo);
    EXPECT_EQ(d.walks, 2);
    EXPECT_EQ(after.size(), 1u);
    EXPECT_EQ(memo.stats().changed_layout, 1u);
    d.world.arrays[1].types.push_back(1);   // a column added
    d.get(memo);
    EXPECT_EQ(d.walks, 3);
    d.world.arrays[0].name = "A2";   // a renamed array (the census classifies by name)
    d.get(memo);
    EXPECT_EQ(d.walks, 4);
    EXPECT_EQ(memo.stats().changed_layout, 3u);
}

TEST(WrittenCellsMemo, ScriptSetAndGoneBlockWalkAgain) {
    memo_t memo(cells_memo_mode::on);
    driver d;
    d.get(memo);
    d.world.scripts.push_back(12);   // a copy's script born
    d.get(memo);
    EXPECT_EQ(d.walks, 2);
    EXPECT_EQ(memo.stats().changed_scripts, 1u);
    d.world.blocks.pop_back();   // a watched block gone: the probe cannot resolve
    d.get(memo);
    EXPECT_EQ(d.walks, 3);
    EXPECT_EQ(memo.stats().changed_scripts, 2u);
}

TEST(WrittenCellsMemo, VerifyCatchesForcedStaleMemo) {
    memo_t memo(cells_memo_mode::verify);
    driver d;
    const std::vector<cell_col> first = d.get(memo);
    EXPECT_EQ(d.get(memo), first);   // verify: a hit walks again and agrees
    EXPECT_EQ(memo.stats().verified, 1u);
    EXPECT_EQ(memo.stats().mismatches, 0u);
    // A change the key cannot see: the memo is stale.
    d.world.hidden_extra = true;
    const std::vector<cell_col> returned = d.get(memo);
    EXPECT_EQ(returned, first);   // the memo's columns are what the record carries
    EXPECT_EQ(memo.stats().mismatches, 1u);
    ASSERT_EQ(memo.notes().size(), 1u);
    EXPECT_NE(memo.notes()[0].find("mismatch"), std::string::npos);
    // The memo was dropped: the next bind walks and takes the fresh columns.
    EXPECT_EQ(d.get(memo), d.world.walk());
    EXPECT_EQ(memo.stats().first, 2u);
    // A forced stale value is caught the same way.
    memo.force_value({{42, 0}});
    const std::vector<cell_col> stale = d.get(memo);
    EXPECT_EQ(stale.size(), 1u);
    EXPECT_EQ(memo.stats().mismatches, 2u);
    // Mode on keeps a forced value (nothing compares).
    memo_t on(cells_memo_mode::on);
    driver e;
    e.get(on);
    on.force_value({{42, 0}});
    EXPECT_EQ(e.get(on).size(), 1u);
    EXPECT_EQ(on.stats().mismatches, 0u);
}

TEST(WrittenCellsMemo, OffWalksEveryBindAndDropForgets) {
    memo_t off(cells_memo_mode::off);
    driver d;
    for (int i = 0; i < 3; ++i) d.get(off);
    EXPECT_EQ(d.walks, 3);
    EXPECT_EQ(off.stats().hits, 0u);
    memo_t on(cells_memo_mode::on);
    driver e;
    e.get(on);
    on.drop();
    EXPECT_EQ(on.stats().drops, 1u);
    e.get(on);
    EXPECT_EQ(e.walks, 2);
    on.drop();
    on.drop();   // an empty memo's drop is not counted
    EXPECT_EQ(on.stats().drops, 2u);
}

TEST(WrittenCellsMemo, ModeParser) {
    using bmmo::game::parse_cells_memo_mode;
    EXPECT_EQ(parse_cells_memo_mode(nullptr, cells_memo_mode::on), cells_memo_mode::on);
    EXPECT_EQ(parse_cells_memo_mode("off", cells_memo_mode::on), cells_memo_mode::off);
    EXPECT_EQ(parse_cells_memo_mode("0", cells_memo_mode::on), cells_memo_mode::off);
    EXPECT_EQ(parse_cells_memo_mode("verify", cells_memo_mode::on), cells_memo_mode::verify);
    EXPECT_EQ(parse_cells_memo_mode("on", cells_memo_mode::off), cells_memo_mode::on);
    EXPECT_EQ(parse_cells_memo_mode("bogus", cells_memo_mode::verify), cells_memo_mode::verify);
}

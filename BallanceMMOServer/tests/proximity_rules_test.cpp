// U1 (docs/world-agreement-gaps-design.md §7.1, step G1): the proximity
// union's sensor rules, session/proximity_rules.hpp.  The float results are
// pinned as hex constants; the same vectors give the same constants on the
// x86 MSVC and GCC builds (.local-tools/work/WG1/u1_gen.cpp prints them).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <session/proximity_rules.hpp>

namespace {
    using bmmo::session::probe_ball;
    using bmmo::session::probe_metric;
    using bmmo::session::probe_nearest;

    uint32_t bits(float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    probe_ball ball(uint32_t player, float x, float y, float z) {
        probe_ball b;
        b.player = player;
        b.position[0] = x;
        b.position[1] = y;
        b.position[2] = z;
        return b;
    }

    const float kRef[3] = {12.345f, -3.21f, 7.89f};
    const float kA[3] = {52.3456f, -63.2109f, 7.8899f};
    const float kB[3] = {-42.6543f, -3.2101f, 7.8901f};
    const float kC[3] = {0.1f, 0.2f, 0.3f};
}

TEST(ProximityRules, MetricPinnedBits) {
    struct row { const float* p; uint8_t axes; uint32_t expected; };
    const row rows[] = {
        {kA, 1, 0x44c80189u}, {kA, 2, 0x456101bbu}, {kA, 4, 0x322aa100u}, {kA, 5, 0x44c80189u}, {kA, 7, 0x45a28140u},
        {kB, 1, 0x453d0ec5u}, {kB, 2, 0x322b7240u}, {kB, 4, 0x322c4400u}, {kB, 5, 0x453d0ec5u}, {kB, 7, 0x453d0ec5u},
        {kC, 1, 0x4315f0a5u}, {kC, 2, 0x413a0cb3u}, {kC, 4, 0x42666eb0u}, {kC, 5, 0x434f8c51u}, {kC, 7, 0x435b2d1cu},
    };
    for (const row& r: rows) {
        EXPECT_EQ(bits(probe_metric(r.p, kRef, r.axes)), r.expected) << "axes " << int(r.axes);
        // The square makes the operands' order irrelevant to the bits.
        EXPECT_EQ(bits(probe_metric(kRef, r.p, r.axes)), r.expected) << "axes " << int(r.axes);
    }
    EXPECT_EQ(probe_metric(kA, kRef, 0), 0.0f);
}

TEST(ProximityRules, NearestUsesTheBlocksAxes) {
    // G-c5: A at XZ 40 m and 60 m below a block measuring X and Z; B at XZ
    // 55 m at the block's height.  Axes 5 picks A (in range of a 50 m block),
    // axes 7 picks B (3D 55 m against 72 m).
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const std::vector<probe_ball> balls{ball(1, 40.0f, -60.0f, 0.0f), ball(2, 55.0f, 0.0f, 0.0f)};
    EXPECT_EQ(probe_nearest(balls, origin, 5), 0);
    EXPECT_EQ(probe_nearest(balls, origin, 7), 1);
    const std::vector<probe_ball> pinned{ball(7, kA[0], kA[1], kA[2]), ball(9, kB[0], kB[1], kB[2])};
    EXPECT_EQ(probe_nearest(pinned, kRef, 5), 0);
    EXPECT_EQ(probe_nearest(pinned, kRef, 7), 1);
}

TEST(ProximityRules, TieTakesTheFirstAndEmptyIsMinusOne) {
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    // Equal metrics (25 each): the first, i.e. the lower player id.
    const std::vector<probe_ball> tie{ball(3, 5.0f, 0.0f, 0.0f), ball(4, 0.0f, 0.0f, -5.0f)};
    EXPECT_EQ(probe_nearest(tie, origin, 5), 0);
    EXPECT_EQ(probe_nearest(tie, origin, 7), 0);
    EXPECT_EQ(probe_nearest({}, origin, 7), -1);
    // A strictly nearer later ball wins.
    const std::vector<probe_ball> later{ball(3, 5.0f, 0.0f, 0.0f), ball(4, 0.0f, 0.0f, -4.999f)};
    EXPECT_EQ(probe_nearest(later, origin, 5), 1);
}

// ---------------------------------------------------------------------------
// U8 (§7.1, step Q7): the unit rule (session/mechanism_units.hpp) on
// abstract-graph fixtures transcribed from L11's descriptions
// (.local-tools/work/WAD/cons/l11_P_Modul_18_MF_Script.txt and
// ..._29_...): the fan is the graph unit Physics Force with S = the two
// permission locals, the bridge the block unit {probe, Get Cell, Test} with
// its Get Name operation; a third shape is refused with the blocks named.
// U9's CK-free parts: slot binding and the join-order cap.

#include <session/mechanism_units.hpp>
#include <session/member_lives.hpp>

namespace {
    namespace mu = bmmo::session::units;

    struct fixture {
        mu::graph_model m;
        int next_walk = 0;
        void block(uint32_t id, const char* proto, uint32_t parent, std::vector<uint32_t> inputs = {},
                   std::vector<uint32_t> outputs = {}, std::vector<uint32_t> locals = {}, uint32_t target = 0) {
            mu::block b;
            b.id = id;
            b.prototype = proto;
            b.parent = parent;
            b.inputs = std::move(inputs);
            b.outputs = std::move(outputs);
            b.locals = std::move(locals);
            b.target = target;
            b.walk = next_walk++;
            if (parent) m.blocks[parent].children.push_back(id);
            for (uint32_t p: b.inputs) m.params[p].id = p;
            for (uint32_t p: b.outputs) m.params[p].id = p;
            for (uint32_t p: b.locals) m.params[p].id = p;
            if (target) m.params[target].id = target;
            m.blocks[id] = b;
        }
        void source(uint32_t in, uint32_t src) { m.params[in].id = in; m.params[in].source = src; }
        void writes(uint32_t out, uint32_t dst) { m.params[out].destinations.push_back(dst); }
        void link(uint32_t id, uint32_t graph, uint32_t from, bool from_in, int from_index, uint32_t to, int to_index,
                  int delay = 1) {
            mu::link l;
            l.id = id;
            l.graph = graph;
            l.from = {from, from_in, from_index};
            l.to = {to, true, to_index};
            l.delay = delay;
            m.links.push_back(l);
        }
    };

    // P_Modul_18_MF Script (L11): root 1; Physics Force #13 with its eight
    // blocks (7 m probe 27, Get Cell 21, Box Box Intersection 22,
    // SetPhysicsForce 26, Binary Switch 23 / 25, Identity 20 / 24) and the
    // locals TurnON_Permission 100, TurnOFF_Permission 101, a constant 102.
    fixture fan() {
        fixture f;
        f.block(1, "", 0, {}, {}, {});                       // the script
        f.block(10, "Binary Switch", 1, {300});
        f.block(11, "Activate Script", 1);
        f.block(12, "TT Scaleable Proximity", 1, {301});     // the 80 m probe (not in the unit)
        f.block(13, "", 1, {}, {}, {100, 101, 102});         // Physics Force
        f.block(20, "Identity", 13, {}, {206});
        f.block(21, "Get Cell", 13, {}, {200});
        f.block(22, "Box Box Intersection", 13, {201, 202});
        f.block(23, "Binary Switch", 13, {208});
        f.block(24, "Identity", 13, {}, {207});
        f.block(25, "Binary Switch", 13, {209});
        f.block(26, "SetPhysicsForce", 13, {203, 204}, {}, {}, 205);
        f.block(27, "TT Scaleable Proximity", 13, {210, 211});
        f.block(15, "Deactivate Script", 1);
        f.block(17, "Deactivate Script", 1);
        f.source(201, 200);          // Box Box Intersection's Entity 0 <- the read
        f.source(202, 900);          // its Entity 1: the Kollisionsquader (shared)
        f.source(204, 200);          // SetPhysicsForce's Pos Referential <- the read
        f.source(205, 200);          // and its target
        f.source(203, 901);          // a shared force vector
        f.writes(206, 101);          // the Identity blocks write the permissions
        f.writes(207, 100);
        f.source(208, 101);          // the switches read them
        f.source(209, 100);
        f.source(210, 102);          // the probe's distance: a graph constant
        f.source(211, 999);          // ObjectA: Ball_Pos_Frame (shared)
        // Physics Force's own links (12).
        f.link(1001, 13, 27, false, 1, 21, 0);
        f.link(1002, 13, 27, false, 0, 22, 0);
        f.link(1003, 13, 13, true, 1, 27, 1);
        f.link(1004, 13, 23, false, 0, 20, 0);
        f.link(1005, 13, 25, false, 0, 24, 0);
        f.link(1006, 13, 22, false, 0, 25, 0);
        f.link(1007, 13, 22, false, 1, 23, 0);
        f.link(1008, 13, 24, false, 0, 26, 0);
        f.link(1009, 13, 20, false, 0, 26, 1);
        f.link(1010, 13, 13, true, 2, 23, 0);
        f.link(1011, 13, 13, true, 0, 27, 0);
        f.link(1012, 13, 13, true, 0, 20, 0);
        // The script's links into Physics Force (fan-out) and one elsewhere.
        f.link(1101, 1, 11, false, 0, 13, 0);
        f.link(1102, 1, 15, false, 0, 13, 1, 1);
        f.link(1103, 1, 17, false, 0, 13, 2, 3);
        f.link(1104, 1, 12, false, 0, 11, 0);
        return f;
    }

    // P_Modul_29_MF Script (L11): root 2; the 4 m probe 32, Get Cell 33 and
    // Test 34 in the root graph, the Get Name operation 600 between them.
    fixture bridge() {
        fixture f;
        f.block(2, "", 0, {}, {}, {800, 801});
        f.block(30, "Physics WakeUp", 2);
        f.block(31, "Binary Switch", 2);
        f.block(32, "TT Scaleable Proximity", 2, {703, 704});
        f.block(33, "Get Cell", 2, {}, {700});
        f.block(34, "Test", 2, {701, 702});
        f.block(35, "Wave Player", 2);
        f.block(36, "", 2);   // 10 Hinges
        f.source(703, 999);   // ObjectA
        f.source(704, 801);   // the distance: a root constant
        f.source(701, 800);   // the comparison
        mu::operation op;
        op.id = 600;
        op.graph = 2;
        op.name = "Get Name";
        op.inputs = {601};
        op.output = 602;
        f.m.operations[600] = op;
        f.source(601, 700);   // Get Name reads the cell
        f.m.params[602].id = 602;
        f.source(702, 602);   // the Test reads the name
        f.link(2001, 2, 30, false, 0, 32, 0);        // WakeUp -> probe In (fan-out)
        f.link(2002, 2, 31, false, 1, 32, 1);        // the reset -> probe Off (fan-out)
        f.link(2003, 2, 32, false, 0, 33, 0);        // inside
        f.link(2004, 2, 33, false, 0, 34, 0);        // inside
        f.link(2005, 2, 34, false, 0, 35, 0);        // Test True -> Wave Player (fan-in)
        f.link(2006, 2, 34, false, 0, 32, 1);        // Test True -> probe Off (inside)
        f.link(2007, 2, 34, false, 0, 36, 2);        // Test True -> 10 Hinges.in2 (fan-in)
        return f;
    }

    std::vector<uint32_t> link_ids(const mu::graph_model& m, const std::vector<size_t>& at) {
        std::vector<uint32_t> out;
        for (size_t i: at) out.push_back(m.links[i].id);
        std::sort(out.begin(), out.end());
        return out;
    }
}

TEST(MechanismUnits, TheFanIsTheGraphUnitPhysicsForce) {
    const fixture f = fan();
    const mu::unit u = mu::find_unit(f.m, 21, 27);
    EXPECT_TRUE(u.error.empty()) << u.error;
    EXPECT_EQ(mu::shape::graph, u.kind);
    EXPECT_EQ(13u, u.graph);
    EXPECT_EQ((std::vector<uint32_t>{13}), u.copy_roots());
    EXPECT_EQ((std::vector<uint32_t>{22, 26}), u.consumers);
    EXPECT_EQ((std::vector<uint32_t>{20, 23, 24, 25}), u.control);
    EXPECT_EQ(8u, u.blocks.size());
    EXPECT_EQ((std::vector<uint32_t>{100, 101}), u.state);
    EXPECT_TRUE(u.operations.empty());
    EXPECT_EQ(12u, u.inside.size());
    EXPECT_EQ((std::vector<uint32_t>{1101, 1102, 1103}), link_ids(f.m, u.fan_out));
    EXPECT_TRUE(u.fan_in.empty());
    EXPECT_EQ(3, f.m.links[u.fan_out[2]].delay) << "a fan-out link keeps its delay";
    // The shared sources: the Kollisionsquader, the force vector, Ball_Pos_Frame.
    EXPECT_EQ((std::vector<uint32_t>{900, 901, 999}), u.shared_sources);
}

TEST(MechanismUnits, TheBridgeIsTheBlockUnitWithItsGetName) {
    const fixture f = bridge();
    const mu::unit u = mu::find_unit(f.m, 33, 32);
    EXPECT_TRUE(u.error.empty()) << u.error;
    EXPECT_EQ(mu::shape::blocks, u.kind);
    EXPECT_EQ((std::vector<uint32_t>{32, 33, 34}), u.copy_roots());
    EXPECT_EQ((std::vector<uint32_t>{34}), u.consumers);
    EXPECT_TRUE(u.control.empty());
    EXPECT_TRUE(u.state.empty());
    EXPECT_EQ((std::vector<uint32_t>{600}), u.operations);
    EXPECT_EQ((std::vector<uint32_t>{2003, 2004, 2006}), link_ids(f.m, u.inside));
    EXPECT_EQ((std::vector<uint32_t>{2001, 2002}), link_ids(f.m, u.fan_out));
    EXPECT_EQ((std::vector<uint32_t>{2005, 2007}), link_ids(f.m, u.fan_in));
    EXPECT_EQ((std::vector<uint32_t>{800, 801, 999}), u.shared_sources);
}

TEST(MechanismUnits, AThirdShapeIsRefusedWithTheBlocksNamed) {
    {
        // A consumer outside the Get Cell's graph: the script's Binary
        // Switch reads the cell too.
        fixture f = fan();
        f.source(300, 200);
        const mu::unit u = mu::find_unit(f.m, 21, 27);
        EXPECT_NE(std::string::npos, u.error.find("Binary Switch #1 (a consumer) outside the Get Cell's graph")) << u.error;
    }
    {
        // A block unit writing a local of its graph that it also reads:
        // graph state outside a graph unit.
        fixture f = bridge();
        f.writes(700, 802);
        f.m.params[802].id = 802;
        f.m.blocks[2].locals.push_back(802);
        f.source(701, 802);
        const mu::unit u = mu::find_unit(f.m, 33, 32);
        EXPECT_NE(std::string::npos, u.error.find("a block unit with graph state")) << u.error;
        EXPECT_NE(std::string::npos, u.error.find("writes a parameter outside the unit")) << u.error;
    }
    {
        // A graph whose written locals are not its state: the probe
        // writes a local of the graph nobody of the unit reads.
        fixture f = fan();
        f.m.blocks[13].locals.push_back(103);
        f.m.params[103].id = 103;
        f.m.blocks[27].outputs.push_back(212);
        f.m.params[212].id = 212;
        f.writes(212, 103);
        const mu::unit u = mu::find_unit(f.m, 21, 27);
        EXPECT_NE(std::string::npos, u.error.find("written locals are not its state")) << u.error;
    }
}

TEST(MechanismUnits, SlotBindingTakesTheFirstCandidateOfEachSlot) {
    // Candidates sorted by player id with their join orders: slot 2 has
    // two (an invariant violation; the first wins), slot 1 none, a slot at
    // or past K binds nothing.
    const std::vector<int> slots{2, 0, 2, -1, 5};
    EXPECT_EQ((std::vector<int>{1, -1, 0, -1}), mu::bind_slots(slots, 4));
    EXPECT_EQ((std::vector<int>{-1}), mu::bind_slots({}, 1));
    EXPECT_TRUE(mu::bind_slots(slots, 0).empty());
}

TEST(MechanismUnits, TheJoinOrderCapAndK) {
    using bmmo::session::capped_join_order;
    using bmmo::session::join_order_verdict;
    // An order below K.
    auto c = capped_join_order({0, 2}, {1}, 4);
    EXPECT_EQ(join_order_verdict::order, c.verdict);
    EXPECT_EQ(3, c.order);
    // Every order below K held or retiring, one retiring: wait.
    c = capped_join_order({0, 2, 3}, {1}, 4);
    EXPECT_EQ(join_order_verdict::wait, c.verdict);
    // Every order below K held by a live life: refused.
    c = capped_join_order({0, 1, 2, 3}, {}, 4);
    EXPECT_EQ(join_order_verdict::refuse, c.verdict);
    c = capped_join_order({0, 1, 2, 3}, {7}, 4);
    EXPECT_EQ(join_order_verdict::refuse, c.verdict) << "a retiring order at or past K does not help";
    // K from the room capacity: maximum_members 100 gives 64, and the 65th
    // physics joiner is refused without a reload.
    EXPECT_EQ(64, mu::mechanism_slot_count(100, 64));
    EXPECT_EQ(8, mu::mechanism_slot_count(8, 64));
    EXPECT_EQ(1, mu::mechanism_slot_count(0, 64));
    std::set<uint8_t> held;
    for (int i = 0; i < 64; ++i) held.insert(static_cast<uint8_t>(i));
    EXPECT_EQ(join_order_verdict::refuse, capped_join_order(held, {}, mu::mechanism_slot_count(100, 64)).verdict);
}

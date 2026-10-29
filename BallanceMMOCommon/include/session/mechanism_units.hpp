#pragma once

// World agreement's per-player mechanism units, CK-free
// (docs/world-agreement-gaps-design.md §2.10, part I, rule P5; step Q7).
//
// Under WORLD_PLAYER_MECHANISMS every world copies, once per player slot,
// the sub-graph of a mechanism script that acts on the ball whose identity
// it reads (the fan's force, the rope bridge's stone test).  This header is
// the rule that finds that sub-graph - the UNIT - on an abstract graph the
// host extracts from CK (game/proximity_union.hpp; SimTool's census), so the
// rule is the same code in the three targets and testable without CK (U8).
//
// For an identity read R (a Get Cell of CurrentLevel's ActiveBall) and its
// gating probe P (the one rewired proximity block whose output is linked to
// R's input):
//   D  the identity consumers: every block with an input or a target whose
//      source chain starts at R's output (R's output, its destinations, and
//      the outputs of parameter operations reading any of those);
//   C  the control path: the blocks of R's graph on a behaviour-link path
//      (links of that graph between its children) from P to a block of D;
//   U  = {P, R} + D + C;
//   S  the graph's locals that a block of U writes (an output's
//      destination) and a block of U reads (an input's or target's source).
// Shape:
//   graph   U is every child of R's graph, that graph is a sub-behaviour
//           graph (it has a parent) and S is every local a child of it
//           writes: the unit is the whole graph (the fan's Physics Force).
//   blocks  otherwise: the unit is U's blocks (the bridge's probe, Get Cell
//           and Test in the root graph), and S must be empty.
// Anything else is refused with the blocks named (§2.10 "Shapes"): a block
// of D or P outside R's graph, a block unit with graph state, a graph
// whose written locals are not S, or a unit block writing a parameter the
// copy would not own.
//
// The links of slot k's copy (§2.10 "Links and sources"): `inside` (both
// ends in the unit: copied between slot k's blocks; for a graph unit the
// graph's own links, which the copy carries), `fan_out` (from outside into
// the unit: added from the same source to slot k's copy) and `fan_in` (from
// the unit to outside: added from slot k's copy to the same target), each
// with its initial delay.  `operations`: the parameter operations of a
// block unit's graph between two of its blocks (the bridge's Get Name, R's
// output -> the Test's input), which slot k needs of its own - §2.10 does
// not name them; a graph unit carries its operations with it.

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bmmo::session::units {

    // A behaviour IO end of a link: the block that owns the IO, whether the
    // IO is one of its inputs or outputs, and its index.
    struct io_ref {
        uint32_t block = 0;
        bool input = false;
        int index = 0;
        bool operator==(const io_ref& o) const { return block == o.block && input == o.input && index == o.index; }
    };

    // A behaviour link of a graph (`graph`: the graph whose link list holds
    // it).  CK's naming: the link goes from `from` (an output IO, or the
    // graph's own input IO) to `to` (an input IO, or the graph's own output).
    struct link {
        uint32_t id = 0;
        uint32_t graph = 0;
        io_ref from, to;
        int delay = 0;   // the initial activation delay
    };

    // A parameter.  `source`: an input's (or a target's) real source, 0 for
    // none; `destinations`: an output's destinations; a local has neither.
    struct param {
        uint32_t id = 0;
        uint32_t source = 0;
        std::vector<uint32_t> destinations;
    };

    // A parameter operation of a graph: its inputs (parameter ids, with their
    // sources in `params`) and its output parameter.
    struct operation {
        uint32_t id = 0;
        uint32_t graph = 0;
        std::string name;
        std::vector<uint32_t> inputs;
        uint32_t output = 0;
    };

    struct block {
        uint32_t id = 0;
        std::string prototype;            // the prototype's name ("" for a graph without one)
        uint32_t parent = 0;              // 0: a root script
        std::vector<uint32_t> children;   // sub-behaviour index order
        std::vector<uint32_t> inputs;     // input parameter ids, index order
        std::vector<uint32_t> outputs;    // output parameter ids
        std::vector<uint32_t> locals;     // local parameter ids
        uint32_t target = 0;              // the target parameter (an input), 0 for none
        int walk = -1;                    // pre-order walk position in its root script (the root is 0)
    };

    struct graph_model {
        std::map<uint32_t, block> blocks;
        std::map<uint32_t, param> params;
        std::map<uint32_t, operation> operations;
        std::vector<link> links;
    };

    enum class shape : uint8_t { none, graph, blocks };

    struct unit {
        uint32_t read = 0, probe = 0;
        uint32_t graph = 0;                   // R's graph (the unit itself for shape graph)
        shape kind = shape::none;
        std::vector<uint32_t> blocks;         // U, walk order
        std::vector<uint32_t> consumers;      // D, walk order
        std::vector<uint32_t> control;        // C, walk order
        std::vector<uint32_t> state;          // S, the graph's local order
        std::vector<uint32_t> operations;     // a block unit's own operations (id order)
        std::vector<size_t> inside, fan_out, fan_in;   // indexes into graph_model::links
        std::vector<uint32_t> shared_sources;          // sources a copy keeps (outside the unit and S)
        std::string error;                    // the refusal, "" when the unit is one of the two shapes
        // What the copy step copies: the graph (shape graph) or U's blocks.
        std::vector<uint32_t> copy_roots() const {
            if (kind == shape::graph) return {graph};
            return blocks;
        }
    };

    inline const char* shape_name(shape s) {
        return s == shape::graph ? "graph" : s == shape::blocks ? "blocks" : "none";
    }

    namespace detail {
        inline std::string label(const graph_model& m, uint32_t id) {
            const auto it = m.blocks.find(id);
            if (it == m.blocks.end()) return "#" + std::to_string(id);
            return (it->second.prototype.empty() ? std::string("graph") : it->second.prototype) + " #"
                   + std::to_string(it->second.walk);
        }
        // Every block under `root` (the root included), pre-order.
        inline void subtree(const graph_model& m, uint32_t root, std::set<uint32_t>& out) {
            if (!out.insert(root).second) return;
            const auto it = m.blocks.find(root);
            if (it == m.blocks.end()) return;
            for (uint32_t c: it->second.children) subtree(m, c, out);
        }
        // The parameters a block owns (inputs, outputs, locals, target), its
        // whole subtree's for a graph.
        inline void owned_params(const graph_model& m, const std::set<uint32_t>& blocks, std::set<uint32_t>& out) {
            for (uint32_t b: blocks) {
                const auto it = m.blocks.find(b);
                if (it == m.blocks.end()) continue;
                out.insert(it->second.inputs.begin(), it->second.inputs.end());
                out.insert(it->second.outputs.begin(), it->second.outputs.end());
                out.insert(it->second.locals.begin(), it->second.locals.end());
                if (it->second.target) out.insert(it->second.target);
            }
        }
        inline uint32_t source_of(const graph_model& m, uint32_t p) {
            const auto it = m.params.find(p);
            return it == m.params.end() ? 0u : it->second.source;
        }
        inline std::vector<uint32_t> by_walk(const graph_model& m, std::set<uint32_t> ids) {
            std::vector<uint32_t> v(ids.begin(), ids.end());
            std::sort(v.begin(), v.end(), [&m](uint32_t a, uint32_t b) {
                const auto ia = m.blocks.find(a), ib = m.blocks.find(b);
                const int wa = ia == m.blocks.end() ? -1 : ia->second.walk, wb = ib == m.blocks.end() ? -1 : ib->second.walk;
                return wa != wb ? wa < wb : a < b;
            });
            return v;
        }
    }

    // §2.10's unit rule for the identity read `read` and its gating probe
    // `probe`.  The result's `error` names the refusal; `kind` is set even
    // then when the membership could be computed.
    inline unit find_unit(const graph_model& m, uint32_t read, uint32_t probe) {
        unit u;
        u.read = read;
        u.probe = probe;
        std::string error;
        const auto add = [&error](const std::string& text) { error += (error.empty() ? "" : "; ") + text; };
        const auto rb = m.blocks.find(read);
        if (rb == m.blocks.end() || rb->second.parent == 0 || rb->second.outputs.empty()) {
            u.error = "the Get Cell has no graph or no output";
            return u;
        }
        const uint32_t graph = rb->second.parent;
        u.graph = graph;
        const block& g = m.blocks.at(graph);
        const std::set<uint32_t> children(g.children.begin(), g.children.end());
        const uint32_t value = rb->second.outputs.front();

        // The parameters whose value is the read's.
        std::set<uint32_t> reach{value};
        for (bool grew = true; grew;) {
            grew = false;
            for (uint32_t p: std::vector<uint32_t>(reach.begin(), reach.end())) {
                const auto it = m.params.find(p);
                if (it == m.params.end()) continue;
                for (uint32_t d: it->second.destinations)
                    if (reach.insert(d).second) grew = true;
            }
            for (const auto& [id, op]: m.operations) {
                if (!op.output || reach.count(op.output)) continue;
                for (uint32_t in: op.inputs)
                    if (reach.count(detail::source_of(m, in))) {
                        reach.insert(op.output);
                        grew = true;
                        break;
                    }
            }
        }
        // D: every block with an input or a target fed from that set.
        std::set<uint32_t> d;
        for (const auto& [id, b]: m.blocks) {
            if (id == read) continue;
            bool fed = b.target && reach.count(detail::source_of(m, b.target));
            for (uint32_t in: b.inputs) fed = fed || reach.count(detail::source_of(m, in)) != 0;
            if (!fed) continue;
            d.insert(id);
            if (b.parent != graph) add(detail::label(m, id) + " (a consumer) outside the Get Cell's graph");
        }
        if (!children.count(probe)) add("the gating probe outside the Get Cell's graph");
        // C: the graph's children on a link path from P to a block of D.
        std::map<uint32_t, std::vector<uint32_t>> forward, backward;
        for (const link& l: m.links) {
            if (l.graph != graph || !children.count(l.from.block) || !children.count(l.to.block)) continue;
            forward[l.from.block].push_back(l.to.block);
            backward[l.to.block].push_back(l.from.block);
        }
        const auto closure = [](std::map<uint32_t, std::vector<uint32_t>>& edges, std::set<uint32_t> start) {
            std::vector<uint32_t> stack(start.begin(), start.end());
            while (!stack.empty()) {
                const uint32_t at = stack.back();
                stack.pop_back();
                for (uint32_t next: edges[at])
                    if (start.insert(next).second) stack.push_back(next);
            }
            return start;
        };
        std::set<uint32_t> d_in_graph;
        for (uint32_t id: d)
            if (children.count(id)) d_in_graph.insert(id);
        const std::set<uint32_t> from_probe = closure(forward, {probe});
        const std::set<uint32_t> to_consumers = closure(backward, d_in_graph);
        std::set<uint32_t> members{read, probe};
        members.insert(d.begin(), d.end());
        std::set<uint32_t> c;
        for (uint32_t id: from_probe)
            if (to_consumers.count(id) && !members.count(id)) c.insert(id);
        members.insert(c.begin(), c.end());
        // S, and the graph's written locals.
        std::set<uint32_t> graph_locals(g.locals.begin(), g.locals.end());
        std::set<uint32_t> written_by_unit, read_by_unit, written_in_graph;
        for (uint32_t id: children) {
            const block& b = m.blocks.at(id);
            const bool in_unit = members.count(id) != 0;
            for (uint32_t out: b.outputs) {
                const auto it = m.params.find(out);
                if (it == m.params.end()) continue;
                for (uint32_t dst: it->second.destinations) {
                    written_in_graph.insert(dst);
                    if (in_unit) written_by_unit.insert(dst);
                }
            }
            if (!in_unit) continue;
            for (uint32_t in: b.inputs) read_by_unit.insert(detail::source_of(m, in));
            if (b.target) read_by_unit.insert(detail::source_of(m, b.target));
        }
        std::set<uint32_t> state, written_locals;
        for (uint32_t local: g.locals) {
            if (written_in_graph.count(local)) written_locals.insert(local);
            if (written_by_unit.count(local) && read_by_unit.count(local)) {
                state.insert(local);
                u.state.push_back(local);
            }
        }
        const bool whole = g.parent != 0 && members == children;
        u.kind = whole ? shape::graph : shape::blocks;
        if (whole && state != written_locals) add("a graph unit whose written locals are not its state");
        if (!whole && !state.empty()) add("a block unit with graph state");
        u.blocks = detail::by_walk(m, members);
        u.consumers = detail::by_walk(m, d);
        u.control = detail::by_walk(m, c);

        // What the copy owns: the graph's subtree, or U's blocks and their
        // operations between two of them.
        std::set<uint32_t> copied_blocks;
        if (whole) detail::subtree(m, graph, copied_blocks);
        else copied_blocks = members;
        std::set<uint32_t> owned;
        detail::owned_params(m, copied_blocks, owned);
        if (!whole) {
            // Operations of the graph fed by a unit block's output (or such an
            // operation's) whose output feeds a unit block, transitively.
            std::set<uint32_t> outs;
            for (uint32_t b: members)
                for (uint32_t o: m.blocks.at(b).outputs) outs.insert(o);
            std::set<uint32_t> fed;   // operations reached from the unit's outputs
            for (bool grew = true; grew;) {
                grew = false;
                for (const auto& [id, op]: m.operations) {
                    if (op.graph != graph || fed.count(id)) continue;
                    for (uint32_t in: op.inputs)
                        if (outs.count(detail::source_of(m, in))) {
                            fed.insert(id);
                            if (op.output) outs.insert(op.output);
                            grew = true;
                            break;
                        }
                }
            }
            std::set<uint32_t> unit_inputs;
            for (uint32_t b: members) {
                const block& bb = m.blocks.at(b);
                for (uint32_t in: bb.inputs) unit_inputs.insert(detail::source_of(m, in));
                if (bb.target) unit_inputs.insert(detail::source_of(m, bb.target));
            }
            std::set<uint32_t> needed;   // fed operations whose output reaches a unit input
            for (bool grew = true; grew;) {
                grew = false;
                for (uint32_t id: fed) {
                    if (needed.count(id)) continue;
                    const operation& op = m.operations.at(id);
                    bool feeds = unit_inputs.count(op.output) != 0;
                    for (uint32_t other: needed)
                        for (uint32_t in: m.operations.at(other).inputs)
                            feeds = feeds || detail::source_of(m, in) == op.output;
                    if (feeds) {
                        needed.insert(id);
                        grew = true;
                    }
                }
            }
            u.operations.assign(needed.begin(), needed.end());
            for (uint32_t id: needed) {
                const operation& op = m.operations.at(id);
                owned.insert(op.inputs.begin(), op.inputs.end());
                if (op.output) owned.insert(op.output);
            }
        }
        // A unit block writing a parameter the copy does not own would write
        // the original's (and every other slot's) parameter.
        for (uint32_t b: copied_blocks) {
            for (uint32_t out: m.blocks.at(b).outputs) {
                const auto it = m.params.find(out);
                if (it == m.params.end()) continue;
                for (uint32_t dst: it->second.destinations)
                    if (!owned.count(dst))
                        add(detail::label(m, b) + " writes a parameter outside the unit");
            }
        }
        // Sources: kept (shared) unless the copy owns them.
        std::set<uint32_t> shared;
        for (uint32_t b: copied_blocks) {
            const block& bb = m.blocks.at(b);
            std::vector<uint32_t> ins = bb.inputs;
            if (bb.target) ins.push_back(bb.target);
            for (uint32_t in: ins) {
                const uint32_t s = detail::source_of(m, in);
                if (s && !owned.count(s)) shared.insert(s);
            }
        }
        u.shared_sources.assign(shared.begin(), shared.end());
        // The links.
        for (size_t i = 0; i < m.links.size(); ++i) {
            const link& l = m.links[i];
            if (whole) {
                if (copied_blocks.count(l.graph)) {
                    u.inside.push_back(i);   // the graph's own (or a nested graph's): carried by the copy
                    continue;
                }
                if (l.graph != g.parent) continue;
                const bool from_unit = l.from.block == graph, to_unit = l.to.block == graph;
                if (to_unit && !from_unit) u.fan_out.push_back(i);
                else if (from_unit && !to_unit) u.fan_in.push_back(i);
                else if (from_unit && to_unit) u.inside.push_back(i);
            } else {
                if (l.graph != graph) continue;
                const bool from_unit = members.count(l.from.block) != 0, to_unit = members.count(l.to.block) != 0;
                if (from_unit && to_unit) u.inside.push_back(i);
                else if (to_unit) u.fan_out.push_back(i);
                else if (from_unit) u.fan_in.push_back(i);
            }
        }
        u.error = error;
        return u;
    }

    // One line per unit for the anchor logs and the census.
    inline std::string describe(const graph_model& m, const unit& u) {
        const auto list = [&m](const std::vector<uint32_t>& ids) {
            std::string s;
            for (uint32_t id: ids) s += (s.empty() ? "" : ", ") + detail::label(m, id);
            return s;
        };
        std::string text = std::string("shape=") + shape_name(u.kind) + " U=" + std::to_string(u.blocks.size()) + " D=["
                           + list(u.consumers) + "] C=[" + list(u.control) + "] |S|=" + std::to_string(u.state.size())
                           + " ops=" + std::to_string(u.operations.size()) + " links inside "
                           + std::to_string(u.inside.size()) + " in " + std::to_string(u.fan_out.size()) + " out "
                           + std::to_string(u.fan_in.size());
        if (!u.error.empty()) text += " REFUSED: " + u.error;
        return text;
    }

    // The slot count of a session (§2.10): the room capacity clamped to
    // [1, max_players].
    inline uint8_t mechanism_slot_count(uint32_t maximum_members, uint32_t max_players) {
        const uint32_t k = std::clamp<uint32_t>(maximum_members, 1u, max_players);
        return static_cast<uint8_t>(std::min<uint32_t>(k, 255u));
    }

    // Slot binding (§2.10 "Binding"): for each slot k < `slots`, the index
    // into `slot_of` (the candidates' slots, -1 for none) of the first
    // candidate whose slot is k, -1 when slot k has no candidate.  A
    // candidate with a slot outside [0, slots) binds nothing (the server's
    // cap makes it impossible; the caller counts it).
    inline std::vector<int> bind_slots(const std::vector<int>& slot_of, uint8_t slots) {
        std::vector<int> bound(slots, -1);
        for (size_t i = 0; i < slot_of.size(); ++i) {
            const int k = slot_of[i];
            if (k < 0 || k >= static_cast<int>(slots) || bound[static_cast<size_t>(k)] >= 0) continue;
            bound[static_cast<size_t>(k)] = static_cast<int>(i);
        }
        return bound;
    }

}

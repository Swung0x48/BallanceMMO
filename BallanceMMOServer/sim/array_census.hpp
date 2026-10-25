#pragma once

// SimTool --array-reads (docs/record-format-gaps-design.md 2.2 and 7.2 S2,
// step K0): the array-read census of a world at its anchor, read-only.
//
// For every block of every selected script that is not keep-local
// (session/resync_rules.hpp keep_local_script), recursively, every data
// array the block works on - its target, or an input parameter whose source
// is a data array - with:
//
//   - the block's prototype, its walk position in the root script (pre-order,
//     the root is 0: the resync design's 2.4 key) and its path;
//   - the array's name and the row and column when the block's row / column
//     input is a constant (Get Cell, Set Cell, Row Search, Iterator If, ...);
//     otherwise "*";
//   - the access: read, write or both (by prototype; an unknown prototype on
//     an array is "both").
//
// Each entry is classified (design 2.2):
//
//   load-time   the block's root script is a *_Init script; or (step K0's
//               addition, the level tables) a read of an array whose every
//               writer block in the whole composition is in a load-time
//               script (Levelinit_*, *_Init);
//   unwritten   (step K0's addition) a read of an array (or of a constant
//               column of it) that no block of the composition writes: the
//               level file's data.  Kept apart from load-time because the
//               census sees blocks only: a native writer (a CK manager, host
//               C++) would not show; step K2 confirms the class;
//   private     the array is a session-private copy (a name with "BMMO_":
//               the server's ball identity copies "CurrentLevel_BMMO_Ball_N",
//               the checkpoint phase copy);
//   recorded    (array, row 0, column) is one of the record's three cells
//               (game/script_replay.hpp cell_of: CurrentLevel[0][4],
//               IngameParameter[0][1], IngameParameter[0][2]);
//   sound       the block's behaviour links and parameter flow, followed to
//               the end of its graph, reach only sound blocks (Wave Player,
//               TT ProximityVolumeControl, ...), blocks on the same array and
//               pure flow blocks (Identity, Binary Switch, Op, Delayer, ...),
//               and no link or parameter leaves the start block's graph (an
//               exported output, a parameter owned outside the graph, a nested
//               graph's own output); a write is sound only when every read of
//               that array in the census is sound too;
//   unrecorded  anything else.
//
// Scope.  The server's world rewires its ball identity reads to private
// copies at the anchor (physics_world::rewire_ball_identity_reads); a client
// world (the headless client, the retail core host) does not while the
// world-agreement design's bit is clear.  `client_scope` classifies a read of
// a server ball identity copy as a read of the array it was copied from, i.e.
// as the client world makes it: S2's control.
//
// Only the public CK2 API is used, and nothing is written: parameter values
// are read with update = FALSE, so no parameter operation runs.  Step K2
// moves the census into script_replay (array_reads) for the hosts' anchor log.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "CKAll.h"

#include <game/script_replay.hpp>
#include <session/resync_rules.hpp>

namespace bmmo::sim {
    class array_census {
    public:
        enum class access { read, write, both };
        enum class kind { recorded, private_copy, load_time, unwritten, sound, unrecorded };

        struct entry {
            CK_ID block = 0;
            std::string owner, script, path, prototype, array;
            int walk = 0;
            int row = -1, column = -1;   // -1: not a constant ("*")
            access how = access::read;
            kind what = kind::unrecorded;
            std::string note;            // client scope: the copy the server reads instead
        };

        struct result {
            int selected = 0, keep_local = 0, blocks = 0;
            std::vector<std::string> kept;   // the keep-local scripts, "owner/script"
            std::vector<entry> entries;
            int count(kind k) const {
                return static_cast<int>(std::count_if(entries.begin(), entries.end(),
                                                      [k](const entry& e) { return e.what == k; }));
            }
        };

        static const char* name_of(kind k) {
            switch (k) {
                case kind::recorded: return "recorded";
                case kind::private_copy: return "private";
                case kind::load_time: return "load-time";
                case kind::unwritten: return "unwritten";
                case kind::sound: return "sound";
                default: return "unrecorded";
            }
        }
        static const char* name_of(access a) {
            return a == access::read ? "read" : a == access::write ? "write" : "both";
        }

        static result run(CKContext* context, const bmmo::game::script_replay::selection& sel, bool client_scope) {
            result out;
            if (!context) return out;
            for (CK_ID id: sel.scripts) {
                auto* script = CKBehavior::Cast(context->GetObject(id));
                if (!script) continue;
                CKBeObject* owner = script->GetOwner();
                const std::string owner_name = owner && owner->GetName() ? owner->GetName() : "";
                const std::string script_name = script->GetName() ? script->GetName() : "";
                if (bmmo::session::keep_local_script(owner_name, script_name)) {
                    ++out.keep_local;
                    out.kept.push_back(owner_name + "/" + script_name);
                    continue;
                }
                ++out.selected;
                const bool init = script_name.find("_Init") != std::string::npos;
                int walk_index = 0;
                walk(script, script->GetName() ? script->GetName() : "?", [&](CKBehavior* block, const std::string& path) {
                    const int position = walk_index++;
                    ++out.blocks;
                    for (CKDataArray* array: arrays_of(context, block)) {
                        entry e;
                        e.block = block->GetID();
                        e.owner = owner_name;
                        e.script = script_name;
                        e.path = path;
                        e.prototype = prototype_of(block);
                        e.walk = position;
                        e.how = access_of(e.prototype);
                        e.array = array->GetName() ? array->GetName() : "?";
                        constant_int(block, row_pin(block), e.row);
                        constant_int(block, column_pin(block), e.column);
                        if (whole_row(e.prototype) && e.column >= 0) {
                            // The key column; the block outputs the whole row.
                            e.note = "(key column " + std::to_string(e.column) + ", outputs the row)";
                            e.column = -1;
                        }
                        if (init) e.what = kind::load_time;
                        else e.what = classify(e, client_scope);
                        out.entries.push_back(e);
                    }
                });
            }
            // A level table: a read of an array whose every writer block, in
            // any script of the composition, is in a load-time script reads
            // what the level's loading left; one that no block writes is
            // `unwritten` (the level file's data, unless a native writer the
            // census cannot see writes it).  Column-aware: a writer of
            // another constant column does not count (array_writers gives
            // every row-moving writer column "*").
            const std::map<std::string, std::vector<writer>> writers = array_writers(context);
            for (entry& e: out.entries) {
                if (e.what != kind::unrecorded || e.how != access::read) continue;
                auto it = writers.find(e.array);
                std::map<std::string, int> relevant;
                bool load_only = true;
                if (it != writers.end())
                    for (const writer& w: it->second) {
                        if (e.column >= 0 && w.column >= 0 && w.column != e.column) continue;
                        ++relevant[w.root + " [" + w.prototype + "] col " + (w.column < 0 ? std::string("*") : std::to_string(w.column))];
                        load_only = load_only && load_time_script(w.root);
                    }
                std::string roots;
                for (const auto& [what, n]: relevant) roots += (roots.empty() ? "" : ", ") + what + " x" + std::to_string(n);
                if (relevant.empty()) {
                    e.what = kind::unwritten;
                    e.note += std::string(e.note.empty() ? "" : " ") + "(no block writes it)";
                } else if (load_only) {
                    e.what = kind::load_time;
                    e.note += std::string(e.note.empty() ? "" : " ") + "(table; writers: " + roots + ")";
                } else {
                    e.note += std::string(e.note.empty() ? "" : " ") + "(writers: " + roots + ")";
                }
            }
            // Sound, reads first: a write is sound only if every read of its
            // array is.  Arrays whose reads are all sound:
            std::map<std::string, bool> sound_array;
            for (entry& e: out.entries) {
                if (e.what != kind::unrecorded || e.how == access::write) continue;
                const bool s = sound_flow(context, e.block, true);
                if (s) e.what = kind::sound;
                auto it = sound_array.emplace(e.array, true).first;
                it->second = it->second && s;
            }
            for (entry& e: out.entries) {
                if (e.what != kind::unrecorded || e.how != access::write) continue;
                auto it = sound_array.find(e.array);
                const bool reads_sound = it != sound_array.end() && it->second;
                if (reads_sound && sound_flow(context, e.block, false)) e.what = kind::sound;
            }
            return out;
        }

        static std::string cell_text(const entry& e) {
            const auto part = [](int v) { return v < 0 ? std::string("*") : std::to_string(v); };
            return e.array + "[" + part(e.row) + "][" + part(e.column) + "]";
        }

        // One line per entry (unrecorded and sound always, the rest with
        // `all`), the count per class, and the unrecorded entries grouped by
        // script, prototype and cell.
        static void print(const result& r, int level, bool client_scope, bool all) {
            std::printf("array-reads level=%d scope=%s selected=%d keep_local=%d blocks=%d entries=%zu"
                        " recorded=%d private=%d load-time=%d unwritten=%d sound=%d unrecorded=%d\n",
                        level, client_scope ? "client" : "server", r.selected, r.keep_local, r.blocks, r.entries.size(),
                        r.count(kind::recorded), r.count(kind::private_copy), r.count(kind::load_time),
                        r.count(kind::unwritten), r.count(kind::sound), r.count(kind::unrecorded));
            for (const std::string& k: r.kept) std::printf("array-reads keep-local %s\n", k.c_str());
            for (const entry& e: r.entries) {
                if (!all && e.what != kind::unrecorded && e.what != kind::sound) continue;
                std::printf("array-read %s %s %s/%s #%d [%s] %s id=%u path=%s%s%s\n", name_of(e.what), name_of(e.how),
                            e.owner.c_str(), e.script.c_str(), e.walk, e.prototype.c_str(), cell_text(e).c_str(),
                            static_cast<unsigned>(e.block), e.path.c_str(), e.note.empty() ? "" : " ", e.note.c_str());
            }
            std::map<std::string, int> groups;
            for (const entry& e: r.entries) {
                if (e.what != kind::unrecorded) continue;
                ++groups[script_family(e.script) + " " + e.prototype + " " + name_of(e.how) + " " + cell_text(e)];
            }
            for (const auto& [what, n]: groups) std::printf("array-reads unrecorded %s x%d\n", what.c_str(), n);
            std::printf("ARRAY-READS: level=%d scope=%s unrecorded=%d sound=%d unwritten=%d\n", level,
                        client_scope ? "client" : "server", r.count(kind::unrecorded), r.count(kind::sound),
                        r.count(kind::unwritten));
        }

        // "P_Modul_18_MF Script" -> "P_Modul_18" (a level's modules of one
        // kind share the family; design A1 counts by it).
        static std::string script_family(const std::string& script) {
            std::string s = script;
            const size_t space = s.find(' ');
            if (space != std::string::npos) s.resize(space);
            for (const char* tail: {"_MF", "_Init"}) {
                const size_t at = s.rfind(tail);
                if (at != std::string::npos && at + std::strlen(tail) == s.size()) s.resize(at);
            }
            return s;
        }

    private:
        template <typename F> static void walk(CKBehavior* beh, const std::string& path, F&& fn) {
            fn(beh, path);
            if (beh->IsUsingFunction()) return;
            const int children = beh->GetSubBehaviorCount();
            for (int i = 0; i < children; ++i)
                if (CKBehavior* child = beh->GetSubBehavior(i))
                    walk(child, path + "/" + (child->GetName() ? child->GetName() : "?"), fn);
        }

        static std::string prototype_of(CKBehavior* block) {
            if (block->IsUsingFunction() && block->GetPrototypeName()) return block->GetPrototypeName();
            return block->GetName() ? block->GetName() : "?";
        }

        static CKDataArray* array_of(CKContext* context, CKParameter* source) {
            if (!source) return nullptr;
            CKParameterManager* pm = context->GetParameterManager();
            if (!pm || !pm->IsDerivedFrom(source->GetGUID(), CKPGUID_OBJECT)) return nullptr;
            return CKDataArray::Cast(source->GetValueObject(FALSE));
        }

        // The block's target and every input whose source is a data array,
        // each array once.
        static std::vector<CKDataArray*> arrays_of(CKContext* context, CKBehavior* block) {
            std::vector<CKDataArray*> out;
            const auto add = [&out](CKDataArray* a) {
                if (a && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
            };
            if (CKParameterIn* target = block->GetTargetParameter()) add(array_of(context, target->GetRealSource()));
            const int inputs = block->GetInputParameterCount();
            for (int i = 0; i < inputs; ++i)
                if (CKParameterIn* in = block->GetInputParameter(i)) add(array_of(context, in->GetRealSource()));
            return out;
        }

        static int pin_named(CKBehavior* block, std::initializer_list<const char*> names) {
            const int inputs = block->GetInputParameterCount();
            for (int i = 0; i < inputs; ++i) {
                CKParameterIn* in = block->GetInputParameter(i);
                const char* n = in ? in->GetName() : nullptr;
                if (!n) continue;
                for (const char* want: names)
                    if (std::strcmp(n, want) == 0) return i;
            }
            return -1;
        }
        static int row_pin(CKBehavior* block) { return pin_named(block, {"Row Index"}); }
        static int column_pin(CKBehavior* block) {
            return pin_named(block, {"Column Index", "Column", "Sorting Column"});
        }

        // A pin whose source is an int local parameter that no output
        // parameter of the block's root script writes into (CKParameterOut
        // destinations): a constant.  Anything else (another block's output,
        // an operation's, a local of another type) is not.  A local belongs
        // to its script: only that script's blocks and operations write it.
        static bool constant_int(CKBehavior* block, int pin, int& value) {
            value = -1;
            if (pin < 0) return false;
            CKParameterIn* in = block->GetInputParameter(pin);
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            if (!source || source->GetClassID() != CKCID_PARAMETERLOCAL) return false;
            if (source->GetDataSize() != static_cast<int>(sizeof(int))) return false;
            CKParameterManager* pm = source->GetCKContext() ? source->GetCKContext()->GetParameterManager() : nullptr;
            if (!pm || !pm->IsDerivedFrom(source->GetGUID(), CKPGUID_INT)) return false;
            CKBehavior* root = block;
            while (root->GetParent()) root = root->GetParent();
            if (written(root, source)) return false;
            int v = -1;
            if (source->GetValue(&v, FALSE) != CK_OK) return false;
            value = v;
            return true;
        }
        static bool written(CKBehavior* graph, CKParameter* local) {
            const auto targets = [local](CKParameterOut* out) {
                if (!out) return false;
                for (int d = 0; d < out->GetDestinationCount(); ++d)
                    if (out->GetDestination(d) == local) return true;
                return false;
            };
            for (int i = 0; i < graph->GetOutputParameterCount(); ++i)
                if (targets(graph->GetOutputParameter(i))) return true;
            if (graph->IsUsingFunction()) return false;
            for (int i = 0; i < graph->GetParameterOperationCount(); ++i)
                if (CKParameterOperation* op = graph->GetParameterOperation(i))
                    if (targets(op->GetOutParameter())) return true;
            for (int i = 0; i < graph->GetSubBehaviorCount(); ++i)
                if (CKBehavior* child = graph->GetSubBehavior(i))
                    if (written(child, local)) return true;
            return false;
        }

        static access access_of(const std::string& p) {
            static const std::set<std::string> reads = {
                "Get Cell", "Get Row", "Row Search", "Iterator", "Iterator If", "Get Highest", "Get Lowest",
                "Get Nearest", "Test Cell", "Value Count", "Column Sum", "Column Product", "Get Column Name",
                "Array Save", "Get Key Row", "Create Group From Array"};
            static const std::set<std::string> writes = {
                "Set Cell", "Set Row", "Add Row", "Remove Row", "Clear Array", "Insert Column", "Remove Column",
                "Move Row", "Move Column", "Swap Rows", "Sort Rows", "Shuffle Order", "Reverse Order",
                "Set Key Row", "Array Load", "Make Rows Unique", "Column Transform", "Column Operation",
                "Remove Key Row"};
            if (reads.count(p)) return access::read;
            if (writes.count(p)) return access::write;
            return access::both;
        }

        // The level's loading: Levelinit_* and the *_Init scripts.
        static bool load_time_script(const std::string& root) {
            return root.rfind("Levelinit", 0) == 0 || root.find("_Init") != std::string::npos;
        }

        static bool whole_row(const std::string& p) {
            static const std::set<std::string> rows = {
                "Iterator", "Iterator If", "Get Nearest", "Get Highest", "Get Lowest", "Get Key Row", "Get Row"};
            return rows.count(p) != 0;
        }

        // Every block of the composition that writes an array (access write
        // or both), by array name: its root script, prototype and constant
        // column (-1: any).  Only the column-confined writers keep their
        // column; a row-moving or column-moving writer (Sort Rows on its
        // sorting column, Add / Remove / Move / Swap Rows, Shuffle, Clear,
        // ...) changes every column's content.
        static bool column_confined(const std::string& p) {
            return p == "Set Cell" || p == "Column Transform" || p == "Column Operation";
        }
        struct writer { std::string root, prototype; int column = -1; };
        static std::map<std::string, std::vector<writer>> array_writers(CKContext* context) {
            std::map<std::string, std::vector<writer>> out;
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int i = 0; i < count; ++i) {
                auto* block = CKBehavior::Cast(context->GetObject(ids[i]));
                if (!block || !block->IsUsingFunction()) continue;
                if (access_of(prototype_of(block)) == access::read) continue;
                std::vector<CKDataArray*> arrays = arrays_of(context, block);
                if (arrays.empty()) continue;
                CKBehavior* root = block;
                while (root->GetParent()) root = root->GetParent();
                writer w;
                w.root = root->GetName() ? root->GetName() : "?";
                w.prototype = prototype_of(block);
                if (column_confined(w.prototype)) constant_int(block, column_pin(block), w.column);
                for (CKDataArray* a: arrays) out[a->GetName() ? a->GetName() : "?"].push_back(w);
            }
            return out;
        }

        static kind classify(entry& e, bool client_scope) {
            static const char* const kBallCopy = "_BMMO_Ball_";
            const size_t copy = e.array.find(kBallCopy);
            if (client_scope && copy != std::string::npos) {
                // A client world (the world-agreement bit clear) has no such
                // copy: its block reads the array the server copied.
                e.note = "(server reads " + e.array + ")";
                e.array.resize(copy);
            } else if (e.array.find("BMMO_") != std::string::npos) {
                return kind::private_copy;
            }
            for (int c = 0; c < bmmo::game::script_replay::kCellCount; ++c) {
                const char* array = nullptr;
                int row = 0, column = 0;
                bmmo::game::script_replay::cell_of(c, array, row, column);
                if (array && e.array == array && e.row == row && e.column == column) return kind::recorded;
            }
            return kind::unrecorded;
        }

        // ---- the sound walk

        static bool sound_block(const std::string& p) {
            if (p == "TT ProximityVolumeControl") return true;
            return p.find("Wave") != std::string::npos || p.find("Sound") != std::string::npos;
        }
        static bool flow_block(const std::string& p) {
            static const std::set<std::string> flow = {
                "Identity", "Binary Switch", "Op", "Nop", "Delayer", "Test", "Sequencer", "Parameter Selector",
                "Switch On Parameter"};
            return flow.count(p) != 0;
        }

        static CKDataArray* target_array(CKContext* context, CKBehavior* block) {
            CKParameterIn* target = block ? block->GetTargetParameter() : nullptr;
            return target ? array_of(context, target->GetRealSource()) : nullptr;
        }

        // `inner` is `graph` or a behaviour nested in it.
        static bool inside(CKBehavior* graph, CKBehavior* inner) {
            for (CKBehavior* b = inner; b; b = b->GetParent())
                if (b == graph) return true;
            return false;
        }

        // Whether a value written into `param` can be read outside `graph`:
        // an output parameter of `graph` itself (exported) or of a behaviour
        // outside it, a local or any other parameter whose owner is not a
        // behaviour inside `graph`.
        static bool leaves(CKBehavior* graph, CKParameter* param) {
            CKObject* owner = param->GetOwner();
            CKBehavior* by = owner ? CKBehavior::Cast(owner) : nullptr;
            if (!by) {
                // An operation's output: its owner is the operation, the
                // operation's owner the behaviour it belongs to.
                auto* op = owner ? CKParameterOperation::Cast(owner) : nullptr;
                CKObject* op_owner = op ? op->GetOwner() : nullptr;
                by = op_owner ? CKBehavior::Cast(op_owner) : nullptr;
                return !by || !inside(graph, by);
            }
            if (CKParameterOut::Cast(param) && by == graph) return true;
            return !inside(graph, by);
        }

        // The blocks of `graph` (any depth) with an input or target fed by
        // `param`, directly, through a destination it writes, or through
        // the parameter operations of `graph` and the graphs nested in it.
        // `left` is set when the value can be read outside `graph` (leaves).
        static void consumers(CKBehavior* graph, CKParameter* param, std::set<CKParameter*>& seen,
                              std::vector<CKBehavior*>& out, bool& left) {
            if (!param || !seen.insert(param).second) return;
            if (auto* p_out = CKParameterOut::Cast(param))
                for (int d = 0; d < p_out->GetDestinationCount(); ++d) {
                    CKParameter* dest = p_out->GetDestination(d);
                    if (!dest) continue;
                    if (leaves(graph, dest)) left = true;
                    consumers(graph, dest, seen, out, left);
                }
            // Readers are searched in the whole root script (a shortcut can
            // read a parameter anywhere in it); one outside `graph` leaves.
            CKBehavior* root = graph;
            while (root->GetParent()) root = root->GetParent();
            operation_consumers(graph, root, param, seen, out, left);
            std::vector<CKBehavior*> readers;
            fed_blocks(root, param, readers);
            for (CKBehavior* reader: readers) {
                if (inside(graph, reader)) out.push_back(reader);
                else left = true;
            }
        }
        static void operation_consumers(CKBehavior* graph, CKBehavior* at, CKParameter* param,
                                        std::set<CKParameter*>& seen, std::vector<CKBehavior*>& out, bool& left) {
            if (at->IsUsingFunction()) return;
            for (int i = 0; i < at->GetParameterOperationCount(); ++i) {
                CKParameterOperation* op = at->GetParameterOperation(i);
                if (!op) continue;
                CKParameterIn* a = op->GetInParameter1();
                CKParameterIn* b = op->GetInParameter2();
                if (!((a && a->GetRealSource() == param) || (b && b->GetRealSource() == param))) continue;
                if (!inside(graph, at)) left = true;
                else consumers(graph, op->GetOutParameter(), seen, out, left);
            }
            for (int i = 0; i < at->GetSubBehaviorCount(); ++i)
                if (CKBehavior* child = at->GetSubBehavior(i)) operation_consumers(graph, child, param, seen, out, left);
        }
        static void fed_blocks(CKBehavior* graph, CKParameter* param, std::vector<CKBehavior*>& out) {
            for (int i = 0; i < graph->GetSubBehaviorCount(); ++i) {
                CKBehavior* child = graph->GetSubBehavior(i);
                if (!child) continue;
                bool fed = false;
                if (CKParameterIn* t = child->GetTargetParameter()) fed = t->GetRealSource() == param;
                for (int k = 0; !fed && k < child->GetInputParameterCount(); ++k)
                    if (CKParameterIn* in = child->GetInputParameter(k)) fed = in->GetRealSource() == param;
                if (fed) out.push_back(child);
                if (!child->IsUsingFunction()) fed_blocks(child, param, out);
            }
        }

        // From `start`: its output links and its output parameters'
        // consumers, transitively, to the end of start's graph (the start's
        // parent).  A block's links are those of its own parent (a consumer
        // may sit in a graph nested in start's).  True when every block met
        // is a sound block, a block on the same array or a flow block, and
        // nothing leaves: no link reaches a graph's own output (start's, or a
        // nested one's: not followed further, so counted as leaving) and no
        // parameter is readable outside start's graph.
        static bool sound_flow(CKContext* context, CK_ID start_id, bool need_sound) {
            auto* start = CKBehavior::Cast(context->GetObject(start_id));
            CKBehavior* graph = start ? start->GetParent() : nullptr;
            if (!graph) return false;
            CKDataArray* array = target_array(context, start);
            std::vector<CKBehavior*> todo{start};
            std::set<CKBehavior*> visited{start};
            bool reached_sound = false;
            while (!todo.empty()) {
                CKBehavior* block = todo.back();
                todo.pop_back();
                if (block != start) {
                    if (!block->IsUsingFunction()) return false;
                    const std::string p = prototype_of(block);
                    if (sound_block(p)) reached_sound = true;
                    else if (!flow_block(p) && !(array && target_array(context, block) == array)) return false;
                }
                std::vector<CKBehavior*> next;
                CKBehavior* parent = block->GetParent();
                if (!parent) return false;
                for (int i = 0; i < parent->GetSubBehaviorLinkCount(); ++i) {
                    CKBehaviorLink* link = parent->GetSubBehaviorLink(i);
                    CKBehaviorIO* from = link ? link->GetInBehaviorIO() : nullptr;
                    CKBehaviorIO* to = link ? link->GetOutBehaviorIO() : nullptr;
                    if (!from || !to || from->GetOwner() != block) continue;
                    if (from->GetType() != CK_BEHAVIORIO_OUT) continue;
                    auto* target = CKBehavior::Cast(to->GetOwner());
                    if (!target || target == parent) return false;   // a graph's own output: leaves
                    next.push_back(target);
                }
                std::set<CKParameter*> seen;
                bool left = false;
                for (int i = 0; i < block->GetOutputParameterCount(); ++i)
                    consumers(graph, block->GetOutputParameter(i), seen, next, left);
                if (left) return false;
                for (CKBehavior* n: next)
                    if (visited.insert(n).second) todo.push_back(n);
            }
            return reached_sound || !need_sound;
        }
    };
}

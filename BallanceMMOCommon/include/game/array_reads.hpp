#pragma once

// The array-read census (docs/record-format-gaps-design.md 2.2 and 7.2 S2;
// built read-only in SimTool at step K0, moved here at step RK2 so every
// world runs it at its anchor: the server's (physics_world), the headless
// client's (session_client) and the retail core host's (core_host); SimTool
// --array-reads prints it, sim/array_census.hpp).  One implementation: the
// census decides which cells the record's written-cell section carries
// (written_cells, bound by session/rollback_binding_scripts.hpp), and it
// classifies by that same table.
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
//               level file's data (step RK2 confirmed the class: no source
//               in the repository - BallanceMMO, the Ballanced submodule's
//               CK2, physics_RT and TT - names the two arrays it holds today,
//               Physicalize_Convex and Physicalize_Balls, so no native code
//               writes them);
//   private     the array is a session-private copy (a name with "BMMO_":
//               the ball identity copies "CurrentLevel#BMMOId_*" (step G1),
//               the checkpoint phase copy);
//   recorded    (array, row 0, column) is one of the record's three cells
//               (game/script_replay.hpp cell_of: CurrentLevel[0][4],
//               IngameParameter[0][1], IngameParameter[0][2]); or (step RK2,
//               the user's decision Q4 of 2026-10-24: "the written cells are
//               recorded with the other cells, no filtering") a column of
//               the written-cell section: a constant column that a selected
//               script writes with a column-confined writer (Set Cell, Column
//               Transform, Column Operation), of an int or float column type
//               - every row of it travels with the record (script_replay
//               kSectionCells), and every read of that column is recorded too;
//   sound       the block's behaviour links and parameter flow, followed to
//               the end of its graph, reach only sound blocks (Wave Player,
//               TT ProximityVolumeControl, ...), blocks on the same array and
//               pure flow blocks (Identity, Binary Switch, Op, Delayer, ...),
//               and no link or parameter leaves the start block's graph (an
//               exported output, a parameter owned outside the graph, a nested
//               graph's own output); a write is sound only when every read of
//               that array in the census is sound too (since RK2 the fan's
//               sound-slot blocks are recorded, so this class is empty on
//               L1-L13; it stays as the census's report of such a read);
//   collectible (step F4, the user's decision of 2026-10-29: collectibles are
//               per-world like the airship, neither recorded nor rolled back)
//               a read whose every in-play writer is a Set Cell of one
//               constant column in a script outside the selection: a
//               P_Extra_* script writing its own row, or Event_handler's level
//               reset - proven from the graph at the anchor
//               (session/collectible_rules.hpp; the sector manager's
//               PH[*][4] "Set?" read on L1-L13 and the arena).  A map that
//               breaks the rule keeps the read unrecorded, the note says why;
//   unrecorded  anything else.
//
// Scope.  Every world rewires its mechanism ball identity reads to private
// copies at the anchor under WORLD_PROXIMITY_UNION (game/proximity_union.hpp,
// steps G1 / G3 / WG4).  `client_scope` (SimTool's S2 control) classifies a
// read of an identity copy as a read of the array it was copied from - a
// world with the bit clear.
//
// Only the public CK2 API is used, and nothing is written: parameter values
// are read with update = FALSE, so no parameter operation runs.

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "CKAll.h"

#include <game/script_replay.hpp>
#include <game/written_cells_memo.hpp>
#include <session/collectible_rules.hpp>
#include <session/resync_rules.hpp>

namespace bmmo::game {
    // Step OPB: the memo's verify compares two walks' columns.
    inline bool operator==(const written_column& a, const written_column& b) {
        return a.array == b.array && a.column == b.column;
    }

    class array_reads {
    public:
        enum class access { read, write, both };
        enum class kind { recorded, private_copy, load_time, unwritten, sound, collectible, unrecorded };

        struct entry {
            CK_ID block = 0;
            CK_ID array_id = 0;          // the array the block works on (a copy's id in client scope)
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
            // The written-cell section's columns (written_cells), and their
            // names "array[*][column] (rows)" for the report.
            std::vector<written_column> written;
            std::vector<std::string> written_names;
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
                case kind::collectible: return "collectible";
                default: return "unrecorded";
            }
        }
        static const char* name_of(access a) {
            return a == access::read ? "read" : a == access::write ? "write" : "both";
        }

        // The record's written-cell section (step RK2, decision Q4): every
        // (array, constant column) a selected, not keep-local, script writes
        // with a column-confined writer, of an int or float column, that is
        // neither a private copy nor one of the three recorded cells.
        // Sorted by array id, then column.  Walks the selection only (not
        // the composition): cheap enough for every binding.
        static std::vector<written_column> written_cells(CKContext* context, const script_replay::selection& sel) {
            std::vector<entry> entries;
            int selected = 0, keep_local = 0, blocks = 0;
            collect(context, sel, false, entries, selected, keep_local, blocks, nullptr);
            return table_of(context, entries);
        }

        // Step OPB (rewind-cost-design R.4 row B, option B1): written_cells
        // memoized (game/written_cells_memo.hpp has the rules).  The watch
        // list the walk leaves: every column-confined writer block of the
        // selection's non-keep-local scripts (only such a block can put a
        // column into the section: table_of) with its row and column pin
        // indices, and every array those blocks name.  At each bind the key
        // is built again from it: each block re-fetched by id (another
        // object, or none: walk), the arrays its target and inputs resolve to
        // now (arrays_of: GetRealSource, then the value - a repointed or
        // rewired pin), its row and column pins' real sources and values, and
        // each array's object, name, column count and column types.
        struct cells_watch {
            struct block_watch {
                CK_ID id = 0;
                CKBehavior* block = nullptr;
                int row_pin = -1, column_pin = -1;
                // Whether the walk read the pin as a constant (constant_int:
                // a never-written int local of the block's script); only a
                // constant's value is in the key (a row an Iterator drives
                // changes every frame and never enters the section), its
                // real source always.
                bool row_constant = false, column_constant = false;
            };
            std::vector<block_watch> blocks;
            std::vector<std::pair<CK_ID, CKDataArray*>> arrays;
            size_t count_blocks = 0, count_pins = 0, count_dynamic = 0, count_arrays = 0;
        };
        using cells_memo = written_cells_memo<std::vector<written_column>, cells_watch>;

        static std::vector<written_column> written_cells(CKContext* context, const script_replay::selection& sel,
                                                         cells_memo& memo) {
            const auto probe = [context, &sel](const cells_watch& w, cells_memo_key& key) {
                return cells_key(context, sel, w, key);
            };
            const auto walk = [context, &sel](cells_watch* w, cells_memo_key* key) {
                std::vector<written_column> out = written_cells(context, sel);
                if (w && key) {
                    watch_of(context, sel, *w);
                    cells_key(context, sel, *w, *key);
                }
                return out;
            };
            const auto describe = [](const std::vector<written_column>& cells) {
                std::string s = "[";
                for (const written_column& c: cells)
                    s += (s.size() > 1 ? " " : "") + std::to_string(static_cast<unsigned>(c.array)) + ":"
                       + std::to_string(c.column);
                return s + "]";
            };
            return memo.get(probe, walk, describe);
        }

        static result run(CKContext* context, const script_replay::selection& sel, bool client_scope) {
            result out;
            if (!context) return out;
            collect(context, sel, client_scope, out.entries, out.selected, out.keep_local, out.blocks, &out.kept);
            // Decision Q4 (step RK2): the written-cell section's columns are
            // recorded, the writes and every read of the column.
            out.written = table_of(context, out.entries);
            for (const written_column& w: out.written) {
                auto* a = CKDataArray::Cast(context->GetObject(w.array));
                out.written_names.push_back(std::string(a && a->GetName() ? a->GetName() : "?") + "[*]["
                                            + std::to_string(w.column) + "] (" + std::to_string(a ? a->GetRowCount() : 0)
                                            + " rows)");
            }
            for (entry& e: out.entries) {
                if (e.what != kind::unrecorded || e.column < 0) continue;
                for (const written_column& w: out.written)
                    if (w.array == e.array_id && w.column == e.column) {
                        e.what = kind::recorded;
                        e.note = "(written-cell section)";
                    }
            }
            // A level table: a read of an array whose every writer block, in
            // any script of the composition, is in a load-time script reads
            // what the level's loading left; one that no block writes is
            // `unwritten` (the level file's data, unless a native writer the
            // census cannot see writes it).  Column-aware: a writer of
            // another constant column does not count (array_writers gives
            // every row-moving writer column "*").
            //
            // Step F4: a read whose in-play writers are only per-world
            // collectible state (session/collectible_rules.hpp) is
            // `collectible`.  Only here, on reads, after table_of: the
            // written-cell section (and with it the record) is the first
            // pass's and cannot change by this class.
            const std::map<std::string, std::vector<writer>> writers = array_writers(context);
            for (entry& e: out.entries) {
                if (e.what != kind::unrecorded || e.how != access::read) continue;
                auto it = writers.find(e.array);
                std::map<std::string, int> relevant;
                std::vector<bmmo::session::collectible_writer> described;
                bool load_only = true;
                if (it != writers.end())
                    for (const writer& w: it->second) {
                        if (e.column >= 0 && w.column >= 0 && w.column != e.column) continue;
                        ++relevant[w.root + " [" + w.prototype + "] col " + (w.column < 0 ? std::string("*") : std::to_string(w.column))];
                        load_only = load_only && load_time_script(w.root);
                        bmmo::session::collectible_writer d;
                        d.root = w.root;
                        d.prototype = w.prototype;
                        d.column = w.column;
                        d.load_time = load_time_script(w.root);
                        d.selected = std::find(sel.scripts.begin(), sel.scripts.end(), w.root_id) != sel.scripts.end();
                        d.row = w.row;
                        d.key_column = w.key_column;
                        described.push_back(d);
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
                    int column = -1;
                    const std::string why = bmmo::session::collectible_verdict(described, column);
                    if (why.empty()) {
                        e.what = kind::collectible;
                        e.note += " (per-world collectible state: column " + std::to_string(column)
                                  + " written in play only by P_Extra_* scripts on their own rows and Event_handler's"
                                    " level reset, all outside the selection)";
                    } else {
                        e.note += " (not per-world collectible state: " + why + ")";
                    }
                }
            }
            // Sound, reads first: a write is sound only if every read of its
            // array is.  Arrays whose reads are all sound:
            std::map<std::string, bool> sound_array;
            for (entry& e: out.entries) {
                // A per-world collectible read is not a sound read: it keeps
                // its array's writes out of `sound`, as it did while it was
                // still counted unrecorded (before step F4).
                if (e.what == kind::collectible && e.how != access::write) {
                    sound_array.emplace(e.array, false).first->second = false;
                    continue;
                }
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

        // The report, one line per element: the count per class, the
        // keep-local scripts, one line per entry (unrecorded, sound and
        // collectible always, the rest with `all`), the written-cell section, the
        // unrecorded entries grouped by script, prototype and cell, and the
        // closing ARRAY-READS line.  SimTool prints it (sim/array_census.hpp);
        // the hosts log it at their anchor.
        static std::vector<std::string> lines(const result& r, int level, bool client_scope, bool all) {
            std::vector<std::string> out;
            const char* scope = client_scope ? "client" : "server";
            out.push_back(textf("array-reads level=%d scope=%s selected=%d keep_local=%d blocks=%d entries=%zu"
                                " recorded=%d private=%d load-time=%d unwritten=%d sound=%d collectible=%d unrecorded=%d",
                                level, scope, r.selected, r.keep_local, r.blocks, r.entries.size(),
                                r.count(kind::recorded), r.count(kind::private_copy), r.count(kind::load_time),
                                r.count(kind::unwritten), r.count(kind::sound), r.count(kind::collectible),
                                r.count(kind::unrecorded)));
            for (const std::string& k: r.kept) out.push_back("array-reads keep-local " + k);
            for (const entry& e: r.entries) {
                if (!all && e.what != kind::unrecorded && e.what != kind::sound && e.what != kind::collectible) continue;
                out.push_back(std::string("array-read ") + name_of(e.what) + " " + name_of(e.how) + " " + e.owner + "/"
                              + e.script + " #" + std::to_string(e.walk) + " [" + e.prototype + "] " + cell_text(e)
                              + " id=" + std::to_string(static_cast<unsigned>(e.block)) + " path=" + e.path
                              + (e.note.empty() ? "" : " ") + e.note);
            }
            for (const std::string& w: r.written_names) out.push_back("array-reads written-cells " + w);
            std::map<std::string, int> groups;
            for (const entry& e: r.entries) {
                if (e.what != kind::unrecorded) continue;
                ++groups[script_family(e.script) + " " + e.prototype + " " + name_of(e.how) + " " + cell_text(e)];
            }
            for (const auto& [what, n]: groups) out.push_back("array-reads unrecorded " + what + " x" + std::to_string(n));
            out.push_back(textf("ARRAY-READS: level=%d scope=%s unrecorded=%d sound=%d unwritten=%d written_columns=%zu"
                                " collectible=%d",
                                level, scope, r.count(kind::unrecorded), r.count(kind::sound), r.count(kind::unwritten),
                                r.written.size(), r.count(kind::collectible)));
            return out;
        }

        // The anchor's census in a world (design 2.2 "Placement", step RK2):
        // after the world's rewire and placement, the report's lines
        // (unrecorded, sound and collectible entries by name, the written-cell section,
        // the closing line) to `log`, and the counts the host's status line
        // carries.  Every world calls this one function: the server
        // (physics_world), the headless client (session_client), the retail
        // core host (core_host).
        struct anchor_summary {
            int unrecorded = -1;          // -1: no census ran (no selection)
            int sound = 0;
            size_t written_columns = 0;
        };
        static anchor_summary anchor_log(CKContext* context, const script_replay::selection& sel, int level,
                                         const std::function<void(const std::string&)>& log) {
            anchor_summary out;
            if (!context || sel.scripts.empty()) return out;
            const result r = run(context, sel, false);
            if (log)
                for (const std::string& line: lines(r, level, false, false)) log(line);
            out.unrecorded = r.count(kind::unrecorded);
            out.sound = r.count(kind::sound);
            out.written_columns = r.written.size();
            return out;
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
        static std::string textf(const char* format, ...) {
            char buffer[512];
            va_list args;
            va_start(args, format);
            const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return std::string(buffer, n < 0 ? 0 : (n >= static_cast<int>(sizeof(buffer)) ? sizeof(buffer) - 1 : static_cast<size_t>(n)));
        }

        // The walk of the selection: one entry per (block, array), the
        // first classification (load-time by root, private, the three cells).
        static void collect(CKContext* context, const script_replay::selection& sel, bool client_scope,
                            std::vector<entry>& entries, int& selected, int& keep_local, int& blocks,
                            std::vector<std::string>* kept) {
            if (!context) return;
            for (CK_ID id: sel.scripts) {
                auto* script = CKBehavior::Cast(context->GetObject(id));
                if (!script) continue;
                CKBeObject* owner = script->GetOwner();
                const std::string owner_name = owner && owner->GetName() ? owner->GetName() : "";
                const std::string script_name = script->GetName() ? script->GetName() : "";
                if (bmmo::session::keep_local_script(owner_name, script_name)) {
                    ++keep_local;
                    if (kept) kept->push_back(owner_name + "/" + script_name);
                    continue;
                }
                ++selected;
                const bool init = script_name.find("_Init") != std::string::npos;
                int walk_index = 0;
                walk(script, script->GetName() ? script->GetName() : "?", [&](CKBehavior* block, const std::string& path) {
                    const int position = walk_index++;
                    ++blocks;
                    for (CKDataArray* array: arrays_of(context, block)) {
                        entry e;
                        e.block = block->GetID();
                        e.array_id = array->GetID();
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
                        entries.push_back(e);
                    }
                });
            }
        }

        // Step OPB: the memo's watch list (cells_watch) from the selection.
        static void watch_of(CKContext* context, const script_replay::selection& sel, cells_watch& w) {
            w = cells_watch{};
            if (!context) return;
            CKParameterManager* pm = context->GetParameterManager();
            for (CK_ID id: sel.scripts) {
                auto* script = CKBehavior::Cast(context->GetObject(id));
                if (!script) continue;
                CKBeObject* owner = script->GetOwner();
                const std::string owner_name = owner && owner->GetName() ? owner->GetName() : "";
                const std::string script_name = script->GetName() ? script->GetName() : "";
                if (bmmo::session::keep_local_script(owner_name, script_name)) continue;
                walk(script, std::string(), [&](CKBehavior* block, const std::string&) {
                    if (!column_confined(prototype_of(block))) return;
                    cells_watch::block_watch b;
                    b.id = block->GetID();
                    b.block = block;
                    b.row_pin = row_pin(block);
                    b.column_pin = column_pin(block);
                    int unused = -1;
                    b.row_constant = constant_int(block, b.row_pin, unused);
                    b.column_constant = constant_int(block, b.column_pin, unused);
                    w.blocks.push_back(b);
                    // The pins re-resolved at every bind (arrays_of: the
                    // target and every input), and of those the ones whose
                    // object value can change at run time: an object-typed
                    // real source that is not a local of this script no
                    // block or operation of it writes.
                    const auto count = [&](CKParameterIn* in) {
                        if (!in) return;
                        ++w.count_pins;
                        CKParameter* source = in->GetRealSource();
                        if (!source || !pm || !pm->IsDerivedFrom(source->GetGUID(), CKPGUID_OBJECT)) return;
                        bool fixed = source->GetClassID() == CKCID_PARAMETERLOCAL;
                        if (fixed) {
                            CKBehavior* root = block;
                            while (root->GetParent()) root = root->GetParent();
                            fixed = !written(root, source);
                        }
                        if (!fixed) ++w.count_dynamic;
                    };
                    count(block->GetTargetParameter());
                    const int inputs = block->GetInputParameterCount();
                    for (int i = 0; i < inputs; ++i) count(block->GetInputParameter(i));
                    for (CKDataArray* a: arrays_of(context, block)) {
                        const std::pair<CK_ID, CKDataArray*> key(a->GetID(), a);
                        if (std::find(w.arrays.begin(), w.arrays.end(), key) == w.arrays.end()) w.arrays.push_back(key);
                    }
                });
            }
            w.count_blocks = w.blocks.size();
            w.count_arrays = w.arrays.size();
        }

        // Step OPB: the memo's key now, from the watch list; false when a
        // watched block no longer resolves to the object the walk saw.
        static bool cells_key(CKContext* context, const script_replay::selection& sel, const cells_watch& w,
                              cells_memo_key& key) {
            key.clear();
            if (!context) return false;
            const auto word = [](const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); };
            key.scripts.reserve(sel.scripts.size() * 2);
            for (CK_ID id: sel.scripts) {
                key.scripts.push_back(id);
                key.scripts.push_back(word(context->GetObject(id)));
            }
            for (const cells_watch::block_watch& b: w.blocks) {
                if (context->GetObject(b.id) != b.block) return false;
                const std::vector<CKDataArray*> arrays = arrays_of(context, b.block);
                key.pins.push_back(b.id);
                key.pins.push_back(arrays.size());
                for (CKDataArray* a: arrays) key.pins.push_back(a->GetID());
                const std::pair<int, bool> pins[2] = {{b.row_pin, b.row_constant}, {b.column_pin, b.column_constant}};
                for (const auto& [pin, constant]: pins) {
                    CKParameterIn* in = pin >= 0 ? b.block->GetInputParameter(pin) : nullptr;
                    CKParameter* source = in ? in->GetRealSource() : nullptr;
                    key.pins.push_back(word(source));
                    int value = 0;
                    if (constant && source && source->GetDataSize() == static_cast<int>(sizeof(int)))
                        source->GetValue(&value, FALSE);
                    key.pins.push_back(static_cast<uint32_t>(value));
                }
            }
            for (const auto& [id, at]: w.arrays) {
                auto* a = CKDataArray::Cast(context->GetObject(id));
                key.layout.push_back(id);
                key.layout.push_back(word(a));
                if (!a || a != at) continue;
                const int columns = a->GetColumnCount();
                key.layout.push_back(static_cast<uint32_t>(columns));
                for (int c = 0; c < columns; ++c) key.layout.push_back(static_cast<uint32_t>(a->GetColumnType(c)));
                key.names.push_back(a->GetName() ? a->GetName() : "");
            }
            return true;
        }

        // The written-cell section's columns of a walk (see written_cells).
        static std::vector<written_column> table_of(CKContext* context, const std::vector<entry>& entries) {
            std::vector<written_column> out;
            for (const entry& e: entries) {
                if (e.what != kind::unrecorded || e.how == access::read || e.column < 0) continue;
                if (!column_confined(e.prototype)) continue;
                auto* a = CKDataArray::Cast(context->GetObject(e.array_id));
                if (!a || e.column >= a->GetColumnCount()) continue;
                const CK_ARRAYTYPE type = a->GetColumnType(e.column);
                if (type != CKARRAYTYPE_INT && type != CKARRAYTYPE_FLOAT) continue;
                written_column w;
                w.array = e.array_id;
                w.column = e.column;
                if (std::find_if(out.begin(), out.end(), [&w](const written_column& x) {
                        return x.array == w.array && x.column == w.column; }) == out.end())
                    out.push_back(w);
            }
            std::sort(out.begin(), out.end(), [](const written_column& a, const written_column& b) {
                return a.array != b.array ? a.array < b.array : a.column < b.column;
            });
            return out;
        }

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
        // Step F4: a Set Cell also carries where its row index comes from
        // (row_source_of), for the collectible rule.
        struct writer {
            std::string root, prototype;
            int column = -1;
            CK_ID root_id = 0;
            bmmo::session::collectible_row_source row = bmmo::session::collectible_row_source::other;
            int key_column = -1;
        };
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
                w.root_id = root->GetID();
                w.prototype = prototype_of(block);
                if (column_confined(w.prototype)) constant_int(block, column_pin(block), w.column);
                for (CKDataArray* a: arrays) {
                    writer x = w;
                    if (x.prototype == "Set Cell") row_source_of(context, block, root, a, x.row, x.key_column);
                    out[a->GetName() ? a->GetName() : "?"].push_back(x);
                }
            }
            return out;
        }

        // Where a Set Cell's row index comes from (step F4, the collectible
        // rule): a constant local; the Row Index output of an `Iterator` of
        // the same root script over every row (Starting Row 0, Ending Row -1)
        // of `array`; or the Index output of an `Iterator If` of the same
        // root on `array` whose Column is a constant (`key_column`), whose
        // Operator is a constant `equal` and whose Reference Value is a
        // constant local holding the root script's owner.  Anything else is
        // `other`.
        static void row_source_of(CKContext* context, CKBehavior* block, CKBehavior* root, CKDataArray* array,
                                  bmmo::session::collectible_row_source& row, int& key_column) {
            using rs = bmmo::session::collectible_row_source;
            row = rs::other;
            key_column = -1;
            const int pin = row_pin(block);
            if (pin < 0) return;
            CKParameterIn* in = block->GetInputParameter(pin);
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            if (!source) return;
            int value = -1;
            if (source->GetClassID() == CKCID_PARAMETERLOCAL) {
                if (constant_int(block, pin, value)) row = rs::constant;
                return;
            }
            if (source->GetClassID() != CKCID_PARAMETEROUT) return;
            CKBehavior* it = CKBehavior::Cast(source->GetOwner());
            if (!it || !it->IsUsingFunction() || it->GetOutputParameterCount() < 1
                || it->GetOutputParameter(0) != source)
                return;
            CKBehavior* it_root = it;
            while (it_root->GetParent()) it_root = it_root->GetParent();
            if (it_root != root) return;
            const std::vector<CKDataArray*> on = arrays_of(context, it);
            if (std::find(on.begin(), on.end(), array) == on.end()) return;
            const std::string p = prototype_of(it);
            if (p == "Iterator") {
                int first = -1, last = 0;
                if (constant_int(it, pin_named(it, {"Starting Row"}), first) && first == 0
                    && constant_int(it, pin_named(it, {"Ending Row"}), last) && last == -1)
                    row = rs::iterator_all_rows;
                return;
            }
            if (p != "Iterator If") return;
            int key = -1, op = 0;
            if (!constant_int(it, pin_named(it, {"Column"}), key) || key < 0) return;
            if (!constant_word(it, pin_named(it, {"Operator"}), op) || op != CKEQUAL) return;
            CKObject* owner = root->GetOwner();
            CK_ID reference = 0;
            if (!owner || !constant_word(it, pin_named(it, {"Reference Value"}), reference)
                || reference != owner->GetID())
                return;
            row = rs::iterator_if_owner;
            key_column = key;
        }

        // A pin whose source is a 4-byte local (any type: an enum, an object
        // id) that no output parameter of the block's root script writes:
        // its value.  constant_int's rule without the int type.
        template <typename T> static bool constant_word(CKBehavior* block, int pin, T& value) {
            static_assert(sizeof(T) == 4, "a 4-byte value");
            if (pin < 0) return false;
            CKParameterIn* in = block->GetInputParameter(pin);
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            if (!source || source->GetClassID() != CKCID_PARAMETERLOCAL) return false;
            if (source->GetDataSize() != 4) return false;
            CKBehavior* root = block;
            while (root->GetParent()) root = root->GetParent();
            if (written(root, source)) return false;
            T v{};
            if (source->GetValue(&v, FALSE) != CK_OK) return false;
            value = v;
            return true;
        }

        static kind classify(entry& e, bool client_scope) {
            // The ball identity copies: "CurrentLevel#BMMOId_<owner>_<n>"
            // since step G1 (game/proximity_union.hpp), "CurrentLevel_BMMO_Ball_N" before.
            size_t copy = e.array.find("#BMMOId_");
            if (copy == std::string::npos) copy = e.array.find("_BMMO_Ball_");
            if (client_scope && copy != std::string::npos) {
                // A world with the world-agreement bit clear has no such
                // copy: its block reads the array the bit's world copied.
                e.note = "(server reads " + e.array + ")";
                e.array.resize(copy);
            } else if (e.array.find("BMMO_") != std::string::npos || e.array.find("#BMMOId_") != std::string::npos) {
                return kind::private_copy;
            }
            for (int c = 0; c < script_replay::kCellCount; ++c) {
                const char* array = nullptr;
                int row = 0, column = 0;
                script_replay::cell_of(c, array, row, column);
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

#pragma once

// The sector placement's CK half (docs/sector-placement-design.md 2.3).
// Step S1, read-only: reading the PH table into placement rows (2.3 step 1)
// and finding the level builder's Object Copy block with its dependency words
// (2.3 step 2).  Step S2 (the class sector_placement at the end of the file):
// the apply (copy, rename, inert state, initial values, attributes, groups,
// PH rewrite, all or nothing), the signature, holds_copies and release.
// Public CK2 API only, like sector_activation.hpp: it compiles against the
// retail Virtools SDK and Ballanced.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "CKAll.h"

#include <entity/session.hpp>
#include <physics/physics_rt_api.h>
#include <session/placement_plan.hpp>

#include "menu_driver.hpp"
#include "script_replay.hpp"

namespace bmmo::game {

    // The PH table at the anchor: exactly the five columns the level builder
    // makes (Sector int, Type Name string, Matrix matrix parameter, Object
    // object, Set? bool parameter), in that order; anything else is refused
    // (the empty string means the rows were read).
    inline std::string read_placement_rows(CKContext* context, std::vector<session::placement_row>& rows) {
        rows.clear();
        auto* ph = CKDataArray::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>("PH"), CKCID_DATAARRAY, nullptr));
        if (!ph) return "no data array named PH";
        struct column { const char* name; CK_ARRAYTYPE type; bool matrix, boolean; };
        static const column expected[] = {
            {"Sector", CKARRAYTYPE_INT, false, false},
            {"Type Name", CKARRAYTYPE_STRING, false, false},
            {"Matrix", CKARRAYTYPE_PARAMETER, true, false},
            {"Object", CKARRAYTYPE_OBJECT, false, false},
            {"Set?", CKARRAYTYPE_PARAMETER, false, true},
        };
        if (ph->GetColumnCount() != 5)
            return "PH has " + std::to_string(ph->GetColumnCount()) + " columns, expected 5";
        for (int c = 0; c < 5; ++c) {
            const char* name = ph->GetColumnName(c);
            if (!name || std::strcmp(name, expected[c].name) != 0)
                return "PH column " + std::to_string(c) + " is " + (name ? name : "(unnamed)") + ", expected "
                     + expected[c].name;
            if (ph->GetColumnType(c) != expected[c].type)
                return std::string("PH column ") + expected[c].name + " has type "
                     + std::to_string(static_cast<int>(ph->GetColumnType(c)));
            if (expected[c].matrix && ph->GetColumnParameterGuid(c) != CKPGUID_MATRIX)
                return std::string("PH column ") + expected[c].name + " is not a matrix parameter";
            if (expected[c].boolean && ph->GetColumnParameterGuid(c) != CKPGUID_BOOL)
                return std::string("PH column ") + expected[c].name + " is not a bool parameter";
        }
        const int count = ph->GetRowCount();
        rows.reserve(static_cast<size_t>(count));
        std::map<std::string, CK_ID> ids_by_name;
        for (int r = 0; r < count; ++r) {
            session::placement_row row;
            row.row = r;
            int sector = 0;
            if (!ph->GetElementValue(r, 0, &sector)) return "PH row " + std::to_string(r) + ": no Sector";
            row.sector = sector;
            // The STRING cell holds the string's pointer in both SDKs
            // (GetElement is int-indexed CKDWORD* on retail, size_t-indexed
            // CKUINTPTR* in Ballanced); read it whole, with no length cap,
            // since the type is hashed into the placement signature.
            auto* type_cell = ph->GetElement(r, 1);
            const char* type = type_cell ? reinterpret_cast<const char*>(*type_cell) : nullptr;
            row.type = type ? type : "";
            VxMatrix m;
            if (!ph->GetElementValue(r, 2, &m)) return "PH row " + std::to_string(r) + ": no Matrix";
            static_assert(sizeof(VxMatrix) == sizeof(row.matrix), "VxMatrix is 16 floats");
            std::memcpy(row.matrix, &m, sizeof(row.matrix));
            CKObject* object = ph->GetElementObject(r, 3);
            if (object) row.object = object->GetName() ? object->GetName() : "";
            if (object && row.object.empty()) return "PH row " + std::to_string(r) + " names an unnamed object";
            // The plan keys an object by its name, so one name must mean one
            // object: two rows naming different objects that share a name
            // are refused (fail closed; the apply finds sources by name).
            if (object) {
                auto [it, fresh] = ids_by_name.emplace(row.object, object->GetID());
                if (!fresh && it->second != object->GetID())
                    return "PH row " + std::to_string(r) + " names " + row.object
                         + ", a different object from an earlier row's of the same name";
            }
            rows.push_back(std::move(row));
        }
        return {};
    }

    // An Object Copy block of the level builder (Levelinit_build), with what
    // decides whether it is the PH copier: its parent graph and the block its
    // output 0 is linked to.
    struct builder_copy_block {
        CKBehavior* block = nullptr;
        std::string graph;          // the parent graph's name
        std::string out_target;     // the block output 0 is linked to ("" = none)
        bool chosen = false;        // parent "Copy Objects" and output 0 -> "TT Save IC"
    };

    namespace placement_detail {
        inline CKGUID object_copy_guid() { return CKGUID(0x3f6b0ac7, 0x47d20f78); }

        inline void collect_object_copies(CKBehavior* parent, std::vector<builder_copy_block>& out) {
            if (!parent) return;
            const int count = parent->GetSubBehaviorCount();
            for (int i = 0; i < count; ++i) {
                CKBehavior* sub = parent->GetSubBehavior(i);
                if (!sub) continue;
                if (sub->IsUsingFunction() && sub->GetPrototypeGuid() == object_copy_guid()) {
                    builder_copy_block b;
                    b.block = sub;
                    b.graph = parent->GetName() ? parent->GetName() : "";
                    CKBehaviorIO* out0 = sub->GetOutputCount() > 0 ? sub->GetOutput(0) : nullptr;
                    for (int l = 0; out0 && l < parent->GetSubBehaviorLinkCount(); ++l) {
                        CKBehaviorLink* link = parent->GetSubBehaviorLink(l);
                        if (!link || link->GetInBehaviorIO() != out0 || !link->GetOutBehaviorIO()) continue;
                        CKBehavior* to = link->GetOutBehaviorIO()->GetOwner();
                        if (!b.out_target.empty()) b.out_target += ",";
                        b.out_target += to && to->GetName() ? to->GetName() : "?";
                        if (to && to->GetName() && std::strcmp(to->GetName(), "TT Save IC") == 0
                                && b.graph == "Copy Objects")
                            b.chosen = true;
                    }
                    out.push_back(std::move(b));
                }
                collect_object_copies(sub, out);
            }
        }
    }

    // Every Object Copy block of Levelinit_build in graph order, the PH
    // copier marked `chosen` (design 2.3 step 2).  `error` is set unless
    // exactly one is chosen; it names every block found with its graph.
    inline std::vector<builder_copy_block> find_builder_copy_blocks(CKContext* context, std::string& error) {
        std::vector<builder_copy_block> blocks;
        error.clear();
        CKBehavior* build = find_root_script(context, "Levelinit_build");
        if (!build) { error = "no root script named Levelinit_build"; return blocks; }
        placement_detail::collect_object_copies(build, blocks);
        size_t chosen = 0;
        for (const builder_copy_block& b: blocks) chosen += b.chosen ? 1 : 0;
        if (chosen != 1) {
            error = std::to_string(chosen) + " Object Copy blocks of Levelinit_build match Copy Objects -> TT Save IC"
                    " (expected 1); found:";
            for (const builder_copy_block& b: blocks) error += " " + b.graph + "/Object Copy -> " + b.out_target + ";";
        }
        return blocks;
    }

    // The dependencies the block passes to CopyObjects: its input 0 read the
    // way the block reads it (ObjectCopier.cpp: *(CKDependencies**) of the
    // read data pointer, following the link to Copy Objects' local).  Null if
    // the parameter holds none.
    inline CKDependencies* builder_copy_dependencies(CKBehavior* block) {
        if (!block || block->GetInputParameterCount() < 1) return nullptr;
        void* data = block->GetInputParameterReadDataPtr(0);
        return data ? *static_cast<CKDependencies**>(data) : nullptr;
    }

    // One line per class: the dependency words of `deps` for the classes the
    // placement cares about (design 2.3 step 2), each with its meaning, the
    // Object Copy block's own settings, and the default copy words for
    // comparison.  With m_Flags NONE or FULL the per-class words do not apply
    // (CKDependenciesContext::GetClassDependencies returns 0 or 0xFFFF).
    inline std::string describe_copy_dependencies(CKBehavior* block) {
        std::string out;
        char line[256];
        CKBOOL dynamic = TRUE, awake = TRUE;
        if (block) {
            block->GetLocalParameterValue(0, &dynamic);
            if (block->GetLocalParameterCount() > 1) block->GetLocalParameterValue(1, &awake);
            std::snprintf(line, sizeof(line), "object copy settings: version=0x%08x Dynamic=%d Awake Object=%d%s\n",
                          static_cast<unsigned>(block->GetVersion()), dynamic ? 1 : 0, awake ? 1 : 0,
                          block->GetVersion() < 0x00020000 ? " (version < 2: activates regardless)" : "");
            out += line;
        }
        CKDependencies* deps = builder_copy_dependencies(block);
        CKDependencies* defaults = CKGetDefaultClassDependencies(CK_DEPENDENCIES_COPY);
        if (!deps) {
            out += "dependency words: none (the parameter holds no CKDependencies; CopyObjects uses the defaults)\n";
        } else {
            const unsigned flags = static_cast<unsigned>(deps->m_Flags);
            std::snprintf(line, sizeof(line), "dependency words: m_Flags=%u (%s) size=%d\n", flags,
                          flags & CK_DEPENDENCIES_NONE ? "NONE: every class 0"
                          : flags & CK_DEPENDENCIES_FULL ? "FULL: every class 0xFFFF" : "CUSTOM: per class",
                          deps->Size());
            out += line;
        }
        struct cls { CK_CLASSID cid; const char* name; const char* bits; };
        static const cls classes[] = {
            {CKCID_OBJECT, "OBJECT", "1 NAME, 2 UNIQUENAME"},
            {CKCID_SCENEOBJECT, "SCENEOBJECT", "1 SCENES"},
            {CKCID_BEOBJECT, "BEOBJECT", "1 SCRIPTS, 2 ATTRIBUTES, 4 GROUPS"},
            {CKCID_3DENTITY, "3DENTITY", "1 MESH, 2 CHILDREN, 4 ANIMATIONS"},
            {CKCID_3DOBJECT, "3DOBJECT", "(no own bits)"},
            {CKCID_BEHAVIOR, "BEHAVIOR", "(no own bits)"},
            {CKCID_MESH, "MESH", "1 MATERIAL (other bits unnamed)"},
            {CKCID_MATERIAL, "MATERIAL", "1 TEXTURE"},
            {CKCID_GROUP, "GROUP", "1 OBJECTS"},
        };
        for (const cls& c: classes) {
            std::string word = "-";
            if (deps) {
                const unsigned flags = static_cast<unsigned>(deps->m_Flags);
                if (flags & CK_DEPENDENCIES_NONE) word = "0 (NONE)";
                else if (flags & CK_DEPENDENCIES_FULL) word = "0xffff (FULL)";
                else if (static_cast<int>(c.cid) < deps->Size()) {
                    std::snprintf(line, sizeof(line), "0x%x", static_cast<unsigned>((*deps)[static_cast<unsigned>(c.cid)]));
                    word = line;
                } else word = "out of range";
            }
            std::string def = "-";
            if (defaults && static_cast<int>(c.cid) < defaults->Size()) {
                std::snprintf(line, sizeof(line), "0x%x", static_cast<unsigned>((*defaults)[static_cast<unsigned>(c.cid)]));
                def = line;
            }
            std::snprintf(line, sizeof(line), "  class %-11s cid=%-3d builder=%-14s default=%-6s bits: %s\n", c.name,
                          static_cast<int>(c.cid), word.c_str(), def.c_str(), c.bits);
            out += line;
        }
        if (deps && !(static_cast<unsigned>(deps->m_Flags) & (CK_DEPENDENCIES_NONE | CK_DEPENDENCIES_FULL))) {
            std::string nonzero;
            for (int c = 0; c < deps->Size(); ++c)
                if (const CKDWORD w = (*deps)[static_cast<unsigned>(c)]) {
                    const char* name = CKClassIDToString(static_cast<CK_CLASSID>(c));
                    nonzero += (nonzero.empty() ? "" : " ") + std::string(name ? name : "?") + "(" + std::to_string(c)
                             + ")=" + std::to_string(w);
                }
            out += "  every nonzero builder word: " + (nonzero.empty() ? std::string("none") : nonzero) + "\n";
        }
        return out;
    }

    // ------------------------------------------------------------ step S2

    // What the placement cannot see through CK (design 2.3 step 3): the
    // host's physics.  An unset hook is not checked (the log says so).
    struct placement_hooks {
        std::function<bool(CK3dEntity* entity)> has_body;      // the entity has a physics body
        std::function<bool(CKBehavior* block)> force_exists;   // a SetPhysicsForce block's controller exists
        // Test hook (SimTool --placement-fail-at, design 7.2 V12): step 5
        // refuses at this row, after the row's copy was made.  -1: none.
        int fail_at_row = -1;
    };

    namespace placement_detail {
        inline std::string name_of(CKObject* o) { return o && o->GetName() ? o->GetName() : ""; }

        inline CKDataArray* ph_array(CKContext* context) {
            return CKDataArray::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>("PH"), CKCID_DATAARRAY, nullptr));
        }

        // The root, then its children in HierarchyParser order (TT Save IC's
        // walk, TT_Toolbox_RT SaveIC.cpp).
        inline std::vector<CK3dEntity*> hierarchy_of(CK3dEntity* root) {
            std::vector<CK3dEntity*> out;
            if (!root) return out;
            out.push_back(root);
            for (CK3dEntity* child = root->HierarchyParser(nullptr); child; child = root->HierarchyParser(child))
                out.push_back(child);
            return out;
        }

        inline std::set<CK_ID> all_object_ids(CKContext* context) {
            std::set<CK_ID> ids;
            const XObjectPointerArray& all = context->GetObjectListByType(CKCID_OBJECT, TRUE);
            for (int i = 0; i < all.Size(); ++i)
                if (CKObject* o = all[i]) ids.insert(o->GetID());
            return ids;
        }

        template <typename F> void walk_blocks(CKBehavior* beh, F&& fn) {
            if (!beh) return;
            fn(beh);
            if (beh->IsUsingFunction()) return;
            const int children = beh->GetSubBehaviorCount();
            for (int i = 0; i < children; ++i) walk_blocks(beh->GetSubBehavior(i), fn);
        }


        // A copy's name: "<name>_R<digits>" (session::placement_suffix).
        inline bool has_row_suffix(const std::string& name) {
            const size_t at = name.rfind("_R");
            if (at == std::string::npos || at + 2 >= name.size()) return false;
            for (size_t k = at + 2; k < name.size(); ++k)
                if (name[k] < '0' || name[k] > '9') return false;
            return true;
        }

        inline std::vector<std::string> attribute_names(CKContext* context, CKBeObject* o) {
            std::vector<std::string> out;
            CKAttributeManager* am = context->GetAttributeManager();
            for (int k = 0; o && am && k < o->GetAttributeCount(); ++k) {
                const char* n = am->GetAttributeNameByType(o->GetAttributeType(k));
                out.emplace_back(n ? n : "#" + std::to_string(o->GetAttributeType(k)));
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        // The fingerprint of an object's hierarchy (design 2.2 "Signature"):
        // one entity_print per 3D entity of a 3D entity's hierarchy; another
        // BeObject prints itself (no mesh), any other object its name and
        // class only.
        inline std::vector<session::entity_print> prints_of(CKContext* context, CKObject* object) {
            std::vector<session::entity_print> out;
            if (!object) return out;
            const auto print = [context](CKObject* o) {
                session::entity_print p;
                p.name = name_of(o);
                p.class_id = static_cast<int>(o->GetClassID());
                if (CK3dEntity* e = CK3dEntity::Cast(o))
                    if (CKMesh* mesh = e->GetCurrentMesh()) p.mesh = name_of(mesh);
                if (CKBeObject* b = CKBeObject::Cast(o)) {
                    p.scripts = b->GetScriptCount();
                    p.attributes = attribute_names(context, b);
                }
                return p;
            };
            if (CK3dEntity* root = CK3dEntity::Cast(object)) {
                for (CK3dEntity* e: hierarchy_of(root)) out.push_back(print(e));
            } else {
                out.push_back(print(object));
            }
            return out;
        }

        inline std::string describe_prints(std::vector<session::entity_print> prints) {
            std::sort(prints.begin(), prints.end(), [](const session::entity_print& a, const session::entity_print& b) {
                return a.name < b.name;
            });
            std::string out;
            for (const session::entity_print& p: prints) {
                out += (out.empty() ? "" : " ") + p.name + "{cid " + std::to_string(p.class_id) + ", mesh "
                     + (p.mesh.empty() ? "-" : p.mesh) + ", scripts " + std::to_string(p.scripts) + ", attrs [";
                for (size_t k = 0; k < p.attributes.size(); ++k) out += (k ? "," : "") + p.attributes[k];
                out += "]}";
            }
            return out;
        }
    }

    // The sector placement of one world (docs/sector-placement-design.md
    // 2.3): `apply` gives every PH row whose object an earlier row placed at
    // another matrix its own copy of that object's hierarchy, all or nothing;
    // `signature` names the PH table as it stands; the registry lists the
    // copies THIS object made, which `release` takes back.
    class sector_placement {
    public:
        struct copy_record {
            CK_ID root = 0;                 // the copy's root
            std::string name;               // its name (the plan's)
            int row = 0;
            std::string source;             // the row's object before the copy
            CK_ID source_id = 0;
            size_t entities = 0;            // 3D entities renamed (the hierarchy)
            // Every object the copy made (ids with their class, so a release
            // never destroys a reused id of another class), the census of
            // them by class, the groups and attribute types the copy holds,
            // and its hierarchy's fingerprint.
            std::vector<std::pair<CK_ID, CK_CLASSID>> objects;
            std::string census, groups, attributes, fingerprint;
        };
        struct row_record { int row = 0; CK_ID source = 0; std::string source_name; };

        // Runs the plan on this world's PH table, all or nothing.  Empty
        // string: done (or nothing to do); otherwise why, the world is as it
        // was before the call (every row put back, every object the call made
        // destroyed), and the host must not anchor.
        std::string apply(CKContext* context, const placement_hooks& hooks = {});
        // The signature of the PH table as it stands (design 2.2); every host
        // calls it at every anchor, placement on or off.  0 with `error` set
        // when the table cannot be read.
        uint64_t signature(CKContext* context, std::string* error = nullptr) const;
        // True if a PH row names an object whose name ends in _R<digits> or
        // this object's registry is not empty (design 2.4, option off);
        // `why` names the first such row or the registry.
        bool holds_copies(CKContext* context, std::string* why = nullptr) const;
        const std::vector<copy_record>& copies() const { return copies_; }
        const std::vector<row_record>& rows_written() const { return rows_; }
        // The anchor log lines of the last apply/release, one per line.
        std::string describe() const {
            std::string out;
            for (const std::string& l: lines_) out += l + "\n";
            return out;
        }
        // Puts each recorded row back on its source (when the source still
        // exists under its recorded name) and destroys each recorded copy
        // whose root still exists under its recorded name: every object that
        // copy made that still exists with its recorded class.  Returns how
        // many copies it destroyed; the registry is empty afterwards.
        int release(CKContext* context);

    private:
        void line(const std::string& text) { lines_.push_back(text); }
        std::vector<copy_record> copies_;
        std::vector<row_record> rows_;
        std::vector<std::string> lines_;
    };

    inline uint64_t sector_placement::signature(CKContext* context, std::string* error) const {
        using namespace placement_detail;
        std::vector<session::placement_row> rows;
        const std::string read_error = context ? read_placement_rows(context, rows) : std::string("no context");
        if (error) *error = read_error;
        if (!read_error.empty()) return 0;
        CKDataArray* ph = ph_array(context);
        std::vector<std::vector<session::entity_print>> hierarchies(rows.size());
        for (size_t i = 0; i < rows.size(); ++i)
            hierarchies[i] = prints_of(context, ph->GetElementObject(static_cast<int>(i), 3));
        return session::placement_signature(rows, hierarchies);
    }

    inline bool sector_placement::holds_copies(CKContext* context, std::string* why) const {
        if (!copies_.empty()) {
            if (why) *why = "this process's registry holds " + std::to_string(copies_.size()) + " copies";
            return true;
        }
        std::vector<session::placement_row> rows;
        if (!context || !read_placement_rows(context, rows).empty()) return false;
        for (const session::placement_row& r: rows)
            if (placement_detail::has_row_suffix(r.object)) {
                if (why) *why = "row " + std::to_string(r.row) + " names " + r.object;
                return true;
            }
        return false;
    }

    inline int sector_placement::release(CKContext* context) {
        using namespace placement_detail;
        lines_.clear();
        if (!context) return 0;
        int restored = 0, destroyed = 0;
        if (CKDataArray* ph = ph_array(context)) {
            for (auto it = rows_.rbegin(); it != rows_.rend(); ++it) {
                CKObject* source = context->GetObject(it->source);
                if (!source || name_of(source) != it->source_name || it->row >= ph->GetRowCount()) continue;
                ph->SetElementObject(it->row, 3, source);
                ++restored;
            }
        }
        // One destroy, in a fixed order per copy: the scripts of its
        // hierarchy root first (HierarchyParser order), then everything else
        // it made.  The order is the order of the BEHAVIORDETACH callbacks:
        // a P_Extra copy's Extra block (on the root's script) deactivates
        // its children's particle scripts in its DETACH (TT_Gravity_RT
        // Extra.cpp DestroyExtra), which must happen while their emitters
        // live - a particle block's own DETACH deletes its emitter and, on
        // x64 Ballanced, leaves a non-null garbage pointer in its local
        // (SetLocalParameterObject(0, NULL) copies 8 bytes from a 4-byte
        // CK_ID), which the Extra's deactivation would then read.
        std::vector<CK_ID> ids;
        for (const copy_record& c: copies_) {
            auto* root = CK3dEntity::Cast(context->GetObject(c.root));
            if (!root || name_of(root) != c.name) {
                line("release: " + c.name + " (row " + std::to_string(c.row) + ") is gone already");
                continue;
            }
            ++destroyed;
            std::set<CK_ID> mine;
            for (const auto& [id, cid]: c.objects)
                if (CKObject* o = context->GetObject(id); o && o->GetClassID() == cid) mine.insert(id);
            std::set<CK_ID> listed;
            for (CK3dEntity* e: hierarchy_of(root))
                for (int s = 0; s < e->GetScriptCount(); ++s)
                    if (CKBehavior* script = e->GetScript(s); script && mine.count(script->GetID())
                            && listed.insert(script->GetID()).second)
                        ids.push_back(script->GetID());
            for (const auto& [id, cid]: c.objects)
                if (mine.count(id) && listed.insert(id).second) ids.push_back(id);
        }
        if (!ids.empty()) {
            CKDependencies none;
            context->DestroyObjects(ids.data(), static_cast<int>(ids.size()), 0, &none);
        }
        line("release: " + std::to_string(restored) + " of " + std::to_string(rows_.size()) + " PH row(s) put back, "
             + std::to_string(destroyed) + " of " + std::to_string(copies_.size()) + " copies destroyed ("
             + std::to_string(ids.size()) + " objects)");
        copies_.clear();
        rows_.clear();
        return destroyed;
    }

    inline std::string sector_placement::apply(CKContext* context, const placement_hooks& hooks) {
        using namespace placement_detail;
        using kind = session::placement_action::kind;
        lines_.clear();
        if (!context) return "no context";
        if (!copies_.empty())
            return "this process already holds " + std::to_string(copies_.size()) + " placement copies (release them first)";

        // Step 1: the table and its plan.
        std::vector<session::placement_row> rows;
        if (const std::string e = read_placement_rows(context, rows); !e.empty()) return "PH: " + e;
        CKDataArray* ph = ph_array(context);
        const session::placement_plan plan = session::plan_placements(rows, session::MAX_NAME);
        if (!plan.error.empty()) return "plan: " + plan.error;
        std::string same_pose, rewritten;
        for (const session::placement_action& a: plan.actions) {
            if (a.what != kind::share) continue;
            same_pose += (same_pose.empty() ? "" : ", ") + std::string("r") + std::to_string(a.row) + " = r"
                       + std::to_string(a.holder_row) + " (" + a.source + ")";
            if (a.name != a.source)
                rewritten += (rewritten.empty() ? "" : ", ") + std::string("r") + std::to_string(a.row) + " " + a.source
                           + " -> " + a.name;
        }
        const auto summary = [&]() {
            line("same-pose rows: " + (same_pose.empty() ? std::string("-") : same_pose));
            line("rewritten share rows: " + (rewritten.empty() ? std::string("-") : rewritten));
        };
        if (plan.copies() == 0 && plan.rewrites() == 0) {
            summary();
            line("placement: " + std::to_string(rows.size()) + " rows, nothing to copy, " + std::to_string(plan.shares())
                 + " shared");
            return {};
        }

        // Step 2: the builder's dependencies.
        std::string block_error;
        const std::vector<builder_copy_block> blocks = find_builder_copy_blocks(context, block_error);
        if (!block_error.empty()) return "builder copy block: " + block_error;
        CKBehavior* chosen = nullptr;
        for (const builder_copy_block& b: blocks) {
            line(std::string(b.chosen ? "builder" : "ignored") + " copy block: " + b.graph + "/Object Copy -> "
                 + (b.out_target.empty() ? "(nothing)" : b.out_target));
            if (b.chosen) chosen = b.block;
        }
        CKDependencies* deps = builder_copy_dependencies(chosen);
        {
            const std::string words = describe_copy_dependencies(chosen);
            size_t from = 0;
            for (size_t at = words.find('\n'); at != std::string::npos; from = at + 1, at = words.find('\n', from))
                line(words.substr(from, at - from));
        }
        CKScene* scene = context->GetCurrentScene();
        if (!scene) return "no current scene";
        {
            CKLevel* level = context->GetCurrentLevel();
            line(std::string("scene: the current scene ") + (level && level->GetLevelScene() == scene ? "(the level scene)"
                 : "(NOT the level scene)"));
        }

        // Step 3: no hot source.
        for (const session::placement_action& a: plan.actions) {
            if (a.what != kind::copy) continue;
            auto* root = CK3dEntity::Cast(ph->GetElementObject(a.row, 3));
            const std::string at = "row " + std::to_string(a.row) + ": ";
            if (!root) return at + "the source " + a.source + " is not a 3D entity";
            if (name_of(root) != a.source) return at + "the row names " + name_of(root) + ", the plan " + a.source;
            for (CK3dEntity* e: hierarchy_of(root)) {
                if (hooks.has_body && hooks.has_body(e)) return at + "the source's " + name_of(e) + " has a physics body";
                for (int s = 0; s < e->GetScriptCount(); ++s) {
                    std::string hot;
                    walk_blocks(e->GetScript(s), [&](CKBehavior* beh) {
                        if (!hot.empty()) return;
                        if (script_replay::is_joint_block(beh) && script_replay::joint_handle(beh))
                            hot = "joint block " + name_of(beh) + " holds a handle";
                        else if (script_replay::is_force_block(beh) && hooks.force_exists && hooks.force_exists(beh))
                            hot = "force block " + name_of(beh) + " holds a controller";
                    });
                    if (!hot.empty()) return at + "the source's script " + name_of(e->GetScript(s)) + ": " + hot;
                }
            }
        }
        line(std::string("step 3: no hot source (bodies ") + (hooks.has_body ? "checked" : "NOT checked: no hook")
             + ", force controllers " + (hooks.force_exists ? "checked" : "NOT checked: no hook") + ")");

        // Steps 4-10, undone as a whole on any refusal (step 11): every row
        // written is put back, every object created since `before_all` is
        // destroyed (with notification, so groups, scenes, the attribute
        // lists and the managers forget them).
        const std::set<CK_ID> before_all = all_object_ids(context);
        std::vector<row_record> written;
        std::vector<copy_record> records;
        const auto undo = [&](const std::string& why) -> std::string {
            for (auto it = written.rbegin(); it != written.rend(); ++it)
                ph->SetElementObject(it->row, 3, context->GetObject(it->source));
            std::vector<CK_ID> made;
            for (CK_ID id: all_object_ids(context))
                if (!before_all.count(id)) made.push_back(id);
            if (!made.empty()) {
                CKDependencies none;
                context->DestroyObjects(made.data(), static_cast<int>(made.size()), 0, &none);
            }
            size_t left = 0;
            for (CK_ID id: all_object_ids(context)) left += before_all.count(id) ? 0 : 1;
            const std::string text = why + " (undone: " + std::to_string(written.size()) + " PH row(s) put back, "
                                   + std::to_string(made.size()) + " object(s) destroyed, " + std::to_string(left) + " left)";
            line("REFUSED: " + text);
            return text;
        };
        CKAttributeManager* am = context->GetAttributeManager();
        CKParameterManager* pm = context->GetParameterManager();
        size_t renamed_longest = 0;
        for (const session::placement_action& a: plan.actions) {
            const std::string at = "row " + std::to_string(a.row) + ": ";
            if (a.what == kind::share && a.name != a.source) {
                // Step 10 for a share row whose holder is a copy (L1 row 38).
                CKObject* holder = nullptr;
                for (const copy_record& r: records)
                    if (r.name == a.name) holder = context->GetObject(r.root);
                if (!holder) return undo(at + "the holder copy " + a.name + " does not exist");
                CKObject* now = ph->GetElementObject(a.row, 3);
                written.push_back({a.row, now ? now->GetID() : 0, name_of(now)});
                ph->SetElementObject(a.row, 3, holder);
                line("share row " + std::to_string(a.row) + " " + a.source + " -> " + a.name + " (row "
                     + std::to_string(a.holder_row) + "'s copy)");
                continue;
            }
            if (a.what != kind::copy) continue;
            auto* root = CK3dEntity::Cast(ph->GetElementObject(a.row, 3));
            const std::vector<CK3dEntity*> h = hierarchy_of(root);
            const std::string suffix = session::placement_suffix(a.row);

            // Step 4: the copy, as CKContext::CopyObject makes it, with the
            // builder's dependencies and no ACTIVATE.
            const std::set<CK_ID> before = all_object_ids(context);
            CKDependenciesContext dc(context);
            dc.SetCreationMode(CK_OBJECTCREATION_DYNAMIC);
            dc.SetOperationMode(CK_DEPENDENCIES_COPY);
            if (deps) dc.StartDependencies(deps);
            CK_ID source_id = root->GetID();
            dc.AddObjects(&source_id, 1);
            dc.Copy(nullptr);
            const auto refuse = [&](const std::string& why) {
                if (deps) dc.StopDependencies();
                return undo(at + why);
            };

            // Step 5: check, then rename every copied entity.
            std::vector<CK3dEntity*> hc;
            for (CK3dEntity* o: h) {
                auto* c = CK3dEntity::Cast(dc.Remap(o));
                if (!c || c == o) return refuse("the copy has no copy of " + name_of(o));
                if (c->GetClassID() != o->GetClassID()) return refuse("the copy of " + name_of(o) + " has another class");
                hc.push_back(c);
            }
            std::vector<CK_ID> made;
            for (CK_ID id: all_object_ids(context))
                if (!before.count(id)) made.push_back(id);
            {
                const std::set<CKObject*> mine(hc.begin(), hc.end());
                for (CK_ID id: made) {
                    CKObject* o = context->GetObject(id);
                    if (o && CKIsChildClassOf(o, CKCID_3DENTITY) && !mine.count(o))
                        return refuse("the copy made the 3D entity " + name_of(o) + ", which is not a copy of the hierarchy");
                }
            }
            const XObjectArray remapped = dc.FillRemappedDependencies();
            if (hooks.fail_at_row == a.row) return refuse("test hook: step 5 refuses at this row (--placement-fail-at)");
            // The limit: the wire's MAX_NAME, and what the physics bridge's
            // body name holds (BMMO_PHYSICS_NAME_SIZE with its terminator),
            // since a renamed entity's name is its body's name everywhere.
            constexpr size_t kMaxEntityName = std::min<size_t>(session::MAX_NAME, BMMO_PHYSICS_NAME_SIZE - 1);
            for (size_t i = 0; i < h.size(); ++i) {
                const std::string name = name_of(h[i]) + suffix;
                if (name.size() > kMaxEntityName)
                    return refuse("the copy name " + name + " (" + std::to_string(name.size()) + " characters) is longer than "
                                  + std::to_string(kMaxEntityName));
                if (context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name.c_str()), CKCID_OBJECT, nullptr))
                    return refuse("the copy name " + name + " is already present");
                hc[i]->SetName(const_cast<CKSTRING>(name.c_str()));
                renamed_longest = std::max(renamed_longest, name.size());
            }
            if (name_of(hc[0]) != a.name) return refuse("the copy's root is " + name_of(hc[0]) + ", the plan's " + a.name);

            // Step 6: inert until its row's pass, whatever the source's state:
            // every entity active in the scene, every script stopped with no
            // pending activation, the hierarchy hidden.
            for (CK3dEntity* c: hc) {
                if (!c->IsInScene(scene)) scene->AddObjectToScene(c, FALSE);
                if (!scene->IsObjectActive(c)) scene->Activate(c, FALSE);
            }
            int scripts = 0;
            for (CK3dEntity* c: hc)
                for (int s = 0; s < c->GetScriptCount(); ++s) {
                    CKBehavior* script = c->GetScript(s);
                    if (!script) continue;
                    if (script->IsInScene(scene)) scene->DeActivate(script);
                    if (script->IsActive()) script->Activate(FALSE, FALSE);
                    script->ModifyFlags(0, CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME
                                           | CKBEHAVIOR_DEACTIVATENEXTFRAME);
                    ++scripts;
                }
            for (CK3dEntity* c: hc) c->Show(CKHIDE);

            // Step 7: TT Save IC with hierarchy.
            for (CK3dEntity* c: hc)
                if (c->IsInScene(scene)) scene->SetObjectInitialValue(c, CKSaveObjectState(c));

            // Step 8: attributes in a fixed order - each original's, in its
            // attribute order, removed from the copy first and set again with
            // the original's value (an object value that names a member of
            // the hierarchy names its copy).  An attribute type CK does not
            // copy (CK_ATTRIBUT_DONOTCOPY, CKBeObject::Copy) is not set.
            std::set<std::string> attribute_types;
            int skipped = 0;
            for (size_t i = 0; i < h.size(); ++i) {
                CK3dEntity* o = h[i];
                CK3dEntity* c = hc[i];
                for (int k = 0; k < o->GetAttributeCount(); ++k) {
                    const CKAttributeType type = o->GetAttributeType(k);
                    if (am->GetAttributeFlags(type) & CK_ATTRIBUT_DONOTCOPY) {
                        ++skipped;
                        continue;
                    }
                    if (c->HasAttribute(type)) c->RemoveAttribute(type);
                    c->SetAttribute(type);
                    if (!c->HasAttribute(type))
                        return refuse("cannot set attribute " + std::to_string(type) + " on " + name_of(c));
                    CKParameterOut* from = o->GetAttributeParameter(type);
                    CKParameterOut* to = c->GetAttributeParameter(type);
                    if (from && to) {
                        to->CopyValue(from);
                        if (pm && pm->IsDerivedFrom(to->GetGUID(), CKPGUID_OBJECT)) {
                            CK_ID value = 0;
                            to->GetValue(&value);
                            CKObject* target = context->GetObject(value);
                            CKObject* mapped = target ? dc.Remap(target) : nullptr;
                            if (mapped && mapped != target) {
                                CK_ID id = mapped->GetID();
                                to->SetValue(&id);
                            }
                        }
                    }
                    const char* n = am->GetAttributeNameByType(type);
                    attribute_types.insert(n ? n : "#" + std::to_string(type));
                }
            }

            // Step 9: groups in a fixed order - every copy out of each group
            // that holds a member of the hierarchy (or a copy), then the copy
            // of each member that is in it appended in HierarchyParser order.
            // (Scripts are scene objects, not BeObjects: no group or attribute
            // holds one.)
            std::string group_names;
            {
                const XObjectPointerArray& groups = context->GetObjectListByType(CKCID_GROUP, TRUE);
                std::vector<CKGroup*> touched;
                for (int g = 0; g < groups.Size(); ++g) {
                    CKGroup* group = CKGroup::Cast(groups[g]);
                    if (!group) continue;
                    bool holds = false;
                    for (size_t i = 0; i < h.size() && !holds; ++i) holds = h[i]->IsInGroup(group) || hc[i]->IsInGroup(group);
                    if (holds) touched.push_back(group);
                }
                for (CKGroup* group: touched) {
                    for (CK3dEntity* c: hc)
                        if (c->IsInGroup(group)) group->RemoveObject(c);
                    for (size_t i = 0; i < h.size(); ++i)
                        if (h[i]->IsInGroup(group)) group->AddObject(hc[i]);
                    group_names += (group_names.empty() ? "" : ",") + name_of(group);
                }
            }
            if (deps) dc.StopDependencies();

            // Step 10: the row names its copy.
            written.push_back({a.row, root->GetID(), name_of(root)});
            ph->SetElementObject(a.row, 3, hc[0]);

            // The record: what the copy made (after steps 6-9, which made
            // and destroyed attribute parameters), by class.
            copy_record r;
            r.root = hc[0]->GetID();
            r.name = a.name;
            r.row = a.row;
            r.source = a.source;
            r.source_id = root->GetID();
            r.entities = hc.size();
            std::map<std::string, int> census;
            for (CK_ID id: all_object_ids(context)) {
                if (before.count(id)) continue;
                CKObject* o = context->GetObject(id);
                r.objects.emplace_back(id, o->GetClassID());
                const char* cls = CKClassIDToString(o->GetClassID());
                ++census[cls ? cls : "?"];
            }
            for (const auto& [cls, n]: census) r.census += (r.census.empty() ? "" : " ") + cls + ":" + std::to_string(n);
            r.groups = group_names;
            for (const std::string& t: attribute_types) r.attributes += (r.attributes.empty() ? "" : ",") + t;
            r.fingerprint = describe_prints(prints_of(context, hc[0]));
            line("copy row " + std::to_string(a.row) + " s" + std::to_string(a.sector) + " " + a.source + " -> " + a.name
                 + ": " + std::to_string(hc.size()) + " entities, " + std::to_string(scripts) + " scripts, "
                 + std::to_string(r.objects.size()) + " objects (remapped " + std::to_string(remapped.Size()) + "), census "
                 + r.census + ", groups " + (r.groups.empty() ? "-" : r.groups) + ", attributes "
                 + (r.attributes.empty() ? "-" : r.attributes) + (skipped ? ", " + std::to_string(skipped)
                 + " DONOTCOPY attribute(s) not set" : ""));
            records.push_back(std::move(r));
        }

        // The table the plan promised (design 2.2 "Idempotence"): every row
        // names its action's object now.
        {
            std::vector<session::placement_row> now;
            if (const std::string e = read_placement_rows(context, now); !e.empty()) return undo("PH after the copies: " + e);
            for (size_t i = 0; i < now.size() && i < plan.actions.size(); ++i)
                if (plan.actions[i].what != kind::none && now[i].object != plan.actions[i].name)
                    return undo("row " + std::to_string(now[i].row) + " names " + now[i].object + " after the copies, the plan "
                                + plan.actions[i].name);
            const session::placement_plan again = session::plan_placements(now, session::MAX_NAME);
            if (again.copies() != 0 || again.rewrites() != 0 || !again.error.empty())
                return undo("the placed table re-plans to " + std::to_string(again.copies()) + " copies, "
                            + std::to_string(again.rewrites()) + " rewrites" + (again.error.empty() ? "" : " (" + again.error + ")"));
        }
        copies_ = std::move(records);
        rows_ = std::move(written);
        summary();
        char text[160];
        std::snprintf(text, sizeof(text), "placement: %zu copies, %zu shared (%zu rewritten), longest copied name %zu, signature %016llx",
                      copies_.size(), plan.shares(), plan.rewrites(), renamed_longest,
                      static_cast<unsigned long long>(signature(context)));
        line(text);
        return {};
    }
}

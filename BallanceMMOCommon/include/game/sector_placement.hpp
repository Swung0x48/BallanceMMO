#pragma once

// The sector placement's CK half (docs/sector-placement-design.md 2.3),
// read-only part, step S1: reading the PH table into placement rows (2.3
// step 1) and finding the level builder's Object Copy block with its
// dependency words (2.3 step 2).  Public CK2 API only, like
// sector_activation.hpp.  Nothing here writes to the world; step S2 adds the
// apply (copy, rename, attributes, groups, PH rewrite, all or nothing), the
// signature, holds_copies and release on top of these readers.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "CKAll.h"

#include <session/placement_plan.hpp>

#include "menu_driver.hpp"

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
}

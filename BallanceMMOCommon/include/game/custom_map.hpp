#pragma once

// A custom map loaded into a level slot (step CMAP; the user, 2026-10-31: "load the arena through BML+'s own
// custom-map loading, restore the original Level_13 files").  The retail level loader, from Levelinit.nmo
// (Levelinit_build, sub-behaviour "Load LevelXX"):
//
//   Load LevelXX.In -> Get Cell(CurrentLevel) -> Op(row = level - 1) -> Get Cell(AllLevel[row][0], "Level_NN.nmo")
//     -> Op(res = "3D Entities\Level\" + cell) -> Object Load(File = res) -> Object Create(All_Level) -> ...
//
// and the same "Row Index" picks the level's ball ("set CurrentLevel", AllLevel[row][1]), so a custom map takes a
// slot's row: the slot decides the start ball, the music and the sky (BML+'s CustomMap.LevelNumber).
//
// The two hosts reach the loader differently, each with its platform's means:
// - retail: BML+ 0.3.12 patches Load LevelXX at load time (CustomMaps::PatchLevelLoader: a Binary Switch on a
//   local bool "Custom Level" in front of the In link; TRUE goes straight to Object Load, skipping the row and
//   name ops) and its map menu's LoadMap sets "Custom Level", Object Load's File source, the row and
//   CurrentLevel[0,0].  arm_bml_custom_level() sets exactly those four on BML+'s own patch (BML+ has no public
//   map-loading call; it clears the flag itself at OnStartLevel); the menus then start the level as for a slot.
// - headless (no BML+): find_level_loader()'s `prefix`, the name op's "3D Entities\Level\" local, is pointed at
//   a directory holding the map under the slot's own file name (headless_engine::redirect_level_file); the
//   graph is not changed and no CK object is created, so a level loaded this way runs the world it would run
//   with the file installed in the slot.
//
// Only the public CK2 API is used (compiles against the Virtools SDK and the Ballanced engine).

#include <cstring>
#include <string>

#include "CKAll.h"
#include "menu_driver.hpp"

namespace bmmo::game {
    struct level_loader {
        CKBehavior* load_level = nullptr;   // Levelinit_build / "Load LevelXX"
        CKBehavior* object_load = nullptr;  // its "Object Load"
        CKParameter* file = nullptr;        // Object Load's File source (the name op's result)
        CKParameter* prefix = nullptr;      // the name op's first operand ("3D Entities\Level\")
        CKParameter* row = nullptr;         // the "Row Index" the file name and the ball are read with
        CKParameter* custom_flag = nullptr; // BML+'s "Custom Level" local (retail with BML+ only)
    };

    inline bool find_level_loader(CKContext* context, level_loader& out, std::string& error) {
        out = {};
        CKBehavior* build = find_root_script(context, "Levelinit_build");
        if (!build) { error = "no root script Levelinit_build (Levelinit.nmo not loaded yet)"; return false; }
        out.load_level = find_sub_behavior(build, "Load LevelXX");
        if (!out.load_level) { error = "Levelinit_build has no Load LevelXX"; return false; }
        out.object_load = find_sub_behavior(out.load_level, "Object Load");
        CKParameterIn* file_in = out.object_load && out.object_load->GetInputParameterCount() > 0
            ? out.object_load->GetInputParameter(0) : nullptr;
        out.file = file_in ? file_in->GetDirectSource() : nullptr;
        if (!out.file) { error = "Load LevelXX: Object Load's File source was not found"; return false; }
        // The name op: the Op whose result is Object Load's File source.
        CKBehavior* name_op = nullptr;
        for (int i = 0; i < out.load_level->GetSubBehaviorCount() && !name_op; ++i) {
            CKBehavior* sub = out.load_level->GetSubBehavior(i);
            if (!sub || !sub->GetName() || std::strcmp(sub->GetName(), "Op") != 0) continue;
            if (sub->GetOutputParameterCount() < 1 || sub->GetInputParameterCount() < 2) continue;
            CKParameterOut* result = sub->GetOutputParameter(0);
            for (int d = 0; result && d < result->GetDestinationCount(); ++d)
                if (result->GetDestination(d) == out.file) { name_op = sub; break; }
        }
        if (!name_op) { error = "Load LevelXX: the Op naming the level file was not found"; return false; }
        out.prefix = name_op->GetInputParameter(0)->GetRealSource();
        // The row: the Row Index of the Get Cell whose value is the name op's second operand.
        CKParameter* cell = name_op->GetInputParameter(1)->GetRealSource();
        CKObject* cell_owner = cell && cell->GetClassID() == CKCID_PARAMETEROUT
            ? static_cast<CKParameterOut*>(cell)->GetOwner() : nullptr;
        CKBehavior* get_cell = as_behavior(cell_owner);
        CKParameterIn* row_in = get_cell && get_cell->GetInputParameterCount() > 0 ? get_cell->GetInputParameter(0) : nullptr;
        out.row = row_in ? row_in->GetDirectSource() : nullptr;
        if (!out.prefix || !out.row) { error = "Load LevelXX: the file name's prefix or row was not found"; return false; }
        for (int i = 0; i < out.load_level->GetLocalParameterCount(); ++i) {
            CKParameterLocal* local = out.load_level->GetLocalParameter(i);
            if (local && local->GetName() && std::strcmp(local->GetName(), "Custom Level") == 0) {
                out.custom_flag = local;
                break;
            }
        }
        return true;
    }

    // Retail: what BML+'s CustomMaps::LoadMap writes before its "Load Level", on BML+'s own patch.  `file` is
    // the map's path (backslashes, the mod's OnLoadObject reads "...\Maps\<name>.nmo" as a custom map).  The
    // caller then starts the level through the menus (level_request with the same level).
    inline bool arm_bml_custom_level(CKContext* context, const std::string& file, int level, std::string& error) {
        if (level < 1 || level > 13) { error = "a custom map takes a level slot 1..13"; return false; }
        level_loader loader;
        if (!find_level_loader(context, loader, error)) return false;
        if (!loader.custom_flag) {
            error = "Load LevelXX has no \"Custom Level\" local: BML+'s custom-map patch is not installed";
            return false;
        }
        CKDataArray* current_level = static_cast<CKDataArray*>(context->GetObjectByNameAndClass(
            const_cast<CKSTRING>("CurrentLevel"), CKCID_DATAARRAY, nullptr));
        if (!current_level) { error = "CurrentLevel array is unavailable"; return false; }
        loader.file->SetStringValue(const_cast<CKSTRING>(file.c_str()));
        CKBOOL on = TRUE;
        loader.custom_flag->SetValue(&on, sizeof(on));
        current_level->SetElementValue(0, 0, &level, sizeof(level));
        int row = level - 1;
        loader.row->SetValue(&row, sizeof(row));
        return true;
    }

    // Retail: BML+'s flag back to FALSE (a custom request that failed before the level started; BML+ clears it
    // itself at OnStartLevel).
    inline void disarm_bml_custom_level(CKContext* context) {
        level_loader loader;
        std::string ignored;
        if (!find_level_loader(context, loader, ignored) || !loader.custom_flag) return;
        CKBOOL off = FALSE;
        loader.custom_flag->SetValue(&off, sizeof(off));
    }
}

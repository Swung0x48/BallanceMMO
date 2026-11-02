#pragma once

#include "CKGlobals.h"
#include "CKMaterial.h"

namespace bmmo::game {

    // A runtime IC must keep shared textures even when a static material
    // refers to a dynamic texture (the retail Balls.nmo textures). CKObject's
    // Save sets the chunk's dynamic-reference policy from IsDynamic(), and
    // CKStateChunk::WriteObject otherwise replaces those references with 0.
    // Apply that policy only during serialization: the material keeps its
    // original lifetime and the set remains responsible for its teardown.
    inline CKStateChunk* save_material_initial_state(CKMaterial* material) {
        if (!material) return nullptr;
        struct restore_flags {
            CKMaterial* material;
            CKDWORD added;
            ~restore_flags() { material->ModifyObjectFlags(0, added); }
        } restore{material, CK_OBJECT_DYNAMIC & ~material->GetObjectFlags()};
        material->ModifyObjectFlags(restore.added, 0);
        return CKSaveObjectState(material);
    }

}

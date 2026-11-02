// Runtime material IC regression against the retail CK2/CK2_3D DLLs.
// Build in an x86 MSVC prompt, with the Virtools SDK Include/Lib directories:
//   cl /EHsc /MD /std:c++20 /Zc:strictStrings- /I <SDK>/Include
//      /I BallanceMMOCommon/include scripts/test_material_initial_state.cpp
//      /link /LIBPATH:<SDK>/Lib CK2.lib VxMath.lib
// Put the game's Bin directory on PATH, then run:
//   test_material_initial_state.exe <game>/RenderEngines/CK2_3D.dll
// No game assets, rendering window, server or physics world are needed.

#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdio>
#include "CKAll.h"
#include <game/material_initial_state.hpp>

namespace {
    bool roundtrip(CKContext* context, bool dynamic_material, bool dynamic_texture) {
        auto* material = CKMaterial::Cast(context->CreateObject(CKCID_MATERIAL, const_cast<char*>("IC material"),
            dynamic_material ? CK_OBJECTCREATION_DYNAMIC : CK_OBJECTCREATION_NONAMECHECK));
        auto* texture = CKTexture::Cast(context->CreateObject(CKCID_TEXTURE, const_cast<char*>("IC texture"),
            dynamic_texture ? CK_OBJECTCREATION_DYNAMIC : CK_OBJECTCREATION_NONAMECHECK));
        if (!material || !texture) return false;
        material->SetTexture0(texture);
        material->SetDiffuse(VxColor(1.0f, 1.0f, 1.0f, 1.0f));
        material->EnableAlphaBlend(TRUE);
        material->SetSourceBlend(VXBLEND_SRCALPHA);
        material->SetDestBlend(VXBLEND_INVSRCALPHA);
        material->Show(CKHIDE);
        const CKDWORD flags = material->GetObjectFlags();
        const CKDWORD texture_flags = texture->GetObjectFlags();

        // Positive control: the old capture drops precisely this reference.
        // Rebind it before testing the fixed capture, as on the first trafo.
        if (!dynamic_material && dynamic_texture) {
            CKStateChunk* old = CKSaveObjectState(material);
            if (!old) return false;
            const CKERROR err = CKReadObjectState(material, old);
            DeleteCKStateChunk(old);
            if (err != CK_OK || material->GetTexture() != nullptr) return false;
            material->SetTexture0(texture);
        }

        CKStateChunk* initial = bmmo::game::save_material_initial_state(material);
        if (!initial) return false;
        bool ok = material->GetObjectFlags() == flags && texture->GetObjectFlags() == texture_flags;
        for (int i = 0; i < 3 && ok; ++i) {
            // A reset fades the material, then restores its IC; reuse the
            // same saved state, just as multiple explosions of one set do.
            material->SetDiffuse(VxColor(0.0f, 0.0f, 0.0f, 0.0f));
            material->SetTexture0(nullptr);
            material->EnableAlphaBlend(FALSE);
            ok = CKReadObjectState(material, initial) == CK_OK
                && material->GetTexture() == texture
                && material->GetDiffuse().a == 1.0f
                && material->GetDiffuse().r == 1.0f
                && material->AlphaBlendEnabled()
                && material->GetSourceBlend() == VXBLEND_SRCALPHA
                && material->GetDestBlend() == VXBLEND_INVSRCALPHA
                && material->GetObjectFlags() == flags
                && texture->GetObjectFlags() == texture_flags;
        }
        DeleteCKStateChunk(initial);
        return ok;
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_material_initial_state <CK2_3D.dll>\n");
        return 2;
    }
    if (CKStartUp() != CK_OK) return 2;
    CKContext* context = nullptr;
    if (CKGetPluginManager()->RegisterPlugin(argv[1]) != CK_OK
            || CKCreateContext(&context, nullptr, 0, 0) != CK_OK || !context) {
        CKShutdown();
        return 2;
    }
    int failures = 0;
    for (int material = 0; material < 2; ++material)
        for (int texture = 0; texture < 2; ++texture) {
            const bool ok = roundtrip(context, material != 0, texture != 0);
            std::printf("material_dynamic=%d texture_dynamic=%d: %s\n", material, texture, ok ? "PASS" : "FAIL");
            if (!ok) ++failures;
        }
    CKCloseContext(context);
    CKShutdown();
    return failures ? 1 : 0;
}

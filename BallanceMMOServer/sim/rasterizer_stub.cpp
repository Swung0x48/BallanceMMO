// The static CK2_3D build always asks its bundled bgfx rasterizer for
// registration info.  The headless server never links bgfx; answering with an
// invalid revision makes CK2_3D fall back to its built-in NULL rasterizer.

#include "CKRasterizer.h"

void CKBgfxRasterizerGetInfo(CKRasterizerInfo* info) {
    if (!info) return;
    info->StartFct = nullptr;
    info->CloseFct = nullptr;
    info->InterfaceRevision = 0;
}

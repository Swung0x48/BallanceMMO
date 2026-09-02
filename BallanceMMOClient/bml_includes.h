#pragma once

// BallanceMMOClient targets the latest BMLPlus SDK only.
#ifndef BMMO_USE_BML_PLUS
# define BMMO_USE_BML_PLUS
#endif

#include <BML/BMLAll.h>
#if !defined(BML_MAJOR_VERSION) || BML_MAJOR_VERSION != 0 || BML_MINOR_VERSION != 3 || BML_PATCH_VERSION < 12
# error "BallanceMMOClient must be compiled against the BMLPlus 0.3.12 SDK or later"
#endif

typedef const char* BMMO_CKSTRING;
#define m_bml (m_BML)
#define m_sprite (m_Sprite)
// some Virtools 2.1 pointers were changed to references in the BMLPlus SDK
#define VT21_REF(x) &(x)
#define BMMO_BML_BUILD_VERSION(x) (x).patch

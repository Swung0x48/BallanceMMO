#pragma once

// #define BMMO_USE_BML_PLUS
#ifdef BMMO_USE_BML_PLUS
# include <BMLPlus/BMLAll.h>
  typedef const char* BMMO_CKSTRING;
# define m_bml (m_BML)
# define m_sprite (m_Sprite)
# define VT21_REF(x) &(x) // some Virtools 2.1 pointers were changed to references in 2.5
# if BML_MINOR_VERSION >= 3
#  define BMMO_BML_BUILD_VERSION(x) (x).patch
# else
#  define BMMO_BML_BUILD_VERSION(x) (x).build
# endif
#else
# include <BML/BMLAll.h>
# define VT21_REF(x) (x)
  typedef CKSTRING BMMO_CKSTRING;
#endif

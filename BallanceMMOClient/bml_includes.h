#pragma once

// #define BMMO_USE_BML_PLUS
#ifdef BMMO_USE_BML_PLUS
# include <BMLPlus/BMLAll.h>
  typedef const char* BMMO_CKSTRING;
# define m_bml (m_BML)
# define m_sprite (m_Sprite)
#else
# include <BML/BMLAll.h>
  typedef CKSTRING BMMO_CKSTRING;
#endif

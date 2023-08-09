#pragma once

// #define BMMO_USE_BML_PLUS
#ifdef BMMO_USE_BML_PLUS
# include <BMLPlus/BMLAll.h>
  typedef const char* BMMO_CKSTRING;
#else
# include <BML/BMLAll.h>
  typedef CKSTRING BMMO_CKSTRING;
#endif

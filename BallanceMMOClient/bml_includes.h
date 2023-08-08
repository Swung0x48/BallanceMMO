#pragma once

// #define BMMO_USE_BML_PLUS
#ifdef BMMO_USE_BML_PLUS
# include <BMLPlus/BMLAll.h>
# pragma comment(lib, "lib/BMLPlus/BMLPlus.lib")
# pragma comment(lib, "lib/BMLPlus/CK2.lib")
# pragma comment(lib, "lib/BMLPlus/VxMath.lib")
  typedef const char* BMMO_CKSTRING;
#else
# include <BML/BMLAll.h>
# pragma comment(lib, "lib/BML/Release/BML.lib")
  typedef CKSTRING BMMO_CKSTRING;
#endif

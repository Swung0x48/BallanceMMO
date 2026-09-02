Vendored subset of OpenLibm (https://github.com/JuliaMath/openlibm), upstream commit 82e90aef0657289192efe77be89791c07dea0775.
Only the double/float sin, cos, tan, asin, acos, atan, atan2, exp, log, pow, fmod, scalbn, floor, fabs, copysign, rint sources and their private headers; see LICENSE.md.
Built as one translation unit by bmmo_portable_math.c under bmmo_pm_* names (compat/ holds the MSVC prelude and a stub complex header).

Local modification: e_rem_pio2.c and e_rem_pio2f.c define __ieee754_rem_pio2 / __ieee754_rem_pio2f as plain external functions (upstream marks them __inline for textual inclusion into s_sin.c and friends).

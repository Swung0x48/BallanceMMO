/* Force-included into the qhull compile units of the IVP compact builder.
 *
 * qhull sorts facet merge sets by angle, vertex neighbours by id, and a few
 * other sets with the C library's qsort().  Ties are common (a box has four
 * identical merge angles), and each C runtime leaves tied elements in its
 * own order, which changes the ledge order of the compact surface and with
 * it the collision results.  Route every qsort() call to the engine's
 * deterministic quicksort, which reproduces the Microsoft runtime order that
 * the retail game uses on every platform.
 *
 * <stdlib.h> is included first so its own declaration of qsort() is not
 * renamed; later includes of it are no-ops. */
#ifndef BMMO_DETERMINISTIC_QSORT_SHIM_H
#define BMMO_DETERMINISTIC_QSORT_SHIM_H

#include <stdlib.h>

#include "XDeterministicSort.h"

#define qsort(base, num, width, comp) XDeterministicQSort((base), (num), (width), (comp))

#endif

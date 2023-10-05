#include "testmod.h"

IMod* BMLEntry(IBML* bml) {
    return new testmod(bml);
}

void testmod::OnProcess()
{
	
}
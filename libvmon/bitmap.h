#ifndef _BITMAP_H
#define _BITMAP_H

/* simple implementation of general-purpose bitmap macros taken from the comp.lang.c FAQ */

#include <limits.h>	/* for CHAR_BIT */

#define BITMASK(b)	(1 << ((b) % CHAR_BIT))
#define BITSLOT(b)	((b) / CHAR_BIT)
#define BITSET(a, b)	((a)[BITSLOT(b)] |= BITMASK(b))
#define BITCLEAR(a, b)	((a)[BITSLOT(b)] &= ~BITMASK(b))
#define BITTEST(a, b)	((a)[BITSLOT(b)] & BITMASK(b))
#define BITNSLOTS(nb)	((nb + CHAR_BIT - 1) / CHAR_BIT)

#endif

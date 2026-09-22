#ifndef _AAE_ASSERT_H
#define _AAE_ASSERT_H
/* Freestanding no-op assert: the RP2350 game build has no stderr/abort path. */
#define assert(x) ((void)0)
#endif

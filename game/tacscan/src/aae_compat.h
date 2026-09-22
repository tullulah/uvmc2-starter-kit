/* aae_compat.h — force-included (-include) into every AAE translation unit for
 * the Tac/Scan build. Provides types SegaG80.c expects but that no AAE header
 * defines under our freestanding config, plus the P2 joystick globals its input
 * macros reference (the RP2350/host shims only expose P1). */
#ifndef AAE_COMPAT_H
#define AAE_COMPAT_H

typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef unsigned long  DWORD;

/* Player-2 analog stick — not provided by the shims; storage in aae_stubs.c. */
extern signed char currentJoy2X;
extern signed char currentJoy2Y;

#endif

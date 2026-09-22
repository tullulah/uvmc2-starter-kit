/* ts_audio.c — AAE's sound index, resolved to a .vsmp in the sample bundle.
 *
 * AAE asks for a sound BY NUMBER: `sample_start(channel, samplenum, loop)`
 * (src/samples.c) passes the index into its own sample table, which is the
 * position in samples/samples.json. Translating that number into data belongs to
 * the GAME and to nobody else: the SDK cannot know which .vsmp is Tac/Scan's
 * laser, and putting it there would couple the cartridge to one program. That is
 * why sdk_rp2350.c declares `v_sampleData` weak and it is defined for real here.
 *
 * THE SOUNDS ARE NOT LINKED INTO THE IMAGE, and that is not a size preference. A
 * .um2 runs entirely from the UVM2's SRAM and the loader reserves the low part —
 * dkong, which works, ends at 0x200605C8, right on the line. Linking the 121 KB
 * of samples pushed this game to 0x20077BAC and the console came up with a SOLID
 * RED LED: hardfault, not a hang. They now travel as `aae_tacscan.vsm` next to the
 * .um2 on the card and the player reads them into PSRAM at startup, exactly like
 * the romset. Build it with `make snd` and copy it to the card.
 *
 * ONLY THE .um2 READS THE BUNDLE, and the reason is the bus, not the space. On the
 * Vectrex Studio cartridge the game runs on core 1 while core 0 drives the bus,
 * and the BIOS is explicit that an SD read from the other core comes back garbage
 * — so the card cannot be touched from here. That image has 8 MB of PSRAM behind
 * it and no size problem; wiring its samples up means having the BIOS preload the
 * bundle in SYS_LAUNCH, next to `load_rom`, which it already does for the romset.
 * Until that exists the cartridge build is simply silent, which is what it was.
 *
 * In the simulator none of this is used either: there the JS side resolves the
 * sounds (PitrexSimView decodes the .wav files in samples/).
 *
 * A missing bundle is not an error: the loader returns 0, every index comes back
 * NULL and the game is silent.
 */
#ifdef UVM2_PICO_RUNTIME
#include "uvm2_smp.h"

/* The name on the card. It sits next to the .um2, in the root, because that is
 * where the multicart menu looks and where a user drops files.
 *
 * 8.3 AND NOT "aae_tacscan.vsm", WHICH IS HOW THIS FAILED THE FIRST TIME. uvm2_sd.c
 * matches short directory entries only and truncates the base to 8 characters, so
 * the long name was looked up as "AAE_TACSVSM" while the card held "AAE_TA~1.VSM":
 * no match, no bundle, and a game that boots perfectly and says nothing. Same
 * convention as the romset, which is roms/tacscan.zip for the same reason. */
#define TS_BUNDLE "tacscan.vsm"

void ts_audio_init(void) { uvm2_smp_bundle_load(TS_BUNDLE); }
#else
/* On the cartridge the BIOS has already read TACSCAN.VSM in SYS_LAUNCH, next to the
 * romset and derived from the same name, because the game cannot touch the card
 * from core 1. Resolving an index is then the SDK's weak `v_sampleData`, which goes
 * through the BIOS table — so there is nothing for the game to do here. */
void ts_audio_init(void) { }
#endif

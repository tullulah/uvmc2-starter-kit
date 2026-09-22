/* uvm2_smp.h — digitised samples (.vsmp) through the PSG's volume DAC.
 *
 * The Vectrex has no audio DAC: the AY-3-8912 only makes square waves. A sample
 * is played by ABUSING one channel's 4-bit VOLUME register as a DAC — disable
 * that channel's tone and noise, then write amplitudes at the sample rate. It is
 * the trick behind Spike's voice, and `vinterface::dac_test` already measured it
 * on this console: it works, at around 8 kHz.
 *
 * WHAT THIS FILE SOLVES IS NOT THE DAC, IT IS THE BUS. Writing the PSG needs the
 * Vectrex bus, and the bus belongs to the beam: during a ramp Port A *is* the X
 * integrator's rate, so touching it deforms the stroke. Samples therefore cannot
 * be written "whenever due" from outside — they have to be INJECTED INTO THE
 * DRAW LIST, in the gaps where the beam is already parked.
 *
 * AND THOSE GAPS EXIST, MEASURED. A real stroke's list (tools/uvm2_anatomy.c
 * decodes one) is a chain of 32-bus-cycle micro-segments:
 *
 *     T1CL = 8      wait 0
 *     T1CH = 0      wait 11    <- the ramp runs here: T1 counts 8, there are 11
 *     PORT_A = 159  wait 3     <- already stopped: next segment's Y rate
 *     PORT_B = 0    wait 9     <- mux: sample Y
 *     PORT_B = 1    wait 0
 *     PORT_A = 97   wait 3     <- next segment's X rate
 *
 * Immediately before each `T1CL` the timer has expired, PB7 is high and the
 * integrators are frozen: a PSG write there does not move the beam. It costs four
 * commands (data, BDIR up, BDIR down, and RESTORING the X rate the coming ramp
 * needs) and there is one opportunity every 32 cycles — against the 187 that
 * separate two samples at 8 kHz. So there is always room, for about 2% of the bus.
 *
 * THE CLOCK IS THE BUS, NOT THE WALL. A list is BUILT one frame before it is
 * REPLAYED, and building it costs ~7 ms of the 20 it lasts: a wall clock read
 * while emitting does not say when the sample will sound. A command's position in
 * time is its position in bus cycles inside the list, which is what
 * `uvm2_list_cycles()` already counts — and that is why each voice's cursor
 * advances by ELAPSED cycles rather than one sample per emission. The pitch comes
 * out exact even though the opportunities fall unevenly: it is non-uniform
 * sampling with the right value at each instant, not jitter.
 *
 * THE INTER-FRAME GAP IS NOT A SPECIAL CASE, WHICH WAS NOT OBVIOUS. A game that
 * draws 12 ms of a 20 ms frame leaves 8 ms without strokes, and a DAC held there
 * would be a 50 Hz buzz. But that gap is ALREADY spent on commands:
 * `frame_filler` fills up to `uvm2_pacer_cycles` with its zero-reference
 * alternation, and each of its units carries its own `T1CL`. So the list covers
 * the WHOLE frame and injection is one thing in one place.
 *
 * AND THAT IS WHY ALL THE AUDIO LIVES IN THE CORE THAT BUILDS. Filling the gap
 * from the pacer step would have been the natural thing, and it would have been a
 * race: in dual core the pacer runs on the core that REPLAYS, not the one that
 * builds, and the two would have touched the voice cursors unarbitrated. There is
 * no shared state here to protect.
 *
 * THE HONEST LIMIT: WITH FREE REFRESH (UVM2_HZ=0) THE PITCH FOLLOWS THE DRAWING.
 * With no fixed rate there is no filling, the list is replayed and the next one
 * starts as soon as it is ready; if the builder is slower than the bus, the
 * executor sits idle and wall time passes with no bus cycles — and the sample
 * plays slow in that proportion. With UVM2_HZ=50 bus time and wall time are the
 * same by construction and the pitch is exact. A game that wants faithful audio
 * fixes its refresh; it is a trade you choose, not a fault you discover.
 */
#ifndef UVM2_SMP_H
#define UVM2_SMP_H

#include <stdint.h>

/* How many samples can sound at once. They are mixed by summing deviations from
 * the midpoint (8 = silence) and clamping: one volume register is one DAC, so the
 * mixing is ours or there is none.
 *
 * TEN, AND THE NUMBER COMES FROM THE GAME, NOT FROM TASTE. AAE addresses voices by
 * a fixed id, and Tac/Scan's go up to 9 (SegaG80.h): kVoiceShip 1, kVoiceTunnel 2,
 * kVoiceStinger 3, kVoiceEnemy 4, kVoiceShipRoar 5, kVoiceForm 7, kVoiceExtra 8,
 * kVoiceExtralife 9. This was 4, and the result on hardware was that the shots
 * sounded and the engines did not: everything from voice 4 up was dropped by the
 * range check in uvm2_smp_play, silently. A voice costs a few bytes of state and a
 * loop iteration per injected sample, so the ceiling should fit the caller. */
#ifndef UVM2_SMP_VOICES
#define UVM2_SMP_VOICES 10u
#endif

/* `uvm2_smp_stop(UVM2_SMP_ALL)` stops everything. It is an explicit value and not
 * "any out-of-range voice", which is what it used to be: with the game choosing the
 * voice numbers, one id past the end would have silenced the whole game instead of
 * doing nothing. */
#define UVM2_SMP_ALL 0xFFFFFFFFu

/* The rate the DAC is written at, in Hz. 8000 is what `dac_test` measured on the
 * console and what `tools/audio2vsmp` produces by default. Raising it puts more
 * commands in the list (4 per sample) and more brightness at the points where
 * injection happens; lowering it is cheaper and sounds worse. It need NOT match
 * the `.vsmp` file's own `sample_rate`: the resampling is done here. */
/* 16000 SINCE THE FIRST HARDWARE TRACE, and the reason is the one that trace showed:
 * asked for 8 kHz, the DAC was really being written every 232 bus cycles on average
 * (6.5 kHz) with gaps from 187 to 321 — because a sample can only go in at a drawing
 * gap, and past the deadline it waits for the next one. Against a 6 kHz source that is
 * barely Nyquist, and the zero-order hold plus that jitter is audible as grain.
 *
 * Asking for twice the rate does not create detail the file does not have; it makes
 * the emitter take the FIRST gap after a much shorter deadline, so each value lands
 * closer to when it should and the steps are finer. It costs 4 commands per sample:
 * about 6% more bus cycles in the frame, which is affordable now that the frame is
 * paced at 40 Hz with headroom. Lower it if a game needs those cycles back. */
#ifndef UVM2_SMP_HZ
#define UVM2_SMP_HZ 16000u
#endif

/* The volume register used as the DAC. Channel C by default, the same one the
 * .vsfx effects use (uvm2_audio.c) — so a sample coexists with .vmus music on A
 * and B, and in exchange samples and .vsfx are mutually exclusive. */
#ifndef UVM2_SMP_REG
#define UVM2_SMP_REG 10u
#endif

/** Start `data` (a .vsmp: .word rate, .word n, packed 4-bit, low nibble = even
 *  sample) on voice `voice`. `loop` != 0 repeats it forever. Re-triggering a
 *  voice restarts it, which is what AAE expects. */
void uvm2_smp_play(const void *data, unsigned voice, int loop);

/* ── THE SAMPLE BUNDLE, IN PSRAM AND NOT IN THE IMAGE ─────────────────────────
 *
 * SAMPLES DO NOT FIT LINKED IN, and it is not a matter of squeezing. A .um2 image
 * runs entirely from the UVM2's SRAM and the loader reserves the low part: the SDK
 * puts the line at 0x20060000 (memmap_psram.ld), and MEASURED, dkong — which
 * works — ends at 0x200605C8, i.e. right on it. Linking Tac/Scan's 22 sounds
 * (121 KB at 6 kHz) pushed the image to 0x20077BAC and the console came up with a
 * SOLID RED LED: hardfault, not a hang. Lowering the rate does not fix it either —
 * the budget left below the line is ~26 KB, which would be 1.3 kHz.
 *
 * So they go where these same games' romset already goes: read from the card into
 * PSRAM at startup. The image returns to its size and the 8 MB are ample.
 *
 * File format (written by the game's own tool):
 *     "VSMB"              4 bytes
 *     u32 n               how many sounds
 *     u32 offset[n]       byte offset of each .vsmp in the file; 0 = absent
 *     ...the .vsmp blobs, 4-byte aligned...
 *
 * A missing file is NOT a fault that hangs: `uvm2_smp_bundle_entry` returns 0,
 * `v_playSample` does nothing and the game stays silent — the same way the romset
 * behaves when it is not there. */

/** THE NAME MUST BE 8.3. uvm2_sd.c matches short directory entries only and skips
 *  the VFAT ones (uvm2_sd.c:445), and `a83` truncates the base to 8 characters. So
 *  "aae_tacscan.vsm" is looked up as "AAE_TACSVSM", the card holds "AAE_TA~1.VSM",
 *  nothing matches, the bundle never loads and the game is SILENT WITHOUT AN ERROR.
 *  That is exactly how this failed the first time on hardware. Use "tacscan.vsm".
 *
 *  Read `path` from the card into PSRAM. 1 if the bundle loaded and is valid.
 *  Call it ONCE at startup, while the bus is still free: doing it lazily on the
 *  first trigger would put an SD read in the middle of a frame. */
int uvm2_smp_bundle_load(const char *path);

/** Where a bundle should be loaded, and how much fits. The Vectrex Studio cartridge
 *  reads the card itself (its BIOS does, in SYS_LAUNCH) and needs to know where to
 *  put it; the address is defined once, here. */
void *uvm2_smp_bundle_buffer(uint32_t *max_bytes);

/** Validate and install a bundle ALREADY in memory at `base`. This is the half that
 *  is common to both boards; only the reading differs. */
int uvm2_smp_bundle_set(const void *base, uint32_t bytes);

/** Entry `idx` of the bundle, or 0 if there is no bundle or no such entry. This
 *  is what a game returns from `v_sampleData`. */
const void *uvm2_smp_bundle_entry(unsigned idx);

/** Stop one voice (or all of them with `voice >= UVM2_SMP_VOICES`) and, if none
 *  is left, park the DAC at the midpoint so no step is left sounding. */
void uvm2_smp_stop(unsigned voice);

/** Still sounding? This is what AAE asks to decide whether to re-trigger. */
int uvm2_smp_playing(unsigned voice);

/** Is any voice active? The draw path checks this so it pays nothing when there
 *  is no audio — which is the case for 43 of the 44 ports. */
/* INLINE, because it is asked on the emitter's hot path. See the note on the counter in
 * uvm2_smp.c: as an out-of-line function it was one call per ramp just to read an integer,
 * and the emitter does it 1,844 times per frame. The semantics are identical. */
extern unsigned uvm2_smp_s_active;
static inline int uvm2_smp_active(void) { return uvm2_smp_s_active != 0u; }

/** The frame the voice has reached if the video runs at `fps`: what VPy's
 *  SAMPLE_POS needs so the picture follows the AUDIO and not the other way round
 *  (a vector movie counting its own frames drifts the moment one goes over
 *  budget). 0 if that voice is not sounding. */
unsigned uvm2_smp_pos(unsigned voice, unsigned fps);

/* ── Called by the draw SDK, not by the game ──────────────────────────────── */

/** Close the accounting for the list just built: `cycles` is what the COMPLETE
 *  list measured, and with it the voice clock stays continuous across frames (the
 *  debt left unemitted at the end of one is charged at the start of the next).
 *  Without this the sample slows down by whatever is dropped at each boundary. */
void uvm2_smp_frame(uint32_t cycles);

/** Is a sample due at cycle `cycles` of the list being built? Returns 1 and
 *  leaves the mixed 4-bit value in `value`. Advances the cursor by the cycles
 *  ELAPSED since the previous emission, not one sample at a time: that is why the
 *  pitch is exact even though the opportunities fall unevenly. */
int uvm2_smp_due(uint32_t cycles, uint8_t *value);

/** The PSG register that must be (re)latched before this list's first sample.
 *  The emitter asks, because it is the one that knows how to pack commands; all
 *  that is known here is whether it is needed. Returns 1 once per frame — the
 *  latch is lost because other code writes the PSG between lists (reading the
 *  pads latches register 14). */
int uvm2_smp_needs_latch(void);

/** The mixer byte the DAC channel needs: tone AND noise disabled on it (bits
 *  0x24 for channel C), over whatever the .vmus player has on A and B.
 *
 *  IT IS NOT COSMETIC. With its tone bit enabled, the channel gates a square wave
 *  at whatever period registers 4/5 happen to hold and the volume register only
 *  scales it — you get a tone, not the sample. Disabled, the channel output is a
 *  constant level set by the volume, which is the whole trick. */
uint8_t uvm2_smp_mixer(void);

/** Tell the injector what the GAME's mixer is, so its channel composes over the game's
 *  own instead of over a constant. Called for every register-7 write that passes
 *  through the SDK; a game that plays a .vmus does not need it (the player's shadow
 *  wins) and a game that drives the PSG itself cannot do without it. */
void uvm2_smp_note_mixer(uint8_t val);

/* Samples put into the last frame's list. Readable over SWD and surfaced in the
 * stats: zero with a voice active means the injection is NOT happening — and "no
 * sound" and "not running" are two different faults. */
extern uint32_t uvm2_smp_injected;

/* Diagnostics for a bundle that did not load: bytes the card returned, and how many
 * entries the header declared. 0 bytes = the file was not found (check the 8.3
 * name); bytes but 0 entries = found and rejected (wrong magic, truncated table). */
extern uint32_t uvm2_smp_bundle_read;
uint32_t uvm2_smp_bundle_count(void);

#endif

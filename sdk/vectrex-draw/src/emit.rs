//! The SEAM between the beam model and the board.
//!
//! STEP 3 of the unification. The model emits a sequence of VIA writes separated by delays;
//! who takes them to the hardware is each cartridge's business:
//!
//! ours writes the bus immediately (over SIO or over the PIO stream) and the multicart image
//! stacks them into its command list, which an executor replays afterwards.
//!
//! ── AND THE TWO ARE THE SAME THING, which is more than I thought ────────────
//!
//! I used to talk about "their list" as if it were a foreign shape to be adapted to. It is
//! not: WE DO THE SAME. The PIO path is a command list too — `ring_push` ->
//! `BATCH_BUF[2][64]` -> DMA -> PIO — and the finishing touch is in `stream_park`: with
//! `USE_PARK_REPEAT` it emits ONE word with a repeat counter instead of n words. That is
//! literally `(command, delay)`, the same representation as the other board. What changes is
//! the encoding and who replays (PIO+DMA against a CPU loop synchronised to E), not the idea.
//!
//! It matters for the design and not only for the prose: the seam imposes no new shape on
//! anyone, it NAMES the one both already had. And `e6809_raw` demonstrates it by accident — it
//! already branched between `stream_park` and the CPU wait, so `CartSink::emit` inherits the
//! correct behaviour on both paths without touching a line. That it fitted effortlessly was a
//! signal, and I read it as luck.
//!
//! And that is the right boundary: below the sink live the PIO, the E synchronisation and the
//! board profile (`board.rs`), without duplicating the model.
//!
//! ── THE DELAY'S UNIT, which is this file's design decision ──────────────────
//!
//! Q8 of an E cycle: 256 = one E period (666 ns at 1.5 MHz).
//!
//! It is not cosmetic. The drawing path's delays were in MIXED units — `Y_MUX_ECYC` in E
//! cycles, `BLANK_SETTLE_CYC` in CPU cycles, and `e6809(n)` scaled by `E6809_SCALE_Q8` — and
//! with the value of 64 measured on 2026-08-17, `e6809(2)` is HALF an E cycle. If the seam
//! carried whole cycles, that half would round to zero and we would lose exactly the
//! adjustment that gave the +37% frame rate. With Q8 the UVM2's sink rounds to an integer for
//! its command field and ours converts to CPU cycles without losing anything.

use crate::ramp::{scale, MIN_T1};

/// Writes the beam's lit/blanked state through whichever route the knob says: the PCR (ours)
/// or the shift register (the BIOS and the 6809). One place for both forms, so they cannot
/// diverge.
#[inline]
fn beam<S: BusSink>(sink: &mut S, lit: bool, delay: u32) {
    if crate::ramp::BEAM_VIA_SR.load(Ordering::Relaxed) != 0 {
        sink.emit(REG_SHIFT, if lit { 0xFF } else { 0x00 }, delay);
    } else {
        sink.emit(REG_CNTL, if lit { 0xEE } else { 0xCE }, delay);
    }
}
use core::sync::atomic::Ordering;

/// VIA registers as an OFFSET (0..15), not as an absolute address. Our cartridge maps them to
/// $D000+ and the UVM2 puts them on A0-A3: that translation belongs to the sink, which is where
/// what differs between boards lives.
pub const REG_PORT_B: u8 = 0x0;
pub const REG_PORT_A: u8 = 0x1;
pub const REG_T1_LO: u8 = 0x4;
pub const REG_T1_HI: u8 = 0x5;
pub const REG_SHIFT: u8 = 0xA;

/// Last T1CL written (bit 8 = valid), so it is not repeated. See T1CL_CACHE.
static T1CL_LAST: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

/// Forget the last T1CL. Needed by anyone starting a list from scratch — and by the tests,
/// because the cache makes the EMITTED sequence depend on history: the same vector comes out
/// with or without the T1CL write depending on what was drawn before. The TIMING does not
/// change (the previous gap compensates), but the list does.
#[no_mangle]
pub extern "C" fn vx_t1cl_forget() { T1CL_LAST.store(0, core::sync::atomic::Ordering::Relaxed); }

/// Writes T1CL AND RECORDS what is left in the latch. EVERYONE who writes T1CL has to come
/// through here.
///
/// The cache was updated ONLY in `draw_line_seq`, but the JUMP writes T1CL too — with its own
/// t1 — so after every jump the cache believed the VIA held a value it no longer held and the
/// next stroke skipped a write that was genuinely needed. `UNVEC` did not expose it because it
/// has 9 jumps against 56 strokes; dkong has 168 and came out wrecked.
#[inline]
fn emit_t1cl<S: BusSink>(sink: &mut S, t1: u16, delay: u32) {
    T1CL_LAST.store((t1 & 0xff) as u32 | 0x100, Ordering::Relaxed);
    sink.emit(REG_T1_LO, (t1 & 0xff) as u8, delay);
}
pub const REG_CNTL: u8 = 0xC;

/// One E cycle in the seam's units.
pub const E: u32 = 256;

/* ── THE MICRO-SEGMENT GAPS, AS GLOBALS AND NOT IN THE STRUCT ────────────────────────
 *
 * They started as fields of `Timings`/`CTimings`, which is where they conceptually belong. It
 * DID NOT WORK: with both structs' sizes checked at compile time (44 bytes in both, a
 * `_Static_assert` in C and an `assert!` in Rust) and the right value on the C side (probed:
 * 4), reading them from Rust made ONE command per vector come out with its gap saturated at
 * 4095 and the frame blow out from 23,831 to 788,661 cycles. And it happened with ANY of the
 * four separately, even the one that visibly produced its correct gap. So the size adds up and
 * something about the trip through the ABI does not.
 *
 * The layer's other knobs (the speed cap, DRAW_SCALE, MIN_T1...) go through `#[no_mangle]`
 * globals and have worked for months. Same here: it is the proven path, and in passing the
 * panel can touch them hot like the others.
 *
 * They are GAPS (what separates one write from the next). 0 = the long-standing value, which
 * is the cadence measured from the reference's asterock. */
#[used] #[no_mangle]
pub static MT_ORA_Y: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
#[used] #[no_mangle]
pub static MT_ORB_KEEP: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
#[used] #[no_mangle]
pub static MT_SR_ON: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
#[used] #[no_mangle]
pub static MT_ORA_X_ON: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

/* PROBE: the field as RUST sees it, and the gap that comes out of it. The C side says 4 and
 * the emitted command comes out saturated at 4095, so the value has to be seen HERE. */
#[used] #[no_mangle]
pub static DBGX_FIELD: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
#[used] #[no_mangle]
pub static DBGX_GAP: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
#[inline(never)]
fn vx_dbg_x(field: u32, gap: u32) {
    DBGX_FIELD.store(field, core::sync::atomic::Ordering::Relaxed);
    DBGX_GAP.store(gap, core::sync::atomic::Ordering::Relaxed);
}


/// Where the writes come out. Each cartridge implements it.
pub trait BusSink {
    /// Writes `data` to register `reg` and waits `delay_q8` AFTERWARDS.
    fn emit(&mut self, reg: u8, data: u8, delay_q8: u32);

    /// Lengthens the gap of the LAST command already emitted. Does nothing by default.
    ///
    /// It exists because the one that blanks the beam is the NEXT call, not the one that
    /// emitted the ramp: without this there is no way to give the last lit ramp time to
    /// finish. MEASURED in their Major Havoc frame — gap 11 if the micro-segment continues, 16
    /// if what follows blanks, and the separation is 175 of 175.
    fn extend_last(&mut self, _extra_q8: u32) {}

    /// Waits for a ramp of `t1` counts to finish, plus `extra_q8`. WITHOUT writing.
    ///
    /// SEPARATE FROM `emit` ON PURPOSE: it is the only REAL difference between the two boards
    /// along the whole drawing path. Our cartridge asks the hardware — it polls the VIA's T1
    /// flag, as the BIOS does in `LF345: BITB <VIA_int_flags`. The UVM2 image cannot: its
    /// command list is replayed by an executor, and reading the VIA in the middle of a list
    /// while we drive the data bus is what caused the ghost vectors. There the delay is counted
    /// and that is it.
    ///
    /// Putting it in the `trait` forces that difference to be WRITTEN DOWN, instead of hidden
    /// in two implementations that believe they are the same.
    ///
    /// IT IS SEPARATE FROM `emit` FOR A SECOND REASON, which appeared with `draw_line`: in one
    /// of its branches the ramp starts when T1CH is written but the wait happens AFTER lighting
    /// the beam, two writes later. If waiting were a parameter of writing, that branch could
    /// not be expressed without lying. In a command list the delay is added to the PREVIOUS
    /// command, which is exactly where it lives.
    ///
    /// `extra_q8` is SIGNED on purpose. Negative = blank the beam BEFORE the ramp finishes,
    /// which is physically what is needed if the ramp lasts longer than its `t1` counts (the
    /// 6522 counts t1 + 1.5, and the transport adds its own). With an unsigned type that side
    /// of the adjustment could not even be expressed — measured on the UVM2 on 2026-08-18:
    /// raising the delay WIDENS the gap at the vertices, i.e. the knee is below zero.
    fn wait_ramp(&mut self, t1: u16, extra_q8: i32);

    /// The beam has been left blanked. Bookkeeping for whoever wants to keep it.
    fn beam_blanked(&mut self) {}

    /// Y's sample-and-hold now holds `vy`, so the next vector that repeats that value can skip
    /// the sampling entirely.
    fn y_held(&mut self, _vy: i8) {}

    /// Does Y's S&H already hold `vy`? If so, the whole sampling is redundant — four writes and
    /// the charge window, 25% of the vector's cost. Each board keeps the count because each has
    /// its own cache; the DECISION belongs to the model.
    fn y_can_skip(&mut self, _vy: i8) -> bool {
        false
    }

    /// Is the beam lit right now? Only `keep_lit` looks at it.
    fn beam_is_lit(&self) -> bool {
        false
    }

    /// The beam has been left LIT.
    fn beam_lit(&mut self) {}

    /// Does the DAC ALREADY hold this `vx`? X is the LIVE DAC (no sample-and-hold), so this is
    /// "the last thing written to PORT_A is vx". It is the MISSING HALF of `keep_lit`.
    fn x_can_skip(&mut self, _vx: i8) -> bool {
        false
    }
}

/// The path's gaps, all in Q8 of an E cycle.
///
/// They are passed in rather than read from global knobs because TODAY they live in different
/// places and in different units on each cartridge, and moving them at the same time as the
/// code would make it impossible to know which of the two broke something. Declared debt, not
/// design.
#[derive(Clone, Copy)]
pub struct Timings {
    /// Scale of the gaps that imitate the 6809 (`e6809`), in Q8. 256 = as-is.
    pub e6809_q8: u32,
    /// Charge window for Y's sample-and-hold.
    pub y_mux_q8: u32,
    /// `Moveto_d`'s settling, size-dependent in the BIOS; here its worst case. A single line
    /// that once accounted for half the system's delay.
    pub moveto_settle_q8: u32,
    /// Between starting the ramp and lighting the beam, so the spot is already travelling when
    /// it lights. Without this a bright dot is left at the DEPARTING vertex.
    pub beam_on_q8: u32,
    /// How long the beam stays lit AFTER the integrators stop: the ramp has ended but the beam
    /// is still arriving. It is NOT the same quantity as lighting — measured by bisection on
    /// the console on 2026-08-07, blanking asks for 2.5 times more, and it makes physical
    /// sense: starting from rest and braking against the yoke's inductance are not the same
    /// transient.
    pub blank_settle_q8: i32,
    /// Chain lit strokes without blanking in between. It saves the seam at every vertex; in
    /// exchange, a frame that ends lit leaves the beam lit.
    pub keep_lit: bool,
    /// WHAT THE DAC IS GIVEN TO SETTLE THE X VELOCITY, before starting the ramp.
    ///
    /// THE TWO AXES ARE NOT ON EQUAL FOOTING, and this evens them up. Y is SAMPLED: it is
    /// written to the DAC, the mux is opened `y_mux_q8` cycles so the sample-and-hold charges,
    /// and it is closed — so it arrives settled. X goes STRAIGHT to the DAC and the ramp starts
    /// three commands later, with no window at all. If the DAC has not arrived, the X velocity
    /// at the start of the stroke is whatever it happens to be.
    ///
    /// OBSERVED on the console on 2026-08-24 over the grid, with the geometry STILL: the
    /// VERTICAL lines move about 5 mm horizontally and the horizontals are considerably
    /// stiller. A vertical is drawn with vx = 0 and its X was set by the previous jump; a
    /// horizontal carries X in the ramp. That the axis without a settling window is exactly the
    /// one that shimmers is the signature to check.
    ///
    /// 0 = as before. It is a knob so it can be refuted in a minute without recompiling.
    pub x_settle_q8: u32,

}

impl Timings {
    /// A gap of `n` "6809 cycles", with its scale applied. It is `e6809(n)`.
    #[inline(always)]
    fn e(&self, n: u32) -> u32 {
        n * self.e6809_q8
    }
}

/// `Moveto_d` ($F312) — reposition with the beam blanked.
///
/// MOVED from `vinterface.rs`, not rewritten: the register sequence, the gaps and their
/// comments are the ones that were there. The only thing that changes is that the writes go
/// out through the sink instead of straight through `bus_write`, which is what lets the UVM2
/// image use THIS code and not a copy of it.
///
/// `Y_HELD`'s BOOKKEEPING BELONGS TO THE EMITTER, not to the backend.
///
/// `ramp_params` requires bit 8 of `Y_HELD` to apply the selective cap, but the static was
/// written by EACH BACKEND on its own account — and the UVM2's did not: it kept its own `s_y`
/// to decide the mux jump and left `ramp`'s at zero. Result: **VCAP_SLOW NEVER fired on that
/// cartridge**. Measured on the console on 2026-08-24, with VCAP_SLOW at 30, 16 and 8:
/// VCAP_SLOW_HITS = 0 in all three, and Y_HELD = 0 on the probe.
///
/// It was an invisible contract between two crates: one reads a static the other had to
/// remember to write. And making it a trait default does NOT work, which was my first attempt:
/// `CSink` and the firmware OVERRIDE both methods, so the default would not run in precisely
/// the ones that fail. It goes at the call sites, where it cannot be dodged.
///
/// The sink's callback stays: each backend still uses it for ITS cache.
///
/// WHAT INVALIDATES IT IS NOT THE EMITTER. Blanking the beam does NOT discharge Y's S&H: what
/// loses it is somebody touching mux channel 0, and that happens when reading the JOYSTICK,
/// which shares the CD4052 with the beam. That is each board's business, so it is exposed and
/// that is that.
///
/// I nearly called it from `beam_blanked`, which would have left `y_can_skip` false for ever:
/// four extra writes and the charge window PER VECTOR, 25% of its cost. The selective cap's
/// test caught it by coming out at zero.
#[inline(always)]
pub fn y_hold_lost() {
    crate::ramp::Y_HELD.store(0, Ordering::Relaxed);
}

/* REVERTED 2026-08-24. The emitter used to write this so VCAP_SLOW could fire on the UVM2 too,
 * where it had been dead for ever. The diagnosis was correct — `Y_HELD` was written by each
 * backend and the UVM2's did not do it — but TURNING IT ON changes how 37% of the vectors are
 * drawn in EVERY game on both cartridges, and on the console it came out far worse: dkong and
 * SnowBros broken.
 *
 * Re-enabling it is a behaviour change and deserves its own test, not a free ride on a
 * bookkeeping fix. The bookkeeping goes back to the backend. */
#[allow(dead_code)]
#[inline(always)]
fn y_hold_is(vy: i8) {
    crate::ramp::Y_HELD.store(0x100 | (vy as u8 as u32), Ordering::Relaxed);
}

/* THE GAP AFTER `T1CH`, ACCORDING TO WHAT COMES NEXT.
 *
 * MEASURED in their frame 120 of Major Havoc, over its 623 ramps of t1 = 8: it leaves 11 E
 * cycles when the micro-segment CONTINUES (424 cases) and 16 when what follows BLANKS the beam
 * (175), without a single crossed case — classified by whether there is an SR write before the
 * next T1CH. It is physical and not a quirk of theirs: the last ramp of a lit stroke has to
 * finish BEFORE the beam is closed, and cutting it gives a short stroke.
 *
 * It is applied by lengthening the gap ALREADY emitted (`extend_last`), because the one that
 * blanks is the next call and the stroke's emitter cannot know when it emits. */

/// 1 = a lit stroke is emitted as ONE ramp with its whole t1, like the reference cartridge.
/// **IT IS THE DEFAULT since 2026-09-04**, confirmed on the console: with micro-segments Major
/// Havoc came out dotted and with the whole stroke it comes out CLEAN. 0 goes back to the
/// series of T1=8 micro-segments (`-DUVM2_MICROSEGMENTS`).
///
/// MEASURED in their Major Havoc capture (8 frames spread over the 20 s): **it does not split a
/// lit stroke ONCE** — zero runs of identical consecutive micro-segments — and it uses t1 from
/// 8 up to 252, with strokes of up to 200 units in ONE ramp. We emitted t1 = 8 on all of them,
/// without exception (427 of 427 on the bench, 754 of 754 in mhavoc), so a stroke that does not
/// fit in a ramp of 8 gets split — and every joint is 22-24 cycles with the beam LIT AND
/// STILL, i.e. a dot. It matches the console: the bench (text, short strokes) never splits and
/// comes out clean; mhavoc (long diagonals) splits nearly everything and dots.
///
/// The comment below took the micro-segment series from the ASTEROCK capture. The two captures
/// do not say the same thing.
#[no_mangle]
pub static INTEGER_STROKE: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(1);

const H_T1CH_CONT:  u32 = 11;
const H_T1CH_CLOSE: u32 = 16;
/* AND WHEN THE SR COMES RIGHT AFTER THE T1CH, 29 — not 16. Same capture: 21 cases, and in the
 * 175 with gap 16 the next command is ALWAYS ORA, never SR. The gap is not set by the blanking,
 * it is set by HOW LONG it takes to get there. */
const H_T1CH_BLANK:  u32 = 29;
/* AND 21 WHEN WHAT FOLLOWS IS THE LONG UNIT THAT BLANKS INSIDE ITS WINDOW. A fourth value of
 * the same rule, measured like the other three: with t1 = 8 their T1CH gaps are 11 (the
 * micro-segment continues, x424), 16 (x175), 21 (x3) and 29 (blanks now, x21). The 3 of 21 are
 * exactly the ones followed by `ORA ORB=00+3 SR=00+8`, i.e. the long in-window form. */
const H_T1CH_CLOSE_LONG: u32 = 21;
/* The grid of the gap after a lit stroke's T1CH, and its base. See draw_line_seq. */
const T1CH_STEP: u32 = 7;
const T1CH_BASE: u32 = 13;
/* The gap AFTER SR=00: 3 if ORB follows (175 of their cases, and we already did it) and 15 if
 * ORA follows (13 of theirs; we used 0 in 24). */
const H_SR_OFF_A_ORA: u32 = 15;

/// 1 = permits skipping the recharge of Y's sample-and-hold when it already holds the value.
/// **Off by default**: see the measurement in `moveto_seq`.
#[no_mangle]
pub static SKIP_Y: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

/// 1 = ANOTHER blanked unit follows this one before the next lit stroke.
///
/// IT DECIDES WHERE THE BEAM IS BLANKED, and the rule is theirs, measured in their frame 120.
/// Of their 199 blanks (one per stroke, exactly):
///
///   * 178 fall INSIDE the blanked unit's mux window — and in all 178 that unit is the ONLY one
///     before the next stroke.
///   * 21 fall EARLIER, right against the T1CH — and in all 21 there are TWO units (priming +
///     jump) or the re-zero block.
///
/// Not one crossed case. And it is physical: with a single unit there is no journey to draw, so
/// the blanking can go inside; as soon as there is real transport the beam has to be dead
/// BEFORE moving or the traverse comes out painted.
///
/// We said `t1 <= 8`, which is right for the 178 and wrong for the 21: the priming unit also
/// carries t1 = 8, so we blanked inside its window and the jump behind it got drawn. It is the
/// same class of artefact as the "star of rays into the centre".
#[no_mangle]
pub static UNITS_CONTINUE: core::sync::atomic::AtomicU32 =
    core::sync::atomic::AtomicU32::new(0);

pub fn moveto_seq<S: BusSink>(sink: &mut S, vx: i8, vy: i8, t1: u16, k: &Timings) {
    let (vx, vy) = crate::ramp::trim_dac(vx, vy);   /* see trim_dac: here, not in the ramp */
    let sr = crate::ramp::BEAM_VIA_SR.load(Ordering::Relaxed) != 0;
    // LET THE LAST LIT RAMP FINISH. How much depends on whether the SR comes RIGHT after the
    // T1CH (the branch below) or whether ORA and ORB come first (the SR dialect's short jump).
    let lit = sink.beam_is_lit();
    let only_this_one = UNITS_CONTINUE.load(Ordering::Relaxed) == 0;
    /* t1 IS NOT PART OF THE RULE: only the count is. We had it as `t1 <= 8` and it failed in
     * both directions — we blanked outside on their 3 lone units of t1 = 18, and inside on the
     * 21 priming+jump ones. See UNITS_CONTINUE. */
    let blank_now = lit && !(sr && only_this_one);
    if lit {
        let h = if blank_now { H_T1CH_BLANK }
                else if sr && t1 > 8 { H_T1CH_CLOSE_LONG }
                else { H_T1CH_CLOSE };
        sink.extend_last((h - H_T1CH_CONT) * E);
    }
    // BLANK FIRST (SR=0x00). With keep-lit the beam arrives at the jump LIT; if Y/mux is
    // touched before blanking, the traverse to the destination is drawn lit. The SR dialect's
    // SHORT jump is the exception: its pen-up unit blanks INSIDE the mux window, like the
    // reference, so there the blanking is not brought forward.
    if blank_now {
        sink.emit(REG_SHIFT, 0x00, H_SR_OFF_A_ORA * E);
        sink.beam_blanked();
    }
    if sr {
        if t1 <= 8 {
            // SHORT JUMP = the reference's PEN-UP unit, verbatim: with the beam lit it blanks
            // INSIDE the mux window (x18.1/frame: ORA+4 ORB=00+9 SR=00+4 ORB=01+1 ORA+4 T1CL+1
            // T1CH+12); already blanked, the unit without the SR (x27.9/frame: ORA+6 ORB=00+11
            // ORB=01+1 ORA+4 T1CL+1 T1CH+12).
            if sink.beam_is_lit() {
                sink.emit(REG_PORT_A, vy as u8, 3 * E);  // Y; gap 4
                sink.emit(REG_PORT_B, 0x00, 8 * E);      // open mux; gap 9
                sink.emit(REG_SHIFT, 0x00, 3 * E);       // beam OFF inside the window; gap 4
                sink.beam_blanked();
            } else {
                /* THE ORA(Y) GAP IS 3, EVEN WITH THE BEAM ALREADY BLANKED. Measured in their
                 * frame 120: of their 270 units with no SR inside, all 270 carry gap 3 —
                 * (3,9) x228 and (3,10) x42, i.e. what changes with the beam's state is the MUX
                 * WINDOW, not the ORA. The 5 came from the asterock capture, which does not say
                 * the same thing. */
                sink.emit(REG_PORT_A, vy as u8, 3 * E);  // Y; gap 4
                sink.emit(REG_PORT_B, 0x00, 10 * E);     // open mux; gap 11
            }
            sink.emit(REG_PORT_B, 0x01, 0);              // close mux; gap 1
            sink.y_held(vy);
            sink.emit(REG_PORT_A, vx as u8, 3 * E + k.x_settle_q8); // X; gap 4
            emit_t1cl(sink, t1, 0);                    // T1CL; gap 1
            sink.emit(REG_T1_HI, 0x00, H_T1CH_CONT * E);          // T1CH arms; gap 12
            return;
        }
        // The reference's LONG JUMP, VERBATIM from the capture (its "long vectors" class,
        // x2475: ORA+4 ORB=00+11 ORB=01+1 ORA+9 T1CL+1 T1CH+t1+17). No PCR, no SR inside the
        // unit (the blanking already went out above), T1 with the real count.
        // RAW E CYCLES, like the micro-segment: k.e() scales by e6809_q8 (64 on the UVM2) and
        // halves the gaps — measured: the cadence came out 1/3/1/3.
        /* IF Y'S S&H ALREADY HOLDS THIS VALUE, THE WHOLE SAMPLING IS REDUNDANT — and one
         * capture suggested the reference skips it. That would be its MOST COMMON jump class:
         * `ORA+7 T1CL+1 T1CH+42`, three writes, 70 of its 96 jumps in that frame. We always did
         * all six.
         *
         * MEASURED AGAIN, AND IT COMES OUT THE OPPOSITE WAY. In their frame 120 of Major Havoc
         * the reference loads Y on **647 of 647** units, jumps included, and 98 of those loads
         * are REDUNDANT (the same value it already held). It never skips it.
         *
         * The "70 of its 96 jumps" above came from the ASTEROCK capture, and the two captures do
         * not say the same thing. Between them, physics wins: channel 0 is C304, 10 nF, and a
         * capacitor discharges — skipping the recharge is betting it has not drifted. That is
         * why the measured drift was 7 times larger in Y than in X, which is the live DAC and
         * retains nothing.
         *
         * The mechanism stays behind a knob in case some console needs the cycles, but off:
         * `SKIP_Y=1` brings it back. */
        /* A LONG UNIT THAT IS THE ONLY BLANKED ONE: the blanking goes INSIDE its mux window,
         * exactly as in the short form but with the SR at the other end of the window. Theirs,
         * verbatim (x3 in frame 120, all t1 = 18):
         *
         *     ORA(y)+3  ORB=00+3  SR=00+8  ORB=01+0  ORA(x)+6  T1CL+0  T1CH+34
         *
         * The mux window is 11 cycles in BOTH forms (3+8 here, 8+3 in the short one): what
         * changes is where the SR falls inside it, not how much C304 charges. And the ORA(x)
         * gap drops from 9 to 7 because the SR write has already eaten bus. */
        if sink.beam_is_lit() {
            sink.emit(REG_PORT_A, vy as u8, 3 * E);   // Y; gap 4
            sink.emit(REG_PORT_B, 0x00, 3 * E);       // open mux; gap 4
            sink.emit(REG_SHIFT, 0x00, 8 * E);        // beam OFF inside the window; gap 9
            sink.beam_blanked();
            sink.emit(REG_PORT_B, 0x01, 0);           // close mux; gap 1
            sink.y_held(vy);
            sink.emit(REG_PORT_A, vx as u8, 6 * E + k.x_settle_q8); // X; gap 7
            emit_t1cl(sink, t1, 0);
            sink.emit(REG_T1_HI, (t1 >> 8) as u8, 0);
            // AND THE RAMP WAIT, which I had left at zero here: the T1CH came out with gap 0
            // against their 34 (t1 = 18), i.e. the unit never travelled what it asked for.
            // Same arithmetic as the long jump below: t1 + 16.
            sink.wait_ramp(t1, k.moveto_settle_q8 as i32 + 16 * E as i32);
            return;
        }
        let skip_y = SKIP_Y.load(Ordering::Relaxed) != 0 && sink.y_can_skip(vy);
        if !skip_y {
            sink.emit(REG_PORT_A, vy as u8, 3 * E);   // Y to the DAC; gap 4
            sink.emit(REG_PORT_B, 0x00, 10 * E);      // abre mux; ventana 11
            sink.emit(REG_PORT_B, 0x01, 0);           // close mux; gap 1
            sink.y_held(vy);
        }
        sink.emit(REG_PORT_A, vx as u8,
                  (if skip_y { 6 * E } else { 8 * E }) + k.x_settle_q8); // X; gap 7 or 9
        emit_t1cl(sink, t1, 0);                 // T1CL; gap 1
        sink.emit(REG_T1_HI, (t1 >> 8) as u8, 0);
        // The reference's wait after arming: t1 + 16 (measured: +141 for t1=124, +78 for
        // t1=64 — i.e. t1 + 14..17; 16 is taken and it is sweepable).
        /* THE GAP AFTER T1CH IS A MULTIPLE OF 7.
         *
         * In their frame 120 the ones of this class are 35, 42, 49, 56, 70 and 77 — all 7*n,
         * without exception — and with `gap = 7 * ceil((t1 + 13) / 7)` all NINE distinct t1
         * values come out exactly (22, 26, 28, 29, 31, 37, 41, 52, 56, 60). It is a law and not
         * an adjustment: 13 is the ONLY integer that satisfies all nine inequalities at once.
         * The 7 is presumably their polling loop.
         *
         * We used `t1 + 16`, which is the mean of that and is wrong by +-3 half the time: every
         * error is a ramp too long or too short, i.e. a vector that overshoots or falls short.
         *
         * The LONG UNIT THAT BLANKS IN-WINDOW (the branch above) is NOT on this grid: its three
         * cases of t1 = 18 give 34, and 7*ceil(31/7) would be 35. That is why the change goes
         * only here and not in `wait_ramp`. */
        let gap = T1CH_STEP * ((t1 as u32 + T1CH_BASE + T1CH_STEP - 1) / T1CH_STEP);
        sink.wait_ramp(t1, (gap as i32 - t1 as i32) * E as i32 + k.moveto_settle_q8 as i32);
        return;
    }
    sink.emit(REG_PORT_A, vy as u8, k.e(5)); // STA — Y to the DAC; gap 6 (CLR dp)
    // Y'S WINDOW: lengthening it from `e(9)` to `y_mux_q8` (2 -> 14 E cycles) was tried,
    // reasoning that a capacitor does not accept `e6809_q8`'s discount, which was measured by
    // frame rate. The physics backed the change: tau = 1.8 us = 2.7 cycles, so 2 cycles charge
    // to 52%.
    //
    // MEASURED ON THE CONSOLE on 2026-08-18 and REVERTED: bus_cycles 29529 -> 30333, exactly
    // the +804 that 67 moves x 12 cycles predicts, with overrun going from 0 to 1295. The
    // mechanism was confirmed to the digit AND THE DRAWING DID NOT CHANGE AT ALL. So the short
    // window was not the cause, and lengthening it only takes from the frame budget.
    //
    // It stays as it was, with the note, so nobody "fixes" it again by reading the physics
    // without looking at the measurement.
    /* EXACT GAPS OF THE 6809'S JUMP (BIOS Moveto_d), counted from its direct-page assembly;
     * its cycle is an E cycle, so they are our delays minus the write's own cycle:
     *     CLR VIA_port_b   (6) abrir mux;  PSHS A (5) + LDA# (2) + STA VIA_cntl (4) -> 11
     *     STA VIA_cntl     (4) PCR;        CLR VIA_shift_reg (6)                    ->  6
     *     CLR VIA_shift_reg(6) SR=0;       INC VIA_port_b (6)                       ->  6
     *     INC VIA_port_b   (6) cierra mux; PULS A (5) + STA VIA_port_a (4)          ->  9
     *     STA VIA_port_a   (4) X to DAC;   LDA# (2) + STA VIA_t1_cnt_lo (4)         ->  6
     *     STA VIA_t1_cnt_lo(4) T1CL;       CLR VIA_t1_cnt_hi (6)                    ->  6
     *
     * We were starting the jump's ramp NINE cycles early, with the X DAC still arriving. And
     * since everything drawn afterwards starts from wherever the jump lands, that displaced the
     * interior strokes — the symptom that remained after making only the stroke cycle-exact. */
    sink.emit(REG_PORT_B, 0x00, k.e(10)); // CLR — open mux; gap 11
    sink.emit(REG_CNTL, 0xCE, k.e(5)); // STA — PCR; gap 6
    sink.beam_blanked();
    /* `extend_last` DOES NOT GO HERE: the command already emitted is the PCR, not the stroke's
     * T1CH. The top of `moveto_seq` does it, which is where the last emitted one IS the T1CH. */
    sink.emit(REG_SHIFT, 0x00, k.e(5)); // CLR shift — beam off; gap 6
    sink.emit(REG_PORT_B, 0x01, k.e(8)); // INC — close mux; gap 9
    sink.y_held(vy); // leaves the S&H charged with ITS vy: a draw_line repeating it skips it
    sink.emit(REG_PORT_A, vx as u8, k.e(5) + k.x_settle_q8); // STB — X to the DAC; gap 6
    emit_t1cl(sink, t1, k.e(5)); // T1CL; gap 6 (CLR T1CH dp)
    // THE HIGH BYTE, FOR REAL. It was hard-coded to 0, and that capped the ramp at 255 even
    // though the VIA's T1 counter is 16-bit — 8 bits of travel thrown away.
    sink.emit(REG_T1_HI, (t1 >> 8) as u8, 0);
    sink.wait_ramp(t1, k.moveto_settle_q8 as i32);
}

/// `Draw_Line_d` — the lit stroke. The bulk of the drawing path.
///
/// MOVED from `vinterface.rs`: the sequence, the order of the three branches and their gaps
/// are the ones that were there. The only new thing is that the writes go out through the sink.
///
/// A DEVIATION FROM THE BIOS, inherited and deliberate: the beam is lit through the CNTL
/// (0xEE/0xCE) and not through the shift register with ACR = 0x98. The SR path never drew
/// anything on this hardware at the time; the CNTL one is proven.
pub fn draw_line_seq<S: BusSink>(sink: &mut S, vx: i8, vy: i8, t1: u16, k: &Timings) {
    let (vx, vy) = crate::ramp::trim_dac(vx, vy);   /* see trim_dac: here, not in the ramp */
    // ── THE REFERENCE'S PLOTTER, VERBATIM ($CA51) ────────────────────────────────
    //
    // Each vector is a series of T1=8 micro-segments at the same rate (vx,vy). The beam is
    // lit ONCE (SR=0x01, their exact value) when it was not already, and it is NOT blanked
    // between chained strokes — only a jump (moveto) or the re-zero blank. The cadence is in
    // RAW E CYCLES, measured from their asterock bus capture (not through k.e(), which
    // scales by the 6809 and halved the gaps):
    //   ORA(Y) -6-> ORB=0 -8-> ORB=1 -1-> ORA(X) -4-> T1CL=8 -1-> T1CH=0 -12-> next
    // The gap of 12 after T1CH lets the ramp of 8 finish before the next micro-segment.
    // t1 IS ROUNDED TO MULTIPLES OF 8: no remainder micro-segment.
    //
    // I once emitted the tail here with whatever count was left (T1CL = t1 % 8), justifying
    // it with the reference having T1CL=4/6/7 in its capture. I MEASURED IT WRONG: those are
    // 1.11 per FRAME against its 230.2 of T1=8, i.e. 0.5%. With the tail, we emitted 71 of
    // 308 units per frame with T1 between 1 and 6 — 23%.
    //
    // And a ramp of 1 to 6 counts does not draw: the beam barely moves, but the unit costs
    // its ~32 bus cycles WITH THE BEAM LIT, so it is a DOT. Those were the dots visible at
    // every vertex on the console.
    //
    // The price of the rounding is up to 4 counts of t1 per stroke, which at the typical
    // rate (48) is ~1.2 device units — and the debt chain charges it to the next stroke, so
    // it does not accumulate.
    const T1M: u16 = 8;
    let n = core::cmp::max(1, ((t1 as u32) + (T1M as u32) / 2) / (T1M as u32));

    /* THE RATE IS RECOMPUTED FOR THE TIME THAT WILL ACTUALLY RUN.
     *
     * `ramp_params` splits the distance between speed and time and returns some `t1`; here that
     * time is rounded to n micro-segments of 8 — and until now the RATE was left as it was. The
     * distance is `v*t1/s`, so changing the time without touching the speed changes what gets
     * drawn.
     *
     * MEASURED with the reference geometry as input, its first segment asks for 2.5 units:
     * `ramp_params` gave `t1=10, vy=40` (correct: 40*10/160 = 2.5), this ran it at `t1=8` with
     * the same rate and it came out **2.0 units — 20% short**. The reference draws that same
     * segment with `t1=8, vy=50`: it chooses the rate FOR the time, which is what was missing
     * here.
     *
     * The error is not random: it depends on where t1 falls relative to the multiple of 8, so
     * similar strokes shorten similarly and the figure comes out deformed systematically, not
     * noisily. */
    /* THE FOUR GAPS, RESOLVED ONCE. They are GAPS (what separates one write from the next); the
     * emitter wants gap-1 scaled by E. Outside the loop and without a closure: that way the
     * value in use is visible and there is no doubt about capture. */
    let gap = |v: u32, def: u32| -> u32 {
        let gap = if v > 0 { v } else { def };
        (gap.max(1) - 1) * E
    };
    use core::sync::atomic::Ordering as O;
    /* THE FOUR DEFAULT GAPS ARE THE MEASURED ONES, SINCE 2026-09-04. They were 6/8/4/9, from
     * the asterock capture, and the tuned ports overrode them by hand with 4/10/9/4 — which are
     * the ones validated all day against their Major Havoc capture (the bench matches 99.8% of
     * their strokes with them). A measured value you have to remember to set in every game is
     * not a default, it is a trap. */
    let h_ora_y    = gap(MT_ORA_Y.load(O::Relaxed), 4);
    let h_orb_keep = gap(MT_ORB_KEEP.load(O::Relaxed), 10);
    let h_sr_on    = gap(MT_SR_ON.load(O::Relaxed), 9);
    let h_ora_x    = gap(MT_ORA_X_ON.load(O::Relaxed), 4);

    /* NO RESCALING HAPPENS HERE ANY MORE. `ramp_params_chain_q4` does it, returning the t1
     * already rounded to n micro-segments and the rate computed for it — and that way the DEBT
     * sees that rounding. When it was done here, its residue fell outside the accounting and the
     * position drifted +8.45 units in X across the frame with the debt at zero.
     *
     * It is kept for the INTEGER path (no sub-units), where the ramp does not round to multiples
     * of 8 and this adjustment is still needed: without it, a stroke that asks for 2.5 units
     * comes out at 2.0 (measured with the reference geometry). */
    let integer = INTEGER_STROKE.load(O::Relaxed) != 0;
    let (n, t1u) = if integer { (1u32, t1) } else { (n, T1M) };
    let runs = if integer { t1 as i32 } else { (n as i32) * (T1M as i32) };
    let (vx, vy) = if runs as u16 == t1 {
        (vx, vy)                                  // it already arrives adjusted
    } else {
        let rescale = |v: i8| -> i8 {
            let num = (v as i32) * (t1 as i32);
            let q = if num >= 0 { (num + runs / 2) / runs }
                    else        { (num - runs / 2) / runs };
            q.clamp(-128, 127) as i8
        };
        (rescale(vx), rescale(vy))
    };

    for i in 0..n {
        sink.emit(REG_PORT_A, vy as u8, h_ora_y); // ORA=Y
        let turn_on = !sink.beam_is_lit();
        if turn_on {
            sink.emit(REG_PORT_B, 0x00, 5 * E); // ORB=0 mux opens, latches Y ; gap 6
            sink.emit(REG_SHIFT, 0x01, h_sr_on);  // SR=0x01 beam ON (once)
            sink.beam_lit();
        } else {
            sink.emit(REG_PORT_B, 0x00, h_orb_keep); // ORB=0 mux opens, latches Y
        }
        sink.emit(REG_PORT_B, 0x01, 0);         // ORB=1 mux closes ; gap 1
        // On the unit that LIGHTS, the reference leaves gap 9 after X (its most common SR=01
        // pattern: ORA+6 ORB+6 SR=01+4 ORB+1 ORA+9 T1CL+1 T1CH+12): the 0x01 pattern takes 7
        // cycles to reach the lit bit and that gap has the ramp already running when the beam
        // appears. On chained ones, gap 4.
        sink.emit(REG_PORT_A, vx as u8,
                  if turn_on { h_ora_x } else { 3 * E }); // ORA=X
        emit_t1cl(sink, t1u, 0);              // THIS ramp's duration ; gap 1
        /* The gap after T1CH lets the ramp finish: with micro-segments it is the fixed 8 plus
         * 3; with the whole stroke, its duration plus the same 3. */
        sink.emit(REG_T1_HI, 0x00, (H_T1CH_CONT - T1M as u32 + t1u as u32) * E);
    }
    sink.y_held(vy);
    // WITHOUT blanking: keep-lit between chained strokes, like the reference. moveto_seq (the
    // jump) and the re-zero do the blanking.
}


pub fn draw_line_patterned_seq<S: BusSink>(
    sink: &mut S, vx: i8, vy: i8, t1: u16, k: &Timings, gaps: &[(u16, u16)],
) {
    if gaps.is_empty() {
        return draw_line_seq(sink, vx, vy, t1, k);
    }
    if !sink.y_can_skip(vy) {
        sink.emit(REG_PORT_A, vy as u8, k.e(2));
        sink.emit(REG_PORT_B, 0x00, k.y_mux_q8);
        sink.emit(REG_PORT_B, 0x01, k.e(4));
        sink.y_held(vy);
    }
    sink.emit(REG_PORT_A, vx as u8, k.e(3));
    emit_t1cl(sink, t1, 0);
    // The ramp starts BEFORE lighting, exactly as in draw_line_seq: lighting with the spot
    // still leaves a bright dot at the departing vertex.
    sink.emit(REG_T1_HI, (t1 >> 8) as u8, k.beam_on_q8);

    let mut cur: u16 = 0;
    /* THE GAPS ARE IN RAMP COUNTS, AND THOSE ARE RAW E CYCLES. `k.e()` scales by `e6809_q8`
     * — 64 on the UVM2, not 256 — so putting a T1 position through it leaves it at a quarter:
     * the whole pattern bunches into the ramp's first stretch. It is the same trap already
     * noted in the long jump, and it was seen on the console with esb's text: the letters piled
     * into a corner. `wait_ramp` confirms it — `d = t1 + extra/256`, i.e. one T1 count = one
     * bus cycle. */
    // THROUGH `beam`, NOT THE PCR DIRECTLY. This used to write REG_CNTL directly, and with the
    // default dialect — the beam through the shift register, BEAM_VIA_SR starting at 1 — those
    // writes DO NOTHING: the whole sweep came out lit, gaps included, and with it the jumps
    // between sweeps. Seen on the console with esb's text (2026-09-21): rows of dots joined by
    // a fan of streaks coming out of one dot. Nobody had caught it because since the SR dialect
    // became the default nobody had come through here — the uvm2_draw_delta_patterned note says
    // so.
    if gaps[0].0 > 0 {
        beam(sink, true, gaps[0].0 as u32 * E);
        sink.beam_lit();
        cur = gaps[0].0;
    }
    for (i, &(a, b)) in gaps.iter().enumerate() {
        let _ = a;                       // `a` was already reached by the previous delay
        let end = b.min(t1);
        let next = gaps.get(i + 1).map(|g| g.0).unwrap_or(t1);
        // blanked through the gap
        beam(sink, false, end.saturating_sub(cur) as u32 * E);
        sink.beam_blanked();
        cur = end;
        // and lit until the next gap (or until the end)
        if next > cur {
            beam(sink, true, (next - cur) as u32 * E);
            sink.beam_lit();
            cur = next;
        }
    }
    // Whatever is left of the ramp, polled as usual: the settling at the end is what turns
    // bright dots into vertices, and there you do not count, you ask.
    sink.wait_ramp(t1.saturating_sub(cur), k.e(4) as i32 + k.blank_settle_q8);
    beam(sink, false, 0);
    sink.beam_blanked();
}

/// A STRAIGHT LINE WHOSE BLANKING IS DICTATED BY THE SHIFT REGISTER: the BIOS's text.
///
/// `draw_line_patterned_seq` toggles the beam gap by gap through the PCR, and that is TWO
/// commands per gap. Measured with real esb text (634 characters,
/// tools/uvm2_raster_text.c): 30.4 commands per character, and the gaps are most of it.
///
/// The VIA can do it on its own. With ACR = 0x98 the shift register is in mode 110 -- it
/// shifts 8 bits out at the Phi2 rate and stops, leaving CB2 (~BLANK) holding the last one --
/// so ONE write paints EIGHT dots. A character 8 dots wide goes from ~3 commands per row to 1,
/// which is exactly how the Vectrex draws its own text.
///
/// THE LETTER'S SIZE IS NOT CHOSEN HERE, and you need to know it: the 8 shifts last 8 Phi2
/// cycles whatever happens, so a character's width is set by the ramp's SPEED (`vx`). `step`
/// is the T1 counts between writes and must be 8 for the dots to come out contiguous; the
/// caller picks `vx` so that 8 cycles are the width it wants. Asking for another size means
/// changing `vx`, not `step`.
///
/// The last byte leaves CB2 holding its bit 0. It always closes with SR = 0x00: otherwise a
/// row that ends on a lit dot leaves the beam running until the next write.
pub fn draw_line_sr_seq<S: BusSink>(
    sink: &mut S, vx: i8, vy: i8, t1: u16, k: &Timings, pattern: &[u8], step: u16,
) {
    if pattern.is_empty() {
        return draw_line_seq(sink, vx, vy, t1, k);
    }
    /* Y IS ALWAYS RE-SAMPLED IN A SWEEP, whatever it costs.
     *
     * `y_can_skip` says whether the S&H already holds this velocity, and for a normal vector
     * that is correct: it lasts 20-40 counts. A text sweep lasts n*step — up to 255, i.e.
     * ~168 us against ~25 — and Y's S&H is a CAPACITOR: in that time it droops enough for the
     * line to come out slanted. And since the droop is proportional to the voltage, it slants
     * MORE the higher the text sits, which is exactly what the console showed: straight at the
     * bottom, very diagonal at the top, and worse as the scroll goes up.
     *
     * The three writes it costs (ORA, open mux, close mux) are the price of the line coming out
     * straight. It is the asymmetry the `vxs_y_can_skip` note already pointed at.
     *
     * AND Y'S WINDOW HERE IS TOO SHORT, which is why the caller sets it beforehand.
     *
     * `y_mux_q8` is fixed (4 E cycles). The normal path uses `set_y`, which scales it with how
     * FAR Y has to go (`hold_between`, a logarithmic law) because mux channel 0 is a 10 nF
     * capacitor. A text sweep asks for vy = 0 right after a big jump in Y, and with 4 cycles the
     * capacitor does not finish discharging: a residual velocity is left and the line comes out
     * SLANTED — all the more the higher the text is, because the bigger the jump was. Measured
     * on the console: straight at Y=0 and progressively more diagonal as it rises.
     *
     * If the caller has already set Y (uvm2_draw_sweep_sr calls `set_y`), this skips it and does
     * not spoil it with a short window. */
    if !sink.y_can_skip(vy) {
        sink.emit(REG_PORT_A, vy as u8, k.e(2));
        sink.emit(REG_PORT_B, 0x00, k.y_mux_q8);
        sink.emit(REG_PORT_B, 0x01, k.e(4));
        sink.y_held(vy);
    }
    sink.emit(REG_PORT_A, vx as u8, k.e(3));
    emit_t1cl(sink, t1, 0);
    sink.emit(REG_T1_HI, (t1 >> 8) as u8, k.beam_on_q8);

    let mut cur: u16 = 0;
    for &b in pattern {
        if cur >= t1 {
            break;
        }
        let d = step.min(t1 - cur);
        /* `step - 1` AND NOT `step`: the executor spends 1 + delay cycles per command (see
         * uvm2_exec), so asking for `step` of delay consumes `step + 1` ramp counts. The pattern
         * ran one count ahead per character and the LAST of each sweep fell outside the ramp —
         * on the bench it showed up as the sixth glyph of every group of six degraded. */
        let gap = if d > 1 { (d as u32 - 1) * E } else { 0 };
        sink.emit(REG_SHIFT, b, gap);
        cur += d;
        if b & 1 != 0 { sink.beam_lit(); } else { sink.beam_blanked(); }
    }
    /* SR = 0x00 NEEDS ITS 8 CYCLES, and with a zero gap it does not get them. The
     * `beam_off_and_wait` note in uvm2_draw.c says so and the console showed it: writing zero
     * does not blank immediately, the register takes 8 shifts to get it out through CB2. If the
     * re-zero clamp comes right after, the beam is still alive while the integrators discharge —
     * and that is not a streak, it is a CURVE (the discharge is RC). On screen, a fan of curves
     * converging on the centre. The same gap the rest of the file uses. */
    sink.wait_ramp(t1.saturating_sub(cur), k.e(4) as i32 + k.blank_settle_q8);
    sink.emit(REG_SHIFT, 0x00, H_SR_OFF_A_ORA * k.e6809_q8);
    sink.beam_blanked();
}

/// The two values `moveto` uses when `VARIABLE_T1` is off, so the caller does not have to know
/// `DRAW_SCALE`.
pub fn fixed_ramp(dx: i8, dy: i8) -> (i8, i8, u16) {
    let _ = MIN_T1.load(Ordering::Relaxed);
    (dx, dy, scale() as u16)
}

#[cfg(test)]
mod test {
    use super::*;

    /// A sink that only RECORDS. It is step 3's verification: the sequence is checked on the
    /// Mac, with no console — and it has to be that way, because on the multicart board every
    /// SWD read resets it.
    #[derive(Default)]
    struct Paper {
        v: std::vec::Vec<(u8, u8, u32)>,
        blanked: u32,
        y: std::vec::Vec<i8>,
        skip_y: Option<i8>,
        lit: bool,
    }
    impl BusSink for Paper {
        fn emit(&mut self, r: u8, d: u8, q: u32) {
            self.v.push((r, d, q));
        }
        fn wait_ramp(&mut self, t1: u16, extra: i32) {
            // Recorded as a pseudo-command so the test can assert WHERE the wait falls.
            self.v.push((0xFF, 0, 0x8000_0000 | t1 as u32));
            self.v.push((0xFE, 0, extra as u32));
        }
        fn y_can_skip(&mut self, vy: i8) -> bool {
            self.skip_y == Some(vy)
        }
        fn beam_is_lit(&self) -> bool {
            self.lit
        }
        fn beam_lit(&mut self) {
            self.lit = true;
        }
        fn beam_blanked(&mut self) {
            self.blanked += 1;
        }
        fn y_held(&mut self, vy: i8) {
            self.y.push(vy);
        }
    }

    /* THE BEAM KNOB IS A GLOBAL AND THE TESTS RUN IN PARALLEL.
     *
     * Three tests need the PCR dialect (`BEAM_VIA_SR = 0`) and set it without restoring it, so
     * they clobbered each other and the ones that expect the default dialect: the suite failed
     * 3 or 4 tests depending on the order they happened to run in. This serialises it and puts
     * the value back on the way out. */
    static KNOB: std::sync::Mutex<()> = std::sync::Mutex::new(());
    pub struct Dialect(std::sync::MutexGuard<'static, ()>, u32);
    impl Dialect {
        pub fn via_pcr() -> Self { Self::set(0) }
        pub fn set(v: u32) -> Self {
            let g = KNOB.lock().unwrap_or_else(|e| e.into_inner());
            let before = crate::ramp::BEAM_VIA_SR.load(Ordering::Relaxed);
            crate::ramp::BEAM_VIA_SR.store(v, Ordering::Relaxed);
            Dialect(g, before)
        }
        pub fn change(&self, v: u32) { crate::ramp::BEAM_VIA_SR.store(v, Ordering::Relaxed); }
    }
    impl Drop for Dialect {
        fn drop(&mut self) { crate::ramp::BEAM_VIA_SR.store(self.1, Ordering::Relaxed); }
    }

    /// The EXPECTED list is transcribed from the original `moveto` in vinterface.rs, not
    /// generated by this code. If it were generated with the same thing it verifies, it would
    /// verify nothing — it would only say the function equals itself.
    #[test]
    fn moveto_emits_the_bios_sequence() {
        /* THIS TEST EXERCISES PCR BLANKING, which since 2026-09-04 is no longer the default
         * (`BEAM_VIA_SR` starts at 1). It is still a live path — `-DUVM2_BEAM_VIA_PCR` — so it
         * is pinned here instead of deleting the test. */
        let _dialect = Dialect::via_pcr();
        /* THE CADENCE THIS TEST ASSERTS IS ASTEROCK'S (6/8/4/9). Since 2026-09-04 the defaults
         * are MAJOR HAVOC's (4/10/9/4), which are the ones validated against that capture; both
         * are theirs and both are real. They are pinned here so the test exercises ONE concrete
         * cadence and not whatever today's default happens to be. */
        MT_ORA_Y.store(6, core::sync::atomic::Ordering::Relaxed);
        MT_ORB_KEEP.store(8, core::sync::atomic::Ordering::Relaxed);
        MT_SR_ON.store(4, core::sync::atomic::Ordering::Relaxed);
        MT_ORA_X_ON.store(9, core::sync::atomic::Ordering::Relaxed);
        let k = timings(0, 0);
        let mut p = Paper::default();
        moveto_seq(&mut p, 40, -20, 0x5A, &k);

        assert_eq!(
            p.v,
            std::vec![
                // THE GAPS ARE THE 6809'S (BIOS Moveto_d), counted from its direct-page
                // assembly: delay = gap - 1, because the write spends one cycle.
                (REG_PORT_A, (-20i8) as u8, 5 * E),  // Y to the D/A; gap 6
                (REG_PORT_B, 0x00, 10 * E),          // open mux;     gap 11
                (REG_CNTL, 0xCE, 5 * E),             // PCR;          gap 6
                (REG_SHIFT, 0x00, 5 * E),            // SR=0;         gap 6
                (REG_PORT_B, 0x01, 8 * E),           // close mux;    gap 9
                (REG_PORT_A, 40u8, 5 * E),           // X to the D/A; gap 6
                (REG_T1_LO, 0x5A, 5 * E),            // T1CL;         gap 6
                (REG_T1_HI, 0x00, 0),                // T1CH starts the ramp
                (0xFF, 0, 0x8000_005A),              // and it waits for it to finish
                (0xFE, 0, 0),                        // plus Moveto_d's settling
            ]
        );
        assert_eq!(p.blanked, 1, "the beam is blanked ONCE, on the 0xCE");
        assert_eq!(p.y, std::vec![-20i8], "the S&H is left holding ITS vy after closing the mux");
    }

    fn timings(beam_on_q8: u32, blank_settle_q8: i32) -> Timings {
        Timings {
            e6809_q8: 256,
            y_mux_q8: 14 * 256,
            moveto_settle_q8: 0,
            beam_on_q8,
            blank_settle_q8,
            keep_lit: false,
            x_settle_q8: 0,
        }
    }

    /// THE REFERENCE'S PLOTTER, which has been the specification since 2026-09-03: the stroke
    /// is a series of T1=8 micro-segments at the SAME rate, raw cadence 6/8/1/4/1/12 (E cycles,
    /// measured from their asterock bus capture), the beam is lit ONCE through SR=0x01 (the
    /// pattern takes 7 cycles to reach the lit bit: the ramp is already running when the beam
    /// appears) and it is NOT blanked at the end — keep-lit; the jump and the re-zero blank.
    #[test]
    fn draw_line_starts_the_ramp_before_lighting() {
        /* THE CADENCE THIS TEST ASSERTS IS ASTEROCK'S (6/8/4/9). Since 2026-09-04 the defaults
         * are MAJOR HAVOC's (4/10/9/4), which are the ones validated against that capture; both
         * are theirs and both are real. They are pinned here so the test exercises ONE concrete
         * cadence and not whatever today's default happens to be. */
        MT_ORA_Y.store(6, core::sync::atomic::Ordering::Relaxed);
        MT_ORB_KEEP.store(8, core::sync::atomic::Ordering::Relaxed);
        MT_SR_ON.store(4, core::sync::atomic::Ordering::Relaxed);
        MT_ORA_X_ON.store(9, core::sync::atomic::Ordering::Relaxed);
        /* THIS TEST COVERS THE MICRO-SEGMENT PATH, which since 2026-09-04 is no longer the
         * default but still exists behind `-DUVM2_MICROSEGMENTS`. The whole-stroke one is
         * `a_stroke_is_one_ramp`. */
        INTEGER_STROKE.store(0, core::sync::atomic::Ordering::Relaxed);
        vx_t1cl_forget(); // the T1CL cache is global state shared between tests
        let k = timings(2 * E, 11 * E as i32);
        let mut p = Paper::default();
        draw_line_seq(&mut p, 30, -10, 0x3E, &k);
        /* 0x3E = 62 counts -> 8 micro-segments of 8, i.e. 64 counts in reality. The FIRST
         * one, literally.
         *
         * X COMES OUT 29 AND NOT THE 30 ASKED FOR, AND THAT IS CORRECT: the distance is v*t1/s,
         * so 30 for the 62 requested is 11.6 units, and over the 64 that actually run v = 29 is
         * needed. Previously the rate was left as it came and 12.0 was drawn — 3% too far here,
         * and up to 20% on short strokes. See the `rescale` block above. */
        assert_eq!(
            &p.v[..7],
            &[
                (REG_PORT_A, (-10i8) as u8, 5 * E), // Y to the DAC;  gap 6
                (REG_PORT_B, 0x00, 5 * E),          // open mux;      gap 6
                (REG_SHIFT, 0x01, 3 * E),           // beam ON (once); gap 4
                (REG_PORT_B, 0x01, 0),              // close mux;     gap 1
                (REG_PORT_A, 29u8, 8 * E),          // X to the DAC (rescaled); gap 9
                (REG_T1_LO, 8, 0),                  // T1CL=8;        gap 1
                (REG_T1_HI, 0x00, H_T1CH_CONT * E), // T1CH arms;     gap 12
            ]
        );
        // The dialect's invariants, over the whole list:
        assert_eq!(p.v.iter().filter(|c| c.0 == REG_T1_HI).count(), 8, "62 -> round(62/8) = 8 micro-segments");
        assert!(p.v.iter().filter(|c| c.0 == REG_T1_LO).all(|c| c.1 == 8),
                "EVERY micro-segment is T1=8: a shorter ramp does not draw, it burns a dot");
        assert_eq!(p.v.iter().filter(|c| c.0 == REG_SHIFT).count(), 1, "SR=0x01 ONCE");
        assert!(p.v.iter().all(|c| c.0 != REG_CNTL), "the PCR does nothing in this dialect");
        assert!(p.lit, "beam_lit must have been called");
        assert_eq!(p.blanked, 0, "keep-lit: draw_line does NOT blank; the jump and re-zero do");
    }

    /// A STRAIGHT LINE WITH ONE GAP, IN A SINGLE RAMP. What to look at: the DACs and T1 are
    /// programmed ONCE — there is no second header half way — the beam is blanked and lit again
    /// while the ramp runs, and the final wait is for the REMAINDER, not for the whole `t1`:
    /// otherwise, on the board that counts the delay it would wait twice.
    #[test]
    fn a_ramp_with_a_gap() {
        let k = timings(2 * E, 11 * E as i32);
        let mut p = Paper::default();
        // The PCR dialect, which is the one written out byte for byte below. The default is
        // the OTHER one (it starts at 1), and it is checked in this test's second half.
        let dialect = Dialect::via_pcr();
        // 62 ramp counts, blanked from 20 to 30
        draw_line_patterned_seq(&mut p, 30, -10, 0x3E, &k, &[(20, 30)]);
        assert_eq!(
            p.v,
            std::vec![
                (REG_PORT_A, (-10i8) as u8, 2 * E), // STA — Y
                (REG_PORT_B, 0x00, 14 * E),         // CLR — mux ch0
                (REG_PORT_B, 0x01, 4 * E),          // INC — mux off
                (REG_PORT_A, 30u8, 3 * E),          // STB — X
                (REG_T1_LO, 0x3E, 0),               // T1CL
                (REG_T1_HI, 0x00, 2 * E),           // the ramp starts, and only here
                (REG_CNTL, 0xEE, 20 * E),           // lit for the first 20 counts
                (REG_CNTL, 0xCE, 10 * E),           // blanked for 10 counts — THE GAP
                (REG_CNTL, 0xEE, 32 * E),           // lit until the end
                (0xFF, 0, 0x8000_0000),             // wait for the REMAINDER (none left)
                (0xFE, 0, 4 * E + 11 * E),          // + latency and settling
                (REG_CNTL, 0xCE, 0),                // blank
            ]
        );
        // two DAC/T1 programmings would mean splitting the line in two; here there is ONE
        assert_eq!(p.v.iter().filter(|c| c.0 == REG_T1_HI).count(), 1);
        assert_eq!(p.v.iter().filter(|c| c.0 == REG_PORT_A).count(), 2); // vy y vx

        /* AND THE SAME GAP IN THE DEFAULT DIALECT, which is what was missing and cost a trip
         * to the console: this routine wrote the PCR directly, and with the beam governed by the
         * shift register — which is how it starts — those writes DO NOTHING. The whole sweep
         * came out lit, gaps included, and with it the jumps between sweeps: on screen, rows of
         * dots joined by a fan of streaks. The rest of the file already went through `beam`;
         * this was the only one that did not.
         *
         * It goes in the SAME test and not in a separate one because `BEAM_VIA_SR` is a global
         * and the tests run in parallel: separated, they clobber the knob between them. */
        dialect.change(1);
        let mut q = Paper::default();
        draw_line_patterned_seq(&mut q, 30, -10, 0x3E, &k, &[(20, 30)]);
        assert!(q.v.iter().all(|c| c.0 != REG_CNTL), "the PCR does nothing in this dialect");
        let sr: std::vec::Vec<u8> = q.v.iter().filter(|c| c.0 == REG_SHIFT).map(|c| c.1).collect();
        assert_eq!(sr, std::vec![0xFF, 0x00, 0xFF, 0x00], "light, THE GAP, light, blank");

        /* WITH THE REAL CLOCK, which is what was missing. Both halves above use
         * `timings(2*E, ..)`, i.e. e6809_q8 = E: at that scale `k.e(n)` and `n*E` give the SAME
         * thing and a badly converted ramp position goes unnoticed. On the UVM2 e6809_q8 is 64,
         * and through that the whole pattern went into the ramp's first quarter — the text piled
         * into a corner that was seen on the console. The gaps are T1 counts and they have to
         * come out in raw cycles whatever happens. */
        let real = Timings { e6809_q8: 64, ..timings(2 * E, 11 * E as i32) };
        let mut w = Paper::default();
        draw_line_patterned_seq(&mut w, 30, -10, 0x3E, &real, &[(20, 30)]);
        let gaps: std::vec::Vec<u32> =
            w.v.iter().filter(|c| c.0 == REG_SHIFT).map(|c| c.2).collect();
        assert_eq!(gaps[0], 20 * E, "lit until the ramp's count 20");
        assert_eq!(gaps[1], 10 * E, "blanked for 10 counts");
    }

    /// The Y sampling is NEVER skipped in the micro-segment dialect: the reference re-latches
    /// Y on EVERY unit (measured in the capture: ORB=00/01 on all 187,850 micro-segments),
    /// because the S&H capacitor leaks and the unit is what refreshes it. The optimisation of
    /// skipping the sampling belonged to the one-ramp-per-vector model.
    #[test]
    fn draw_line_skips_the_y_sampling() {
        /* THE CADENCE THIS TEST ASSERTS IS ASTEROCK'S (6/8/4/9). Since 2026-09-04 the defaults
         * are MAJOR HAVOC's (4/10/9/4), which are the ones validated against that capture; both
         * are theirs and both are real. They are pinned here so the test exercises ONE concrete
         * cadence and not whatever today's default happens to be. */
        MT_ORA_Y.store(6, core::sync::atomic::Ordering::Relaxed);
        MT_ORB_KEEP.store(8, core::sync::atomic::Ordering::Relaxed);
        MT_SR_ON.store(4, core::sync::atomic::Ordering::Relaxed);
        MT_ORA_X_ON.store(9, core::sync::atomic::Ordering::Relaxed);
        /* THIS TEST COVERS THE MICRO-SEGMENT PATH, which since 2026-09-04 is no longer the
         * default but still exists behind `-DUVM2_MICROSEGMENTS`. The whole-stroke one is
         * `a_stroke_is_one_ramp`. */
        INTEGER_STROKE.store(0, core::sync::atomic::Ordering::Relaxed);
        vx_t1cl_forget(); // the T1CL cache is global state shared between tests
        let k = timings(2 * E, 0);
        let mut p = Paper { skip_y: Some(-10), ..Default::default() };
        draw_line_seq(&mut p, 30, -10, 0x3E, &k);
        assert_eq!(p.v[0], (REG_PORT_A, (-10i8) as u8, 5 * E),
                   "it starts with Y even though the S&H says it already holds it");
        let windows = p.v.iter().filter(|c| c.0 == REG_PORT_B && c.1 == 0x00).count();
        assert_eq!(windows, 8, "one mux window per micro-segment, like the reference");
    }

    /// T1's high byte has to TRAVEL. It was hard-coded to 0 and that capped the ramp at 255
    /// even though the counter is 16-bit, leaving the speed cap with no range.
    #[test]
    fn t1_carries_both_bytes() {
        /* THIS TEST EXERCISES PCR BLANKING, which since 2026-09-04 is no longer the default
         * (`BEAM_VIA_SR` starts at 1). It is still a live path — `-DUVM2_BEAM_VIA_PCR` — so it
         * is pinned here instead of deleting the test. */
        let _dialect = Dialect::via_pcr();
        let k = timings(0, 0);
        let mut p = Paper::default();
        moveto_seq(&mut p, 1, 1, 0x0123, &k);
        assert_eq!(p.v[6], (REG_T1_LO, 0x23, 5 * E), "the jump's T1CL; gap 6 (CLR T1CH dp)");
        assert_eq!(p.v[7].1, 0x01, "the high byte is being lost again");
    }

    /// A LIT STROKE IS A SINGLE RAMP, with its whole t1 — like the reference.
    ///
    /// MEASURED over 8 frames of their Major Havoc capture: it does not split a stroke ONCE,
    /// and its lit t1 values run from 8 to 252. We emitted t1 = 8 ALWAYS and split the rest;
    /// every joint leaves the beam lit and still for 22-24 cycles, i.e. a dot. Confirmed on the
    /// console: with micro-segments Major Havoc dots, with the whole stroke it does not.
    #[test]
    fn a_stroke_is_one_ramp() {
        vx_t1cl_forget();
        INTEGER_STROKE.store(1, core::sync::atomic::Ordering::Relaxed);
        let k = timings(2 * E, 11 * E as i32);
        let mut p = Paper::default();
        draw_line_seq(&mut p, 30, -10, 0x3E, &k);
        let t1cl: std::vec::Vec<_> = p.v.iter().filter(|(r, _, _)| *r == REG_T1_LO).collect();
        assert_eq!(t1cl.len(), 1, "a single ramp, not eight micro-segments");
        assert_eq!(t1cl[0].1, 62, "and with the t1 that was asked for, not with 8");
        /* AND THE RATE IS NOT RESCALED, because it is no longer needed: the ramp runs exactly
         * the 62 counts asked for, so the 30 that came in are the 30 that go out. With
         * micro-segments it ran 64 and it had to come down to 29. */
        let ora: std::vec::Vec<_> = p.v.iter().filter(|(r, _, _)| *r == REG_PORT_A).collect();
        assert_eq!(ora[1].1, 30u8, "the rate is emitted as it came");
    }
}

// ── Bridge to C: the UVM2 image's sink ───────────────────────────────────────
//
// Function pointers because whoever implements the sink there is C. The context travels
// opaque: `uvm2_draw.c` gets by with its own global state.
//
// ALL INTEGER, without a single float, which is the crate's rule: the image is compiled
// `softfp` and the firmware `eabihf`, and the linker compares Tag_ABI_VFP_args even though
// none crosses.

use core::ffi::c_void;

/// The sink as seen from C.
#[repr(C)]
pub struct CSink {
    pub ctx: *mut c_void,
    pub emit: extern "C" fn(*mut c_void, u32, u32, u32),
    pub wait_ramp: extern "C" fn(*mut c_void, u32, i32),
    /// Optional: a board that does not keep that bookkeeping passes null.
    pub beam_blanked: Option<extern "C" fn(*mut c_void)>,
    pub y_held: Option<extern "C" fn(*mut c_void, i32)>,
    /* AT THE END, AND OPTIONAL, on purpose. The C consumer builds its sink with
     * `struct vx_sink s = { 0 }`, so a board that does not fill these three in gets exactly the
     * old behaviour instead of reading garbage.
     *
     * WHY THEY APPEAR NOW. The emitter has been asking for them for a while (`y_can_skip`,
     * `beam_is_lit`), but `CSink` did not have them: any C caller fell into the trait's
     * defaults — false and false — with nothing saying so. Our own cartridge's firmware DOES
     * implement them, because it uses the crate as an rlib and the trait directly. Result: both
     * boards shared the emitter and only one had the optimisations. Measured on 2026-08-26 on
     * the UVM2: `Y_HELD` at 0 all the time and `VCAP_SLOW_HITS` at 0, which is what exposed
     * it. */
    pub y_can_skip: Option<extern "C" fn(*mut c_void, i32) -> i32>,
    pub beam_is_lit: Option<extern "C" fn(*mut c_void) -> i32>,
    pub beam_lit: Option<extern "C" fn(*mut c_void)>,
    pub x_can_skip: Option<extern "C" fn(*mut c_void, i32) -> i32>,
    pub extend_last: Option<extern "C" fn(*mut c_void, u32)>,
}

impl BusSink for CSink {
    fn emit(&mut self, reg: u8, data: u8, delay_q8: u32) {
        (self.emit)(self.ctx, reg as u32, data as u32, delay_q8);
    }
    fn wait_ramp(&mut self, t1: u16, extra_q8: i32) {
        (self.wait_ramp)(self.ctx, t1 as u32, extra_q8);
    }
    fn extend_last(&mut self, extra_q8: u32) {
        if let Some(f) = self.extend_last {
            f(self.ctx, extra_q8);
        }
    }
    fn beam_blanked(&mut self) {
        if let Some(f) = self.beam_blanked {
            f(self.ctx);
        }
    }
    fn y_held(&mut self, vy: i8) {
        if let Some(f) = self.y_held {
            f(self.ctx, vy as i32);
        }
    }
    fn y_can_skip(&mut self, vy: i8) -> bool {
        match self.y_can_skip {
            Some(f) => f(self.ctx, vy as i32) != 0,
            None => false,
        }
    }
    fn beam_is_lit(&self) -> bool {
        match self.beam_is_lit {
            Some(f) => f(self.ctx) != 0,
            None => false,
        }
    }
    fn beam_lit(&mut self) {
        if let Some(f) = self.beam_lit {
            f(self.ctx);
        }
    }
    fn x_can_skip(&mut self, vx: i8) -> bool {
        match self.x_can_skip {
            Some(f) => f(self.ctx, vx as i32) != 0,
            None => false,
        }
    }
}

/// The gaps, as seen from C. Same fields and same unit (Q8 of an E cycle).
#[repr(C)]
pub struct CTimings {
    pub e6809_q8: u32,
    pub y_mux_q8: u32,
    pub moveto_settle_q8: u32,
    pub beam_on_q8: u32,
    pub blank_settle_q8: i32,
    pub keep_lit: u32,
    /* AT THE END ON PURPOSE: this struct crosses the crate boundary into C, so a new field
     * goes behind the others so as not to move the ones already there. See x_settle_q8 in
     * Timings. */
    pub x_settle_q8: u32,
}

impl CTimings {
    fn to_timings(&self) -> Timings {
        Timings {
            e6809_q8: self.e6809_q8,
            y_mux_q8: self.y_mux_q8,
            moveto_settle_q8: self.moveto_settle_q8,
            beam_on_q8: self.beam_on_q8,
            blank_settle_q8: self.blank_settle_q8,
            keep_lit: self.keep_lit != 0,
            x_settle_q8: self.x_settle_q8,
        }
    }
}

/// `Draw_Line_d` for C callers.
#[no_mangle]
pub extern "C" fn vx_draw_line_seq(sink: *mut CSink, vx: i32, vy: i32, t1: u32,
                                   k: *const CTimings) {
    if sink.is_null() || k.is_null() {
        return;
    }
    let (s, kk) = unsafe { (&mut *sink, &*k) };
    draw_line_seq(s, vx.clamp(-128, 127) as i8, vy.clamp(-128, 127) as i8, t1 as u16,
                  &kk.to_timings());
}

/// The line with gaps, for C callers.
///
/// `gaps` points at `n` (start, end) pairs in T1 counts, ordered and non-overlapping. A null
/// pointer or `n == 0` is exactly a normal line, so the caller does not need two paths.
#[no_mangle]
pub extern "C" fn vx_draw_line_patterned_seq(sink: *mut CSink, vx: i32, vy: i32, t1: u32,
                                             k: *const CTimings,
                                             gaps: *const u16, n: u32) {
    if sink.is_null() || k.is_null() {
        return;
    }
    let (s, kk) = unsafe { (&mut *sink, &*k) };
    let t = kk.to_timings();
    let vx8 = vx.clamp(-128, 127) as i8;
    let vy8 = vy.clamp(-128, 127) as i8;
    if gaps.is_null() || n == 0 {
        return draw_line_seq(s, vx8, vy8, t1 as u16, &t);
    }
    // A fixed cap on purpose: this runs in a bare-metal image, with no allocator. One gap too
    // many is lost — that stretch comes out lit — and you can see that; a dynamic allocation
    // here would be a failure you cannot see.
    const MAX: usize = 16;
    let count = (n as usize).min(MAX);
    let mut buf = [(0u16, 0u16); MAX];
    for i in 0..count {
        buf[i] = unsafe { (*gaps.add(i * 2), *gaps.add(i * 2 + 1)) };
    }
    draw_line_patterned_seq(s, vx8, vy8, t1 as u16, &t, &buf[..count]);
}

/// `draw_line_sr_seq` for C callers. See there for why `step` is not the size.
#[no_mangle]
pub extern "C" fn vx_draw_line_sr_seq(sink: *mut CSink, vx: i32, vy: i32, t1: u32,
                                      k: *const CTimings,
                                      pattern: *const u8, n: u32, step: u32) {
    if sink.is_null() || k.is_null() {
        return;
    }
    let (s, kk) = unsafe { (&mut *sink, &*k) };
    let t = kk.to_timings();
    let vx8 = vx.clamp(-128, 127) as i8;
    let vy8 = vy.clamp(-128, 127) as i8;
    if pattern.is_null() || n == 0 {
        return draw_line_seq(s, vx8, vy8, t1 as u16, &t);
    }
    // The same rule as the patterned one: a fixed cap, no allocator, and whatever is left over
    // is lost visibly instead of allocating memory in bare metal.
    const MAX: usize = 64;
    let count = (n as usize).min(MAX);
    let mut buf = [0u8; MAX];
    for i in 0..count {
        buf[i] = unsafe { *pattern.add(i) };
    }
    draw_line_sr_seq(s, vx8, vy8, t1 as u16, &t, &buf[..count], step as u16);
}

/// `Moveto_d` for C callers. The SAME function the firmware uses: that is the whole point of
/// this file.
#[no_mangle]
pub extern "C" fn vx_moveto_seq(sink: *mut CSink, vx: i32, vy: i32, t1: u32, k: *const CTimings) {
    if sink.is_null() || k.is_null() {
        return;
    }
    let (s, kk) = unsafe { (&mut *sink, &*k) };
    let t = kk.to_timings();
    moveto_seq(s, vx.clamp(-128, 127) as i8, vy.clamp(-128, 127) as i8, t1 as u16, &t);
}

#[cfg(test)]
mod comparison {
    use crate::ramp::ramp_params;
    use std::println;

    /// The reference's `fixup`, transcribed from uvm2_draw.c: while both deltas fit in half the
    /// DAC and there is ramp left, it DOUBLES the delta and HALVES the duration. It preserves
    /// the distance (delta x time) and uses more of the DAC's range, which is its way of
    /// shortening the stroke.
    fn reference(dx: i32, dy: i32) -> (i32, i32, u32) {
        let (mut x, mut y, mut s) = (dx, dy, 160u32);
        while x.abs() < 64 && y.abs() < 64 && s > 32 {
            x *= 2;
            y *= 2;
            s /= 2;
        }
        (x, y, s)
    }

    /// THE QUESTION: do both models move the beam THE SAME?
    ///
    /// The distance is velocity x time in both. If they do not match, the geometry comes out
    /// wrong and no timing knob fixes it — which is exactly what the console had been saying
    /// all afternoon.
    #[test]
    fn travel_of_both_models() {
        println!("\n  dx  dy |     ref: dac x t = dist |   T1: v x t1 = dist |  error");
        println!("  -------+-------------------------+---------------------+-------");
        let mut worst = 0i64;
        for &(dx, dy) in &[(1, 0), (2, 0), (3, 1), (5, 0), (8, 3), (12, 0), (20, 7),
                           (32, 0), (48, 16), (64, 0), (100, 40), (127, 0)] {
            let (rx, _ry, rs) = reference(dx, dy);
            let dr = rx as i64 * rs as i64;
            let (vx, _vy, t1) = ramp_params(dx as i8, dy as i8);
            let dt = vx as i64 * t1 as i64;
            let err = if dr != 0 { (dt - dr) * 100 / dr } else { 0 };
            if err.abs() > worst.abs() { worst = err; }
            println!("  {dx:3} {dy:3} | {rx:6} x {rs:3} = {dr:7} | {vx:5} x {t1:3} = {dt:7} | {err:4}%");
        }
        println!("\n  worst deviation: {worst}%\n");
    }
}

/// ONE TEST_LOCK FOR EVERY TEST THAT MOVES KNOBS.
///
/// The speed cap and MIN_T1 are GLOBALS and cargo runs the tests in parallel. There used to be
/// one mutex PER MODULE, and two different locks do not serialise against each other:
/// `t1_ceiling_is_per_vector` failed only when it ran at the same time as the `pentagon` ones,
/// and passed when run on its own — the worst possible failure, because it invites you to blame
/// the code you have just touched.
#[cfg(test)]
pub(crate) static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

#[cfg(test)]
mod pentagon {
    use crate::ramp::{ramp_params, scale, MIN_T1};
    use core::sync::atomic::Ordering;
    use std::{format, println, vec, vec::Vec};

    /// A CLOSED FIGURE HAS TO CLOSE. It is the property that catches this whole family of
    /// faults, which is why it is checked over several shapes and several settings instead of
    /// over one lone pentagon.
    ///
    /// THIS TEST ALREADY EXISTED AND COULD NOT FAIL. It measured the accumulated closing error,
    /// PRINTED it, and had not one assert: a report disguised as a test, which says "ok" on
    /// every `cargo test` while the figure grows. The +6.30 unit error found on 2026-08-24 —
    /// and which cost an afternoon of knobs on the console — had been there from the start,
    /// printed, with nobody reading it.
    ///
    /// WHY THIS PROPERTY IS ENOUGH: `ramp_params` splits a delta between velocity and time,
    /// both integers, so a lone stroke CANNOT be exact. One stroke's error is invisible; what
    /// shows is that it has a CONSTANT SIGN and adds up. A closed polygon turns that sum into a
    /// number: if the bias exists, it does not close.
    fn closing_error(v: &[(i32, i32)]) -> (f64, f64) {
        let s = scale() as i64;
        let (mut ex, mut ey) = (0i64, 0i64);
        for i in 0..v.len() - 1 {
            let (dx, dy) = (v[i + 1].0 - v[i].0, v[i + 1].1 - v[i].1);
            let (vx, vy, t1) = ramp_params(dx as i8, dy as i8);
            ex += vx as i64 * t1 as i64 - dx as i64 * s;
            ey += vy as i64 * t1 as i64 - dy as i64 * s;
        }
        (ex as f64 / s as f64, ey as f64 / s as f64)
    }

    use super::TEST_LOCK;

    #[test]
    fn closed_figures_close() {
        let _t = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());

        // pentagon, big square, small square, a zigzag that returns, long diagonal
        let figures: [(&str, Vec<(i32, i32)>); 5] = [
            ("pentagon", vec![(0,60),(-57,19),(-35,-49),(35,-49),(57,19),(0,60)]),
            ("big square", vec![(-100,-100),(100,-100),(100,100),(-100,100),(-100,-100)]),
            ("small square", vec![(-6,-6),(6,-6),(6,6),(-6,6),(-6,-6)]),
            // the zigzag returns IN STEPS: closing it in one stroke asked for -180 units and
            // a ramp only expresses +-127, so it was clipped and the test accused the model of
            // a fault that belonged to the figure. The test caught that itself.
            ("zigzag", { let mut v = vec![(-90,0)];
                         for i in 1..=12 { v.push((-90 + i*15, if i % 2 == 0 { 0 } else { 9 })); }
                         for i in 1..=12 { v.push((90 - i*15, 0)); }
                         v }),
            ("diagonal", vec![(-120,-120),(120,120),(-120,-120)]),
        ];

        // TOLERANCE PER STROKE, not absolute: a stroke cannot be exact, but the error must
        // not GROW with the length of the chain. Half a unit per stroke is generous — a ramp's
        // rounding is worth at most that — and even so the bias breaks it.
        let mut bad = Vec::new();
        // WITHOUT THE SPEED-CAP DIMENSION: the speed cap is no longer an adjustment, it is the
        // DAC's limit (see DAC_CAP in ramp.rs). The MIN_T1 sweep is kept, which is the split's
        // other floor and still exists — the property this test catches (a closed figure
        // CLOSES) did not depend on the knob.
        for floor in [31u32, 8, 60] {
            MIN_T1.store(floor, Ordering::Relaxed);
            for (name, v) in figures.iter() {
                let (ex, ey) = closing_error(v);
                let n = (v.len() - 1) as f64;
                let cap = 0.5 * n;
                println!("  MIN_T1 {floor:3}  {name:16} closes at {ex:+7.2},{ey:+7.2}  (cap +-{cap:.1})");
                if ex.abs() > cap || ey.abs() > cap {
                    bad.push(format!("{name} at MIN_T1={floor}: {ex:+.2},{ey:+.2}"));
                }
            }
        }
        MIN_T1.store(31, Ordering::Relaxed);
        assert!(bad.is_empty(), "figures that do not close:\n  {}", bad.join("\n  "));
    }

    /// THE BIAS IS DETECTED IN THE MEAN, NOT IN THE MAGNITUDE, and this is what would have
    /// caught the 2026-08-24 fault when it was introduced.
    ///
    /// A lone stroke CANNOT be exact: velocity and time are integers. Its error is at most half
    /// a unit and that is unavoidable. What is NOT unavoidable is the error always having the
    /// SAME SIGN — then it stops being noise and becomes a debt that adds up stroke after
    /// stroke. The real fault was +0.075 units per stroke: invisible under any per-stroke cap,
    /// and +6.30 after 84 of them.
    ///
    /// So the MEAN is measured over every delta. Honest noise averages to zero; a bias does
    /// not.
    #[test]
    fn ramp_error_has_no_bias() {
        let _t = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let s = scale() as f64;
        let mut bad = Vec::new();
        // WITHOUT SWEEPING THE SPEED CAP: it no longer exists. The property — that the ramp's
        // error averages to zero and is not a signed debt — did not depend on the speed cap,
        // and it is checked in the one configuration that exists.
        {
            let (mut sum, mut n) = (0f64, 0f64);
            for d in 1..=127i32 {
                let (vx, _, t1) = ramp_params(d as i8, 0);
                sum += (vx as f64 * t1 as f64 - d as f64 * s) / s;
                n += 1.0;
            }
            let mean = sum / n;
            println!("  mean error per stroke: {mean:+.4} units");
            // 0.02 units per stroke is 1.7 after 84 — already visible. The bar has to sit
            // below what you can see, not below what annoys you.
            if mean.abs() > 0.02 {
                bad.push(format!("bias of {mean:+.4} per stroke"));
            }
        }
        assert!(bad.is_empty(), "the ramp has a BIAS, and a bias accumulates:\n  {}",
                bad.join("\n  "));
    }
}

#[cfg(test)]
mod velocity {
    use crate::ramp::{ramp_params, scale, MIN_T1};
    use core::sync::atomic::Ordering;
    use std::println;

    use super::TEST_LOCK;

    fn reference(dx: i32, dy: i32) -> (i32, u32) {
        let (mut x, mut y, mut s) = (dx, dy, 160u32);
        while x.abs() < 64 && y.abs() < 64 && s > 32 { x *= 2; y *= 2; s /= 2; }
        (x.abs().max(y.abs()), s)
    }

    /* THE "WHICH CAP MATCHES THE REFERENCE" TEST WAS WITHDRAWN (2026-09-09). It swept the
     * speed cap looking for the value that matched the reference's velocities, and that cap no
     * longer exists: the cap is the DAC's. A test of a knob goes with the knob.
     *
     * What it was for, kept because it is still true: both models preserve the distance, but
     * the SPLIT between velocity and time differs — we go faster for less time. And the
     * deflection amplifier has a finite slew rate; asking for more than it can follow leaves
     * the stroke short, which is the gap at the vertices. */


    /// t1's CEILING IS COMPUTED PER VECTOR. Three things, and all three are checkable:
    ///
    ///   a) at factory values nothing changes — no delta exceeds DRAW_SCALE
    ///   b) the minor axis NEVER rounds to zero, which is the limit that defines the ceiling
    ///   c) lowering the speed cap gives real range to the vector that can be slowed, and does
    ///      NOT give it to the one that would flatten — which is exactly what a global constant
    ///      could not do
    ///
    /// (c) is what justifies the change: with the ceiling fixed at 160 both got 160 and the cap
    /// was saturated.
    #[test]
    fn t1_ceiling_is_per_vector() {
        let _t = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let s = scale();
        let floor = MIN_T1.load(Ordering::Relaxed) as i32;

        // (a) and (b), over all 65,024 possible deltas
        let (mut worst, mut over) = (0i32, 0u32);
        for dx in -127i32..=127 {
            for dy in -127i32..=127 {
                if dx == 0 && dy == 0 { continue; }
                let (vx, vy, t1) = ramp_params(dx as i8, dy as i8);
                let t1 = t1 as i32;
                assert!(t1 >= floor, "t1 {t1} below the floor at ({dx},{dy})");
                if t1 > worst { worst = t1; }
                if t1 > s { over += 1; }
                // (b) no axis with a delta is left standing still
                if dx != 0 { assert_ne!(vx, 0, "X axis dead at ({dx},{dy}) with t1={t1}"); }
                if dy != 0 { assert_ne!(vy, 0, "Y axis dead at ({dx},{dy}) with t1={t1}"); }
            }
        }
        assert_eq!(over, 0, "nobody should exceed DRAW_SCALE");
        assert_eq!(worst, s, "the geometric maximum IS DRAW_SCALE, no more and no less");

        // (c) THE TRANSPORT CAP RULES OVER THE GEOMETRY, and by default it is 160.
        //
        // This block used to claim the opposite: that at cap=8 the diagonal reached 2540. That
        // is true with T1_TRANSPORT=4095 — but releasing the ceiling at the same time as
        // VCAP_SLOW was woken up broke the drawing on both cartridges, so the default went back
        // to 160. The test follows the code, not the other way round.
        use crate::ramp::T1_TRANSPORT;
        // The part that forced t1 with cap=8 goes with the knob; what is kept is that the
        // ceiling is computed PER VECTOR, which is what this test catches.
        let (_, vy_flat, flat) = ramp_params(127, 1);
        let (_, _, diagonal) = ramp_params(127, 127);
        let cap = T1_TRANSPORT.load(Ordering::Relaxed) as i32;
        assert!(diagonal as i32 <= cap && flat as i32 <= cap,
                "nobody may exceed the transport cap");
        assert_ne!(vy_flat, 0, "and no axis with a delta is left standing still");

        // AND THE PER-VECTOR LAW, checked against the ceiling and not against t1.
        //
        // It used to be observed indirectly: with the cap forcing t1 upwards, two vectors ended
        // at different t1 and that gave away that their ceilings differed. Without a speed cap
        // t1 does not reach the ceiling, so you have to look at the ceiling itself — which is
        // `d_minor * DRAW_SCALE` and is still per vector.
        T1_TRANSPORT.store(4095, Ordering::Relaxed);
        for (dx, dy) in [(127i8, 127i8), (127, 1), (60, 20), (5, 5)] {
            let (_, _, t) = ramp_params(dx, dy);
            let minor = (dx as i32).abs().min((dy as i32).abs()).max(1);
            assert!(t as i32 <= minor * s,
                    "({dx},{dy}): t1={t} exceeds its per-vector ceiling {}", minor * s);
        }
        T1_TRANSPORT.store(cap as u32, Ordering::Relaxed);

    }

    /* THE "SELECTIVE CAP DEPENDS ON THE BACKEND" TEST WAS WITHDRAWN (2026-09-09) along with
     * VCAP_SLOW.
     *
     * It pinned a CONTRACT worth not losing sight of: whoever does not write `Y_HELD` had no
     * selective cap. There is no selective cap for anyone now — the rule lowered the cap on
     * SHORT vectors, which are the text glyphs, and that lengthened their t1. */




}

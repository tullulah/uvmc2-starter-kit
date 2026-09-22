//! `ramp_params` and its knobs — the heart of the beam model, in ONE place.
//!
//! STEP 2 of the unification. This is not new code: it is EXACTLY what lived in
//! `vinterface.rs`, moved across as-is with its comments. It was moved, not rewritten, and
//! that distinction is the verification — the body had to come out character for character
//! identical, because every constant in here cost a session on the console.
//!
//! Both cartridges consume it: the firmware links it as an `rlib` and the multicart image as
//! a `staticlib` through `vx_ramp_params`.
//!
//! The knobs are `#[no_mangle]` so the debug panel still resolves them by short name now that
//! they no longer live in the `vinterface` module.

use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

/// THE DRAWING SCALE — and it is a MAGIC NUMBER, as its own comment below says: "it is
/// 0xA0 = 160 and it is empirical". Nobody derived it from anything; it was raised until the
/// figures came out a size that looked right. The reference implementation, which draws
/// correctly on this same console, uses 128 — another magic number, except that theirs works.
///
/// RUNTIME since 2026-08-25, so it can be swept from the panel instead of recompiling for
/// every value. That does not make it derived: it is still empirical, and what is needed is
/// to explain where it comes from, not to find the one that looks best.
#[used]
#[no_mangle]
pub static DRAW_SCALE: AtomicU32 = AtomicU32::new(0xA0);

/// FINE TRIM OF THE NEGATIVE RATES, PER AXIS, IN 1/256.
///
/// The DAC does not deviate the same at `+k` as at `-k`: they are different bit patterns, and
/// that is already measured in this project — the error depends on the VALUE, not on the
/// distance. In a stroke where both axes ask for the SAME number (a 45-degree diagonal to the
/// north-east) the deviation is the same in both and the direction comes out clean; where
/// they ask for OPPOSITE numbers (the north-west/south-east diagonal) `|vx|` and `|vy|` stop
/// being genuinely equal, the outbound and return legs tilt in opposite directions and you
/// see TWO lines.
///
/// That is how it looked on the calibration star: with the zero adjusted, seven arms close —
/// the four straight ones and the north-east/south-west diagonal — and ONLY the
/// north-west/south-east one stays open. No zero fixes that (it moves both axes at once) and
/// no scale does either (`abs()`, symmetric): it asks for the sign to be corrected.
///
/// `v' = v * (256 + k) / 256` for v < 0. With k = 0 it does nothing, which is the default.
#[no_mangle]
pub static NEG_RATE_X: AtomicI32 = AtomicI32::new(0);
#[no_mangle]
pub static NEG_RATE_Y: AtomicI32 = AtomicI32::new(0);

fn trim_neg(v: i32, k: i32) -> i32 {
    if v >= 0 || k == 0 { return v; }
    (v * (256 + k) / 256).clamp(-128, 127)
}

/// IT IS APPLIED WHEN WRITING TO THE DAC, NOT WHEN COMPUTING THE RAMP. And that is not a
/// detail: it used to be in `ramp_params_q` and DID NOTHING to the figures.
///
/// Strokes go through `ramp_params_chain`, which carries an accumulated DEBT: whatever one
/// stroke overshoots is subtracted from the next. On an arm of the star — out and back — the
/// debt cancels the trim and the figure comes out unchanged; jumps carry no debt, so the only
/// thing that moved was WHERE the next thing lands. Observed exactly that way: "diagx only
/// moves the text in X, the star does not budge".
///
/// Placed here, the model — the ramp, the debt, the geometry — keeps the ideal `v` and the
/// only thing that changes is the number the DAC sees, which is precisely what we want to
/// compensate.
pub fn trim_dac(vx: i8, vy: i8) -> (i8, i8) {
    (trim_neg(vx as i32, NEG_RATE_X.load(Ordering::Relaxed)) as i8,
     trim_neg(vy as i32, NEG_RATE_Y.load(Ordering::Relaxed)) as i8)
}

/// The current value. Read once per vector, not in a tight loop.
///
/// WITH A FIXED RAMP, THE SCALE IS THE DURATION. The distance is `vx*t1/s`, so only with
/// `s = t1` does `vx = dx` come out exact — and that is what makes `s_pos` (what the drawer
/// BELIEVES it has advanced) match the real travel. With s=160 and t1=127 they diverge by 21%
/// per vector and every `move_abs` starts from the wrong position: MEASURED in the emulator on
/// 2026-08-25, Kong came out deformed until the two were tied together.
///
/// It is tied here and not in the caller, because "remember to set both" is exactly the class
/// of divergence this project has been paying for. And it explains in passing why the
/// reference's 128 IS its scale: in the fixed-time model they are the same number.
#[inline]
pub fn scale() -> i32 {
    let fixed = FIXED_RAMP.load(Ordering::Relaxed);
    if fixed > 0 { fixed as i32 } else { DRAW_SCALE.load(Ordering::Relaxed) as i32 }
}

/// Dwell floor, RUNTIME since 2026-08-05 so it can be swept against glyph shape.
///
/// **DEFAULT 31, MEASURED on hardware 2026-08-05**: below it glyph strokes come out
/// visibly stepped, at it they are straight. Swept on the calibration screen against
/// the sample text, which is 1-2 unit strokes — exactly the vectors this bounds.
///
/// WHY a floor is needed at all, and it is NOT the arithmetic. The first theory was
/// integer truncation in `vx = dx * s / t1`, and it was wrong: rounding to nearest
/// (now done below) should have made t1 = 8 the BEST case (127/8 = 15.875 -> 16,
/// +0.8%) against t1 = 31 (4.096 -> 4, -2.4%). 31 still won, so the error term does
/// not decide this.
///
/// What decides it is that **the deflection lag is FIXED while the vector duration is
/// not**. `BEAM_ON_DELAY_CYC` and `BLANK_SETTLE_CYC` are 2 E cycles each, so on an
/// 8-cycle stroke they are half its duration — the beam is lit from 25% to 125% of the
/// travel, which is what "stepped" letters are. At 31 the same fixed lag is ~13%.
///
/// So this is a property of the TUBE, not a tuning preference: the minimum vector
/// duration this yoke can render faithfully. Task #10 (scope on the yoke) would
/// measure the same thing directly instead of through the user's eye.
///
/// COSTS FRAME TIME. It is the dwell floor, so every short vector pays it — see the
/// measurement in the task list before lowering it for speed.
#[used]
#[no_mangle]
pub static MIN_T1: AtomicU32 = AtomicU32::new(8);
/* ^ 8 AND NOT 31, BY DEFAULT SINCE 2026-09-04: it is what the reference cartridge measures on
 * ALL its short lit strokes (1861 of ~1900 in 8 frames), and what the tuned ports were already
 * setting by hand. */

/// Ramp floor ONLY for the ones that start from rest (the jump and the first stroke after it).
/// See the long block in `ramp_params`. Out of the box equal to MIN_T1 = inert.
#[used]
#[no_mangle]
pub static MIN_T1_START: AtomicU32 = AtomicU32::new(31);

/// `T1_LAG` for those same ramps. Out of the box equal to T1_LAG = inert.
#[used]
#[no_mangle]
pub static T1_LAG_START: AtomicU32 = AtomicU32::new(0);

/// How many ramps were charged the start floor. A COUNTER, because a branch that fires
/// silently has already cost sessions in this project: if this does not climb, the knob is
/// doing nothing and the measurement must not be believed.
#[used]
#[no_mangle]
pub static START_HITS: AtomicU32 = AtomicU32::new(0);

/// 1 = the next ramp starts from rest. `vx_chain_reset` sets it (i.e. a jump or a re-zero)
/// and the first CHAINED stroke behind it consumes it.
static STARTING: AtomicU32 = AtomicU32::new(1);

                       // brightness the studio intro was validated with. Glitch-
                       // safe (HW-tested). Vector geometry is unchanged (distance =
                       // vel·t1). Lower toward 64 if a denser scene overruns the
                       // 20 ms budget — that was the original intro-fit value.
// Peak DAC velocity (of 0x7F). Below the full ±127 swing so the integrator op-amp
// doesn't overshoot on mid-length segments (the platform "stretch" at low MIN_T1).
// ramp_params raises t1 to hold velocity ≤ VCAP; short vectors are already under it.
/// Velocity cap, RUNTIME alongside MIN_T1 — the two interact (t1 is the max of both
/// floors), so sweeping one without the other cannot find the optimum.
/* THE SPEED CAP IS THE DAC'S, AND IT IS NOT AN ADJUSTMENT.
 *
 * There used to be a `VCAP` here, a knob that bounded the beam's SPEED because slowing it down
 * charges the phosphor more: more brightness. The price did not show until 2026-09-09,
 * comparing our list command by command against the reference capture of the SAME picture
 * (the high-score screen):
 *
 *   its 427 lit strokes ALL run at t1 = 8, with rates of median 49 and maximum 51.
 *   Ours came out 315 at t1 = 8 and 109 at t1 = 16, and those 109 at rate 29-31: the chooser
 *   saw that ~50 was needed, forbade it because VCAP = 42 and DOUBLED t1 to halve the rate.
 *
 * AND ONE RAMP OF t1 = 16 DOES NOT TRAVEL WHAT TWO OF t1 = 8 DO. On the console the table came
 * out with its rows stacked, and with -DUVM2_MICROSEGMENTS — which splits those strokes into
 * stretches of 8 — it STRAIGHTENS OUT. That was the experiment that closed it.
 *
 * THEIR RULE is what remains: t1 is the smallest one that keeps the rate inside the DAC. The
 * cap is not a preference, it is the converter's limit. They get brightness from Z, not from
 * slowing the beam — and their dwell per unit (3.2 cycles) falls out by itself because the
 * stroke is short.
 *
 * WHAT IS LOST BY REMOVING IT, for the record: a fixed-frame sweep put VCAP = 42 within 2% of
 * their cycles per unit and 4% of their phosphor charge (VCAP 29/34/38/42 -> cyc/unit
 * 5.54/4.85/4.26/3.93 against their 4.00). That adjustment equalised the frame's MEAN by
 * slowing the text below what its geometry allows: one number cannot serve both long vectors
 * and glyphs. */
const DAC_CAP: u32 = 127;

/* ── THE JUMPS' CAP, SEPARATE FROM THE STROKES' ────────────────────────────────────
 *
 * A speed cap bounds the beam's SPEED, and slowing it down is what gives brightness: the
 * phosphor charges with intensity x TIME, so a fast stroke comes out dark. But that **only
 * counts with the beam lit**. A jump travels blanked: slowing it lights nothing and only
 * spends frame.
 *
 * MEASURED in the reference capture (816 frames of asterock), separating its ramp units by
 * beam state:
 *
 *     LIT     (drawing): median rate 35    1,403 cycles/frame  (175 units)
 *     BLANKED (jumping): median rate 64   10,232 cycles/frame  (317 units)
 *     -> IT JUMPS AT 1.8x THE SPEED IT DRAWS AT
 *
 * We used ONE cap for both, and that forces a choice between bright strokes and fast jumps
 * when there is no choice to make. The cost measured in asterock: to match their phosphor
 * charge (3.27 lit cycles per unit of length) we needed VCAP=48, and with it the frame went
 * from 33,205 to 72,578 cycles — from 45 to 20.7 Hz — because the ~266 jumps per frame
 * stretched along with the strokes: ~267 cycles per jump against the reference's ~32.
 *
 * `VCAP_JUMP` is gone too. It existed so jumps would not be slowed (they travel blanked,
 * slowing them lights nothing) while VCAP slowed the strokes; the four targets that set it put
 * it at 127, i.e. it was already the DAC's cap. Without VCAP there is nothing to exempt them
 * from. */

/// TRANSPORT CAP — not a preference, the width of a field.
///
/// It replaces `T1_CEILING`, which was 160 (i.e. DRAW_SCALE: wherever it happened to be baked
/// in) with a FALSE justification written next to it. It said the VIA's T1 counter is 8 bits
/// and that 255 was its physical cap: the 6522's T1 counts **16 bits** and `emit` writes both
/// halves (T1CL and T1CH, see `emit.rs`), so neither 160 nor 255 limited anything. The real
/// ceiling is computed per vector in `ramp_params`.
///
/// The only thing left as a constant is this, and for a checkable reason: the delay travels in
/// a 12-BIT field of the UVM2's command (delay 12 + register 4 + data 8 = 24 bits, 3 bytes per
/// command), so 4095 counts is the most the executor can wait. Above that the value SPILLS
/// into the register and data fields: corrupt commands and a black screen with no warning.
///
/// In E cycles that is 2.73 ms, 13.7% of a frame, against a geometric t1 that never exceeds
/// 160 — 25x of headroom. Runtime because another board may carry the delay differently; if
/// one ever carries it narrower, lowering it here is all that is needed.
#[used]
#[no_mangle]
/// DEFAULT 160, NOT 4095, and that is a WITHDRAWAL with a reason. 4095 is the transport's real
/// limit and the analysis is still correct; but releasing the ceiling at the same time as
/// VCAP_SLOW was woken up let the slowed vectors go from 160 to 677, and on the console the
/// drawing broke on both cartridges. 160 reproduces the long-standing T1_CEILING. Raising it is
/// an experiment, and it is done with ONE variable and a measurement.
pub static T1_TRANSPORT: AtomicU32 = AtomicU32::new(160);

/// Map a vector delta to (velocity_x, velocity_y, t1_scale) for the variable-T1
/// model. Preserves the exact displacement of the fixed model (delta·0x7F):
///   distance = velocity · T1, so velocity = delta·0x7F / T1.
/// T1 ∝ length (dominant axis → full ±127 swing) but is clamped to [MIN_T1,0x7F];
/// deriving velocity from the CLAMPED T1 keeps the distance correct even when the
/// clamp kicks in (short vectors), instead of over-drawing them.
/// 1 = the minor axis's CEILING overrules the speed cap. IT IS THE DEFAULT, and not out of
/// inertia: the `t1_ceiling_is_per_vector` test asserts that no axis with a delta is left
/// standing still, and releasing it was tried once and broke the drawing on both cartridges.
///
/// 0 lets the speed cap win. What it buys and what it costs is in the `ramp_params_q` note;
/// compare on the console.
#[no_mangle]
pub static CEILING_RULES: AtomicU32 = AtomicU32::new(1);

/// 1 = the chain corrects with the debt (default). 0 = it does not apply it (it still records
/// it).
///
/// It exists so what it contributes can be MEASURED. With the input geometry in 1/16 the debt
/// is necessary — the input's rounding accumulates — but at 1/256 the input is already almost
/// exact and the correction may be ADDING the +-1 instead of removing it.
#[no_mangle]
pub static DEBT_ON: AtomicU32 = AtomicU32::new(1);

/* `VCAP_SLOW` / `VCAP_DV` / `VCAP_SLOW_HITS` WITHDRAWN along with VCAP (2026-09-09).
 *
 * They lowered the cap for slowed vectors, and their condition was `m <= VCAP_DV` — i.e. they
 * applied to the SHORT ones, which are exactly the text glyphs. With the cap already at the
 * DAC's limit there is nothing to lower, and lowering it would reintroduce the long t1 by
 * another door. No target ever set them. */


/// FIXED-TIME JUMP, LIKE THE REFERENCE CARTRIDGE. 0 = the old model (variable time).
///
/// MEASURED in the Major Havoc capture (`via.csv`, one 20 ms frame, 593 units):
///
/// ```text
/// T1=8  micro-segments: 501 units, beam LIT     in 79%   -> drawing
/// T1=31 jumps         :  72 units, beam BLANKED in 99%   -> moving
///                         |vx| median 64, max 126
/// others              :  20 units, T1 up to 191, |vx| median 113 -> LONG jumps
/// ```
///
/// So their jump dialect is **fixed time and variable rate**, exactly the opposite of ours
/// (rate fixed at the jump cap, variable time). The difference is cost: their jump costs ~40
/// cycles ALWAYS; ours 53, with T1 median 9 and maximum 160.
///
/// The time is only stretched when the rate would overflow the DAC — which is exactly where
/// their 20 long units come from: at s=160 a jump longer than 127*31/160 = 24 units no longer
/// fits in +-127 with t1=31, and then the minimum time is `m*s/127`.
///
/// ZERO OUT OF THE BOX on purpose: until someone gives it a value, no port changes.
#[used]
#[no_mangle]
pub static T1_JUMP: AtomicU32 = AtomicU32::new(0);

#[inline(always)]
/// `ramp_params` for a JUMP: the same as the strokes' but with the jump cap, because the beam
/// is blanked and brightness does not count.
pub fn ramp_params_jump(dx: i8, dy: i8) -> (i8, i8, u16) {
    let fixed = T1_JUMP.load(Ordering::Relaxed) as i32;
    if fixed > 0 {
        return fixed_time_jump(dx, dy, fixed);
    }
    ramp_params_with(dx, dy, DAC_CAP)
}

/// The reference cartridge's jump model: `t1` fixed, and the rate is whatever comes out.
///
/// It does not share a body with `ramp_params_with` because that function's two floors
/// (`t1_floor`, proportional to the length, and `t1_vcap`, from the speed cap) exist to SPLIT
/// the distance between speed and time — and there is no split to make here: the time is
/// given. The only thing preserved is the rounding, which has to be the SAME (the same
/// division with `T1_EXTRA_Q8`) or jumps would stop matching strokes.
fn fixed_time_jump(dx: i8, dy: i8, fixed: i32) -> (i8, i8, u16) {
    let s = scale();
    let m = core::cmp::max((dx as i32).abs(), (dy as i32).abs());
    if m == 0 {
        return (0, 0, MIN_T1.load(Ordering::Relaxed) as u16);
    }
    // The minimum time for the rate to fit in the DAC, rounded UP: truncating, the rate
    // overshoots, the DAC clips it at +-127 and the jump always falls SHORT in the same
    // direction — the systematic bias `t1_vcap` documents in the strokes' model.
    let t1_fits = (m * s + 126) / 127;
    let t1 = fixed.max(t1_fits).min(T1_TRANSPORT.load(Ordering::Relaxed) as i32).max(1);
    let den = (t1 as i64) * 256 + T1_EXTRA_Q8.load(Ordering::Relaxed) as i64;
    let round_div = |num: i32| -> i32 {
        let n = num as i64 * 256;
        (if n >= 0 { (n + den / 2) / den } else { (n - den / 2) / den }) as i32
    };
    let vx = round_div(dx as i32 * s).clamp(-128, 127) as i8;
    let vy = round_div(dy as i32 * s).clamp(-128, 127) as i8;
    (vx, vy, t1 as u16)
}

pub fn ramp_params(dx: i8, dy: i8) -> (i8, i8, u16) {
    ramp_params_with(dx, dy, DAC_CAP)
}

/// The model, with whatever speed cap the caller passes. Strokes and jumps used to use
/// different ones: it is the SAME split of distance between speed and time, with a different
/// speed ceiling.
fn ramp_params_with(dx: i8, dy: i8, vcap_in: u32) -> (i8, i8, u16) {
    ramp_params_q(dx as i32, dy as i32, vcap_in, 0)
}

/* THE SAME MODEL WITH SUB-UNITS. `dx`/`dy` arrive in 1/2^q device units; q = 0 is exactly the
 * old behaviour.
 *
 * WHY. The drawing API took device integers, and that grid is TEN TIMES coarser than the
 * reference's: it places dots with the granularity of the rate (1/20 of a unit at t1=8) and we
 * only on the integer. MEASURED in mhavoc over 81,552 vectors: 0.22 units of error per axis —
 * the theoretical maximum is 0.5, i.e. uniform rounding with no bias but all the noise — 3.6%
 * of vectors entirely sub-unit and 0.26% DISAPPEARING because both endpoints land on the same
 * point. With moves of 2 units median and glyphs 2-3 units tall, that is the deformation you
 * see.
 *
 * The hardware did not prevent it: the distance is v*t1/s and with t1 free the fractional
 * distances express themselves (they ask for v=51,t1=8 = 2.55 units). It was our arithmetic
 * that imposed it. It is fixed here by dividing by 2^q everywhere a LENGTH is multiplied by
 * the scale. */
/// DIAGNOSTIC: the largest |delta| that has entered ramp_params_q (in the internal unit) and
/// how many times it exceeded 2^15. A delta of more than 2048 units does not fit on the
/// screen: if it appears, somebody is passing a broken position, and in 32 bits m*s*256
/// overflows above ~66k.
#[no_mangle]
pub static RAMP_M_MAX: AtomicU32 = AtomicU32::new(0);
#[no_mangle]
pub static RAMP_M_LARGE: AtomicU32 = AtomicU32::new(0);
fn ramp_params_q(dx: i32, dy: i32, vcap_in: u32, q: u32) -> (i8, i8, u16) {
    ramp_params_q_v(dx, dy, vcap_in, q, true)
}
/// The same one with `want_v = false` for callers that only need t1 (the chain recomputes
/// vx/vy directly over the duration rounded to micro-segments): it saves the rounding of both
/// axes. 2026-09-16, measured on the board: a stroke's ramp was 789 cycles.
fn ramp_params_q_v(dx: i32, dy: i32, vcap_in: u32, q: u32, want_v: bool) -> (i8, i8, u16) {
    let f = 1i32 << q;
    let m = core::cmp::max(dx.abs(), dy.abs());
    /* The three counters that used to live here -- RAMP_M_MAX, RAMP_M_LARGE and
     * START_HITS -- were written on EVERY ramp and read by nothing: not by this
     * crate, not by the SDK, not by the cartridge firmware. On a Cortex-M33 each is an
     * LDREX/STREX pair in the hottest function of the draw builder. Same shape as the
     * FLICKER_TELEM cleanup: diagnostics that outlived the diagnosis. */
    /* ── TWO FLOORS, DEPENDING ON WHETHER THE RAMP STARTS FROM REST ──────────────────
     *
     * A stroke that CONTINUES the previous one enters with the integrators already moving; the
     * first one after a blanked jump has to accelerate from rest, and that is the distance
     * that gets lost. Both paid the same floor, so raising it to rescue the second charged the
     * first for it too.
     *
     * MEASURED ON THE CONSOLE (2026-08-26, dkong on the UVM2): with MIN_T1 = 31 the ladders'
     * loose rungs come out displaced and the chained girders come out right; with 94
     * EVERYTHING comes out right and the framerate drops from 23 to 14.3 fps. The arithmetic
     * says why:
     *
     *     23 -> 14.3 fps is 26.4 ms/frame; at 1.5 MHz, 39,700 cycles
     *     39,700 / (94-31) = 630 operations paying the floor
     *     and the cartridge reports 549 vectors + 179 jumps = 728
     *
     * So 86% of what was drawn was sitting on the floor: MIN_T1 was not rescuing a few short
     * strokes, it was setting the duration of nearly everything. With the floor separated,
     * only the ramps that start from rest pay it — the jumps and each figure's first stroke —
     * and not the hundreds of interior segments.
     *
     * WHO IS WHO, and no new data is needed: `vx_chain_reset()` is called by whoever
     * repositions the beam (a jump or a re-zero), so "starts from rest" is exactly "it is the
     * first ramp after a reset". The jump READS the flag without consuming it and the first lit
     * stroke consumes it, so both — the jump and the stroke that follows it, which also starts
     * from rest — are charged the start floor.
     *
     * OUT OF THE BOX THEY EQUAL THE OLD ONES: until they are swept, the behaviour is identical
     * to before, byte for byte. */
    let starting = STARTING.load(Ordering::Relaxed) != 0;
    let min_t1 = if starting { MIN_T1_START.load(Ordering::Relaxed) }
                 else        { MIN_T1.load(Ordering::Relaxed) } as i32;
    let mut vcap = vcap_in as i32;

    if m == 0 {
        return (0, 0, min_t1 as u16); // degenerate (dot); minimal ramp
    }

    /* ── THE FIXED-TIME MODEL, LIKE THE BIOS AND LIKE THE REFERENCE ──────────────
     *
     * FIXED_RAMP = 0 -> the old model (variable time). >0 -> that value is the duration of ALL
     * ramps, and the length comes entirely out of the DAC: vx = dx.
     *
     * WHY. The 6809 assembly that draws this same figure correctly on this same console loads
     * `T1CL = $7F` ONCE and then, for each vector, only does `CLR T1CH` to fire it. Every
     * vector lasts 127 counts. The reference does the same with m_Scale = 128. We split the
     * distance between `vx` AND `t1`, with t1 from 31 to 160 — and that turns ANY error in the
     * DURATION model (the counter's +1.5, the settling, the E phase) into a distance error
     * divided by t1: 1% on a long stroke, 5% on a short one, 19% with MIN_T1=8.
     *
     * That is exactly the symptom measured on 2026-08-25: Kong's perimeter perfect and the
     * interior detail displaced, the same straight line longer in 8 ramps than in 1, and the
     * 6809 drawing correctly what we bend.
     *
     * With fixed time there is no split, so there is nowhere to concentrate the error — and
     * besides vx = dx exactly, so the rounding residue disappears and the debt chain is left
     * with nothing to do. The cost is the other side of the coin: a short stroke lasts as long
     * as a long one, which is exactly the saving MIN_T1 was chasing. That is why it is a knob
     * and not a replacement: both have to be MEASURED on the same console. */
    let fixed = FIXED_RAMP.load(Ordering::Relaxed) as i32;
    if fixed > 0 {
        return ((dx / f).clamp(-128, 127) as i8, (dy / f).clamp(-128, 127) as i8, fixed as u16);
    }
    // 0xA0 = 160, NOT 0x7F: the comment that used to be here said 0x7F and had been lying
    // for a long time. `s` governs the length AND the speed of every vector
    // (vx = dx*s/t1, and t1 is clamped to s), so reasoning about ramp_params with 127 in your
    // head gives wrong numbers. It was caught because t1's histogram had a >=128 bucket that
    // cannot exist with s=127.
    let s = scale();
    let t1_floor = (s * m / (127 * f)).clamp(min_t1, s); // dwell floor (∝ length)
    // VELOCITY CAP: at MIN_T1=24 mid-length segments (24 < m ≤ 110) run at the full
    // ±127 swing, and the integrator op-amp overshoots the endpoint → platform
    // vectors "stretch". MIN_T1=110 avoided it by throttling their velocity (long
    // t1). Replicate that WITHOUT raising the global floor: if the dominant velocity
    // (m·0x7F / t1) would exceed VCAP, raise t1 so it lands at VCAP (distance is
    // preserved: velocity·t1 stays). Short vectors are already below VCAP → their
    // short dwell (the flicker win) is untouched. t1 is capped at 0x7F.
    // DIVIDE UPWARDS — a SYSTEMATIC bias of -0.79 units per vector used to live here.
    // `t1_vcap` is a FLOOR: the minimum time for the speed not to exceed the cap. Truncating
    // downwards leaves the floor short, the speed overshoots, the DAC clips it at +-127 and
    // the vector comes out SHORT — always in the same direction, so it accumulates LINEARLY
    // with the number of strokes.
    //
    //   m=50, cap=127:  50*160/127 = 62.99 -> 62    vx = round(8000/62) = 129 -> 127
    //                   travel = 127*62/160 = 49.2                          -> -0.79
    //   rounding up:                        -> 63    vx = round(8000/63) = 127
    //                   travel = 127*63/160 = 50.006                        -> +0.006
    //
    // MEASURED on the host on 2026-08-24 over all 127 deltas: worst case -0.788 before, and a
    // row of four strokes accumulated -3.15. It is the bias the VPY_MAX_CONSECUTIVE_DRAWS
    // comment in the SDK describes ("fixed per movement, accumulates by count") and which was
    // being covered up by re-zeroing the beam every few strokes.
    let vc = vcap.max(1);
    let t1_vcap = ((m * s + vc * f - 1) / (vc * f)).max(1);
    // THE CEILING IS COMPUTED PER VECTOR, IT IS NOT FIXED.
    //
    // `t1` and the speed are the two halves of the same product — v = d*s/t1 — so lengthening
    // the ramp sinks the speed, and the first to die is the MINOR AXIS: when its v rounds to
    // zero the vector collapses onto its major axis. Requiring only that it moves, |v| >= 1:
    //
    //     t1 <= d_minor * s
    //
    // IT CANNOT CONTRADICT THE FLOOR: d_minor >= 1 gives a ceiling >= s, and `t1_floor` is
    // already clamped to s, so the interval never inverts. AND NO MORE CAN BE DEMANDED: with
    // |v| >= 2 the ceiling drops to d_minor*s/2, which for a (127,1) gives 80 where its
    // geometry asks for 160 — it would steal length from the major axis to save precision on
    // the minor. A very elongated vector HAS its minor axis nearly still; that is not a defect
    // to fix, it is what elongated means.
    //
    // WHAT THIS FIXES. With the ceiling fixed at 160, `t1_vcap` was flattened and the speed
    // cap had no travel left: at cap = 8 it asked for 2540 and got 160, a saturated knob —
    // which is exactly what the screen showed on 2026-08-17, the spur almost closed without
    // quite going away. Now a long diagonal reaches its 2540, while the (127,1) stays at 160,
    // which is the CORRECT answer for IT and the one a global constant cannot give: slowing it
    // further would flatten it against the horizontal.
    //
    // AT FACTORY VALUES NOTHING CHANGES. With the cap at 127 no vector exceeds t1 = 160 (swept
    // over all 65,024 deltas: 0.0% exceed it), so this only opens up range when the cap is
    // deliberately lowered.
    let d_minor = match (dx.abs(), dy.abs()) {
        (0, b) => b,                  // no X axis to lose
        (a, 0) => a,
        (a, b) => a.min(b),
    };
    let ceiling = (d_minor * s / f)
        .min(T1_TRANSPORT.load(Ordering::Relaxed) as i32)
        .max(min_t1);                 // in case the transport cap sits below the floor
    /* WHO WINS WHEN THE CEILING AND THE CAP CONTRADICT EACH OTHER.
     *
     * `ceiling` protects the slope: past it, the MINOR axis's rate rounds to 0 and the diagonal
     * straightens out. `t1_vcap` protects the speed. On a shallow diagonal the two do not both
     * fit, and until now the ceiling won — with the rate then exceeding the cap.
     *
     * MEASURED in a frame of our Major Havoc: 341 of 754 lit strokes (45%) come out above 51,
     * and some at 125. At that speed the stroke deposits 2.5 times less charge per unit of
     * length than one at 51, while the joint between micro-segments still burns its 22-24
     * cycles STANDING STILL: a faint line with a bright dot at each end, which is the symptom
     * of "you can see every micro-segment dot".
     *
     * (AND MIND THE EASY COMPARISON: the reference never exceeds 51 in its capture, but that is
     * NOT a cap of theirs — its frame is 85% straight text strokes and its shallowest diagonal
     * has slope 0.48. It never meets this case.)
     *
     * With the cap winning, the minor axis is lost in ONE micro-segment but NOT lost: the debt
     * records it and charges it to the next one, which is exactly what it is for. That debt did
     * not exist when the ceiling was written.
     *
     * CEILING_RULES = 0 lets the cap win. It is NOT the default: the invariant "no axis with a
     * delta is left standing still" is asserted in `t1_ceiling_is_per_vector`, and releasing
     * the ceiling was tried once and broke the drawing on BOTH cartridges. It is compared on
     * the console before deciding, not here. */
    let t1 = if CEILING_RULES.load(Ordering::Relaxed) != 0 {
        t1_floor.max(t1_vcap).min(ceiling)
    } else {
        t1_floor.max(t1_vcap).min(ceiling.max(t1_vcap))
    };
    // COMPENSATE THE START INSTEAD OF SLOWING THE BEAM.
    //
    // OBSERVED on the console on 2026-08-24 over the grid: with a high cap the drawing "runs
    // away" — it falls short — but it does NOT shimmer, i.e. the error is STATIC. With a low
    // cap the geometry comes out right and what shimmers is the framerate, because t1 shoots
    // up.
    //
    // A static error is not fixed by slowing down: it is compensated. If the amplifier takes a
    // FIXED time to get up to speed, the beam travels v*(t1 - T) instead of v*t1. Lengthening
    // t1 by T returns the distance WITHOUT touching the speed, and it costs T cycles per
    // vector instead of multiplying t1 by 2.5 the way lowering the cap does.
    //
    // T is a TIME, so its relative weight is greater on short vectors — which is exactly the
    // signature of "the drawing runs away" when the beam moves fast.
    //
    // 0 = as before. It is adjusted hot from the panel; the good value is the one that makes
    // the grid measure what it says it measures.
    // IT IS APPLIED AT THE END, NOT HERE — see the end of the function.
    //
    // It used to be here, added to `t1` BEFORE computing `vx`, and that cancelled it exactly:
    // the distance commanded is `vx*t1/s`, so recomputing `vx` with the already-lengthened
    // `t1` gives `dx` again. The knob only made the ramp slower and longer, without returning
    // a single unit of distance. MEASURED on the console on 2026-08-25: T1_LAG=8 changed the
    // drawing NOT AT ALL, and that is why a hypothesis that had never actually been tested got
    // discarded.

    // PICK THE t1 THAT DEVIATES LEAST, between the computed one and the next.
    //
    // Rounding `vx` to nearest bounds ONE stroke's error, but it does not centre it: depending
    // on where s/t1 falls, the rounding nearly always goes the same way and then it stops being
    // noise and becomes a DEBT that adds up. MEASURED with the `ramp_error_has_no_bias` test:
    // +0.028 units per stroke at cap=96, i.e. 2.4 after 84 of them.
    //
    // t1 and t1+1 give two different vx*t1 products and one of them lands closer. It costs one
    // more division and at most one ramp cycle, and the error stops having a preferred
    // direction — which is the only thing that makes it accumulate.
    let extra = T1_EXTRA_Q8.load(Ordering::Relaxed) as i32;
    /* Hoisted out of the loop: m*s >= 0, so dividing by f = 2^q is a shift (exact). */
    let n_q8 = (m * s * 256) >> q;
    let msf = (m * s) >> q;
    let error_of = |t: i32| -> i32 {
        if t <= 0 { return i32::MAX; }
        let den = t * 256 + extra;
        let v = ((n_q8 + den / 2) / den).clamp(-128, 127);   // n_q8 >= 0
        (v * t - msf).abs()
    };
    let t1 = if t1 < ceiling && error_of(t1 + 1) < error_of(t1) { t1 + 1 } else { t1 };
    // ROUND, DO NOT TRUNCATE. Distance is velocity x time, so `vx * t1` has to stay
    // proportional to `dx * s` — but integer division always rounds DOWN, and the loss
    // is the fractional part of `s / t1`, which lands wherever it lands:
    //
    //   t1 =  8 -> 127/8  = 15.875 -> 15  ->  94.5% of the length   (glyphs stepped)
    //   t1 = 24 -> 127/24 =  5.29  ->  5  ->  94.5%
    //   t1 = 31 -> 127/31 =  4.096 ->  4  ->  97.6%                 (glyphs clean)
    //
    // MEASURED on hardware 2026-08-05: the user found letters stepped at MIN_T1 = 8 and
    // straight at 31, which is this table and nothing else — the error is not monotonic
    // in the floor, so no choice of floor fixes it. Rounding to nearest halves the worst
    // case and, more importantly, removes the dependence on where s/t1 happens to fall.
    // Symmetric around zero so a stroke and its mirror get the same length.
    // The divisor is the ramp's REAL duration, not `t1`. See T1_EXTRA_Q8: the 6522 counts
    // t1 + 1.5 on a one-shot, and dividing by plain `t1` travels too far. With
    // T1_EXTRA_Q8 = 0 this is exactly the old arithmetic.
    let den = t1 * 256 + extra;
    let round_div = |num: i32| -> i32 {
        let n = num * 256;
        if n >= 0 { (n + den / 2) / den } else { (n - den / 2) / den }
    };
    let (vx, vy) = if want_v {
        (round_div(dx * s / f).clamp(-128, 127) as i8, round_div(dy * s / f).clamp(-128, 127) as i8)
    } else { (0i8, 0i8) };
    // AND NOW, THE START DELAY. With `vx` already chosen, lengthening the ramp by T makes the
    // beam travel `vx*(t1+T)/s` — more than was asked for, which is exactly the distance it
    // loses while getting up to speed. It is a TIME, so it weighs more on short strokes: the
    // signature of "the drawing runs away" when the beam moves fast.
    let t1 = t1 + if starting { T1_LAG_START.load(Ordering::Relaxed) }
                  else        { T1_LAG.load(Ordering::Relaxed) } as i32;
    (vx, vy, t1 as u16)
}

/// Counts added to t1 to compensate for however long the amplifier takes to get up to speed.
/// See the block in `ramp_params`. 0 = the old behaviour.
#[used]
#[no_mangle]
pub static T1_LAG: AtomicU32 = AtomicU32::new(0);

/// 0 = we do not know what is in the S&H. Otherwise, 0x100 | vy.
#[used]
#[no_mangle]
pub static Y_HELD: AtomicU32 = AtomicU32::new(0);

/// EXTRA counts T1 runs beyond its value, in Q8 (256 = 1 count). **0 = the old behaviour**,
/// 384 = 1.5 counts, which is what the 6522 does on a one-shot.
///
/// THE 6522 COUNTS t1 + 1.5, NOT t1. And `ramp_params` programs the speed assuming the ramp
/// lasts `t1`, so every vector travels 1.5/t1 TOO FAR. At t1 = 31, which used to be MIN_T1's
/// floor and where most of them landed, that is 4.8% per vector — and it is not random noise
/// but a bias ALWAYS in the direction of travel, so it accumulates LINEARLY with the number of
/// moves.
///
/// That is exactly what the VPY_MAX_CONSECUTIVE_DRAWS comment in the SDK describes: "the
/// position error is FIXED PER MOVEMENT ... so it accumulates by COUNT". The description was
/// right; the attribution ("the deflection lag at the end of each blanked move") was not.
/// Measured 2026-08-07: with MOVETO_SETTLE_ECYC = 8, which is ~900 cycles and SIX TIMES the
/// real deflection delay (125-150 cycles, measured by bisection the same day), the test image
/// still shimmered exactly the same.
///
/// If this is the cause, correcting it leaves the error bounded instead of growing, and then
/// VPY_MAX_CONSECUTIVE_DRAWS can rise above 1 — which is 3 out of 4 zero_beam calls removed,
/// 23% of the bus traffic and ~5% of the frame.
///
/// MIND DRAW_SCALE. It is 0xA0 = 160 and it is empirical: if it was adjusted so the figures
/// came out the right size, it is already absorbing the MEAN bias. Correcting it here will
/// make everything come out slightly smaller, uniformly, and DRAW_SCALE may have to go up to
/// compensate. What DRAW_SCALE cannot absorb is the bias depending on t1: today short vectors
/// come out proportionally longer than long ones.
#[used]
#[no_mangle]
/* THE RAMP'S TAIL, MEASURED: 2.5 E cycles = 640 in Q8. On the console (2026-09-15, the ramps
 * bench in eye mode, control rows at 1%) every ramp travels v * (t1 + 2.5): the ramp carries
 * on ~2.5 cycles after T1 expires. With 0 here, a piece of t1=8 came out 31% long and a girder
 * of t1~90 by 2.7%, and that is why the (short) rungs stuck out of the (long) rail. This is
 * the number; the console calibration calls it the fixed term and can tune it per tube. */
pub static T1_EXTRA_Q8: AtomicU32 = AtomicU32::new(640);

/// Light the beam through the SHIFT REGISTER ($FF/$00), like the BIOS and like the 6809
/// assembly that draws correctly, instead of through the PCR ($EE/$CE). It requires ACR = 0x98
/// at startup, which `via_setup` sets by looking at this same knob. 0 = through the PCR, as
/// before.
///
/// The `draw_line_seq` comment says the SR path "never drew anything on this hardware" — but
/// that was tried with the rest of the emitter as it then was, and things have changed since.
/// It is retried because it is ONE of the four measured differences against an implementation
/// that does work.
#[used]
#[no_mangle]
pub static BEAM_VIA_SR: AtomicU32 = AtomicU32::new(1);
/* ^ THE REFERENCE CARTRIDGE'S BEAM DIALECT, BY DEFAULT SINCE 2026-09-04.
 *
 * It was at 0, and turning it on was a matter of EVERY game listing `-DUVM2_BEAM_VIA_SR` in
 * its defines. Measured result: of the build targets, `dkong`, `asteroids` and `snowbros_c`
 * did NOT ask for it — and they are exactly the ones reported as "no brightness" on the
 * console, while mhavoc and asterock, which do ask for it, are visible.
 *
 * A port that did not list the define ran with PCR blanking, MIN_T1 = 31 and cap 127: a
 * DIFFERENT path from the one we had spent the whole session measuring against their capture.
 * The knowledge cannot live in 44 lists of defines; it lives here, and whoever needs the old
 * behaviour turns it off with `-DUVM2_BEAM_VIA_PCR`. */

/// Do not rewrite T1CL when it has not changed, the way the 6809 does (it loads it once and
/// then only fires with `CLR T1CH`). Saves one bus write per vector.
#[used]
#[no_mangle]
pub static T1CL_CACHE: AtomicU32 = AtomicU32::new(1);  // default YES: the 6809 loads T1CL once

/// Put the DAC back to zero when each ramp ends, the way the 6809 does (`CLR VIA_port_a`).
/// 0 = as before (`vx` stays set for the whole gap). See the block in emit.rs.
#[used]
#[no_mangle]
pub static DAC_ZERO: AtomicU32 = AtomicU32::new(1);   // default YES: the reference does it

/// FIXED ramp duration, like the BIOS ($7F) and the reference (128). 0 = the variable-time
/// model, the usual one. See the block in `ramp_params` for why.
#[used]
#[no_mangle]
pub static FIXED_RAMP: AtomicU32 = AtomicU32::new(0);

/// The same function for C callers (the UVM2 image). Pointers because a Rust tuple has no C
/// representation — and INTEGERS, which is the crate's rule: the image is compiled `softfp` and
/// the firmware `eabihf`, so not one float crosses.
///
/// THE SPEED FOR THE PATTERN TO COVER `p` IN `t1` COUNTS, not counting the start-up.
///
/// `vx_ramp_params_with_t1` is no use for this, and the difference was measured on the console
/// by reading its command list: for 14 cells of 122 subunits in 140 counts it returns vx = 88,
/// not 122. That is not a fault of its own — its model counts the start-up delay
/// (`T1_EXTRA_Q8`, about 54 counts), so it picks the speed for the ramp to cover the distance
/// in `t1 + 54`. Correct for a stroke, which only cares where it ends.
///
/// A SWEEP cares about something else: its register writes are spaced in T1 counts and they
/// run out at `t1`, so what has to add up is the distance travelled DURING those `t1` — not
/// the total. With the other one the text came out at 72% of its width, the letters cramped.
/// Whatever is left over at the end is dark tail, which does not matter.
#[no_mangle]
pub extern "C" fn vx_ramp_vel_no_lag(p: i32, t1: u32, f: i32) -> i32 {
    let f = if f > 0 { f } else { 1 };
    let s = scale();
    let den = (f as i64) * (if t1 > 0 { t1 } else { 1 }) as i64;
    let n = p as i64 * s as i64;
    let v = if n >= 0 { (n + den / 2) / den } else { (n - den / 2) / den };
    v.clamp(-127, 127) as i32
}

#[no_mangle]
pub extern "C" fn vx_ramp_params(dx: i32, dy: i32, out_vx: *mut i32, out_vy: *mut i32,
                                 out_t1: *mut u32) {
    let (vx, vy, t1) = ramp_params(dx.clamp(-128, 127) as i8, dy.clamp(-128, 127) as i8);
    unsafe {
        if !out_vx.is_null() { *out_vx = vx as i32; }
        if !out_vy.is_null() { *out_vy = vy as i32; }
        if !out_t1.is_null() { *out_t1 = t1 as u32; }
    }
}

/// THE JUMP'S DEBT ABSORPTION USED TO LIVE HERE, as `vx_ramp_params_q4` ->
/// `ramp_params_jump_with_debt`, and it was DEAD CODE: nothing in the tree called that
/// entry point, so the half of the model that corrects the beam during the dark stretch
/// never ran, and the whole accumulated debt landed on the first lit stroke after every
/// jump instead. It is now `vx_debt_take` / `vx_debt_record`, driven by `move_one` in the
/// SDK — the only place that knows which ramps actually reach the bus, which matters
/// because the duration ladder and the soft start both change the ramp after it is chosen.

/// THE JUMP ABSORBING THE DEBT, AS THE CARTRIDGE DRIVES IT — for the bench.
///
/// The debt is `asked - travelled` accumulated over everything, strokes and jumps. A lit
/// stroke can only note it down; the jump asks for `delta + debt` and with that the
/// position squares up again, because the beam is dark and nobody sees the extra bit.
///
/// This is a COMPOSITION of the three primitives the SDK calls, not a second copy of the
/// rule: take the correction, choose the ramp, record asked against travelled. `move_one`
/// does the same three steps, but spread out, because between the second and the third it
/// may still swap t1 for a rung of the duration ladder or split the jump into a priming
/// unit and a long ramp.
#[cfg(test)]
fn ramp_params_jump_with_debt(dx_q4: i32, dy_q4: i32, vcap: u32, q: u32) -> (i8, i8, u16) {
    let (mut ax, mut ay) = (0i32, 0i32);
    vx_debt_take(dx_q4, dy_q4, q, &mut ax, &mut ay);
    let (vx, vy, t1) = ramp_params_q(dx_q4 + ax, dy_q4 + ay, vcap, q);
    vx_debt_record(dx_q4, dy_q4, q, vx as i32, vy as i32, t1 as u32);
    (vx, vy, t1)
}

/// WHAT A RAMP REALLY TRAVELS — the inverse of `vx_ramp_params_with_t1`.
///
/// (The rates for a GIVEN t1, without choosing it, are needed because the reference cartridge
/// does not compute a jump's duration: it PICKS it from a short ladder. Measured over its 1041
/// jumps that follow a stroke, the ladder {8, 18, 31} with a rate cap of 120 explains 1008
/// (97%) — and in its frame 120 it explains ALL of them. Our model derived t1 from the speed
/// cap and gave continuous values (9, 13, 15) where they use 18.)
///
/// This is needed because a caller that FIXES `t1` instead of letting it be chosen (the text
/// sweep) receives a ROUNDED speed, sometimes clipped to +-127: the ramp covers a distance
/// that is not the one asked for. Recording the requested one as the beam's position makes the
/// SDK believe it is where it is not, and the error accumulates sweep after sweep until it
/// pushes the whole drawing into a corner — seen on the console with esb's text.
///
/// Same arithmetic as `r()` there, inverted: p = v * f * t1 / s.
#[no_mangle]
pub extern "C" fn vx_ramp_dist(v: i32, t1: u32, f: i32) -> i32 {
    let f = if f > 0 { f } else { 1 };
    let s = scale();
    if s <= 0 { return 0; }
    let den = (t1 as i32) * 256 + T1_EXTRA_Q8.load(Ordering::Relaxed) as i32;
    /* v = (p*s*256 + den/2) / den  =>  p = v*den / (s*256), with the same f going back. */
    let n = v as i64 * den as i64 * f as i64;
    let d = s as i64 * 256;
    ((if n >= 0 { n + d / 2 } else { n - d / 2 }) / d) as i32
}

#[no_mangle]
pub extern "C" fn vx_ramp_params_with_t1(dx: i32, dy: i32, f: i32, t1: u32,
                                        out_vx: *mut i32, out_vy: *mut i32) {
    /* IT TAKES THE DIVISOR READY-MADE, not the bit count. With `q` and `1 << q` the board
     * returned `f = 0` where the host gives 256 — a run-time shift, the same family as the RRX
     * that cost half a session. It is not needed here: the caller has `UVM2_Q` as a
     * compile-time constant. */
    let f = if f > 0 { f } else { 1 };
    let s = scale();
    /* ALL IN 32 BITS, ON PURPOSE. The i64 version gave `vy = 127` for `dy = 0` on the board —
     * arithmetically impossible — while the host gave the right value. The only 64-bit
     * operation was `t1 * 256`, i.e. a 64-bit shift: the same family as the RRX that cost half
     * a session. And none is needed: with t1 <= 255 the divisor does not exceed 65,280, and the
     * numerator 5.2 million. */
    let den = (t1 as i32) * 256 + T1_EXTRA_Q8.load(Ordering::Relaxed) as i32;
    let den = if den > 0 { den } else { 1 };
    let r = |p: i32| -> i32 {
        let n = (p / f) * s * 256 + ((p % f) * s * 256) / f;
        let v = if n >= 0 { (n + den / 2) / den } else { (n - den / 2) / den };
        v.clamp(-128, 127)
    };
    unsafe {
        if !out_vx.is_null() { *out_vx = r(dx); }
        if !out_vy.is_null() { *out_vy = r(dy); }
    }
}

#[no_mangle]
pub extern "C" fn vx_ramp_params_jump_qn(dx: i32, dy: i32, q: u32, out_vx: *mut i32,
                                          out_vy: *mut i32, out_t1: *mut u32) {
    let v = DAC_CAP;
    let (vx, vy, t1) = ramp_params_q(dx, dy, v, q);
    unsafe {
        if !out_vx.is_null() { *out_vx = vx as i32; }
        if !out_vy.is_null() { *out_vy = vy as i32; }
        if !out_t1.is_null() { *out_t1 = t1 as u32; }
    }
}

#[no_mangle]
pub extern "C" fn vx_ramp_params_jump_q4(dx_q4: i32, dy_q4: i32, out_vx: *mut i32,
                                          out_vy: *mut i32, out_t1: *mut u32) {
    let v = DAC_CAP;
    let (vx, vy, t1) = ramp_params_q(dx_q4, dy_q4, v, 4);
    unsafe {
        if !out_vx.is_null() { *out_vx = vx as i32; }
        if !out_vy.is_null() { *out_vy = vy as i32; }
        if !out_t1.is_null() { *out_t1 = t1 as u32; }
    }
}


/* ── THE CHAIN, WITH ITS DEBT ───────────────────────────────────────────────────────
 *
 * ONE implementation for all three consumers. On 2026-08-24 there were three different
 * decisions about the same thing: the UVM2 image diffused the residue, the emulator did too
 * (on its own account) and our own cartridge's firmware did not. Three copies of one rule is
 * the exact form of divergence that cost a whole afternoon.
 *
 * WHY IT IS NEEDED. `ramp_params` splits a delta between speed and time, both INTEGERS, so a
 * lone stroke CANNOT be exact. Its error is at most half a unit and that is unavoidable. What
 * is avoidable is it ADDING UP: by keeping count of what is owed and asking the next stroke
 * for it, the chain comes out exact and it costs not one cycle.
 *
 * MEASURED along the real path, 84 strokes of 33 units: +6.30 units of drift (2.5% of the
 * screen) without this, +0.04 with it. */

/// What is owed to the drawing, in THOUSANDTHS of a unit, per axis.
#[used]
#[no_mangle]
pub static DEBT_X: AtomicI32 = AtomicI32::new(0);
#[used]
#[no_mangle]
pub static DEBT_Y: AtomicI32 = AtomicI32::new(0);

/// Forget the chain. Called by whoever repositions the beam: a jump or a re-zero.
#[no_mangle]
pub extern "C" fn vx_chain_reset() {
    /* THE DEBT IS NO LONGER THROWN AWAY HERE, AND THAT WAS THE BUG.
     *
     * This used to set DEBT_X/Y to zero, and `move_one` calls it on EVERY jump. With one
     * `move_abs` per segment — which is what any drawer that does not chain does — the debt
     * was wiped between vectors and NEVER accumulated: it was computed at the end of each
     * stroke and the next jump threw it away before anyone corrected it.
     *
     * The symptom, measured by integrating our stream against the input geometry: the position
     * at the start of each stroke drifted +8.45 units median in X, growing from +2.64 in the
     * frame's first third to +13.29 in the last. The reference's is +0.00 with a worst case of
     * 0.03 over the frame's 427 strokes.
     *
     * And it explains why three consecutive fixes to the debt bookkeeping did not move the
     * output BY ONE BYTE: there was no debt to correct.
     *
     * The debt is "where the beam really is minus where we think it is". A jump does not fix
     * that — it is precisely the place to correct it unseen, because it travels blanked. The
     * jump absorbs it, through `vx_debt_take` / `vx_debt_record`.
     *
     * What IS reset is the START flag: the beam is repositioned, so the next ramp starts from
     * rest. That has nothing to do with the debt and the two were together out of habit.
     *
     * AND FOR TWO WEEKS NOBODY ABSORBED IT. Keeping the debt here was only half the fix:
     * the absorbing half lived in `ramp_params_jump_with_debt`, whose only entry point
     * (`vx_ramp_params_q4`) had no callers anywhere, so the jump ran through
     * `ramp_params_q` bare and the whole debt landed on the first LIT stroke after it.
     * Wired up in `move_one` 2026-09-20.
     *
     * TRIED AND REVERTED (2026-09-04), and why it is not an objection any more: preserving
     * the debt across jumps plus full absorption left **Y practically perfect** (+0.01
     * median against the reference's +0.00, coming from -0.52) and **threw X to +705**. The
     * asymmetry was the missing "an axis not asked does not move" guard on the jump — 145
     * of that frame's 427 segments have dx = 0 and almost none dy = 0, so X took that path
     * far more often and the correction leaked in as movement. With the guard, measured by
     * `jump_absorbs_or_not` on the same frame: X median -0.619 -> -0.038. */
    STARTING.store(1, Ordering::Relaxed);
}

/// HOW MUCH THE JUMP MUST ABSORB, in the caller's Q units.
///
/// The jump is the only dark stretch, so it is the only place where moving the beam to
/// where the drawing believes it is costs nothing to look at. This returns what to ADD to
/// the requested delta; it does NOT touch the debt.
///
/// AN AXIS THE JUMP DOES NOT MOVE IS NOT CORRECTED. Measured in the raw commands of a
/// frame: a jump with dx = 0 came out with vx = 1 because the debt leaked in as movement,
/// 0.05 units of drift each, and over 187 jumps that is 9.3 units — the very drift being
/// chased. That axis keeps its debt for the next jump that does move it.
///
/// WHY THIS IS SPLIT FROM THE BOOKKEEPING, and the previous attempt was not: folding both
/// into the params function (`ramp_params_jump_with_debt`) makes the accounting assume
/// the ramp it just computed is the one that gets emitted. In `move_one` it is often not:
/// the duration ladder replaces t1 and recomputes the rates afterwards, and the soft start
/// splits the jump into a priming unit plus a long ramp. On top of that the jump's ramp is
/// computed TWICE per transport — once to decide about priming, once to emit — so a
/// function that mutated the debt would count every jump twice. Taking and recording
/// separately lets the caller record what it actually emitted, however it got there.
#[no_mangle]
pub extern "C" fn vx_debt_take(dx: i32, dy: i32, q: u32, out_ax: *mut i32, out_ay: *mut i32) {
    let f = 1i32 << q;
    let a = |r: i32| if r >= 0 { (r * f + 500) / 1000 } else { (r * f - 500) / 1000 };
    let ax = if dx == 0 { 0 } else { a(DEBT_X.load(Ordering::Relaxed)) };
    let ay = if dy == 0 { 0 } else { a(DEBT_Y.load(Ordering::Relaxed)) };
    unsafe {
        if !out_ax.is_null() { *out_ax = ax; }
        if !out_ay.is_null() { *out_ay = ay; }
    }
}

/// ONE EMITTED RAMP'S SHARE OF THE LEDGER: what it was asked for, minus what it travels.
///
/// Call it once per ramp actually written to the bus, with that ramp's own slice of the
/// request — a jump split into a priming unit and a long ramp calls it twice, and the two
/// slices add up to the whole jump. The travel is computed HERE, with `travel_mil`, and
/// not by the caller: that number carries the ramp's physical 2.5-count tail, and a second
/// copy of the rule in C is exactly the kind of divergence this file keeps paying for.
#[no_mangle]
pub extern "C" fn vx_debt_record(dx: i32, dy: i32, q: u32, vx: i32, vy: i32, t1: u32) {
    let f = 1i32 << q;
    let t = t1 as u16;
    DEBT_X.store((DEBT_X.load(Ordering::Relaxed) + dx * 1000 / f - travel_mil(vx, t))
                      .clamp(-4000, 4000), Ordering::Relaxed);
    DEBT_Y.store((DEBT_Y.load(Ordering::Relaxed) + dy * 1000 / f - travel_mil(vy, t))
                      .clamp(-4000, 4000), Ordering::Relaxed);
}

/// Really throw the debt away. The RE-ZERO uses it, because that does return the beam to a
/// known point: there, what we believed and what is there coincide again and nothing is owed.
#[no_mangle]
pub extern "C" fn vx_debt_reset() {
    DEBT_X.store(0, Ordering::Relaxed);
    DEBT_Y.store(0, Ordering::Relaxed);
}

/// What the ramp REALLY travels, in thousandths: v * t1 / DRAW_SCALE with its fraction.
#[inline(always)]
fn travel_mil(v: i32, t1: u16) -> i32 {
    /* WITH THE TAIL. The rate is computed for (t1 + T1_EXTRA_Q8/256) and the REAL travel is
     * that: accounting for plain v*t1 here made the debt see every stroke 24% short (t1=8) and
     * charge it to the next one — with T1_EXTRA_Q8=640 the console looked "far, far worse".
     * The tail is physical, not an emitter decision: the beam travels v*(t1+2.5) whatever the
     * rate asks for. */
    let t_q8 = t1 as i32 * 256 + T1_EXTRA_Q8.load(Ordering::Relaxed) as i32;
    let a = v * t_q8;
    let d = scale() * 256;
    (a / d) * 1000 + ((a % d) * 1000) / d
}

/// `ramp_params` for a stroke INSIDE A CHAIN: it asks for the delta plus whatever was owed
/// and records what is still owed.
pub fn ramp_params_chain(dx: i8, dy: i8) -> (i8, i8, u16) {
    let (rx, ry) = (DEBT_X.load(Ordering::Relaxed), DEBT_Y.load(Ordering::Relaxed));
    /* To nearest, not truncating: truncating reintroduces the very bias this removes. */
    let round = |r: i32| if r >= 0 { (r + 500) / 1000 } else { (r - 500) / 1000 };
    let px = (dx as i32 + round(rx)).clamp(-128, 127) as i8;
    let py = (dy as i32 + round(ry)).clamp(-128, 127) as i8;

    let (vx, vy, t1) = ramp_params(px, py);
    /* Consumed HERE and not in the jump: the jump and the stroke that follows it both start
     * from rest, so both have to be charged the start floor. */
    STARTING.store(0, Ordering::Relaxed);

    /* Bounded: if a stroke is clipped, the debt cannot grow unchecked or the next one would
     * fly off. Four units is far more than any rounding can owe. */
    DEBT_X.store((rx + dx as i32 * 1000 - travel_mil(vx as i32, t1)).clamp(-4000, 4000),
                  Ordering::Relaxed);
    DEBT_Y.store((ry + dy as i32 * 1000 - travel_mil(vy as i32, t1)).clamp(-4000, 4000),
                  Ordering::Relaxed);
    (vx, vy, t1)
}

/// `ramp_params_chain` WITH SUB-UNITS: `dx`/`dy` in 1/16 of a unit.
///
/// THE TWO THINGS ARE COMPLEMENTARY AND SHOULD NOT BE CONFUSED. The chain's debt corrects the
/// residue THE RAMP commits when rounding `v` — but it received `i8`, so it could not correct
/// what the API had already thrown away before: `VS_RND` divides the game's coordinates by 127
/// and rounds to an integer BEFORE the chain sees anything. MEASURED in mhavoc: 0.22 units of
/// error per axis there, with 3.6% of the vectors entirely sub-unit.
///
/// The debt is still kept in THOUSANDTHS OF A UNIT, without changing its semantics or its
/// caps: it is only converted to 1/16 on the way in and from 1/16 on the way out.
pub fn ramp_params_chain_q4(dx_q4: i32, dy_q4: i32) -> (i8, i8, u16) {
    ramp_params_chain_qn(dx_q4, dy_q4, 4)
}

/* THE INPUT'S PRECISION IS A PARAMETER, not a detail of the bench.
 *
 * MEASURED against the 210 vectors of one of their Major Havoc frames: with the geometry in
 * 1/16 of a unit, 22 of their rates (10.5%) CANNOT be reproduced — not because of how we
 * round, but because what they ask for does not fit that grid. Their vector at rate 32 with
 * t1 = 8 measures 32*8/160 = 1.6 units exactly, and 1/16 can only say 1.5625 or 1.625. At 1/64
 * or finer all 210 come out EXACT.
 *
 * `dx * s / f` TRUNCATES before `round_div` rounds, and that is the only division that loses:
 * which is why raising the input's precision fixes the whole 10.5%. */
pub fn ramp_params_chain_qn(dx_q4: i32, dy_q4: i32, q: u32) -> (i8, i8, u16) {
    let (rx, ry) = (DEBT_X.load(Ordering::Relaxed), DEBT_Y.load(Ordering::Relaxed));
    let f = 1i32 << q;
    /* To nearest, not truncating: truncating reintroduces the very bias this removes. */
    let a_q4 = |r: i32| if r >= 0 { (r * f + 500) / 1000 } else { (r * f - 500) / 1000 };
    /* AN AXIS THAT IS NOT ASKED FOR DOES NOT MOVE, NOT EVEN FOR THE DEBT.
     *
     * The debt exists to correct the rounding residue, but adding it blindly injects movement
     * into an axis whose delta is ZERO — and that is not correcting, it is inventing. MEASURED
     * with the reference geometry as input: of its 189 pure vertical vectors, all 189 come out
     * with vx = 0 in their stream and NONE in ours (vx in {-2, 1, 2}), summing to +81, i.e.
     * +4 units of rightward drift per frame that accumulate until the next re-zero. It is the
     * diagonal drift seen on the console.
     *
     * `a_vertical_does_not_move_x` already covered this for `ramp_params`, but not for the
     * chained version, which is the one strokes use.
     *
     * The debt is NOT lost: it waits for the next vector that does move that axis. */
    let use_debt = DEBT_ON.load(Ordering::Relaxed) != 0;
    let px = if dx_q4 == 0 || !use_debt { dx_q4 } else { dx_q4 + a_q4(rx) };
    let py = if dy_q4 == 0 || !use_debt { dy_q4 } else { dy_q4 + a_q4(ry) };

    let (_, _, t1) = ramp_params_q_v(px, py, DAC_CAP, q, false);   // t1 only: see ramp_params_q_v
    /* THE ROUNDING TO MICRO-SEGMENTS, HERE AND NOT IN THE EMITTER — OTHERWISE THE DEBT DOES
     * NOT SEE IT.
     *
     * `draw_line_seq` splits the stroke into n micro-segments of 8 counts and rescales the
     * rate for the time it is really going to run. That rescaling ROUNDS, and its residue fell
     * outside the accounting: the debt was computed with the earlier (vx, t1), saw
     * `asked - travelled = 0` and corrected nothing.
     *
     * MEASURED by integrating our stream against the input geometry: the position at the start
     * of each stroke drifted +8.45 units median in X, growing from +2.64 in the frame's first
     * third to +13.29 in the last, with the debt constantly at zero. The reference's is +0.00
     * with a worst case of 0.03 over the 427 strokes.
     *
     * By returning the already-rounded t1 and the rate computed FOR IT, the debt measures what
     * is actually emitted and the emitter has nothing to rescale.
     *
     * THE 8 IS THE SAME AS `T1M` in emit.rs. If one changes, the other changes: they are the
     * same decision (the reference dialect's micro-segment) written in two places. */
    const MICRO: i32 = 8;
    let n = core::cmp::max(1, (t1 as i32 + MICRO / 2) / MICRO);
    let runs = n * MICRO;
    /* THE RATE, COMPUTED ONCE FOR THE t1 THAT REALLY RUNS — AND IN 32 BITS.
     *
     * Rescaling the rate `ramp_params_q` computed for ANOTHER t1 rounds twice. In their frame
     * 120 one stroke was left different because of that: it asks for 1.3477 units, they emit
     * 27 (1.3477*160/8 = 26.95) and we came out with 22 for t1 = 10 which rescaled to 8 gave
     * 27.5 -> 28.
     *
     * THIS SAME CHANGE WAS ATTEMPTED ONCE AND SATURATED ON THE BOARD, giving 127 on almost
     * every stroke while the host was correct, and the similarity fell from 99.8% to 2.1%. The
     * cause was the 64-bit arithmetic, not the formula: rewritten in 32 bits, the board agrees
     * with the host. The fixed RRX was not the only broken 64-bit path. */
    let sc = scale();
    let den32 = runs * 256 + T1_EXTRA_Q8.load(Ordering::Relaxed) as i32;
    let den32 = if den32 > 0 { den32 } else { 1 };
    let fq = 1i32 << q;
    /* TRUNCATED division by 2^q with shifts: Rust's `x / fq` truncates towards zero, and `>>`
     * rounds downwards; for x < 0 we shift -x and flip the sign. Exact. */
    let div_t = |x: i32| -> i32 { if x >= 0 { x >> q } else { -((-x) >> q) } };
    let direct = |p: i32| -> i8 {
        let pe = div_t(p);
        let n = pe * sc * 256 + div_t((p - pe * fq) * sc * 256);
        let v = if n >= 0 { (n + den32 / 2) / den32 } else { (n - den32 / 2) / den32 };
        v.clamp(-128, 127) as i8
    };
    let (vx, vy, t1) = (direct(px), direct(py), runs as u16);
    STARTING.store(0, Ordering::Relaxed);

    /* And the debt is only touched on the axis that moved: if nothing was asked for, there is
     * no new residue to record, and whatever was already there keeps waiting its turn. */
    if dx_q4 != 0 {
        DEBT_X.store((rx + dx_q4 * 1000 / f - travel_mil(vx as i32, t1)).clamp(-4000, 4000),
                      Ordering::Relaxed);
    }
    if dy_q4 != 0 {
        DEBT_Y.store((ry + dy_q4 * 1000 / f - travel_mil(vy as i32, t1)).clamp(-4000, 4000),
                      Ordering::Relaxed);
    }
    (vx, vy, t1)
}

#[no_mangle]
pub extern "C" fn vx_ramp_params_chain_qn(dx: i32, dy: i32, q: u32, out_vx: *mut i32,
                                          out_vy: *mut i32, out_t1: *mut u32) {
    let (vx, vy, t1) = ramp_params_chain_qn(dx, dy, q);
    unsafe {
        if !out_vx.is_null() { *out_vx = vx as i32; }
        if !out_vy.is_null() { *out_vy = vy as i32; }
        if !out_t1.is_null() { *out_t1 = t1 as u32; }
    }
}

#[no_mangle]
pub extern "C" fn vx_ramp_params_chain_q4(dx_q4: i32, dy_q4: i32, out_vx: *mut i32,
                                          out_vy: *mut i32, out_t1: *mut u32) {
    let (vx, vy, t1) = ramp_params_chain_q4(dx_q4, dy_q4);
    unsafe {
        if !out_vx.is_null() { *out_vx = vx as i32; }
        if !out_vy.is_null() { *out_vy = vy as i32; }
        if !out_t1.is_null() { *out_t1 = t1 as u32; }
    }
}

/* ── PER-JUMP DEBT: TRIED AND DISCARDED (2026-09-03) ────────────────────────────────
 *
 * A variant of `ramp_params_jump` lived here, with its own pair of accumulators so a jump's
 * residue would be charged to ANOTHER jump — where the beam is blanked and correcting is
 * invisible — instead of contaminating the next lit stroke, which is what the `move_one` note
 * asked for and why the previous attempt (putting them in the same chain) had failed.
 *
 * MEASURED over the 255 real jumps of an asterock frame: the accumulated error RISES from 86.8
 * to 99.8 units in X and from 91.6 to 100.6 in Y. It gets worse. It is removed rather than
 * left uncalled: a function that compiles and nobody uses is the trap this file has already
 * paid for with `set_ramp` and with `fixup`.
 *
 * AND A WARNING ABOUT HOW THAT WAS MEASURED. The figure comes from a copy of the model in
 * Python, and in the same session I wrote THREE measurements of the ramp's bias and ALL THREE
 * came out as artefacts: one clamped to -127 instead of -128, another simulated neither
 * `error_of` nor T1_EXTRA_Q8, and the third — a harness that called the real function inside
 * the emulated CPU — returned 0,0,0 for every input, so it was measuring the delta itself.
 * Before touching this again: the good harness is the one in `correr_uvm2.mjs` (it prints
 * "vx_ramp_params IN THE EMULATED CPU" with cases whose result is known from the host), and
 * the first thing to check is that it does NOT return zeros. */

/// The jump, from C. Called by whoever repositions the beam (moveto).
#[no_mangle]
pub extern "C" fn vx_ramp_params_jump(dx: i32, dy: i32, out_vx: *mut i32, out_vy: *mut i32,
                                       out_t1: *mut u32) {
    let (vx, vy, t1) = ramp_params_jump(dx.clamp(-128, 127) as i8, dy.clamp(-128, 127) as i8);
    unsafe {
        if !out_vx.is_null() { *out_vx = vx as i32; }
        if !out_vy.is_null() { *out_vy = vy as i32; }
        if !out_t1.is_null() { *out_t1 = t1 as u32; }
    }
}

/// The same one, for C callers. See `vx_ramp_params`.
#[no_mangle]
pub extern "C" fn vx_ramp_params_chain(dx: i32, dy: i32, out_vx: *mut i32, out_vy: *mut i32,
                                       out_t1: *mut u32) {
    let (vx, vy, t1) = ramp_params_chain(dx.clamp(-128, 127) as i8, dy.clamp(-128, 127) as i8);
    unsafe {
        if !out_vx.is_null() { *out_vx = vx as i32; }
        if !out_vy.is_null() { *out_vy = vy as i32; }
        if !out_t1.is_null() { *out_t1 = t1 as u32; }
    }
}

#[cfg(test)]
mod vertical_bias {
    use super::*;
    /// A VERTICAL VECTOR CANNOT MOVE X. The emulated CPU's probe returned `(0,50) -> vx=1`,
    /// and in asterock's real list EVERY vertical stroke came out with vx=1 (351 of them with
    /// `vx=1 vy=24`): 0.39 units of displacement per stroke, always to the same side, which is
    /// the horizontal drift seen on the console. This test says whether the bias is in the
    /// MODEL or in the emulator.
    ///
    /// It prints what the HOST gives for the same cases as the emulated CPU's probe, so the two
    /// sides can be put next to each other. `cargo test -- --nocapture`.
    #[test]
    fn print_the_probe_cases() {
        /* THE TEST_LOCK, like the other tests in this file: these knobs are GLOBAL STATE and
         * without serialising they clobber each other between tests (adding them without the
         * lock broke `t1_ceiling_is_per_vector`). */
        let _t = crate::emit::test_knobs();
        MIN_T1.store(8, Ordering::Relaxed);
        MIN_T1_START.store(8, Ordering::Relaxed);
        DRAW_SCALE.store(160, Ordering::Relaxed);
        T1_TRANSPORT.store(160, Ordering::Relaxed);
        /* MATCHED TO THE CARTRIDGE. Without this the test runs with the crate's DEFAULTS and
         * the cartridge with whatever uvm2_draw_init sets: comparing the two was comparing two
         * configurations, not two implementations. */
        T1_EXTRA_Q8.store(0, Ordering::Relaxed);
        T1_LAG.store(0, Ordering::Relaxed);
        T1_LAG_START.store(0, Ordering::Relaxed);
        for (dx, dy) in [(0i8, 50i8), (50, 0), (68, 0), (-46, 0), (6, 0)] {
            let (vx, vy, t1) = ramp_params(dx, dy);
            std::println!("  HOST ({dx},{dy}) -> vx={vx} vy={vy} t1={t1}");
        }
    }

    #[test]
    fn a_vertical_does_not_move_x() {
        /* THE TEST_LOCK, like the other tests in this file: these knobs are GLOBAL STATE and
         * without serialising they clobber each other between tests (adding them without the
         * lock broke `t1_ceiling_is_per_vector`). */
        let _t = crate::emit::test_knobs();
        MIN_T1.store(8, Ordering::Relaxed);
        MIN_T1_START.store(8, Ordering::Relaxed);
        DRAW_SCALE.store(160, Ordering::Relaxed);
        for dy in [3i8, 7, 24, 50, 60, 120, -24, -60] {
            let (vx, _vy, _t1) = ramp_params(0, dy);
            assert_eq!(vx, 0, "dx=0 dy={dy} deberia dar vx=0 y da vx={vx}");
        }
    }

    #[test]
    fn fixed_time_jump_only_stretches_when_it_does_not_fit() {
        let _t = crate::emit::test_knobs();
        MIN_T1.store(8, Ordering::Relaxed);
        MIN_T1_START.store(8, Ordering::Relaxed);
        DRAW_SCALE.store(160, Ordering::Relaxed);
        T1_TRANSPORT.store(160, Ordering::Relaxed);
        T1_EXTRA_Q8.store(0, Ordering::Relaxed);
        T1_JUMP.store(31, Ordering::Relaxed);

        // At s=160 and t1=31 the rate fits in +-127 up to 127*31/160 = 24 units.
        for m in [1i8, 5, 12, 24] {
            let (vx, _vy, t1) = ramp_params_jump(m, 0);
            assert_eq!(t1, 31, "m={m} should fit in the fixed time and gives t1={t1}");
            assert!(vx.abs() as i32 <= 127);
        }
        // And beyond that it stretches JUST ENOUGH, instead of clipping the rate: if it stayed
        // at 31 the DAC would saturate and the jump would always fall short the same way.
        for m in [40i8, 80, 127] {
            let (vx, _vy, t1) = ramp_params_jump(m, 0);
            assert!(t1 > 31, "m={m} does not fit in t1=31 and should stretch; gives t1={t1}");
            let travel = (vx as i32) * (t1 as i32) / 160;
            assert!((travel - m as i32).abs() <= 1,
                    "m={m}: asked {m}, travel {travel} (vx={vx} t1={t1})");
        }
        // The minor axis is not lost: a nearly horizontal jump keeps its dy.
        let (_vx, vy, _t1) = ramp_params_jump(24, 3);
        assert!(vy != 0, "a jump's minor axis cannot round to zero");

        T1_JUMP.store(0, Ordering::Relaxed); // do not contaminate the other tests
    }

    #[test]
    fn debt_does_not_move_an_unasked_axis() {
        let _t = crate::emit::test_knobs();
        MIN_T1.store(8, Ordering::Relaxed);
        MIN_T1_START.store(8, Ordering::Relaxed);
        DRAW_SCALE.store(160, Ordering::Relaxed);
        T1_TRANSPORT.store(160, Ordering::Relaxed);
        T1_EXTRA_Q8.store(0, Ordering::Relaxed);
        T1_JUMP.store(0, Ordering::Relaxed);
        DEBT_X.store(0, Ordering::Relaxed);
        DEBT_Y.store(0, Ordering::Relaxed);

        /* Diagonals that leave a debt, with pure verticals interleaved: the verticals must
         * NOT carry that debt into X. It is the real case — 189 verticals in a reference
         * frame, all with vx = 0. */
        let mut sum_vx = 0i32;
        for k in 0..40 {
            ramp_params_chain_q4(37 + k % 5, 23 + k % 7, );
            let (vx, _vy, _t1) = ramp_params_chain_q4(0, 40 + k % 3);
            assert_eq!(vx, 0, "a vertical with dx=0 cannot come out with vx={vx}");
            sum_vx += vx as i32;
        }
        assert_eq!(sum_vx, 0, "and with no accumulated drift in X");
    }

    #[test]
    fn subunits_reach_positions_integers_cannot() {
        let _t = crate::emit::test_knobs();
        MIN_T1.store(8, Ordering::Relaxed);
        MIN_T1_START.store(8, Ordering::Relaxed);
        DRAW_SCALE.store(160, Ordering::Relaxed);
        T1_TRANSPORT.store(160, Ordering::Relaxed);
        T1_EXTRA_Q8.store(0, Ordering::Relaxed);
        T1_JUMP.store(0, Ordering::Relaxed);

        // 2.5 units cannot be asked for in integers: either 2 or 3. In Q4 it is 40 sixteenths.
        let (vx_e, _, t1_e) = ramp_params(2, 0);
        let (vx_q, _, t1_q) = ramp_params_q(40, 0, 42, 4);
        let rec = |v: i8, t: u16| v as f64 * t as f64 / 160.0;
        let d_e = (rec(vx_e, t1_e) - 2.5).abs();
        let d_q = (rec(vx_q, t1_q) - 2.5).abs();
        assert!(d_q < d_e / 2.0,
                "Q4 should land far closer to 2.5: integer {:.3} (err {:.3}), \
                 Q4 {:.3} (err {:.3})", rec(vx_e, t1_e), d_e, rec(vx_q, t1_q), d_q);

        // And a SUB-UNIT vector, which does not even exist in integers: 0.5 units = 8 sixteenths.
        let (vx0, vy0, _) = ramp_params(0, 0);
        assert_eq!((vx0, vy0), (0, 0), "in integers half a pixel is lost entirely");
        let (vxh, _, t1h) = ramp_params_q(8, 0, DAC_CAP, 4);
        assert!(rec(vxh, t1h) > 0.3 && rec(vxh, t1h) < 0.7,
                "half a unit should come out ~0.5 and comes out {:.3}", rec(vxh, t1h));

        // Q4 with already-integer values cannot change what it used to do.
        for d in [1i8, 3, 7, 20, 60, -5, -33] {
            assert_eq!(ramp_params(d, 0), ramp_params_q(d as i32 * 16, 0, DAC_CAP, 4),
                       "d={d}: an exact integer has to give the same on both paths");
        }
    }
}

#[cfg(test)]
mod x_bias_2026_09_04 {
    use super::*;
    /// THE FRAME'S REAL CASES. With the reference geometry as input, their vx and ours differ
    /// by 1-2 on 88% of the micro-segments while vy matches. This asks the MODEL about those
    /// same cases, with no cartridge in between.
    #[test]
    fn print_the_x_bias() {
        let _t = crate::emit::test_knobs();
        MIN_T1.store(8, Ordering::Relaxed);
        MIN_T1_START.store(8, Ordering::Relaxed);
        DRAW_SCALE.store(160, Ordering::Relaxed);
        T1_TRANSPORT.store(160, Ordering::Relaxed);
        T1_EXTRA_Q8.store(0, Ordering::Relaxed);
        DEBT_X.store(0, Ordering::Relaxed);
        DEBT_Y.store(0, Ordering::Relaxed);
        // (dx_q4, dy_q4) -> what they emit
        let cases = [((0, 40), (50, 0)), ((41, 0), (0, 51)), ((0, -40), (-50, 0)),
                     ((-11, 20), (25, -13)), ((11, 20), (25, 13)), ((21, 0), (0, 26))];
        for ((dx, dy), (svy, svx)) in cases {
            let (vx, vy, t1) = ramp_params_chain_q4(dx, dy);
            std::println!("  dx_q4={dx:4} dy_q4={dy:4} -> ours vy={vy:4} vx={vx:4} t1={t1:3} | theirs vy={svy:4} vx={svx:4}");
        }
    }
}


#[cfg(test)]
mod drift_bench {
    use super::*;
    /// REPLAYS THE WHOLE FRAME ON THE HOST and follows the debt vector by vector.
    ///
    /// It exists because measuring this on the cartridge costs one build and one emulator run
    /// per attempt, and because a build that fails silently reads exactly like a change with no
    /// effect — which has happened. Here the geometry is the same (Major Havoc's frame 120,
    /// read from the header the capture player generates) and the model is the one that gets
    /// compiled, so what this says is what the board will do.
    ///
    /// THE FILE STATES THE PRECISION. It used to be written by hand as 16 in five places, and
    /// on regenerating the table in 1/256 the bench blew up on overflow instead of warning that
    /// it was reading a different unit.
    ///
    /// Where the geometry comes from. `VX_GEOM` changes it without touching the bench, which
    /// is what lets a specific picture be measured with the same instrument the reference
    /// frame was measured with.
    ///
    /// The reference capture itself is NOT part of this kit. Without it the file reads as
    /// empty, `geometry()` returns nothing, and the benches that depend on it have no data to
    /// compare against — which is the honest outcome, not a pass.
    fn capture_file() -> std::string::String {
        std::env::var("VX_GEOM").unwrap_or_else(|_| "reference/vf_geom.h".into())
    }

    fn q_bits() -> u32 {
        let t = std::fs::read_to_string(capture_file()).unwrap_or_default();
        t.lines()
            .find_map(|l| l.trim().strip_prefix("#define VF_GEOM_QBITS "))
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(4)
    }

    fn geometry() -> std::vec::Vec<(i32, i32, i32, i32)> {
        let t = std::fs::read_to_string(capture_file()).unwrap_or_default();
        let mut v = std::vec::Vec::new();
        for l in t.lines() {
            let l = l.trim();
            if !l.starts_with('{') { continue; }
            let n: std::vec::Vec<i32> = l.trim_matches(|c| c=='{'||c=='}'||c==',')
                .split(',').filter_map(|x| x.trim().parse().ok()).collect();
            if n.len() >= 4 { v.push((n[0], n[1], n[2], n[3])); }
        }
        v
    }

    /// ONE PASS OVER THE GEOMETRY, with the jump absorbing the debt or not.
    ///
    /// Both variants exist in the tree and until now only one was measured: the bench called
    /// `ramp_params_jump_with_debt` while the cartridge called, through
    /// `vx_ramp_params_jump_qn`, plain `ramp_params_q`. So this measured the designed model and
    /// the board ran another. `with_debt` picks which, with everything else equal, which is the
    /// only way the comparison means anything.
    ///
    /// Returns (median, worst, worst debt) of the beam's position error at the END of each
    /// stroke, in device units.
    fn run_pass(g: &[(i32, i32, i32, i32)], q: u32, with_debt: bool) -> (f64, f64, i32) {
        DEBT_X.store(0, Ordering::Relaxed);
        DEBT_Y.store(0, Ordering::Relaxed);
        let f = (1u32 << q) as f64;
        let (mut bx, mut by) = (0.0f64, 0.0f64);    // where the beam is, in units
        let (mut px_, mut py_) = (0.0f64, 0.0f64);  // where the drawer BELIEVES it is
        let mut errors: std::vec::Vec<f64> = std::vec::Vec::new();
        let mut worst = 0.0f64;
        let mut worst_debt = 0i32;
        let mut pinned = 0usize;
        let mut jumps = 0usize;
        for (x0, y0, x1, y1) in g.iter() {
            let (jx4, jy4) = (*x0 - (px_ * f).round() as i32,
                              *y0 - (py_ * f).round() as i32);
            if jx4 != 0 || jy4 != 0 {
                let (mut ax, mut ay) = (0i32, 0i32);
                if with_debt { vx_debt_take(jx4, jy4, q, &mut ax, &mut ay); }
                let (mut vx, mut vy, mut t1) =
                    ramp_params_q(jx4 + ax, jy4 + ay, DAC_CAP, q);
                /* THE DURATION LADDER, which `move_one` applies to every jump and this
                 * bench did not model. It replaces t1 with the first rung of {8, 18, 31}
                 * that fits under the rate cap and recomputes the rates for it — so the
                 * ramp that reaches the bus is NOT the one the params function chose, and
                 * a bench that skips this measures a jump the board never emits. */
                let m = (jx4 + ax).abs().max((jy4 + ay).abs()) as i64;
                for r in [8u16, 18, 31] {
                    if m * scale() as i64 <= 120i64 * (1i64 << q) * r as i64 {
                        if r != t1 {
                            t1 = r;
                            let (mut nx, mut ny) = (0i32, 0i32);
                            vx_ramp_params_with_t1(jx4 + ax, jy4 + ay, 1i32 << q,
                                                  t1 as u32, &mut nx, &mut ny);
                            vx = nx.clamp(-128, 127) as i8;
                            vy = ny.clamp(-128, 127) as i8;
                        }
                        break;
                    }
                }
                if vx.abs() == 127 || vy.abs() == 127 { pinned += 1; }
                jumps += 1;
                bx += vx as f64 * t1 as f64 / 160.0;
                by += vy as f64 * t1 as f64 / 160.0;
                if with_debt {
                    vx_debt_record(jx4, jy4, q, vx as i32, vy as i32, t1 as u32);
                }
            }
            px_ = *x0 as f64 / f; py_ = *y0 as f64 / f;
            let (vx, vy, t1) = ramp_params_chain_qn(x1 - x0, y1 - y0, q);
            bx += vx as f64 * t1 as f64 / 160.0;
            by += vy as f64 * t1 as f64 / 160.0;
            px_ = *x1 as f64 / f; py_ = *y1 as f64 / f;
            let e = bx - px_;
            errors.push(e);
            if e.abs() > worst.abs() { worst = e; }
            let d = DEBT_X.load(Ordering::Relaxed);
            if d.abs() > worst_debt.abs() { worst_debt = d; }
            let _ = by;
        }
        errors.sort_by(|a, b| a.partial_cmp(b).unwrap());
        std::println!("      {jumps} jumps, {pinned} ramps pinned at the DAC's limit");
        (errors[errors.len() / 2], worst, worst_debt)
    }

    /// THE TWO JUMP VARIANTS, SIDE BY SIDE, over the same geometry.
    #[test]
    fn jump_absorbs_or_not() {
        let _t = crate::emit::test_knobs();
        knobs_like_the_cartridge();
        T1_EXTRA_Q8.store(0, Ordering::Relaxed);
        let q = q_bits();
        let g = geometry();
        if g.is_empty() { return; }
        let (m0, p0, d0) = run_pass(&g, q, false);
        let (m1, p1, d1) = run_pass(&g, q, true);
        std::println!("  {} segments, q={q}", g.len());
        std::println!("  jump WITHOUT debt (what runs today): X median {m0:+.3}  worst {p0:+.3}  worst debt {d0:+}");
        std::println!("  jump WITH debt (the whole model):    X median {m1:+.3}  worst {p1:+.3}  worst debt {d1:+}");
    }

    #[test]
    fn debt_does_not_run_away() {
        let _t = crate::emit::test_knobs();
        MIN_T1.store(8, Ordering::Relaxed);
        MIN_T1_START.store(8, Ordering::Relaxed);
        DRAW_SCALE.store(160, Ordering::Relaxed);
        T1_TRANSPORT.store(160, Ordering::Relaxed);
        T1_EXTRA_Q8.store(0, Ordering::Relaxed);
        T1_JUMP.store(0, Ordering::Relaxed);
        DEBT_X.store(0, Ordering::Relaxed); DEBT_Y.store(0, Ordering::Relaxed);
        knobs_like_the_cartridge();
        let q = q_bits();
        let f = (1u32 << q) as f64;

        let g = geometry();
        if g.is_empty() { return; }                 // no header, nothing to test
        let (mut bx, mut by) = (0.0f64, 0.0f64);    // where the beam is, in units
        let (mut px_, mut py_) = (0.0f64, 0.0f64);  // where the drawer BELIEVES it is
        let mut errors: std::vec::Vec<f64> = std::vec::Vec::new();
        let mut worst_x = 0.0f64; let mut worst_debt = 0i32;
        let mut pinned = 0usize; let mut jumps = 0usize;
        for (i, (x0, y0, x1, y1)) in g.iter().enumerate() {
            /* THE JUMP ONLY WHEN NEEDED, AS THE CARTRIDGE DOES. `uvm2_draw_move_abs` emits
             * nothing if the beam is already where it should be, so INSIDE A CHAIN there is no
             * jump — and therefore nobody to absorb the debt. Calling it always, the replica
             * absorbed on every segment and gave -0.18 where the board gives +4.70. */
            let (jx4, jy4) = (*x0 - (px_*f).round() as i32,
                              *y0 - (py_*f).round() as i32);
            if jx4 != 0 || jy4 != 0 {
                let (vx, vy, t1) = ramp_params_jump_with_debt(
                    jx4, jy4, DAC_CAP, q);
                if vx.abs() == 127 || vy.abs() == 127 { pinned += 1; }
                bx += vx as f64 * t1 as f64 / 160.0; by += vy as f64 * t1 as f64 / 160.0;
                jumps += 1;
            }
            px_ = *x0 as f64/f; py_ = *y0 as f64/f;
            // the stroke
            let (vx, vy, t1) = ramp_params_chain_qn(x1 - x0, y1 - y0, q);
            if vx.abs() == 127 || vy.abs() == 127 { pinned += 1; }
            bx += vx as f64 * t1 as f64 / 160.0; by += vy as f64 * t1 as f64 / 160.0;
            px_ = *x1 as f64/f; py_ = *y1 as f64/f;
            let e = bx - px_;
            errors.push(e);
            if e.abs() > worst_x.abs() { worst_x = e; }
            let d = DEBT_X.load(Ordering::Relaxed);
            if d.abs() > worst_debt.abs() { worst_debt = d; }
            if i < 10 {
                std::println!("  HOST seg {i:3}: vy={vy:4} vx={vx:4} t1={t1:3}   beam X {bx:8.3}  error {e:+7.3}  debt {d:+5}");
            }
        }
        let median = { let mut v: std::vec::Vec<f64> = errors.clone(); v.sort_by(|a,b| a.partial_cmp(b).unwrap()); v[v.len()/2] };
        std::println!("  X median {median:+.2}  worst {worst_x:+.2}   worst debt {worst_debt:+}   jumps {jumps}  pinned {pinned}");
    }
}

/* The knobs are GLOBAL atomics and `cargo test` runs the tests in parallel inside the same
 * process: a test that touches one changes it under another mid-measurement. Any bench that
 * compares against the cartridge has to fix the WHOLE configuration, and run with
 * --test-threads=1. These values are read out of the image that runs on the console, not
 * chosen here. */
#[cfg(test)]
pub(crate) fn knobs_like_the_cartridge() {
    T1_EXTRA_Q8.store(0, Ordering::Relaxed);
    T1_LAG.store(0, Ordering::Relaxed);
    T1_LAG_START.store(0, Ordering::Relaxed);
    MIN_T1.store(8, Ordering::Relaxed);
    MIN_T1_START.store(8, Ordering::Relaxed);
    T1_JUMP.store(0, Ordering::Relaxed);
    T1_TRANSPORT.store(160, Ordering::Relaxed);
    DRAW_SCALE.store(160, Ordering::Relaxed);
    FIXED_RAMP.store(0, Ordering::Relaxed);
}

#[cfg(test)]
mod first_call {
    use super::*;
    /* The same delta probed on the cartridge (dx_q4=0, dy_q4=40, the first call after start).
     * If a different (vx,vy,t1) comes out here than on the board, the difference is a knob. */
    #[test]
    fn the_frames_first_delta() {
        let _k = crate::emit::test_knobs();
        knobs_like_the_cartridge();
        vx_chain_reset(); vx_debt_reset();
        let (vx, vy, t1) = ramp_params_chain_q4(0, 40);
        std::println!("HOST:     vy={vy}  t1={t1}  vx={vx}");
    }
}

#[cfg(test)]
mod direct_rate {
    use super::*;
    #[test]
    fn stroke_391() {
        let _k = crate::emit::test_knobs();
        knobs_like_the_cartridge();
        DEBT_ON.store(0, Ordering::Relaxed);
        vx_chain_reset(); vx_debt_reset();
        // the geometry asks for dx=1.3477 dy=-2.4023 units, i.e. 345 and -615 in Q8
        let (vx, vy, t1) = ramp_params_chain_qn(345, -615, 8);
        std::println!("  chain_qn(345,-615,q=8) -> vx={vx} vy={vy} t1={t1}   (theirs: 27, -48, 8)");
        let (ax, ay, at1) = ramp_params_q(345, -615, DAC_CAP, 8);
        std::println!("  ramp_params_q raw      -> vx={ax} vy={ay} t1={at1}");
        std::println!("  scale()={}  T1_EXTRA_Q8={}", scale(), T1_EXTRA_Q8.load(Ordering::Relaxed));
    }
}

#[cfg(test)]
mod ladder {
    use super::*;
    /// The rates for a given t1. Their transport #156 asks for 11.81 units in +X and they emit
    /// (105, 0) with t1 = 18.
    #[test]
    fn rates_for_a_given_t1() {
        let _k = crate::emit::test_knobs();
        knobs_like_the_cartridge();
        let dx = (11.8125 * 256.0) as i32;      // 11.81 units in Q8
        let mut vx = 0i32; let mut vy = 0i32;
        vx_ramp_params_with_t1(dx, 0, 256, 18, &mut vx, &mut vy);
        std::println!("  HOST: dx={dx} q=8 t1=18  ->  vx={vx} vy={vy}   (theirs: 105, 0)");
    }
}

#[cfg(test)]
mod jump_bias {
    use super::*;
    /// IS THE JUMP'S ERROR A BIAS THAT DEPENDS ON THE SIGN? `move_one` says so, measured on
    /// asterock BEFORE sub-unit precision: "negative delta X falls 1.3 units short, positive
    /// delta Y overshoots by 3.9, the other two directions are exact". If that survives in
    /// Q4 it is what makes reordering unsafe -- change the order, change the mix of signs,
    /// change the frame's accumulated error, and THAT flickers. Not a fractional residue
    /// another ramp can absorb: a bias, so it adds up linearly.
    ///
    /// dkong's configuration, not the crate's defaults.  `cargo test -- --nocapture
    /// --test-threads=1 jump_bias`
    #[test]
    fn the_jumps_sign_bias() {
        let _t = crate::emit::test_knobs();
        MIN_T1.store(8, Ordering::Relaxed);
        MIN_T1_START.store(31, Ordering::Relaxed);
        DRAW_SCALE.store(127, Ordering::Relaxed);
        T1_TRANSPORT.store(110, Ordering::Relaxed);
        T1_EXTRA_Q8.store(0, Ordering::Relaxed);
        T1_LAG.store(0, Ordering::Relaxed);
        T1_LAG_START.store(0, Ordering::Relaxed);
        CEILING_RULES.store(1, Ordering::Relaxed);
        FIXED_RAMP.store(0, Ordering::Relaxed);
        let s = DRAW_SCALE.load(Ordering::Relaxed) as i64;
        let q: i64 = 16;                       // Q4
        // Every jump dkong actually makes: 1..120 units, both signs, both axes.
        for (nom, sx, sy) in [("dx>0 dy=0", 1i64, 0i64), ("dx<0 dy=0", -1, 0),
                              ("dx=0 dy>0", 0, 1),      ("dx=0 dy<0", 0, -1),
                              ("dx>0 dy>0", 1, 1),      ("dx<0 dy<0", -1, -1),
                              ("dx<0 dy>0", -1, 1),     ("dx>0 dy<0", 1, -1)] {
            let (mut sx_e, mut sy_e, mut n) = (0.0f64, 0.0f64, 0i64);
            let (mut px, mut py) = (0.0f64, 0.0f64);
            // Only as far as ONE ramp reaches: 127 * min(DRAW_SCALE, T1_TRANSPORT)
            // / DRAW_SCALE. Past that `split` splits, and what would be measured here is
            // the clipping, not the bias.
            let reach = 127 * T1_TRANSPORT.load(Ordering::Relaxed).min(s as u32) as i64 / s;
            for u in 1..=reach {
                let dx = (sx * u * q) as i32;
                let dy = (sy * u * q) as i32;
                let (vx, vy, t1) = ramp_params_q(dx, dy, DAC_CAP, 4);
                // what the ramp actually covers, in the same internal units
                let rx = (vx as i64) * (t1 as i64) * q / s;
                let ry = (vy as i64) * (t1 as i64) * q / s;
                // SIGNED, PER AXIS. Taking the magnitude and guessing a sign says nothing
                // in the mixed quadrants, where the two axes can err in opposite ways.
                let ex = (rx - dx as i64) as f64 / q as f64;
                let ey = (ry - dy as i64) as f64 / q as f64;
                sx_e += ex; sy_e += ey; n += 1;
                if ex.abs() > px.abs() { px = ex; }
                if ey.abs() > py.abs() { py = ey; }
            }
            std::println!("  {nom}   X {:+.3} (worst {:+.2})   Y {:+.3} (worst {:+.2})   {n} jumps",
                          sx_e / n as f64, px, sy_e / n as f64, py);
        }
    }
}

#[cfg(test)]
mod i32_equivalence {
    extern crate std;
    use super::*;
    use core::sync::atomic::Ordering;
/// The FROZEN i64 reference copy of `ramp_params_q`, kept only so the test below can prove
/// the 32-bit rewrite is bit-for-bit equivalent. Its comments are deliberately stripped: the
/// explanations live once, in the real function.
fn ramp_params_q_old(dx: i32, dy: i32, vcap_in: u32, q: u32) -> (i8, i8, u16) {
    let f = 1i32 << q;
    let m = core::cmp::max(dx.abs(), dy.abs());
    let starting = STARTING.load(Ordering::Relaxed) != 0;
    if starting { START_HITS.fetch_add(1, Ordering::Relaxed); }
    let min_t1 = if starting { MIN_T1_START.load(Ordering::Relaxed) }
                 else        { MIN_T1.load(Ordering::Relaxed) } as i32;
    let mut vcap = vcap_in as i32;

    if m == 0 {
        return (0, 0, min_t1 as u16); // degenerate (dot); minimal ramp
    }

    let fixed = FIXED_RAMP.load(Ordering::Relaxed) as i32;
    if fixed > 0 {
        return ((dx / f).clamp(-128, 127) as i8, (dy / f).clamp(-128, 127) as i8, fixed as u16);
    }
    let s = scale();
    let t1_floor = (s * m / (127 * f)).clamp(min_t1, s); // dwell floor (∝ length)
    let vc = vcap.max(1);
    let t1_vcap = ((m * s + vc * f - 1) / (vc * f)).max(1);
    let d_menor = match (dx.abs(), dy.abs()) {
        (0, b) => b,                  // no X axis to lose
        (a, 0) => a,
        (a, b) => a.min(b),
    };
    let ceiling = (d_menor * s / f)
        .min(T1_TRANSPORT.load(Ordering::Relaxed) as i32)
        .max(min_t1);                 // in case the transport cap sits below the floor
    let t1 = if CEILING_RULES.load(Ordering::Relaxed) != 0 {
        t1_floor.max(t1_vcap).min(ceiling)
    } else {
        t1_floor.max(t1_vcap).min(ceiling.max(t1_vcap))
    };

    let error_de = |t: i32| -> i64 {
        if t <= 0 { return i64::MAX; }
        let v = {
            let den = (t as i64) * 256 + T1_EXTRA_Q8.load(Ordering::Relaxed) as i64;
            let n = (m as i64) * (s as i64) * 256 / (f as i64);
            let q = if n >= 0 { (n + den / 2) / den } else { (n - den / 2) / den };
            q.clamp(-128, 127)
        };
        ((v * t as i64) - (m as i64) * (s as i64) / (f as i64)).abs()
    };
    let t1 = if t1 < ceiling && error_de(t1 + 1) < error_de(t1) { t1 + 1 } else { t1 };
    let den = (t1 as i64) * 256 + T1_EXTRA_Q8.load(Ordering::Relaxed) as i64;
    let round_div = |num: i32| -> i32 {
        let n = num as i64 * 256;
        (if n >= 0 { (n + den / 2) / den } else { (n - den / 2) / den }) as i32
    };
    let vx = round_div(dx * s / f).clamp(-128, 127) as i8;
    let vy = round_div(dy * s / f).clamp(-128, 127) as i8;
    let t1 = t1 + if starting { T1_LAG_START.load(Ordering::Relaxed) }
                  else        { T1_LAG.load(Ordering::Relaxed) } as i32;
    (vx, vy, t1 as u16)
}

    #[test]
    fn ramp_params_q_i32_matches_i64() {
        let _k = crate::emit::test_knobs();
        let mut bad = 0;
        for (scale, extra, transp) in [(127u32, 640u32, 110u32), (146, 664, 110), (160, 0, 160), (110, 664, 110)] {
            DRAW_SCALE.store(scale, Ordering::Relaxed);
            T1_EXTRA_Q8.store(extra, Ordering::Relaxed);
            T1_TRANSPORT.store(transp, Ordering::Relaxed);
            let mut d = -2100i32;
            while d <= 2100 {
                let mut e = -2100i32;
                while e <= 2100 {
                    let a = ramp_params_q(d, e, DAC_CAP, 4);
                    let b = ramp_params_q_old(d, e, DAC_CAP, 4);
                    if a != b { bad += 1; if bad <= 5 { std::eprintln!("scale={scale} extra={extra} dx={d} dy={e}: new {:?} old {:?}", a, b); } }
                    e += 7;
                }
                d += 11;
            }
        }
        assert_eq!(bad, 0, "differing cases");
    }
/// The same frozen reference copy for `ramp_params_chain_qn`. See the note above.
fn ramp_params_chain_qn_old(dx_q4: i32, dy_q4: i32, q: u32) -> (i8, i8, u16) {
    let (rx, ry) = (DEBT_X.load(Ordering::Relaxed), DEBT_Y.load(Ordering::Relaxed));
    let f = 1i32 << q;
    let a_q4 = |r: i32| if r >= 0 { (r * f + 500) / 1000 } else { (r * f - 500) / 1000 };
    let usa = DEBT_ON.load(Ordering::Relaxed) != 0;
    let px = if dx_q4 == 0 || !usa { dx_q4 } else { dx_q4 + a_q4(rx) };
    let py = if dy_q4 == 0 || !usa { dy_q4 } else { dy_q4 + a_q4(ry) };

    let (vx, vy, t1) = ramp_params_q(px, py, DAC_CAP, q);
    const MICRO: i32 = 8;
    let n = core::cmp::max(1, (t1 as i32 + MICRO / 2) / MICRO);
    let runs = n * MICRO;
    let sc = scale();
    let den32 = runs * 256 + T1_EXTRA_Q8.load(Ordering::Relaxed) as i32;
    let den32 = if den32 > 0 { den32 } else { 1 };
    let fq = 1i32 << q;
    let directo = |p: i32| -> i8 {
        let n = (p / fq) * sc * 256 + ((p % fq) * sc * 256) / fq;
        let v = if n >= 0 { (n + den32 / 2) / den32 } else { (n - den32 / 2) / den32 };
        v.clamp(-128, 127) as i8
    };
    let (vx, vy, t1) = (directo(px), directo(py), runs as u16);
    STARTING.store(0, Ordering::Relaxed);

    if dx_q4 != 0 {
        DEBT_X.store((rx + dx_q4 * 1000 / f - travel_mil(vx as i32, t1)).clamp(-4000, 4000),
                      Ordering::Relaxed);
    }
    if dy_q4 != 0 {
        DEBT_Y.store((ry + dy_q4 * 1000 / f - travel_mil(vy as i32, t1)).clamp(-4000, 4000),
                      Ordering::Relaxed);
    }
    (vx, vy, t1)
}

    #[test]
    fn chain_qn_matches_before() {
        let _k = crate::emit::test_knobs();
        let mut bad = 0;
        for (scale, extra, transp) in [(127u32, 640u32, 110u32), (146, 664, 110), (160, 0, 160)] {
            DRAW_SCALE.store(scale, Ordering::Relaxed);
            T1_EXTRA_Q8.store(extra, Ordering::Relaxed);
            T1_TRANSPORT.store(transp, Ordering::Relaxed);
            let mut d = -1600i32;
            while d <= 1600 {
                let mut e = -1600i32;
                while e <= 1600 {
                    for debt in [(0i32, 0i32), (350, -1200), (-999, 40)] {
                        DEBT_X.store(debt.0, Ordering::Relaxed); DEBT_Y.store(debt.1, Ordering::Relaxed); STARTING.store(1, Ordering::Relaxed);
                        let a = ramp_params_chain_qn_old(d, e, 4);
                        let da = (DEBT_X.load(Ordering::Relaxed), DEBT_Y.load(Ordering::Relaxed));
                        DEBT_X.store(debt.0, Ordering::Relaxed); DEBT_Y.store(debt.1, Ordering::Relaxed); STARTING.store(1, Ordering::Relaxed);
                        let b = ramp_params_chain_qn(d, e, 4);
                        let db = (DEBT_X.load(Ordering::Relaxed), DEBT_Y.load(Ordering::Relaxed));
                        if a != b || da != db { bad += 1; if bad <= 5 { std::eprintln!("scale={scale} dx={d} dy={e} debt={debt:?}: new {:?}/{:?} old {:?}/{:?}", b, db, a, da); } }
                    }
                    e += 13;
                }
                d += 17;
            }
        }
        assert_eq!(bad, 0, "cases that differ along the chain");
    }
}

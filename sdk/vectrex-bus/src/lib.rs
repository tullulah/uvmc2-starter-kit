//! The PIO+DMA bus stream, SHARED by both cartridges.
//!
//! What lives here and why it cannot live in two places: the `.pio`, the two sentinel bits,
//! the repeated park with its `-1`, the batch ring and its DMA. All of that came from
//! oscilloscope measurements against the console — the phase of the address change inside the
//! E period, the `nop [14]`, the order of the two waits — and a hand copy of those constants
//! is a copy that drifts apart. See `bus_stream.pio`, which carries the whole derivation.
//!
//! ## The seam between boards
//!
//! It imposes nothing: it names what both boards already have. The caller assembles the bus
//! word with ITS pin map (`board::bus_word`) — which is where they genuinely differ, because
//! on the UVM2 the address is split and A14/A15 jump over PB6 — and this crate only shifts it
//! down to the `out` base and adds the sentinel. So the field layout belongs to the board and
//! the protocol belongs here.
//!
//! ## What is NOT here
//!
//! The ramp wait. We poll the VIA's T1 flag like the BIOS does; the UVM2's executor cannot
//! read in the middle of a list. Those are two algorithms, not two constants, and they are
//! deliberately left outside so they cannot hide.
#![no_std]

use core::sync::atomic::{AtomicU32, Ordering};

// ── THE REGISTERS, BY HAND AND WITH PROVENANCE ────────────────────────────────
//
// WHY NOT THE PAC, which would be correct and is what the rule "write bit positions from the
// PAC's typed fields, never from memory" asks for: `rp235x-pac` declares its dependency on
// `cortex-m` ONLY for the hardware floating-point target, and this crate also has to compile
// for `thumbv8m.main-none-eabi` — the .um2 image is linked `-mfloat-abi=softfp` and the
// linker compares `Tag_ABI_VFP_args` even though not one float crosses.
//
// So the numbers are written by hand, BUT NOT FROM MEMORY: every one is read out of
// pico-sdk/src/rp2350/hardware_regs/include/hardware/regs/{dma,pio,dreq,addressmap}.h, and
// the firmware — which does have the PAC — checks them against its typed fields with a
// `const assert`. If they diverge, it breaks the build instead of the console. This has
// bitten once already: the DMA's BUSY bit was not where it was assumed to be (it is 26, not
// the RP2040's 24) and it cost four debug-port hangs.
const DMA_BASE: usize = 0x5000_0000;      // addressmap.h
const DMA_CH_STRIDE: usize = 0x40;
const DMA_READ_ADDR: usize = 0x00;        // dma.h DMA_CH0_READ_ADDR_OFFSET
const DMA_WRITE_ADDR: usize = 0x04;       // dma.h DMA_CH0_WRITE_ADDR_OFFSET
const DMA_TRANS_COUNT: usize = 0x08;      // dma.h DMA_CH0_TRANS_COUNT_OFFSET
const DMA_CTRL_TRIG: usize = 0x0c;        // dma.h DMA_CH0_CTRL_TRIG_OFFSET

pub const DMA_EN: u32 = 1 << 0;           // dma.h ..._EN_LSB 0
pub const DMA_SIZE_WORD: u32 = 2 << 2;    // ..._DATA_SIZE_LSB 2, VALUE_SIZE_WORD 0x2
pub const DMA_INCR_READ: u32 = 1 << 4;    // ..._INCR_READ_LSB 4
pub const DMA_INCR_WRITE: u32 = 1 << 6;   // ..._INCR_WRITE_LSB 6
pub const DMA_CHAIN_LSB: u32 = 13;        // ..._CHAIN_TO_LSB 13
pub const DMA_TREQ_LSB: u32 = 17;         // ..._TREQ_SEL_LSB 17
pub const DMA_BUSY: u32 = 1 << 26;        // ..._BUSY_LSB 26  <-- 24 on the RP2040
pub const DREQ_PIO0_TX0: u32 = 0;         // dreq.h DREQ_PIO0_TX0

const PIO0_BASE: usize = 0x5020_0000;     // addressmap.h
const PIO_FSTAT: usize = 0x04;            // pio.h PIO_FSTAT_OFFSET
pub const PIO_TXF0: u32 = 0x5020_0010;    // pio.h PIO_TXF0_OFFSET 0x10
const PIO_FSTAT_TXEMPTY_LSB: u32 = 24;    // pio.h PIO_FSTAT_TXEMPTY_LSB
const SIO_GPIO_IN: usize = 0xD000_0004;   // sio.h SIO_GPIO_IN_OFFSET; E is bit 31

#[inline(always)]
unsafe fn r(a: usize) -> u32 { (a as *const u32).read_volatile() }
#[inline(always)]
unsafe fn w(a: usize, v: u32) { (a as *mut u32).write_volatile(v) }
#[inline(always)]
const fn ch(n: usize, off: usize) -> usize { DMA_BASE + n * DMA_CH_STRIDE + off }

/// ONE board's `out pins` field layout.
///
/// `out_base`/`out_count` have to match the `out pins, N` of the installed `.pio`: the
/// preamble uses the same base and count for `out pindirs`, so they cannot drift apart by
/// construction — but the width is still a number in the program.
#[derive(Clone, Copy)]
pub struct Layout {
    /// First GPIO of the `out`. The bus word is shifted down to this base.
    pub out_base: u32,
    /// How many pins the `out` drives. 25 on our own cartridge, 27 on the UVM2.
    pub out_count: u32,
    /// WHICH pins of that range we drive, already aligned to `out_base`.
    ///
    /// It is not always "all of them". On the UVM2, PB6 (GP22) is an INPUT and GP23 does not
    /// exist: if the preamble sets them to output, the `out pins` drives them and we fight
    /// the board. With their bit at 0 here, the pad does not drive and the `out pins` over
    /// them is harmless — the same mechanism the preamble already uses, with not a single
    /// branch in the hot path.
    pub out_dirs: u32,
    /// The park pattern, already aligned to `out_base` and WITHOUT the sentinel.
    ///
    /// It is what the SM drives when the FIFO runs dry: `pull noblock` falls back to X
    /// instead of blocking, and X holds this. It has to decode to NOTHING — a parked VIA
    /// address is re-latched on EVERY falling edge of E, and that is idempotent for a port
    /// register but NOT for T1_HI, which restarts the ramp at 1.5 MHz.
    pub park: u32,
}

impl Layout {
    /// A write word: the board's bus word, with the sentinel.
    ///
    /// `out` consumes from the LOW bit, so the only place the SM can look before driving is
    /// bit 0. Hence the shift: THE TWO MOVE TOGETHER or they drift apart, and the symptom
    /// would be a drawing that does not chain its strokes.
    ///
    ///     bit 0        1 = there is a write, 0 = a period of silence
    ///     bits 1..=N   the board's word
    #[inline(always)]
    pub const fn word(&self, bus: u32) -> u32 {
        ((bus >> self.out_base) << 1) | 1
    }

    /// One period of silence: it neither drives the bus nor parks it.
    #[inline(always)]
    pub const fn silence(&self) -> u32 {
        0
    }

    /// Park for `n` periods with ONE word.
    ///
    /// The `-1` is because `jmp y--` jumps WHILE Y is not zero and decrements afterwards, so
    /// with Y = n-1 the body runs n times. This and the `.pio` MOVE TOGETHER.
    ///
    /// It exists because the ramp wait used to push ~34 park words per vector — about 8400
    /// per frame — to produce something the SM already does for free.
    #[inline(always)]
    pub const fn repeat(n: u32) -> u32 {
        2 | ((n - 1) << 2)
    }
}

// ── The batch ring ────────────────────────────────────────────────────────────
//
// 64 words = 43 us of bus: enough to amortise the DMA's start-up, little enough that a
// half-filled batch does not hold the drawing up. Two batches so the CPU can fill one while
// the DMA drains the other.
pub const BATCH: usize = 64;

static mut BATCH_BUF: [[u32; BATCH]; 2] = [[0; BATCH]; 2];
static BATCH_IDX:  AtomicU32 = AtomicU32::new(0);
static BATCH_FILL: AtomicU32 = AtomicU32::new(0);

pub static BATCH_SENT:     AtomicU32 = AtomicU32::new(0);
pub static BATCH_WAITS:    AtomicU32 = AtomicU32::new(0);
/* Index of the delay loop's `nop` in the PIO's instruction memory, so a bench can sweep its
 * phase calibration. 0xFFFF_FFFF = not found. */
pub static VBUS_NOP_PARK: AtomicU32 = AtomicU32::new(0xFFFF_FFFF);
pub static RING_FULL_SEEN: AtomicU32 = AtomicU32::new(0);
pub static RING_OVERRUNS:  AtomicU32 = AtomicU32::new(0);
pub static STREAM_PUSHES:  AtomicU32 = AtomicU32::new(0);
pub static STREAM_STALLS:  AtomicU32 = AtomicU32::new(0);

/// The mask of pins that DRIVE, read right after setting it from the CPU and BEFORE the
/// .pio's preamble runs. It separates "my loop did nothing" from "the preamble overwrites it
/// afterwards": two opposite faults with the same black screen.
pub static DIRS_AFTER_SET: AtomicU32 = AtomicU32::new(0);

/// The pins driving right now, aligned to `out_base`. IO_BANK0's OETOPAD.
unsafe fn read_oe(l: &Layout) -> u32 {
    let mut m = 0u32;
    let mut i = 0u32;
    while i < l.out_count {
        let st = r(0x4002_8000 + 8 * (l.out_base + i) as usize);
        if st & (1 << 13) != 0 { m |= 1 << i; }
        i += 1;
    }
    m
}


/// Fires the accumulated batch. Waits for the previous DMA to finish: there is one channel.
#[inline(always)]
pub unsafe fn batch_flush() {
    let n = BATCH_FILL.load(Ordering::Relaxed);
    if n == 0 {
        return;
    }
    let idx = BATCH_IDX.load(Ordering::Relaxed) as usize & 1;

    let mut n_waits = 0u32;
    while r(ch(0, DMA_CTRL_TRIG)) & DMA_BUSY != 0 {
        n_waits += 1;
        if n_waits > 1_000_000 {
            STREAM_STALLS.fetch_add(1, Ordering::Relaxed);
            break;
        }
    }
    if n_waits != 0 {
        BATCH_WAITS.fetch_add(1, Ordering::Relaxed);
    }

    // The barrier BEFORE handing over the address: the DMA has to see the written words.
    dsb();

    let src = &raw const BATCH_BUF[idx] as *const u32 as u32;
    w(ch(0, DMA_READ_ADDR), src);
    w(ch(0, DMA_WRITE_ADDR), PIO_TXF0);
    w(ch(0, DMA_TRANS_COUNT), n);          // MODE en 31:28 = 0 = NORMAL
    w(ch(0, DMA_CTRL_TRIG),
        DMA_EN
      | DMA_SIZE_WORD
      | DMA_INCR_READ                       // walks the batch
                                            // INCR_WRITE at 0: always the same FIFO
      | (0 << DMA_CHAIN_LSB)                // to itself = no chaining
      | (DREQ_PIO0_TX0 << DMA_TREQ_LSB));   // paced by room in the PIO's FIFO

    BATCH_IDX.store(BATCH_IDX.load(Ordering::Relaxed) + 1, Ordering::Relaxed);
    BATCH_FILL.store(0, Ordering::Relaxed);
    BATCH_SENT.fetch_add(1, Ordering::Relaxed);
}

#[inline(always)]
fn dsb() {
    unsafe { core::arch::asm!("dsb", options(nostack, preserves_flags)) }
}

/// Queues a word. Fires the batch when it fills, or if the DMA is idle.
pub unsafe fn push(word: u32) {
    /* A repeated park: bit0 = 0 (no write) and bit1 = 1. The count is in bits 2..25 and is
     * N-1, exactly as in the .pio. The largest is remembered so it can be drained later. */
    if word & 1 == 0 && word & 2 != 0 {
        let n = (word >> 2) + 1;
        if n > PARK_MAX.load(Ordering::Relaxed) {
            PARK_MAX.store(n, Ordering::Relaxed);
        }
    }

    if IN_LIST.load(Ordering::Relaxed) != 0 {
        let f = LIST_FILL.load(Ordering::Relaxed) as usize;
        if f >= LIST_MAX {
            /* It does not fit: fire what has accumulated (blocking, as before) and carry on. */
            LIST_OVERFLOWS.fetch_add(1, Ordering::Relaxed);
            list_fire();
        }
        let f = LIST_FILL.load(Ordering::Relaxed) as usize;
        let idx = LIST_IDX.load(Ordering::Relaxed) as usize & 1;
        (&raw mut LIST_BUF[idx][f]).write_volatile(word);
        LIST_FILL.store(f as u32 + 1, Ordering::Relaxed);
        STREAM_PUSHES.fetch_add(1, Ordering::Relaxed);
        return;
    }
    let mut fill = BATCH_FILL.load(Ordering::Relaxed) as usize;
    if fill >= BATCH {
        RING_FULL_SEEN.fetch_add(1, Ordering::Relaxed);
        batch_flush();
        fill = BATCH_FILL.load(Ordering::Relaxed) as usize;
        if fill >= BATCH {
            fill = BATCH - 1;
            RING_OVERRUNS.fetch_add(1, Ordering::Relaxed);
        }
    }
    let idx = BATCH_IDX.load(Ordering::Relaxed) as usize & 1;
    (&raw mut BATCH_BUF[idx][fill]).write_volatile(word);
    BATCH_FILL.store(fill as u32 + 1, Ordering::Relaxed);
    STREAM_PUSHES.fetch_add(1, Ordering::Relaxed);

    if fill + 1 >= BATCH || r(ch(0, DMA_CTRL_TRIG)) & DMA_BUSY == 0 {
        batch_flush();
    }
}

// ── A FRAME'S LIST, ONE SINGLE DMA ──────────────────────────────────────────────────
//
// Batches of 64 words tie the CPU to the bus: `batch_flush` waits for the previous DMA, so
// whoever pushes a 20 ms list does not return until it has nearly been drawn. On the UVM2
// core 1 suffers that and it does not matter; on our own cartridge's BIOS core 0 suffered it,
// and core 0 also builds the frame, so building and drawing ran in series (measured: 40 fps
// in the menu with a 20 ms list). Here the whole list accumulates in RAM and is fired as ONE
// DMA that the PIO consumes at its own pace (DREQ): `list_fire` returns immediately and the
// CPU builds the next frame while the previous one goes out over the bus. Two buffers: the
// one being filled and the one the DMA is reading. What is pushed OUTSIDE a list (controllers,
// PSG) still goes through the small batches, immediately.
//
// HOW MUCH FITS IN THE LIST, PER IMAGE — and why it is a knob and not a constant.
//
// These are two buffers of 32-bit words, so 12288 is 98 KB of RAM. In a .um2 image (496 KB,
// the whole game inside) that is a fifth of it, and it competes with the OTHER list:
// uvm2_draw.c's `s_cmds`, which at 3 bytes per command and two buffers costs 6 bytes per
// command of capacity. The two hold THE SAME THING — a command is a bus word — and until now
// they were sized separately.
//
// THE TWO DO NOT OVERFLOW THE SAME WAY, AND THAT IS WHAT DECIDES WHICH GETS THE RAM:
//
//   - `s_cmds` full = GEOMETRY LOST. The rest of the frame is not emitted and it does not
//     look like an error, it looks like a drawing that stops half way.
//   - this list full = `list_fire` fires what has accumulated and carries on (LIST_OVERFLOWS
//     counts it). The overlap with the next frame is lost, nothing more, and on the UVM2 the
//     one who pays is core 1, which has nothing else to do.
//
// So a tight image does well to lower THIS one and spend it on the command cap. esb is the
// case: 1136 segments per frame = 12498 commands measured, and with both at 12288 it did not
// fit. The default stays as it was — our own cartridge's firmware, which links this as an rlib
// and has RAM to spare, does not change.
//
//     cargo build ... with VECTREX_LIST_MAX=8192 in the environment
//
// (the .um2 passes it from the game's Makefile; see UVM2_LIST_MAX in uvm2.mk)
const fn list_max() -> usize {
    match option_env!("VECTREX_LIST_MAX") {
        None => 12288,
        Some(s) => {
            let b = s.as_bytes();
            let mut i = 0usize;
            let mut n = 0usize;
            while i < b.len() {
                let c = b[i];
                assert!(c >= b'0' && c <= b'9', "VECTREX_LIST_MAX: digits only");
                n = n * 10 + (c - b'0') as usize;
                i += 1;
            }
            assert!(n >= 64, "VECTREX_LIST_MAX: below 64 the list does not beat the batch");
            n
        }
    }
}
pub const LIST_MAX: usize = list_max();
static mut LIST_BUF: [[u32; LIST_MAX]; 2] = [[0; LIST_MAX]; 2];
static LIST_IDX:  AtomicU32 = AtomicU32::new(0);
static LIST_FILL: AtomicU32 = AtomicU32::new(0);
static IN_LIST:   AtomicU32 = AtomicU32::new(0);
pub static LIST_OVERFLOWS: AtomicU32 = AtomicU32::new(0);
/// From here on `push` accumulates into the list instead of into the batches.
pub unsafe fn list_begin() {
    LIST_FILL.store(0, Ordering::Relaxed);
    IN_LIST.store(1, Ordering::Relaxed);
}
/// Stops accumulating; the list is now ready for `list_fire`.
pub unsafe fn list_end() { IN_LIST.store(0, Ordering::Relaxed); }
/// Waits for the bus to be done with EVERYTHING before it (the previous list's DMA, the FIFO
/// and the last park). Without `drain`'s short cap: here a whole frame is waited for.
pub unsafe fn list_wait() {
    let mut n = 0u32;
    while r(ch(0, DMA_CTRL_TRIG)) & DMA_BUSY != 0 { n += 1; if n > 50_000_000 { STREAM_STALLS.fetch_add(1, Ordering::Relaxed); break; } }
    n = 0;
    while r(PIO0_BASE + PIO_FSTAT) & (1 << PIO_FSTAT_TXEMPTY_LSB) == 0 { n += 1; if n > 50_000_000 { STREAM_STALLS.fetch_add(1, Ordering::Relaxed); break; } }
    drain();
}
/// Fires the accumulated list as one DMA. Waits for the channel if it is still busy.
pub unsafe fn list_fire() {
    let n = LIST_FILL.load(Ordering::Relaxed);
    if n == 0 { return; }
    batch_flush();                                   // the small pending stuff first
    let mut e = 0u32;
    while r(ch(0, DMA_CTRL_TRIG)) & DMA_BUSY != 0 { e += 1; if e > 50_000_000 { STREAM_STALLS.fetch_add(1, Ordering::Relaxed); break; } }
    dsb();
    let idx = LIST_IDX.load(Ordering::Relaxed) as usize & 1;
    w(ch(0, DMA_READ_ADDR), &raw const LIST_BUF[idx] as *const u32 as u32);
    w(ch(0, DMA_WRITE_ADDR), PIO_TXF0);
    w(ch(0, DMA_TRANS_COUNT), n);
    w(ch(0, DMA_CTRL_TRIG), DMA_EN | DMA_SIZE_WORD | DMA_INCR_READ | (0 << DMA_CHAIN_LSB) | (DREQ_PIO0_TX0 << DMA_TREQ_LSB));
    LIST_IDX.store(LIST_IDX.load(Ordering::Relaxed) + 1, Ordering::Relaxed);
    LIST_FILL.store(0, Ordering::Relaxed);
    BATCH_SENT.fetch_add(1, Ordering::Relaxed);
}
/// Flushes the partial batch.
///
/// DO NOT REMOVE FOR SPEED. Without it, a READ can overtake 63 queued writes (~42 us).
/// Reading a controller is *write the column into PORT_B and then read PORT_A*, and the
/// buttons are active LOW: a read that gets ahead = "pressed". The symptom was buttons 1 and
/// 2 held from boot.
pub unsafe fn flush() {
    if IN_LIST.load(Ordering::Relaxed) != 0 { return; }   // the list fires whole, in list_fire
    batch_flush();
}

/// The longest park pushed since the last `drain`, in E periods.
///
/// AN EMPTY FIFO DOES NOT MEAN A FREE BUS, and that confusion has already cost a fault: one
/// single repeated-park word keeps the SM occupying N E periods long after the FIFO has run
/// dry. Whoever reads the bus right then reads it busy.
static PARK_MAX: AtomicU32 = AtomicU32::new(0);

/// Waits until the bus has no pending work. REALLY.
///
/// Three stages, and all three are needed:
///   1. flush the partial batch (otherwise a read overtakes up to 63 writes);
///   2. wait for the FIFO to empty;
///   3. wait out the E periods the LAST word may still be consuming.
///
/// The other cartridge's firmware does the third with a deadline in microseconds
/// (STREAM_BUS_UNTIL). Here E periods are counted directly, which is the unit the machine
/// counts in and depends on no clock: E comes in on SIO's bit 31 on BOTH boards, so this
/// carries over without touching a number.
///
/// It waits for the largest park seen since the last drain, plus two of margin: when the FIFO
/// empties there is at most one word in flight.
pub unsafe fn drain() {
    flush();

    let mut n = 0u32;
    while r(PIO0_BASE + PIO_FSTAT) & (1 << PIO_FSTAT_TXEMPTY_LSB) == 0 {
        n += 1;
        if n > 100_000 {
            STREAM_STALLS.fetch_add(1, Ordering::Relaxed);
            break;
        }
    }

    let periods = PARK_MAX.swap(0, Ordering::Relaxed) + 2;
    let mut i = 0u32;
    while i < periods {
        let mut g = 0u32;
        while r(SIO_GPIO_IN) & (1 << 31) != 0 { g += 1; if g > 100_000 { return; } }
        let mut g = 0u32;
        while r(SIO_GPIO_IN) & (1 << 31) == 0 { g += 1; if g > 100_000 { return; } }
        i += 1;
    }
}

// ── Installation: the program, the SM and the pins ───────────────────────────

const RESETS_BASE: usize = 0x4002_0000;    // addressmap.h
const RESETS_RESET: usize = 0x0000;
const RESETS_DONE: usize = 0x0008;
const RESET_PIO0: u32 = 1 << 11;           // resets.h RESETS_RESET_PIO0_LSB 11
const RESET_DMA: u32 = 1 << 2;             // resets.h RESETS_RESET_DMA_LSB 2
const ATOMIC_CLR: usize = 0x3000;          // the RP2350 bus's atomic aliases

const PIO_CTRL: usize = 0x00;              // pio.h PIO_CTRL_OFFSET
const PIO_INSTR_MEM0: usize = 0x48;        // pio.h PIO_INSTR_MEM0_OFFSET
const PIO_SM0_CLKDIV: usize = 0xc8;
const PIO_SM0_EXECCTRL: usize = 0xcc;
const PIO_SM0_SHIFTCTRL: usize = 0xd0;
const PIO_SM0_INSTR: usize = 0xd8;
const PIO_SM0_PINCTRL: usize = 0xdc;
const EXECCTRL_WRAP_BOTTOM_LSB: u32 = 7;   // pio.h ..._WRAP_BOTTOM_LSB 7
const EXECCTRL_WRAP_TOP_LSB: u32 = 12;     // pio.h ..._WRAP_TOP_LSB 12
const SHIFTCTRL_OUT_SHIFTDIR: u32 = 1 << 19;
const PINCTRL_OUT_COUNT_LSB: u32 = 20;     // pio.h ..._OUT_COUNT_LSB 20
const PINCTRL_SET_BASE_LSB: u32 = 5;       // pio.h ..._SET_BASE_LSB 5
const PINCTRL_SET_COUNT_LSB: u32 = 26;     // pio.h ..._SET_COUNT_LSB 26

const IO_BANK0_BASE: usize = 0x4002_8000;
const PADS_BANK0_BASE: usize = 0x4003_8000;
const FUNCSEL_PIO0: u32 = 6;               // io_bank0.h ..._FUNCSEL_VALUE_PIO0_n

/// THE `out` WIDTH LIVES IN THE INSTRUCTION, which is why there is only ONE `.pio`.
///
/// `out` encodes the bit count in bits 0..4 (0 means 32). Our cartridge drives 25 pins and
/// the UVM2 27, and that is the ONLY difference between the two programs — the rest (the
/// phase, the order of the two waits, the `nop [14]`, the two sentinels, the repeated park)
/// is identical and calibrated against the console.
///
/// Duplicating the file to change one number would guarantee the calibrations drift apart. It
/// is assembled once and patched at install time: `out pindirs, N` and `out pins, N` are the
/// only instructions carrying the count, and they are recognised by their opcode.
///
///     15..13 = 0b011  -> OUT
///      7..5          -> destination (0 = PINS, 4 = PINDIRS)
///      4..0          -> bit count
fn patch_width(instr: u16, width: u32) -> u16 {
    const OUT: u16 = 0b011 << 13;
    if (instr & (0b111 << 13)) != OUT {
        return instr;
    }
    let dest = (instr >> 5) & 0b111;
    if dest != 0 && dest != 4 {
        return instr;   // out y,1 / out null,N: they drive no pins, leave them alone
    }
    (instr & !0x1F) | ((width & 0x1F) as u16)
}

/// Leaves the bus ready for the stream: pins to the PIO, program loaded, SM running.
///
/// `program` is the already-assembled `.pio`; `wrap`/`wrap_target` are its relative indices.
/// The caller gets them from `pio_proc::pio_file!("src/bus_stream.pio")`, which stays on the
/// side that has the proc-macro so it is not dragged into the image.
pub unsafe fn install(l: &Layout, program: &[u16], wrap_target: u8, wrap: u8) {
    // PIO0 AND DMA OUT OF RESET, explicitly.
    //
    // Nothing assumed: `split(&mut RESETS)` does this for the PIO and NOBODY did it for the
    // DMA. Two completely different DMA designs failed identically, which was the clue, and
    // it cost four debug-port hangs to find.
    w(RESETS_BASE + ATOMIC_CLR + RESETS_RESET, RESET_PIO0 | RESET_DMA);
    while r(RESETS_BASE + RESETS_DONE) & (RESET_PIO0 | RESET_DMA) != (RESET_PIO0 | RESET_DMA) {}

    // SM stopped while its configuration is touched.
    w(PIO0_BASE + PIO_CTRL, 0);

    // The program, at origin 0 and with this board's width.
    //
    // AND WHILE WE ARE HERE, WHERE THE DELAY LOOP'S `nop` IS. Its delay is calibrated against
    // the E phase and is a candidate to explain why a cycle of gap costs 1.0 us instead of
    // the E period's 0.667. Sweeping it requires knowing its index, and what has the program
    // in front of it is this — looking for it from outside by reading the instruction memory
    // does not work: the emulator models it as write-only and returns zero.
    //
    // `nop` is `mov y, y` = 0xA042, with the delay in bits 12:8. The FIRST is on the write
    // path and the SECOND is in `park`, which is the one that executes the gaps.
    let mut seen = 0u32;
    let mut i = 0usize;
    while i < program.len() && i < 32 {
        w(PIO0_BASE + PIO_INSTR_MEM0 + 4 * i, patch_width(program[i], l.out_count) as u32);
        if program[i] & 0xE0FF == 0xA042 {
            seen += 1;
            if seen == 2 { VBUS_NOP_PARK.store(i as u32, Ordering::Relaxed); }
        }
        i += 1;
    }

    // 1 PIO cycle = 1 system cycle. NOTHING here is counted in cycles except the phase
    // calibration's `nop [14]`, so the divider would only change that — and that calibration
    // was measured at this divider.
    w(PIO0_BASE + PIO_SM0_CLKDIV, 1 << 16);

    w(PIO0_BASE + PIO_SM0_EXECCTRL,
        ((wrap_target as u32) << EXECCTRL_WRAP_BOTTOM_LSB)
      | ((wrap as u32) << EXECCTRL_WRAP_TOP_LSB));

    // AUTOPULL OFF: the explicit `pull noblock` IS the parking mechanism. With autopull, an
    // empty FIFO blocks the SM with the pins driving the LAST word — a VIA address that gets
    // re-latched on every falling edge of E, at 1.5 MHz.
    w(PIO0_BASE + PIO_SM0_SHIFTCTRL, SHIFTCTRL_OUT_SHIFTDIR);

    w(PIO0_BASE + PIO_SM0_PINCTRL,
        l.out_base | ((l.out_count & 0x1F) << PINCTRL_OUT_COUNT_LSB));

    // ORDER: START, SET DIRECTIONS, AND ONLY THEN HAND OVER THE PINS.
    //
    // The other way round — which is how it was — there is a window in which FUNCSEL is
    // already PIO and the state machine has no pin directions yet: the bus's 27 pins are
    // INPUTS and the whole bus floats. That gives neither a black screen nor noise; it leaves
    // a state NOBODY sets again, and the symptom shows up far away — the controller reading
    // 0x89 instead of 0xFF, stable, with the stream merely INSTALLED and all the drawing on
    // SIO.
    //
    // In this order the state machine already drives what it should at the instant the pins
    // are handed to it, and there is no window to close.
    //
    // Start it. It blocks on its first `pull block`, which is harmless — and it is WHILE
    // ENABLED that SM_INSTR executes what is written to it. With the machine stopped, the 27
    // `set pindirs` writes did nothing: measured on the console, EXACTLY the same 0x06200201
    // came out as without them, by two different routes.
    w(PIO0_BASE + PIO_SM0_INSTR, 0);        // jmp 0
    w(PIO0_BASE + PIO_CTRL, 1);             // SM_ENABLE bit 0

    // THE PIN DIRECTIONS, FROM THE CPU AND WITH THE MACHINE ALREADY ENABLED.
    //
    // The .pio's preamble sets them with `out pindirs` reading a word off the FIFO, and that
    // is what the other cartridge's firmware does. Here it DID NOT WORK: measured on the
    // console, after installing only 5 of the 27 pins drove (0x06200201 instead of
    // 0x073FFFFF), and already at install time — so it was not that the machine restarted
    // later, it is that the mask never got applied in full.
    //
    // This form is the pico-sdk's own (`pio_sm_set_pindirs_with_mask`): pin by pin, moving
    // the SET group and executing `set pindirs, 0/1` through SM_INSTR. It goes through no
    // FIFO, it does not depend on the program having reached its second instruction, and it
    // can be VERIFIED by reading IO_BANK0 right afterwards — which is what
    // uvm2_stream_dirs_after_install does.
    //
    // The preamble stays as it is and consumes its word: it applies the same mask again, so
    // it is idempotent. The .pio is not touched — the board that works shares it.
    {
        let saved = r(PIO0_BASE + PIO_SM0_PINCTRL);
        let mut i = 0u32;
        while i < l.out_count {
            let gpio = l.out_base + i;
            let dir = (l.out_dirs >> i) & 1;
            w(PIO0_BASE + PIO_SM0_PINCTRL,
                (1u32 << PINCTRL_SET_COUNT_LSB) | (gpio << PINCTRL_SET_BASE_LSB));
            // SET: opcode 111, destination PINDIRS = 4, data in bits 0..4.
            w(PIO0_BASE + PIO_SM0_INSTR, 0xE000 | (4u32 << 5) | dir);
            i += 1;
        }
        w(PIO0_BASE + PIO_SM0_PINCTRL, saved);
    }


    // THE PADS, BEFORE THE FUNCTION AND WITHOUT FORGETTING THE ISOLATION.
    //
    // On the RP2350 a pad powers up at 0x0116: ISO=1, IE=0. An isolated pad DRIVES NOTHING
    // however correct the FUNCSEL is, and it gives no error, no counter and no symptom of its
    // own — only a black screen, which is also the symptom of six other things. It already
    // cost the stream's bring-up once, over GP19 (A15).
    let mut p = 0u32;
    while p < l.out_count {
        let gpio = (l.out_base + p) as usize;
        if l.out_dirs & (1 << p) != 0 {
            let pad = PADS_BANK0_BASE + 4 + 4 * gpio;
            let v = r(pad);
            w(pad, (v & !((1 << 8) | (1 << 7))) | (1 << 6));   // ISO=0, OD=0, IE=1
            w(IO_BANK0_BASE + 8 * gpio + 4, FUNCSEL_PIO0);
        }
        p += 1;
    }

    DIRS_AFTER_SET.store(read_oe(l), Ordering::Relaxed);




    // 1st word: the pin DIRECTIONS. The SM itself sets them with `out pindirs`, using by
    // construction the same base and count as the `out pins` below — so they cannot drift
    // apart. Leaving them to the HAL left the 25 pins as INPUTS: the SM ran, drained the FIFO
    // and executed `out pins`, and the pad drove nothing.
    push_raw(l.out_dirs);
    // 2nd word: the PARK pattern, which goes into X. WITH ITS SENTINEL: the park branch does
    // `mov osr, x` and then `out null, 1` to throw it away before driving, exactly as
    // `pull noblock` does when it falls back to X on its own account. Without the sentinel
    // that `out null` eats the address's low bit and the bus parks somewhere else.
    push_raw(l.word(l.park));
}

/// Writes straight to the FIFO, with no batch. Preamble only: the DMA is not running yet.
unsafe fn push_raw(word: u32) {
    let mut n = 0u32;
    while r(PIO0_BASE + PIO_FSTAT) & (1 << 16) != 0 {   // TXFULL de la SM0
        n += 1;
        if n > 100_000 { STREAM_STALLS.fetch_add(1, Ordering::Relaxed); return; }
    }
    w(PIO_TXF0 as usize, word);
}

/// The assembled program, its `wrap` indices, and its length.
///
/// It is assembled HERE and not in each consumer: if every board ran the assembler on its own
/// account, one of them pointing at an old copy of the `.pio` would be enough for the two
/// calibrations to drift apart silently. The proc-macro runs on the host; none of it goes
/// into the image.
/* MAKING CARGO NOTICE THAT THE .pio CHANGED.
 *
 * `pio_proc::pio_file!` is a procedural macro: it reads the file at compile time, but it does
 * NOT declare it as a dependency, and there is no `build.rs` with `rerun-if-changed` here. So
 * editing `bus_stream.pio` does NOT invalidate the rlib: cargo reuses the cached one and keeps
 * assembling the OLD program, without saying anything.
 *
 * This happened on 2026-09-09 and cost a whole false conclusion: I fixed the period the park
 * loop was burning, took it as verified with an A/B of the VIA writes in the emulator — 41923
 * identical — and both branches of the A/B were THE SAME BINARY. On the console it was still
 * measuring the same 10% as always.
 *
 * `include_bytes!` IS recorded in the dep-info rustc writes, so cargo rebuilds. It costs
 * nothing: the `const _` is discarded. */
const _: &[u8] = include_bytes!("bus_stream.pio");

pub fn program() -> (&'static [u16], u8, u8) {
    static mut CODE: [u16; 32] = [0; 32];
    static mut LENGTH: usize = 0;
    static mut WT: u8 = 0;
    static mut WR: u8 = 0;
    unsafe {
        if LENGTH == 0 {
            let p = pio_proc::pio_file!("src/bus_stream.pio");
            let c = p.program.code;
            LENGTH = c.len();
            let mut i = 0;
            while i < c.len() { CODE[i] = c[i]; i += 1; }
            WT = p.program.wrap.target;
            WR = p.program.wrap.source;
        }
        (&CODE[..LENGTH], WT, WR)
    }
}

/// Leaves the ring as if freshly started. Called by whoever has just taken the DMA out of
/// reset: the channel keeps nothing, but the batch's index and fill do.
pub fn reset() {
    BATCH_IDX.store(0, Ordering::Relaxed);
    BATCH_FILL.store(0, Ordering::Relaxed);
}

/// Stop and start the state machine.
///
/// WHY IT HAS TO BE STOPPED, and why taking its pins away is not enough: with the bus handed
/// to SIO the SM drives nothing, but it IS STILL ALIVE — consuming words, synchronising
/// against E and parking. Leaving it running while someone else talks over the same bus means
/// two machines with an opinion about the same cycle, and the symptom is not noise: it is a
/// wrong and REPEATABLE byte, because the PSG's strobe sequence comes out different but the
/// same every time.
pub unsafe fn sm_stop() {
    w(PIO0_BASE + PIO_CTRL, 0);
}

pub unsafe fn sm_start() {
    w(PIO0_BASE + PIO_CTRL, 1);
}

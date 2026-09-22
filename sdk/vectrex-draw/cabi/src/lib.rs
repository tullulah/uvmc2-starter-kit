//! C wrapper around `vectrex-draw`, for the Ultimate Vectrex Multicart 2 image.
//!
//! It implements nothing: it re-exports the model's `extern "C"` symbols and provides the
//! panic handler a `staticlib` demands. All the logic is in the crate next door, which is the
//! one the firmware links too — which is the whole point of this.

#![no_std]

// Without this the linker discards the whole crate: nothing in this file references it, and
// a dependency's `#[no_mangle]` symbols do not pull themselves in.
pub use vectrex_draw::emit::{vx_draw_line_patterned_seq, vx_draw_line_seq, vx_moveto_seq};
pub use vectrex_draw::ramp::{vx_ramp_params, vx_ramp_params_chain, vx_chain_reset};
pub use vectrex_draw::{vx_probe, vx_probe_div};

/// A panic inside a bare-metal image has nowhere to go. It stops dead: it is visible (the
/// screen freezes) and it corrupts nothing.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}


// ── THE BUS STREAM, IN THE SAME WRAPPER ──────────────────────────────────────
//
// WHY HERE AND NOT IN ITS OWN .a: a `staticlib` REQUIRES a `#[panic_handler]` AT COMPILE
// TIME — it cannot be deferred to the link — and two handlers in the same link is a hard
// error. Two separate wrappers cannot coexist in one image. So there is ONE wrapper and two
// crates behind it, which is where the logic lives.
//
// Behind the `bus` feature: with the knob off not one byte of the stream goes in, which
// matters in an image with 3.8 KB free like dkong's.
#[cfg(feature = "bus")]
mod bus_c {
    use vectrex_bus as bus;
    
    /// THIS board's field layout. The C caller supplies it, because the pin map is its own:
    /// on the UVM2 the address is split (A14/A15 jump over PB6) and `uvm2_bus.h` does that
    /// arithmetic, not this crate.
    ///
    /// RAW POINTER, not `&`. Rust 2024 warns that a shared reference to a `static mut` is
    /// undefined behaviour if anyone mutates it while the reference lives — and
    /// `vbus_install` mutates it. The warning was not cosmetic: it describes exactly what
    /// this code does.
    ///
    /// It is read with `&raw const` / a copy by value, which is what the compiler itself
    /// suggests. The layout is written ONCE at startup and only read afterwards, so copying
    /// it costs nothing and removes the live reference.
    static mut LAYOUT: bus::Layout = bus::Layout {
        out_base: 0,
        out_count: 0,
        out_dirs: 0,
        park: 0,
    };
    
    /// Pins to the PIO, program loaded, SM running and preamble pushed.
    ///
    /// `out_dirs` is aligned to `out_base` and **does not have to be all ones**: on the UVM2,
    /// PB6 (GP22) is an input and GP23 does not exist, so their bits are 0 and the `out pins`
    /// over them is harmless.
    #[no_mangle]
    pub unsafe extern "C" fn vbus_install(out_base: u32, out_count: u32, out_dirs: u32, park: u32) {
        LAYOUT = bus::Layout { out_base, out_count, out_dirs, park };
        let (code, wt, wr) = bus::program();
        bus::install(&*(&raw const LAYOUT), code, wt, wr);
    }
    
    /// One write: the bus word ALREADY assembled with the board's layout.
    #[no_mangle]
    pub unsafe extern "C" fn vbus_word(bus_word: u32) -> u32 {
        let l = *(&raw const LAYOUT);   /* a copy: it leaves no live reference */
        l.word(bus_word)
    }
    
    /// Park for `n` E periods with a single word.
    #[no_mangle]
    pub extern "C" fn vbus_repeat(n: u32) -> u32 {
        bus::Layout::repeat(n)
    }
    
    /// One period of silence: it neither drives nor parks.
    #[no_mangle]
    pub unsafe extern "C" fn vbus_silence() -> u32 {
        let l = *(&raw const LAYOUT);
        l.silence()
    }
    
    #[no_mangle]
    pub unsafe extern "C" fn vbus_push(word: u32) {
        bus::push(word)
    }
    
    /// Flush the partial batch. MANDATORY before a bus READ: without it, a read can overtake
    /// up to 63 queued writes (~42 us). Reading a controller is writing the column and then
    /// reading, and the buttons are active low: getting ahead = "pressed".
    #[no_mangle]
    pub unsafe extern "C" fn vbus_flush() {
        bus::flush()
    }
    
    #[no_mangle]
    pub unsafe extern "C" fn vbus_drain() {
        bus::drain()
    }

    #[no_mangle]
    pub unsafe extern "C" fn vbus_sm_stop() {
        bus::sm_stop()
    }

    #[no_mangle]
    pub unsafe extern "C" fn vbus_sm_start() {
        bus::sm_start()
    }
    
    /// The counters, by index, so they can be read over SWD from C without exporting Rust
    /// symbols one by one. `uvm2_bus.h` fixes the order; THE TWO MOVE TOGETHER.
    ///
    /// `vbus_nop_park` is where the delay loop's `nop` sits in the PIO's instruction memory.
    /// `install` publishes it; it is there so a bench can sweep its phase calibration.
    #[no_mangle]
    pub unsafe extern "C" fn vbus_list_begin() { bus::list_begin() }
    #[no_mangle]
    pub unsafe extern "C" fn vbus_list_end() { bus::list_end() }
    #[no_mangle]
    pub unsafe extern "C" fn vbus_list_wait() { bus::list_wait() }
    #[no_mangle]
    pub unsafe extern "C" fn vbus_list_fire() { bus::list_fire() }
    #[no_mangle]
    pub extern "C" fn vbus_nop_park() -> u32 {
        bus::VBUS_NOP_PARK.load(core::sync::atomic::Ordering::Relaxed)
    }

    #[no_mangle]
    pub extern "C" fn vbus_stat(idx: u32) -> u32 {
        use core::sync::atomic::Ordering::Relaxed;
        match idx {
            0 => bus::STREAM_PUSHES.load(Relaxed),
            1 => bus::STREAM_STALLS.load(Relaxed),
            2 => bus::BATCH_SENT.load(Relaxed),
            3 => bus::BATCH_WAITS.load(Relaxed),
            4 => bus::RING_FULL_SEEN.load(Relaxed),
            5 => bus::RING_OVERRUNS.load(Relaxed),
            _ => 0,
        }
    }
}

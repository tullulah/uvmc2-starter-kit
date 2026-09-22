//! The drawing layer shared between our own cartridge and the multicart.
//!
//! STEP 1 PASSED ON HARDWARE (2026-08-18). Read over SWD with SnowBros running on the
//! multicart: `uvm2_vx_probe = 0x56580001` and `uvm2_vx_probe_div = 142`. So the staticlib
//! does not merely link — it IS LOADED AND EXECUTED inside that image, and the integer
//! intrinsics resolve. The game looks the same and its firmware was not touched.
//!
//! STEP 1 OF 4, and deliberately does nothing useful yet. What is being tested here is not
//! the drawing: it is that a Rust `staticlib` LINKS AND RUNS inside the UVM2 image, which is
//! the unknown that could kill the whole plan (panic symbols, `compiler_builtins`, and above
//! all the ABI attributes). If this does not link, how good the rest of the design is does
//! not matter.
//!
//! THE ABI, MEASURED and not assumed:
//!
//! ```text
//!     firmware      thumbv8m.main-none-eabihf   -> floats in VFP registers
//!     UVM2 image    -mcpu=cortex-m33 -mfloat-abi=softfp
//!
//! ```
//! The linker compares `Tag_ABI_VFP_args` and complains even though not one float crosses,
//! so this crate is compiled for `thumbv8m.main-none-eabi` (software floating point).
//!
//! FROM WHICH COMES THE MOST IMPORTANT RULE IN THIS FILE: **the public API is pure integer,
//! not one `f32` or `f64`, ever**. It is not a style preference — it is what lets the same
//! `.a` serve both boards. Our beam model is already integer arithmetic by design, so keeping
//! it costs nothing.

#![no_std]

// `std` ONLY for the host tests. The crate is still no_std in the real build; this is what
// lets the sequence be verified on the Mac instead of on the console — and it is needed,
// because on the multicart board every SWD read resets it.
#[cfg(test)]
extern crate std;

pub mod emit;
pub mod ramp;

/// Returns a recognisable constant. It is step 1's probe: if the multicart firmware loads
/// the image and this can be read over SWD, the Rust -> .a -> CMake -> .um2 chain works and
/// real code can start moving.
///
/// `extern "C"` and `#[no_mangle]` because the caller is C.
#[no_mangle]
pub extern "C" fn vx_probe() -> u32 {
    0x5658_0001 // "VX" + version
}

/// 32-bit arithmetic crossing the boundary, to check that no unresolved intrinsics appear.
/// A 64-bit division asked for `__aeabi_ldivmod`, which the VPy path's link does not carry —
/// that happened the same day. Better to find out here than with the whole model on top.
#[no_mangle]
pub extern "C" fn vx_probe_div(num: i32, den: i32) -> i32 {
    if den == 0 { return 0; }
    num / den
}

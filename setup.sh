#!/usr/bin/env bash
# One-time setup for the UVMC2 starter kit.
#
# Everything the kit itself needs is already in this folder. What this script
# does is fetch the one dependency that is too big to ship (the Raspberry Pi
# pico-sdk) and check that the host tools are installed AT A USABLE VERSION.
#
# It checks versions and not just presence on purpose: a too-old cmake or cargo
# fails deep inside the build with a message about something else entirely.
#
#   ./setup.sh          fetch and check
#   ./setup.sh --check  only check, fetch nothing
set -euo pipefail

KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Pinned, not "latest": the SDK's startup contract (the IMAGE_DEF block, the
# no_flash binary type, the weak isr_svcall that uvm2_svc_handler overrides)
# is what the multicart loader validates, and a different pico-sdk can move it.
PICO_SDK_VERSION="2.2.0"
PICO_SDK_DIR="$KIT/third_party/pico-sdk"
RUST_TARGET="thumbv8m.main-none-eabi"

# Minimums, each with a reason — not round numbers:
#   cmake  3.13   what pico-sdk 2.2.0 declares
#   python 3.6    f-strings in the packager and the sound tools
#   cargo  1.78   the Cargo.lock files are lockfile format v4
MIN_CMAKE="3.13"
MIN_PYTHON="3.6"
MIN_CARGO="1.78"

ok()   { printf '  \033[32mok\033[0m    %s\n' "$*"; }
warn() { printf '  \033[33mwarn\033[0m  %s\n' "$*"; }
die()  { printf '  \033[31mmissing\033[0m %s\n' "$*"; MISSING=1; }
old()  { printf '  \033[31mtoo old\033[0m %s\n' "$*"; MISSING=1; }

# `sort -V` is the portable way to compare dotted versions in a shell.
vge()  { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }

MISSING=0
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

echo "== host tools =="

if command -v cmake >/dev/null; then
    V="$(cmake --version | head -1 | awk '{print $3}')"
    if vge "$V" "$MIN_CMAKE"; then ok "cmake        $V  (>= $MIN_CMAKE)"
    else old "cmake $V — the pico-sdk needs >= $MIN_CMAKE"; fi
else die "cmake (brew install cmake)"; fi

if command -v python3 >/dev/null; then
    V="$(python3 -c 'import sys;print("%d.%d.%d"%sys.version_info[:3])')"
    if vge "$V" "$MIN_PYTHON"; then ok "python3      $V  (>= $MIN_PYTHON)"
    else old "python3 $V — need >= $MIN_PYTHON"; fi
else die "python3"; fi

# Rust is NOT optional: uvm2_pico.cmake links libvectrex_draw_cabi.a in every
# build and stops with FATAL_ERROR without cargo. UVM2_PIO_STREAM=0 only drops
# the crate's `bus` feature; the beam model itself is Rust.
if command -v cargo >/dev/null; then
    V="$(cargo --version | awk '{print $2}')"
    if vge "$V" "$MIN_CARGO"; then ok "cargo        $V  (>= $MIN_CARGO)"
    else old "cargo $V — the Cargo.lock files are format v4, which needs >= $MIN_CARGO"; fi
else die "cargo — the drawing layer is Rust (https://rustup.rs)"; fi

command -v git >/dev/null && ok "git" || die "git"

# The Arm GNU Toolchain, not Homebrew's arm-none-eabi-gcc: the pico-sdk link
# pulls newlib and Homebrew's build ships no nosys.specs.
ARM_TOOLCHAIN="${UVM2_ARM_TOOLCHAIN:-$(ls -d /Applications/ArmGNUToolchain/*/arm-none-eabi 2>/dev/null | tail -1 || true)}"
[ -n "$ARM_TOOLCHAIN" ] || ARM_TOOLCHAIN="$(ls -d /opt/arm-gnu-toolchain*/arm-none-eabi 2>/dev/null | tail -1 || true)"
if [ -n "$ARM_TOOLCHAIN" ] && [ -x "$ARM_TOOLCHAIN/bin/arm-none-eabi-gcc" ]; then
    V="$("$ARM_TOOLCHAIN/bin/arm-none-eabi-gcc" -dumpversion 2>/dev/null || echo '?')"
    ok "arm gcc      $V  $ARM_TOOLCHAIN"
    # nosys.specs is the thing that actually breaks, so check for it by name
    # rather than trusting the version.
    if find "$ARM_TOOLCHAIN" -name nosys.specs -print -quit 2>/dev/null | grep -q .; then
        ok "nosys.specs  present"
    else
        die "nosys.specs not found under $ARM_TOOLCHAIN — this is the Homebrew build, not the Arm one"
    fi
else
    warn "Arm GNU Toolchain not found under /Applications/ArmGNUToolchain or /opt."
    warn "Install it from developer.arm.com (arm-gnu-toolchain, bare-metal AArch32"
    warn "arm-none-eabi) or point the build at yours:"
    warn "    make uvm2 UVM2_ARM_TOOLCHAIN=/path/to/arm-none-eabi"
fi

if [ "$CHECK_ONLY" = 0 ] && command -v rustup >/dev/null; then
    rustup target add "$RUST_TARGET" >/dev/null 2>&1 || true
fi
if command -v rustup >/dev/null && rustup target list --installed | grep -qx "$RUST_TARGET"; then
    ok "rust target  $RUST_TARGET"
else
    die "rust target $RUST_TARGET (rustup target add $RUST_TARGET)"
fi

echo
echo "== optional, per target =="
command -v ffmpeg >/dev/null && ok "ffmpeg       (make snd: regenerate the .vsm sound bundle)" \
                             || warn "ffmpeg absent — only needed for 'make snd'; the bundle ships pre-built"
command -v cc     >/dev/null && ok "cc           (make host: the desktop test harness)" \
                             || warn "no host cc — 'make host' will not build"
command -v emcc   >/dev/null && ok "emcc         (make sim: the WASM harness)" \
                             || warn "emscripten absent — only needed for 'make sim'"

echo
echo "== pico-sdk $PICO_SDK_VERSION =="
if [ -d "$PICO_SDK_DIR/.git" ]; then
    ok "already at $PICO_SDK_DIR"
elif [ "$CHECK_ONLY" = 1 ]; then
    die "pico-sdk (run ./setup.sh without --check)"
else
    echo "  cloning into third_party/pico-sdk (~40 MB, shallow)..."
    git clone --depth 1 --branch "$PICO_SDK_VERSION" \
        https://github.com/raspberrypi/pico-sdk.git "$PICO_SDK_DIR"
    ok "cloned"
fi
# No submodules on purpose: they are TinyUSB, lwIP, BTstack and mbedTLS, and
# this image enables none of them (pico_enable_stdio_usb is 0 -- a cartridge
# has no USB host, and dragging TinyUSB into a 50 Hz draw loop is exactly the
# kind of thing that costs frames).

echo
if [ "$MISSING" = 0 ]; then
    printf '\033[32mReady.\033[0m  The FIRST build also needs network: the Rust crates are not\n'
    printf 'vendored, so cargo fetches pio/pio-proc from crates.io once.\n\n'
    printf 'Build the smallest example with:\n\n    cd examples/hello_uvmc2 && make uvm2\n\n'
    printf 'or the full one with:\n\n    cd game/tacscan && make uvm2\n\n'
else
    printf '\033[31mSomething is missing -- see above.\033[0m\n'
    exit 1
fi

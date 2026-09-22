#!/bin/sh
# load.sh — put an image on the console OVER SWD, without going through the SD card.
#
#   tools/load.sh build_uvm2/pico/<UVM2_NAME>.elf
#   ATTACHED=1 tools/load.sh <elf>        stay attached, to debug the startup with breakpoints
#
# WHY: every test otherwise costs pulling the card, copying, reinserting and walking the
# menu. With twenty images in an afternoon that is half the time, and it invites batching
# changes — exactly what ruins a bisection, which needs ONE variable per test.
#
# HOW TO USE IT. Start ANY image from the multicart menu first. Nothing is reset: this
# attaches to what is running and writes over it, so the peripherals are left the way a
# real boot leaves them, which is what these images expect.
#
# WHAT IT COST, so it is not repeated:
#   - `monitor reset halt` + jumping to the entry does NOT work: a real image enters through
#     rom_reboot(RAM_IMAGE) and its startup relies on that state. It ended at pc=0xEFFFFFFE.
#   - Entry point and stack come from the ELF. The first two words at 0x20000000 gave
#     sp=0xF0000000: in these images the vector table is not there.
#   - `detach` after a `load` does NOT resume. `continue` blocks; killing the client with
#     TERM halts the target, and with KILL gdb blows up. So the client is left running
#     `continue` in the background and the probe is released afterwards (see below).
#   - Do not put comments between backticks inside the gdb invocation: the shell EXECUTES
#     them as command substitution and the command breaks silently.
#
# CORE 1. Loading this way restarts core 0 with the ELF's sp/pc, but core 1 is still running
# the PREVIOUS image. multicore_launch_core1() then waits for a FIFO handshake that never
# comes, and the image "does not draw" — indistinguishable from a bug in the program. Found
# with tools/probe.sh on 2026-08-24: pc in multicore_fifo_rvalid, lr in
# multicore_launch_core1_raw. uvm2_core1_start() now resets core 1 before launching it, so
# dual-core images load too. (Powering core 1 down through the PSM from outside was tried
# and hung the whole console; do not.)
#
# The transfer runs at about 11 KB/s, so the wait is proportional to the ELF's size: a fixed
# wait printed "done" a sixth of the way through a 359 KB image.
set -e
ELF=${1:?usage: load.sh <image.elf>}
[ -f "$ELF" ] || { echo "no such file: $ELF" >&2; exit 1; }
PORT=1339

PC=$(arm-none-eabi-readelf -h "$ELF" | awk '/Entry point/{print $NF}')
SP=$(arm-none-eabi-nm "$ELF" | awk '/ __StackTop$/{print "0x"$1}')
[ -n "$SP" ] || { echo "no __StackTop in $ELF" >&2; exit 1; }
echo "  from the elf: pc=$PC sp=$SP"

probe-rs gdb --chip RP235x --gdb-connection-string "127.0.0.1:$PORT" >/tmp/uvm2-load-gdb.log 2>&1 &
GDBSRV=$!
trap 'kill -9 $GDBSRV 2>/dev/null || true' EXIT
i=0
while ! grep -q "Firing up GDB stub" /tmp/uvm2-load-gdb.log 2>/dev/null; do
    i=$((i+1)); [ $i -gt 150 ] && { echo "the gdb server did not start"; cat /tmp/uvm2-load-gdb.log; exit 1; }
    sleep 0.1
done
sleep 1

arm-none-eabi-gdb -q "$ELF" \
  -ex 'set confirm off' \
  -ex 'set pagination off' \
  -ex 'set mem inaccessible-by-default off' \
  -ex "target extended-remote 127.0.0.1:$PORT" \
  -ex 'load' \
  -ex "set \$sp = $SP" \
  -ex "set \$pc = $PC" \
  -ex 'printf "  starting: sp=%08x pc=%08x\n", $sp, $pc' \
  -ex 'continue' >/tmp/uvm2-load-cli.log 2>&1 &
CLI=$!
trap - EXIT                      # the server stays alive on purpose
BYTES=$(wc -c < "$ELF")
sleep $(( 4 + BYTES / 11000 ))
grep -E "Start address|starting|Transfer rate" /tmp/uvm2-load-cli.log || true
echo "$CLI $GDBSRV" > /tmp/uvm2-load.pids

# RELEASE BY DEFAULT. An earlier version only PRINTED "remember to release" and stayed
# attached; once it was forgotten, and the console ran at 1 fps drawing garbage — with GDB
# attached the core stumbles and the Vectrex bus sees noise. That LOOKS like a defect in
# the image just loaded: you judge the instrument believing you are judging the image.
if [ "${ATTACHED:-0}" = "1" ]; then
    echo "  ATTACHED=1: gdb $CLI and probe-rs $GDBSRV are STILL attached — tools/release.sh"
    echo "  (the console will stumble meanwhile: do NOT judge the drawing like this)"
else
    "$(dirname "$0")/release.sh" >/dev/null 2>&1 || true
    echo "  probe released — the console runs on its own"
fi

#!/bin/sh
# probe.sh — the REGISTERS and a block of memory, over SWD, through GDB.
#
#   tools/probe.sh                       registers only (r0-r3, PC, LR, SP, xPSR)
#   tools/probe.sh 0x20080200 4          registers + 4 words from that address
#   tools/probe.sh <&uvm2_sd_diag> 13    (take the address from the ELF with nm)
#
# USE IT ON A CONSOLE THAT IS ALREADY HUNG, NOT ON A RUNNING GAME.
#
# GDB HALTS THE CORE to read registers, and on this board the core drives the Vectrex bus
# with its E phase locked to the clock: halting it is a phase violation. The script writes
# "continue" into DHCSR by hand and then detaches, and still — on 2026-09-13 a read with
# exactly this sequence left a running game silent, the third time. A core that is already
# halted does not come back either (DHCSR written both ways from a fresh session: neither
# revived it). Expect every use to cost a power cycle and a reload.
#
# So what it is FOR: the PC of a hang. When the screen is black and tools/stats.py says
# `recals` is not moving, the PC and LR say where it is stuck, which is worth more than
# any counter — that is how core 1 was found spinning in multicore_fifo_rvalid after an SWD
# load (see uvm2_core1.c). For a running game use tools/stats.py, which does not halt.
#
# Do not wait for the GDB server by poking its port with `nc -z`: the stub takes that
# connection for GDB, finds it closed and aborts with "failed to fill whole buffer". The
# script waits for the server's own log line instead.
set -e

ADDR=$1
N=${2:-4}
PORT=1337

probe-rs gdb --chip RP235x --gdb-connection-string "127.0.0.1:$PORT" >/tmp/uvm2-probe-gdb.log 2>&1 &
GDBSRV=$!
trap 'kill $GDBSRV 2>/dev/null || true' EXIT

i=0
while ! grep -q "Firing up GDB stub" /tmp/uvm2-probe-gdb.log 2>/dev/null; do
    i=$((i+1)); [ $i -gt 150 ] && { echo "the gdb server did not start"; cat /tmp/uvm2-probe-gdb.log; exit 1; }
    sleep 0.1
done
sleep 1

CMDS="-ex 'set confirm off'"
# probe-rs publishes a memory map with RAM and flash only, and GDB refuses to read outside
# it: peripheral registers (IO_BANK0, PADS, SIO...) give "Cannot access memory" without this.
CMDS="$CMDS -ex 'set mem inaccessible-by-default off'"
CMDS="$CMDS -ex 'target extended-remote 127.0.0.1:$PORT'"
CMDS="$CMDS -ex 'printf \"\\n== registers ==\\n\"' -ex 'info registers r0 r1 r2 r3 pc lr sp xpsr'"
[ -n "$ADDR" ] && CMDS="$CMDS -ex 'printf \"\\n== memory ==\\n\"' -ex 'x/${N}xw $ADDR'"
# DHCSR (0xE000EDF0): key 0xA05F in the top half, C_DEBUGEN=1, C_HALT=0 — "continue",
# written by hand, because a bare `detach` only resumes what was running before.
CMDS="$CMDS -ex 'set *(unsigned int *)0xE000EDF0 = 0xA05F0001'"
CMDS="$CMDS -ex 'printf \"\\n== detaching ==\\n\"' -ex 'detach'"

eval arm-none-eabi-gdb -q -batch $CMDS 2>&1 | grep -vE "^(Reading|warning: No exec)"

# Leave nothing attached: a live session is the next command's problem.
kill $GDBSRV 2>/dev/null || true
pgrep -f "probe-rs gdb" >/dev/null && echo "  !! a probe-rs gdb server is still running — tools/release.sh"
exit 0

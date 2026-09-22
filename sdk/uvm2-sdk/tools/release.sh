#!/bin/sh
# release.sh — kill whatever load.sh (or a stray GDB server) left attached to the probe.
#
# WHAT IT LEAVES BEHIND DEPENDS ON WHAT WAS ATTACHED. Right after load.sh — which is how
# load.sh ends by default — the image it started keeps running. But a session that had
# HALTED the core (GDB, a breakpoint, ATTACHED=1 left stepping) leaves it halted, and on
# this board a halted core does not come back: the cartridge goes silent and the console
# boots its built-in game. Only a power cycle recovers it.
#
# So the ORDER matters: read FIRST (tools/stats.py), release AFTER, and only when another
# tool needs the probe. Releasing "to clean up" before a read has already killed a running
# image once — the read then had nothing running to look at.
[ -f /tmp/uvm2-load.pids ] && kill -9 $(cat /tmp/uvm2-load.pids) 2>/dev/null
rm -f /tmp/uvm2-load.pids
pkill -9 -f "probe-rs gdb" 2>/dev/null
echo "  released"

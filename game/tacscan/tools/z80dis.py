#!/usr/bin/env python3
"""z80dis — a small Z80 disassembler, enough to read a hot loop.

There is no z80dasm on this machine and the AAE ports keep raising the same
question: the profile says 80% of the frame is in one 256-byte block, and then you
have to READ that block. This covers the base opcode set plus the CB/ED/DD/FD
prefixes, which is what game code is made of; anything it cannot decode is printed
as a byte rather than guessed at, so a wrong decode never masquerades as code.

  python3 tools/z80dis.py build/hot.bin build/hot.txt
  python3 tools/z80dis.py build/hot.bin 0x7A00          (a single origin)

`build/hot.bin` and `build/hot.txt` are what `make host-prof` leaves behind: the
bytes of the hottest blocks, taken from the Z80's own memory (not from the zip —
what matters is what it executes, and the mapping is the driver's business).
"""
import sys

R = ["B", "C", "D", "E", "H", "L", "(HL)", "A"]
RP = ["BC", "DE", "HL", "SP"]
RP2 = ["BC", "DE", "HL", "AF"]
CC = ["NZ", "Z", "NC", "C", "PO", "PE", "P", "M"]
ALU = ["ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "]
ROT = ["RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"]


class Dis:
    def __init__(self, mem, org):
        self.m, self.org, self.p = mem, org, 0

    def byte(self):
        if self.p >= len(self.m):
            raise IndexError
        b = self.m[self.p]; self.p += 1
        return b

    def word(self):
        lo = self.byte(); return lo | (self.byte() << 8)

    def rel(self):
        d = self.byte()
        if d > 127: d -= 256
        return (self.org + self.p + d) & 0xFFFF

    def one(self):
        """Decode one instruction; returns (text, is_flow_end)."""
        op = self.byte()
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        q, pp = y & 1, y >> 1

        if op == 0xCB:
            o = self.byte()
            x2, y2, z2 = o >> 6, (o >> 3) & 7, o & 7
            if x2 == 0: return f"{ROT[y2]} {R[z2]}", False
            return f"{['','BIT','RES','SET'][x2]} {y2},{R[z2]}", False
        if op in (0xDD, 0xFD):
            ix = "IX" if op == 0xDD else "IY"
            o = self.byte()
            if o == 0xCB:
                d = self.byte(); o2 = self.byte()
                dd = d - 256 if d > 127 else d
                x2, y2 = o2 >> 6, (o2 >> 3) & 7
                nm = ROT[y2] if x2 == 0 else ['', 'BIT', 'RES', 'SET'][x2] + f" {y2},"
                return f"{nm} ({ix}{dd:+d})", False
            # the common indexed forms; anything else is reported honestly
            if o == 0x21: return f"LD {ix},${self.word():04X}", False
            if o == 0x23: return f"INC {ix}", False
            if o == 0x2B: return f"DEC {ix}", False
            if o == 0xE5: return f"PUSH {ix}", False
            if o == 0xE1: return f"POP {ix}", False
            if o == 0xE9: return f"JP ({ix})", True
            if (o & 0xC7) == 0x46:                      # LD r,(ix+d)
                d = self.byte(); dd = d - 256 if d > 127 else d
                return f"LD {R[(o >> 3) & 7]},({ix}{dd:+d})", False
            if (o & 0xF8) == 0x70:                      # LD (ix+d),r
                d = self.byte(); dd = d - 256 if d > 127 else d
                return f"LD ({ix}{dd:+d}),{R[o & 7]}", False
            if o == 0x36:
                d = self.byte(); dd = d - 256 if d > 127 else d
                return f"LD ({ix}{dd:+d}),${self.byte():02X}", False
            if (o & 0xC7) == 0x86:                      # alu (ix+d)
                d = self.byte(); dd = d - 256 if d > 127 else d
                return f"{ALU[(o >> 3) & 7]}({ix}{dd:+d})", False
            return f"DB ${op:02X},${o:02X}   ; {ix} form not decoded", False
        if op == 0xED:
            o = self.byte()
            if (o & 0xC7) == 0x43:
                return (f"LD (${self.word():04X}),{RP[(o >> 4) & 3]}" if o & 8 == 0
                        else f"LD {RP[(o >> 4) & 3]},(${self.word():04X})"), False
            named = {0x44: "NEG", 0x45: "RETN", 0x4D: "RETI", 0x46: "IM 0",
                     0x56: "IM 1", 0x5E: "IM 2", 0x57: "LD A,I", 0x5F: "LD A,R",
                     0x47: "LD I,A", 0x4F: "LD R,A", 0x67: "RRD", 0x6F: "RLD",
                     0xA0: "LDI", 0xA1: "CPI", 0xA2: "INI", 0xA3: "OUTI",
                     0xA8: "LDD", 0xA9: "CPD", 0xB0: "LDIR", 0xB1: "CPIR",
                     0xB8: "LDDR", 0xB9: "CPDR", 0xB2: "INIR", 0xB3: "OTIR"}
            if o in named: return named[o], o in (0x45, 0x4D)
            if (o & 0xC7) == 0x42:
                return (f"{'ADC' if o & 8 else 'SBC'} HL,{RP[(o >> 4) & 3]}"), False
            return f"DB $ED,${o:02X}", False

        if x == 0:
            if op == 0x00: return "NOP", False
            if op == 0x08: return "EX AF,AF'", False
            if op == 0x10: return f"DJNZ ${self.rel():04X}", False
            if op == 0x18: return f"JR ${self.rel():04X}", True
            if z == 0:     return f"JR {CC[y - 4]},${self.rel():04X}", False
            if z == 1:
                return (f"ADD HL,{RP[pp]}" if q else f"LD {RP[pp]},${self.word():04X}"), False
            if z == 2:
                t = {0: "LD (BC),A", 1: "LD A,(BC)", 2: "LD (DE),A", 3: "LD A,(DE)"}
                if y < 4: return t[y], False
                a = self.word()
                return [f"LD (${a:04X}),HL", f"LD HL,(${a:04X})",
                        f"LD (${a:04X}),A", f"LD A,(${a:04X})"][y - 4], False
            if z == 3: return f"{'DEC' if q else 'INC'} {RP[pp]}", False
            if z == 4: return f"INC {R[y]}", False
            if z == 5: return f"DEC {R[y]}", False
            if z == 6: return f"LD {R[y]},${self.byte():02X}", False
            return ["RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"][y], False
        if x == 1:
            if op == 0x76: return "HALT", True
            return f"LD {R[y]},{R[z]}", False
        if x == 2:
            return f"{ALU[y]}{R[z]}", False
        # x == 3
        if z == 0: return f"RET {CC[y]}", False
        if z == 1:
            if q == 0: return f"POP {RP2[pp]}", False
            return [("RET", True), ("EXX", False), ("JP (HL)", True),
                    ("LD SP,HL", False)][pp]
        if z == 2: return f"JP {CC[y]},${self.word():04X}", False
        if z == 3:
            if y == 0: return f"JP ${self.word():04X}", True
            if y == 2: return f"OUT ($={self.byte():02X}),A".replace("$=", "$"), False
            if y == 3: return f"IN A,(${self.byte():02X})", False
            return ["", "", "", "", "EX (SP),HL", "EX DE,HL", "DI", "EI"][y], False
        if z == 4: return f"CALL {CC[y]},${self.word():04X}", False
        if z == 5:
            if q == 0: return f"PUSH {RP2[pp]}", False
            if pp == 0: return f"CALL ${self.word():04X}", False
            return f"DB ${op:02X}", False
        if z == 6: return f"{ALU[y]}${self.byte():02X}", False
        return f"RST ${y * 8:02X}", False


def run(mem, org):
    d = Dis(mem, org)
    print(f"\n; ── {org:04X}-{org + len(mem) - 1:04X} " + "─" * 40)
    while d.p < len(mem):
        at = org + d.p
        start = d.p
        try:
            txt, end = d.one()
        except IndexError:
            break
        raw = " ".join(f"{b:02X}" for b in mem[start:d.p])
        print(f"{at:04X}  {raw:<11} {txt}")
        if end:
            print()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    blob = open(sys.argv[1], "rb").read()
    a = sys.argv[2]
    origins = ([int(l, 16) for l in open(a) if l.strip()]
               if not a.startswith("0x") else [int(a, 16)])
    for i, org in enumerate(origins):
        chunk = blob[i * 256:(i + 1) * 256]
        if chunk:
            run(chunk, org)


if __name__ == "__main__":
    main()

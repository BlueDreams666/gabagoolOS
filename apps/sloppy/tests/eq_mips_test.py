#!/usr/bin/env python3
# dsp_mips.S's filter_process_ef against dsp_filter.c's, bit for bit: the
# assembled .text runs on a small MIPS32r2 interpreter (the instructions the
# loop uses, delay slots and HI/LO), the C through ctypes. Also counts the
# instructions it runs per sample, and checks the callee-saved registers.
# Usage: eq_mips_test.py dsp_mips.text.bin libref.so [cases]
import ctypes, random, struct, sys

M32 = 0xffffffff


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


class Mips:
    def __init__(self, code, base):
        self.mem = {}
        self.code, self.base = code, base
        self.r = [0] * 32
        self.hi = self.lo = 0
        self.count = 0

    def lw(self, a):
        assert a % 4 == 0, hex(a)
        return self.mem.get(a, 0)

    def sw(self, a, v):
        assert a % 4 == 0, hex(a)
        self.mem[a] = v & M32

    def lbu(self, a):
        return (self.lw(a & ~3) >> (8 * (a & 3))) & 0xff

    def acc(self):
        return (self.hi << 32 | self.lo) & ((1 << 64) - 1)

    def set_acc(self, v):
        v &= (1 << 64) - 1
        self.hi, self.lo = v >> 32, v & M32

    def run(self, pc, ret):
        r = self.r
        npc = pc + 4
        while pc != ret:
            off = pc - self.base
            assert 0 <= off < len(self.code), hex(pc)
            ins = struct.unpack_from('<I', self.code, off)[0]
            self.count += 1
            op, rs, rt = ins >> 26, (ins >> 21) & 31, (ins >> 16) & 31
            rd, sa, fn = (ins >> 11) & 31, (ins >> 6) & 31, ins & 63
            imm = ins & 0xffff
            simm = imm - 0x10000 if imm & 0x8000 else imm
            target = None
            wr = None
            if op == 0:
                if fn == 0: wr = (rd, r[rt] << sa)                        # sll
                elif fn == 2 and rs == 0: wr = (rd, r[rt] >> sa)          # srl
                elif fn == 3: wr = (rd, s32(r[rt]) >> sa)                 # sra
                elif fn == 4: wr = (rd, r[rt] << (r[rs] & 31))            # sllv
                elif fn == 7: wr = (rd, s32(r[rt]) >> (r[rs] & 31))       # srav
                elif fn == 6: wr = (rd, r[rt] >> (r[rs] & 31))            # srlv
                elif fn == 8: target = r[rs]                              # jr
                elif fn == 16: wr = (rd, self.hi)                         # mfhi
                elif fn == 18: wr = (rd, self.lo)                         # mflo
                elif fn == 17: self.hi = r[rs]                            # mthi
                elif fn == 19: self.lo = r[rs]                            # mtlo
                elif fn == 24: self.set_acc(s32(r[rs]) * s32(r[rt]))      # mult
                elif fn == 33: wr = (rd, r[rs] + r[rt])                   # addu
                elif fn == 35: wr = (rd, r[rs] - r[rt])                   # subu
                elif fn == 36: wr = (rd, r[rs] & r[rt])                   # and
                elif fn == 37: wr = (rd, r[rs] | r[rt])                   # or
                elif fn == 38: wr = (rd, r[rs] ^ r[rt])                   # xor
                elif fn == 43: wr = (rd, int(r[rs] < r[rt]))              # sltu
                elif fn == 42: wr = (rd, int(s32(r[rs]) < s32(r[rt])))    # slt
                else: raise ValueError('op %08x at %x' % (ins, pc))
            elif op == 28 and fn == 0:                                    # madd
                self.set_acc(self.acc() + s32(r[rs]) * s32(r[rt]))
            elif op == 31 and fn == 0:                                    # ext
                wr = (rt, (r[rs] >> sa) & ((1 << (rd + 1)) - 1))
            elif op == 31 and fn == 4:                                    # ins
                size = rd - sa + 1
                m = ((1 << size) - 1) << sa
                wr = (rt, (r[rt] & ~m) | ((r[rs] << sa) & m))
            elif op == 4:
                if r[rs] == r[rt]: target = npc + (simm << 2)             # beq
            elif op == 5:
                if r[rs] != r[rt]: target = npc + (simm << 2)             # bne
            elif op == 6 and rt == 0:
                if s32(r[rs]) <= 0: target = npc + (simm << 2)           # blez
            elif op == 7 and rt == 0:
                if s32(r[rs]) > 0: target = npc + (simm << 2)            # bgtz
            elif op == 9: wr = (rt, r[rs] + simm)                         # addiu
            elif op == 11: wr = (rt, int(r[rs] < (simm & M32)))           # sltiu
            elif op == 12: wr = (rt, r[rs] & imm)                         # andi
            elif op == 13: wr = (rt, r[rs] | imm)                         # ori
            elif op == 14: wr = (rt, r[rs] ^ imm)                         # xori
            elif op == 15: wr = (rt, imm << 16)                           # lui
            elif op == 35: wr = (rt, self.lw((r[rs] + simm) & M32))       # lw
            elif op == 36: wr = (rt, self.lbu((r[rs] + simm) & M32))      # lbu
            elif op == 43: self.sw((r[rs] + simm) & M32, r[rt])           # sw
            else:
                raise ValueError('op %08x at %x' % (ins, pc))
            if wr and wr[0]:
                r[wr[0]] = wr[1] & M32
            pc, npc = npc, (target if target is not None else npc + 4)


class Filter(ctypes.Structure):
    _fields_ = [('coefs', ctypes.c_int32 * 5), ('history', ctypes.c_int32 * 8),
                ('shift', ctypes.c_uint8)]


class FilterEf(ctypes.Structure):
    _fields_ = [('fine', ctypes.c_int32 * 5), ('a30', ctypes.c_int32 * 2),
                ('err', ctypes.c_int32 * 4), ('post', ctypes.c_uint8),
                ('fine_on', ctypes.c_bool)]


CODE, F, EF, BUFS, DATA, STACK, RET = 0x400000, 0x10000, 0x10100, 0x10200, 0x100000, 0x7ff000, 0xdead0000


def run_asm(m, f, ef, chans, count):
    for i in range(5):
        m.sw(F + 4 * i, f.coefs[i]); m.sw(EF + 4 * i, ef.fine[i])
    for i in range(8):
        m.sw(F + 20 + 4 * i, f.history[i])
    m.sw(F + 52, f.shift)
    m.sw(EF + 20, ef.a30[0]); m.sw(EF + 24, ef.a30[1])
    for i in range(4):
        m.sw(EF + 28 + 4 * i, ef.err[i])
    m.sw(EF + 44, ef.post | (int(ef.fine_on) << 8))
    for c, samples in enumerate(chans):
        m.sw(BUFS + 4 * c, DATA + 0x10000 * c)
        for i, v in enumerate(samples):
            m.sw(DATA + 0x10000 * c + 4 * i, v)
    saved = {n: random.getrandbits(32) for n in (1, 16, 17, 18, 19, 20, 21, 22, 23, 28, 30)}
    for n, v in saved.items():
        m.r[n] = v
    m.r[29] = STACK
    m.sw(STACK + 16, len(chans))
    m.r[4], m.r[5], m.r[6], m.r[7], m.r[31] = F, EF, BUFS, count, RET
    m.count = 0
    m.run(CODE, RET)
    for n, v in saved.items():
        if n != 1:
            assert m.r[n] == v, 'register %d not restored' % n
    assert m.r[29] == STACK
    out = [[s32(m.lw(DATA + 0x10000 * c + 4 * i)) for i in range(count)] for c in range(len(chans))]
    hist = [s32(m.lw(F + 20 + 4 * i)) for i in range(8)]
    err = [s32(m.lw(EF + 28 + 4 * i)) for i in range(4)]
    return out, hist, err, m.count


def run_c(lib, f, ef, chans, count):
    arrs = [(ctypes.c_int32 * max(count, 1))(*s) for s in chans]
    ptrs = (ctypes.POINTER(ctypes.c_int32) * 2)(*[ctypes.cast(a, ctypes.POINTER(ctypes.c_int32)) for a in arrs])
    lib.filter_process_ef(ctypes.byref(f), ctypes.byref(ef), ptrs, count, len(chans))
    return [list(a)[:count] for a in arrs], list(f.history), list(ef.err)


def rnd_bits(n):
    """a signed value of about n bits, often at the ends"""
    k = random.random()
    if k < 0.1: return (1 << n) - 1
    if k < 0.2: return -(1 << n)
    return random.randint(-(1 << n), (1 << n) - 1)


def samples(n, kind):
    if kind == 0:   # full-scale noise and rails: saturates
        return [random.choice([0x7fffffff, -0x80000000, random.randint(-2**31, 2**31 - 1)]) for _ in range(n)]
    if kind == 1:   # audio-like, below full scale (dsp's 28-bit sample)
        return [random.randint(-2**28, 2**28) for _ in range(n)]
    return [random.randint(-2**31, 2**31 - 1) >> random.randint(0, 31) for _ in range(n)]


def main():
    code = open(sys.argv[1], 'rb').read()
    lib = ctypes.CDLL(sys.argv[2])
    cases = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
    random.seed(1234)
    stats = {}
    sat = 0
    for case in range(cases):
        f, ef = Filter(), FilterEf()
        if case % 2:
            # a real design: filter_bq_coefs at a random type, cutoff, Q, gain
            lib.filter_bq_coefs(random.randint(1, 5), ctypes.c_ulong(random.randint(1 << 14, 0x7d70a3d7) >> random.randint(0, 14)),
                                ctypes.c_ulong(random.randint(1, 200)), ctypes.c_long(random.randint(-240, 240)),
                                ctypes.byref(f), ctypes.byref(ef))
        else:
            # anything the design's bounds allow: |b| < 2 after post, |a1| < 2,
            # |a2| < 1, fine[] below 2^29, a30[] in s1.30
            for i in range(4):
                f.coefs[i] = rnd_bits(29)
            f.coefs[4] = rnd_bits(28)
            ef.fine_on = random.random() < 0.5
            for i in range(5):
                ef.fine[i] = rnd_bits(29) if ef.fine_on else 0
            ef.a30[0] = max(-2**31, min(2**31 - 1, rnd_bits(31)))
            ef.a30[1] = rnd_bits(30)
            ef.post = random.randint(0, 8) if random.random() < 0.5 else 0
            f.shift = 4
        for i in range(8):
            f.history[i] = random.randint(-2**31, 2**31 - 1) if case % 3 else 0
        for i in range(4):
            ef.err[i] = random.randint(-2**27, 2**27 - 1) if case % 3 else 0
        nch = random.choice([1, 2])
        count = random.choice([0, 1, 2, 3, 17, 64])
        chans = [samples(count, random.randint(0, 2)) for _ in range(nch)]
        f2, ef2 = Filter(), FilterEf()
        ctypes.memmove(ctypes.byref(f2), ctypes.byref(f), ctypes.sizeof(f))
        ctypes.memmove(ctypes.byref(ef2), ctypes.byref(ef), ctypes.sizeof(ef))
        ref = run_c(lib, f2, ef2, chans, count)
        got = run_asm(Mips(code, CODE), f, ef, chans, count)
        if got[:3] != tuple(ref):
            print('MISMATCH case %d fine %d post %d' % (case, ef.fine_on, ef.post))
            print(' c  ', ref)
            print(' asm', got[:3])
            sys.exit(1)
        sat += sum(v in (0x7fffffff, -0x80000000) for ch in ref[0] for v in ch)
        if count == 64 and nch == 1:
            stats.setdefault((bool(ef.fine_on), ef.post > 0), []).append(got[3] / count)
    print('eq_mips_test: %d cases bit-identical to the C (%d saturated samples)' % (cases, sat))
    for k in sorted(stats):
        print('  fine %d posted %d: %.1f instructions a sample (64-sample mono runs, sat paths incl.)'
              % (k[0], k[1], min(stats[k])))


main()

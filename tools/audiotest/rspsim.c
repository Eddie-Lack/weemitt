/*
 * rspsim.c: a model of the RSP that runs the ROM's audio microcode. See
 * rspsim.h.
 *
 * The semantics of each instruction come from the public descriptions of
 * the RSP's instruction set. Only the instructions that the audio microcode
 * uses are modelled; any other sets rsp->error. The model runs one
 * instruction after another and does not model the pipeline.
 */
#include <stdio.h>
#include <string.h>

#include "rspsim.h"

#define DMEM_MASK 0xFFF

/* Coprocessor 0 registers. */
enum {
    C0_MEM_ADDR = 0,
    C0_DRAM_ADDR = 1,
    C0_RD_LEN = 2,
    C0_WR_LEN = 3,
    C0_STATUS = 4,
    C0_DMA_FULL = 5,
    C0_DMA_BUSY = 6,
    C0_SEMAPHORE = 7
};

static uint16_t lane(const RspSim *rsp, int v, int i) {
    return (uint16_t)((rsp->v[v][2 * i] << 8) | rsp->v[v][2 * i + 1]);
}

static void setLane(RspSim *rsp, int v, int i, uint16_t x) {
    rsp->v[v][2 * i] = (uint8_t)(x >> 8);
    rsp->v[v][2 * i + 1] = (uint8_t)x;
}

static int64_t sext48(int64_t a) {
    return (int64_t)((uint64_t)a << 16) >> 16;
}

/* The accumulator's bits 47..16 as a signed value, saturated to 16 bits. */
static uint16_t clampMid(int64_t acc) {
    int64_t m = acc >> 16;

    if (m > 32767) {
        return 0x7FFF;
    }
    if (m < -32768) {
        return 0x8000;
    }
    return (uint16_t)m;
}

/* The accumulator's bits 15..0, saturated as an unsigned value when bits 47..16 do not fit in 16 signed bits. */
static uint16_t clampLow(int64_t acc) {
    int64_t m = acc >> 16;

    if (m > 32767 || m < -32768) {
        return acc < 0 ? 0x0000 : 0xFFFF;
    }
    return (uint16_t)acc;
}

static uint16_t clamp16(int32_t x) {
    if (x > 32767) {
        return 0x7FFF;
    }
    if (x < -32768) {
        return 0x8000;
    }
    return (uint16_t)x;
}

/* Which lane of vt each lane uses, for the element field e of a computational instruction. */
static int selectLane(int e, int i) {
    if (e <= 1) {
        return i;
    }
    if (e <= 3) {
        return (i & ~1) | (e & 1);
    }
    if (e <= 7) {
        return (i & ~3) | (e & 3);
    }
    return e & 7;
}

static uint32_t read32(const uint8_t *m, uint32_t a, uint32_t mask) {
    return ((uint32_t)m[a & mask] << 24) | ((uint32_t)m[(a + 1) & mask] << 16) | ((uint32_t)m[(a + 2) & mask] << 8) |
           m[(a + 3) & mask];
}

static void dma(RspSim *rsp, uint32_t lenReg, int toRdram) {
    uint32_t mem = rsp->c0[C0_MEM_ADDR] & 0x1FF8;
    uint32_t dram = rsp->c0[C0_DRAM_ADDR] & 0xFFFFF8;
    uint32_t length = (lenReg & 0xFFF) | 7;
    uint32_t rows = ((lenReg >> 12) & 0xFF) + 1;
    uint32_t skip = (lenReg >> 20) & 0xFF8;
    uint8_t *bank = (mem & 0x1000) ? rsp->imem : rsp->dmem;
    uint32_t row, i;

    for (row = 0; row < rows; row++) {
        for (i = 0; i <= length; i++) {
            uint32_t m = (mem + i) & DMEM_MASK;
            uint32_t d = dram + i;

            if (d >= rsp->rdramSize) {
                fprintf(stderr, "rspsim: DMA outside RDRAM: %06X\n", d);
                rsp->error = 1;
                return;
            }
            if (toRdram) {
                rsp->rdram[d] = bank[m];
            } else {
                bank[m] = rsp->rdram[d];
            }
        }
        mem = (mem + length + 1) & DMEM_MASK;
        dram += length + 1 + skip;
    }
}

static void vectorLoad(RspSim *rsp, uint32_t inst) {
    int base = (inst >> 21) & 31;
    int vt = (inst >> 16) & 31;
    int op = (inst >> 11) & 31;
    int e = (inst >> 7) & 15;
    int32_t offset = (int32_t)((inst & 0x7F) ^ 0x40) - 0x40;
    static const int sizes[] = { 1, 2, 4, 8, 16, 16 };
    uint32_t addr, start;
    int i, n;

    if (op > 5) {
        fprintf(stderr, "rspsim: vector load %d not modelled\n", op);
        rsp->error = 1;
        return;
    }
    addr = (rsp->r[base] + offset * sizes[op]) & DMEM_MASK;
    switch (op) {
        case 0:
        case 1:
        case 2:
        case 3: /* LBV, LSV, LLV, LDV: any address */
            for (i = 0; i < sizes[op] && e + i < 16; i++) {
                rsp->v[vt][e + i] = rsp->dmem[(addr + i) & DMEM_MASK];
            }
            break;
        case 4: /* LQV: up to the end of the 16-byte block */
            n = 16 - (addr & 15);
            for (i = 0; i < n && e + i < 16; i++) {
                rsp->v[vt][e + i] = rsp->dmem[addr + i];
            }
            break;
        case 5: /* LRV: from the start of the 16-byte block up to addr, into the end of the register */
            n = addr & 15;
            start = addr & ~15u;
            for (i = 0; i < n; i++) {
                int idx = e + 16 - n + i;

                if (idx < 16) {
                    rsp->v[vt][idx] = rsp->dmem[start + i];
                }
            }
            break;
    }
}

static void vectorStore(RspSim *rsp, uint32_t inst) {
    int base = (inst >> 21) & 31;
    int vt = (inst >> 16) & 31;
    int op = (inst >> 11) & 31;
    int e = (inst >> 7) & 15;
    int32_t offset = (int32_t)((inst & 0x7F) ^ 0x40) - 0x40;
    static const int sizes[] = { 1, 2, 4, 8, 16 };
    uint32_t addr;
    int i, n;

    if (op > 4) {
        fprintf(stderr, "rspsim: vector store %d not modelled\n", op);
        rsp->error = 1;
        return;
    }
    addr = (rsp->r[base] + offset * sizes[op]) & DMEM_MASK;
    n = op == 4 ? 16 - (int)(addr & 15) : sizes[op];
    for (i = 0; i < n; i++) {
        rsp->dmem[(addr + i) & DMEM_MASK] = rsp->v[vt][(e + i) & 15];
    }
}

static void vectorOp(RspSim *rsp, uint32_t inst) {
    int funct = inst & 0x3F;
    int vd = (inst >> 6) & 31;
    int vs = (inst >> 11) & 31;
    int vt = (inst >> 16) & 31;
    int e = (inst >> 21) & 15;
    uint16_t s[8], t[8], d[8];
    int i;

    for (i = 0; i < 8; i++) {
        s[i] = lane(rsp, vs, i);
        t[i] = lane(rsp, vt, selectLane(e, i));
    }
    for (i = 0; i < 8; i++) {
        int64_t *acc = &rsp->acc[i];
        int32_t ss = (int16_t)s[i], ts = (int16_t)t[i];
        int32_t r;
        int carry = (rsp->vcoCarry >> i) & 1;
        int ne = (rsp->vcoNe >> i) & 1;

        switch (funct) {
            case 0x00: /* VMULF */
                *acc = sext48((int64_t)ss * ts * 2 + 0x8000);
                d[i] = clampMid(*acc);
                break;
            case 0x04: /* VMUDL */
                *acc = sext48(((int64_t)s[i] * t[i]) >> 16);
                d[i] = clampLow(*acc);
                break;
            case 0x05: /* VMUDM */
                *acc = sext48((int64_t)ss * t[i]);
                d[i] = clampMid(*acc);
                break;
            case 0x06: /* VMUDN */
                *acc = sext48((int64_t)s[i] * ts);
                d[i] = clampLow(*acc);
                break;
            case 0x07: /* VMUDH */
                *acc = sext48((int64_t)ss * ts * 65536);
                d[i] = clampMid(*acc);
                break;
            case 0x08: /* VMACF */
                *acc = sext48(*acc + (int64_t)ss * ts * 2);
                d[i] = clampMid(*acc);
                break;
            case 0x0C: /* VMADL */
                *acc = sext48(*acc + (((int64_t)s[i] * t[i]) >> 16));
                d[i] = clampLow(*acc);
                break;
            case 0x0D: /* VMADM */
                *acc = sext48(*acc + (int64_t)ss * t[i]);
                d[i] = clampMid(*acc);
                break;
            case 0x0E: /* VMADN */
                *acc = sext48(*acc + (int64_t)s[i] * ts);
                d[i] = clampLow(*acc);
                break;
            case 0x0F: /* VMADH */
                *acc = sext48(*acc + (int64_t)ss * ts * 65536);
                d[i] = clampMid(*acc);
                break;
            case 0x10: /* VADD */
                r = ss + ts + carry;
                *acc = (*acc & ~(int64_t)0xFFFF) | (r & 0xFFFF);
                d[i] = clamp16(r);
                break;
            case 0x11: /* VSUB */
                r = ss - ts - carry;
                *acc = (*acc & ~(int64_t)0xFFFF) | (r & 0xFFFF);
                d[i] = clamp16(r);
                break;
            case 0x15: /* VSUBC */
                r = (int32_t)s[i] - (int32_t)t[i];
                *acc = (*acc & ~(int64_t)0xFFFF) | (r & 0xFFFF);
                d[i] = (uint16_t)r;
                break;
            case 0x1D: /* VSAR */
                if (e == 8) {
                    d[i] = (uint16_t)(*acc >> 32);
                } else if (e == 9) {
                    d[i] = (uint16_t)(*acc >> 16);
                } else if (e == 10) {
                    d[i] = (uint16_t)*acc;
                } else {
                    d[i] = 0;
                }
                break;
            case 0x23: { /* VGE */
                int ge = ss > ts || (ss == ts && !(carry && ne));

                d[i] = ge ? s[i] : t[i];
                *acc = (*acc & ~(int64_t)0xFFFF) | d[i];
                rsp->vccLo = (uint16_t)((rsp->vccLo & ~(1 << i)) | (ge << i));
                rsp->vccHi &= (uint16_t)~(1 << i);
                break;
            }
            case 0x24: { /* VCL */
                if (carry) {
                    int le;

                    if (ne) {
                        le = (rsp->vccLo >> i) & 1;
                    } else {
                        uint32_t sum = (uint32_t)s[i] + t[i];
                        int over = sum > 0xFFFF;
                        int zero = (sum & 0xFFFF) == 0;

                        le = ((rsp->vce >> i) & 1) ? (zero || !over) : (zero && !over);
                        rsp->vccLo = (uint16_t)((rsp->vccLo & ~(1 << i)) | (le << i));
                    }
                    d[i] = le ? (uint16_t)-t[i] : s[i];
                } else {
                    int ge;

                    if (ne) {
                        ge = (rsp->vccHi >> i) & 1;
                    } else {
                        ge = s[i] >= t[i];
                        rsp->vccHi = (uint16_t)((rsp->vccHi & ~(1 << i)) | (ge << i));
                    }
                    d[i] = ge ? t[i] : s[i];
                }
                *acc = (*acc & ~(int64_t)0xFFFF) | d[i];
                break;
            }
            case 0x28: /* VAND */
                d[i] = s[i] & t[i];
                *acc = (*acc & ~(int64_t)0xFFFF) | d[i];
                break;
            case 0x2C: /* VXOR */
                d[i] = s[i] ^ t[i];
                *acc = (*acc & ~(int64_t)0xFFFF) | d[i];
                break;
            default:
                fprintf(stderr, "rspsim: vector op %02X not modelled\n", funct);
                rsp->error = 1;
                return;
        }
    }
    /* Flags after the whole vector. */
    switch (funct) {
        case 0x10:
        case 0x11:
            rsp->vcoCarry = rsp->vcoNe = 0;
            break;
        case 0x15:
            rsp->vcoCarry = rsp->vcoNe = 0;
            for (i = 0; i < 8; i++) {
                int32_t r = (int32_t)s[i] - (int32_t)t[i];

                rsp->vcoCarry |= (uint16_t)((r < 0) << i);
                rsp->vcoNe |= (uint16_t)((r != 0) << i);
            }
            break;
        case 0x23:
        case 0x24:
            rsp->vcoCarry = rsp->vcoNe = 0;
            rsp->vce = 0;
            break;
    }
    for (i = 0; i < 8; i++) {
        setLane(rsp, vd, i, d[i]);
    }
}

void rspSimLoad(RspSim *rsp, const uint8_t *text, uint32_t textSize, const uint8_t *data, uint32_t dataSize) {
    memset(rsp->imem, 0, sizeof(rsp->imem));
    memcpy(rsp->imem + 0x80, text, textSize);
    memcpy(rsp->data, data, dataSize);
    rsp->dataSize = dataSize;
    memcpy(rsp->dmem, data, dataSize);
}

int rspSimRunTask(RspSim *rsp, uint32_t listAddr, uint32_t listBytes) {
    uint32_t pc = 0x1080, npc = 0x1084;
    const uint32_t taskAddr = 0xFC0;
    uint64_t limit = rsp->steps + 200000000ull;

    memcpy(rsp->dmem, rsp->data, rsp->dataSize);
    /* The boot microcode leaves the task structure at DMEM 0xFC0 and its address in $1. */
    rsp->dmem[taskAddr + 0x30] = (uint8_t)(listAddr >> 24);
    rsp->dmem[taskAddr + 0x31] = (uint8_t)(listAddr >> 16);
    rsp->dmem[taskAddr + 0x32] = (uint8_t)(listAddr >> 8);
    rsp->dmem[taskAddr + 0x33] = (uint8_t)listAddr;
    rsp->dmem[taskAddr + 0x34] = (uint8_t)(listBytes >> 24);
    rsp->dmem[taskAddr + 0x35] = (uint8_t)(listBytes >> 16);
    rsp->dmem[taskAddr + 0x36] = (uint8_t)(listBytes >> 8);
    rsp->dmem[taskAddr + 0x37] = (uint8_t)listBytes;
    rsp->r[1] = taskAddr;
    rsp->error = 0;

    while (!rsp->error) {
        uint32_t inst = read32(rsp->imem, pc & 0xFFC, 0xFFF);
        uint32_t cur = pc;
        int op = inst >> 26;
        int rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sa = (inst >> 6) & 31;
        uint32_t imm = inst & 0xFFFF;
        uint32_t simm = (uint32_t)(int32_t)(int16_t)imm;
        uint32_t a, b;

        if (++rsp->steps > limit) {
            fprintf(stderr, "rspsim: the task does not end\n");
            return -1;
        }
        pc = npc;
        npc += 4;
        a = rsp->r[rs];
        b = rsp->r[rt];

        switch (op) {
            case 0x00:
                switch (inst & 0x3F) {
                    case 0x00: rsp->r[rd] = b << sa; break;
                    case 0x02: rsp->r[rd] = b >> sa; break;
                    case 0x03: rsp->r[rd] = (uint32_t)((int32_t)b >> sa); break;
                    case 0x04: rsp->r[rd] = b << (a & 31); break;
                    case 0x06: rsp->r[rd] = b >> (a & 31); break;
                    case 0x07: rsp->r[rd] = (uint32_t)((int32_t)b >> (a & 31)); break;
                    case 0x08: npc = a & 0xFFC; break;
                    case 0x09: rsp->r[rd] = cur + 8; npc = a & 0xFFC; break;
                    case 0x0D: return 0; /* BREAK: the task is done */
                    case 0x20:
                    case 0x21: rsp->r[rd] = a + b; break;
                    case 0x22:
                    case 0x23: rsp->r[rd] = a - b; break;
                    case 0x24: rsp->r[rd] = a & b; break;
                    case 0x25: rsp->r[rd] = a | b; break;
                    case 0x26: rsp->r[rd] = a ^ b; break;
                    case 0x27: rsp->r[rd] = ~(a | b); break;
                    case 0x2A: rsp->r[rd] = (int32_t)a < (int32_t)b; break;
                    case 0x2B: rsp->r[rd] = a < b; break;
                    default:
                        fprintf(stderr, "rspsim: special %02X not modelled at %03X\n", inst & 0x3F, cur);
                        rsp->error = 1;
                }
                break;
            case 0x01:
                if ((rt & 1) ? (int32_t)a >= 0 : (int32_t)a < 0) {
                    npc = (pc + (simm << 2)) & 0xFFC;
                }
                if (rt & 0x10) {
                    rsp->r[31] = cur + 8;
                }
                break;
            case 0x02: npc = ((inst & 0x03FFFFFF) << 2) & 0xFFC; break;
            case 0x03: rsp->r[31] = cur + 8; npc = ((inst & 0x03FFFFFF) << 2) & 0xFFC; break;
            case 0x04: if (a == b) npc = (pc + (simm << 2)) & 0xFFC; break;
            case 0x05: if (a != b) npc = (pc + (simm << 2)) & 0xFFC; break;
            case 0x06: if ((int32_t)a <= 0) npc = (pc + (simm << 2)) & 0xFFC; break;
            case 0x07: if ((int32_t)a > 0) npc = (pc + (simm << 2)) & 0xFFC; break;
            case 0x08:
            case 0x09: rsp->r[rt] = a + simm; break;
            case 0x0A: rsp->r[rt] = (int32_t)a < (int32_t)simm; break;
            case 0x0B: rsp->r[rt] = a < simm; break;
            case 0x0C: rsp->r[rt] = a & imm; break;
            case 0x0D: rsp->r[rt] = a | imm; break;
            case 0x0E: rsp->r[rt] = a ^ imm; break;
            case 0x0F: rsp->r[rt] = imm << 16; break;
            case 0x10:
                if (rs == 0) { /* MFC0: the DMA is never busy and the semaphore is always free */
                    rsp->r[rt] = (rd == C0_MEM_ADDR || rd == C0_DRAM_ADDR) ? rsp->c0[rd] : 0;
                } else if (rs == 4) { /* MTC0 */
                    rsp->c0[rd & 15] = b;
                    if (rd == C0_RD_LEN) {
                        dma(rsp, b, 0);
                    } else if (rd == C0_WR_LEN) {
                        dma(rsp, b, 1);
                    }
                } else {
                    rsp->error = 1;
                }
                break;
            case 0x12:
                if (inst & (1u << 25)) {
                    vectorOp(rsp, inst);
                } else {
                    int e = (inst >> 7) & 15;

                    if (rs == 0) { /* MFC2 */
                        rsp->r[rt] = (uint32_t)(int32_t)(int16_t)((rsp->v[rd][e] << 8) | rsp->v[rd][(e + 1) & 15]);
                    } else if (rs == 4) { /* MTC2 */
                        rsp->v[rd][e] = (uint8_t)(b >> 8);
                        if (e < 15) {
                            rsp->v[rd][e + 1] = (uint8_t)b;
                        }
                    } else {
                        fprintf(stderr, "rspsim: cop2 move %d not modelled\n", rs);
                        rsp->error = 1;
                    }
                }
                break;
            case 0x20: rsp->r[rt] = (uint32_t)(int32_t)(int8_t)rsp->dmem[(a + simm) & DMEM_MASK]; break;
            case 0x21:
                rsp->r[rt] = (uint32_t)(int32_t)(int16_t)((rsp->dmem[(a + simm) & DMEM_MASK] << 8) |
                                                         rsp->dmem[(a + simm + 1) & DMEM_MASK]);
                break;
            case 0x23: rsp->r[rt] = read32(rsp->dmem, a + simm, DMEM_MASK); break;
            case 0x24: rsp->r[rt] = rsp->dmem[(a + simm) & DMEM_MASK]; break;
            case 0x25:
                rsp->r[rt] = (uint32_t)((rsp->dmem[(a + simm) & DMEM_MASK] << 8) | rsp->dmem[(a + simm + 1) & DMEM_MASK]);
                break;
            case 0x28: rsp->dmem[(a + simm) & DMEM_MASK] = (uint8_t)b; break;
            case 0x29:
                rsp->dmem[(a + simm) & DMEM_MASK] = (uint8_t)(b >> 8);
                rsp->dmem[(a + simm + 1) & DMEM_MASK] = (uint8_t)b;
                break;
            case 0x2B:
                rsp->dmem[(a + simm) & DMEM_MASK] = (uint8_t)(b >> 24);
                rsp->dmem[(a + simm + 1) & DMEM_MASK] = (uint8_t)(b >> 16);
                rsp->dmem[(a + simm + 2) & DMEM_MASK] = (uint8_t)(b >> 8);
                rsp->dmem[(a + simm + 3) & DMEM_MASK] = (uint8_t)b;
                break;
            case 0x32: vectorLoad(rsp, inst); break;
            case 0x3A: vectorStore(rsp, inst); break;
            default:
                fprintf(stderr, "rspsim: opcode %02X not modelled at %03X\n", op, cur);
                rsp->error = 1;
        }
        rsp->r[0] = 0;
    }
    return -1;
}

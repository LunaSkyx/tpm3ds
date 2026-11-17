/* Hand-written replacement for IMUSE.DLL's ADPCM block decoder (guest
 * 0x922630 at the port's IMUSE base 0x920000; 0x10002630 in the DLL).
 *
 * The lifted body is the hottest guest function while music plays (~13% of
 * all executed guest instructions in level 1 at 10 fps). tools/lifter keeps it
 * as imuse_00922630_ref for tests/test_imuse.c, which compares this function
 * with it: registers, eflags, the decoder state, the output, the one-time
 * table and every stack/argument slot the original writes.
 *
 * decode(state, out, src, nsamples, nchannels, keep_state), cdecl:
 *   state: per channel a step index byte at [ch], then int16 predictors at
 *          [2 + 2*ch] (zeroed first unless keep_state)
 *   src:   big-endian bit stream; samples interleave by channel in out.
 * Per sample: read 'bits' = BITS[step] bits, sign = top bit; the all-ones
 * value escapes to a literal 16-bit sample; otherwise delta = a 64-entry
 * row of the step table (built once at 0x93faf0) plus the step's rounding
 * term, added to or subtracted from the predictor with int16 saturation; the
 * step index moves by a per-width table and is clamped to 0..88. */
#include <stdint.h>
#include "core/cpu.h"
#include "core/gmem.h"

#define IM_INIT_FLAG  0x942770u
#define IM_DELTA_TAB  0x93faf0u   /* uint16 [89][64] */
#define IM_STEP_TAB   0x933100u   /* uint16 [89] (ends at 0x9331b2) */
#define IM_BITS_TAB   0x9331b8u   /* uint8 [89] */
#define IM_ADJ_PTRS   0x935030u   /* int8 *[bits] */

void imuse_00922630(cpu_t *cg)
{
    GMEM_LOCAL;
    const uint32_t esp0 = cg->r[ESP];
    const uint32_t ebp = esp0 - 4u;                 /* after push ebp; mov ebp, esp */
    uint32_t eax, ecx, edx, ebx = cg->r[EBX];

    /* prologue: push ebp; sub esp, 0x24; push ebx; push esi; push edi */
    GWR32(ebp, cg->r[EBP]);
    GWR32(ebp - 0x28u, cg->r[EBX]);
    GWR32(ebp - 0x2cu, cg->r[ESI]);
    GWR32(ebp - 0x30u, cg->r[EDI]);
    GWR32(ebp - 0x20u, 0u);                          /* bit position */

    eax = GRD32(IM_INIT_FLAG);
    if (eax == 0u) {
        /* one-time table: [i][b] = sum over the 6 bits of b of step[i] >> k */
        uint32_t w = 0u;                             /* last [ebp-4] value */
        for (uint32_t b = 0; b < 64u; b++) {
            for (uint32_t i = 0; i < 89u; i++) {
                uint32_t step = GRD16(IM_STEP_TAB + 2u * i), sum = 0u;
                w = step;
                for (uint32_t k = 0; k < 6u; k++) {
                    if (b & (0x20u >> k)) sum += w & 0xffffu;
                    w = (w & 0xffff0000u) | ((w & 0xffffu) >> 1);
                }
                GWR16(IM_DELTA_TAB + 2u * b + 0x80u * i, (uint16_t)sum);
            }
        }
        GWR32(ebp - 4u, w);
        GWR32(ebp - 0x10u, IM_DELTA_TAB + 0x80u);   /* final edi of the outer loop */
        GWR32(IM_INIT_FLAG, 1u);
        ebx = 64u;
    }

    const gptr state = GRD32(ebp + 8u);
    if (GRD32(ebp + 0x1cu) == 0u) {
        GWR8(state + 1u, 0); GWR8(state, 0);
        GWR16(state + 4u, 0); GWR16(state + 2u, 0);
    }
    gptr src = GRD32(ebp + 0x10u);                   /* esi: kept (pushed/popped) */
    const uint32_t src_arg = src;
    uint32_t bitbuf = ((uint32_t)GRD8(src) << 8) | GRD8(src + 1u);
    gptr cur = src + 2u;
    GWR32(ebp - 8u, 0u);
    GWR32(ebp - 0x14u, cur);
    GWR32(ebp - 0x1cu, bitbuf);
    uint32_t nch = GRD32(ebp + 0x18u);
    eax = state;
    ecx = nch;
    edx = bitbuf;
    if (nch != 0u) {
        gptr outp_ch = GRD32(ebp + 0xcu), predp = state + 2u;
        uint32_t bitpos = 0u, sign_var = GRD32(ebp - 0x24u);
        uint32_t ch = 0u, esi_reg = src_arg;          /* esi: src, from channel 1 on the output pointer */
        uint8_t bits_b = GRD8(ebp + 0x10u), mask_b = GRD8(ebp + 0x1cu);
        GWR32(ebp - 0xcu, outp_ch);
        GWR32(ebp - 0x10u, predp);
        for (;;) {
            int32_t stepi = (int8_t)GRD8(state + ch);
            uint32_t pred = (uint32_t)(int32_t)(int16_t)GRD16(predp);
            gptr outp = outp_ch;
            uint32_t ns = GRD32(ebp + 0x14u);
            uint32_t stride = nch << 1;
            GWR32(ebp - 0x34u, 0u);                  /* push edi (0) */
            GWR32(ebp - 0x38u, esi_reg);             /* push esi */
            while (ns != 0u) {
                ns--;
                uint32_t bits = GRD8(IM_BITS_TAB + (uint32_t)stepi);
                bits_b = (uint8_t)bits;
                uint32_t sign = 1u << ((bits - 1u) & 31u);
                mask_b = (uint8_t)(sign - 1u);
                bitpos += bits;
                uint32_t v = (bitbuf & 0xffffu) >> ((16u - bitpos) & 31u);
                uint32_t m = (sign & ~0xffu) | ((sign | mask_b) & 0xffu);
                uint32_t val = v & m;
                if ((int32_t)bitpos > 7) {
                    bitpos -= 8u;
                    bitbuf = (((bitbuf & 0xffffu) << 8) | GRD8(cur)) & 0xffffu;
                    cur++;
                }
                sign_var = sign;
                if (sign & val) val ^= sign;
                else sign_var = 0u;
                uint32_t r;
                if ((uint8_t)val == mask_b) {
                    /* escape: literal sample from the next 16 bits */
                    uint32_t d = bitbuf & 0xffffu;
                    r = (uint32_t)(int32_t)(int16_t)(d << (bitpos & 31u));
                    d = ((d << 8) & 0xff00u) | GRD8(cur); cur++;
                    uint32_t sh = (8u - bitpos) & 0xffu;
                    r = (r & ~0xffu) | ((d >> (sh & 31u)) & 0xffu);
                    d = ((d << 8) & 0xff00u) | GRD8(cur); cur++;
                    bitbuf = d;
                } else {
                    uint32_t idx = (val << ((7u - bits) & 31u)) | ((uint32_t)stepi << 6);
                    uint32_t delta = GRD16(IM_DELTA_TAB + idx * 2u);
                    if (val & 0xffffu)
                        delta += (uint32_t)(int32_t)(int16_t)GRD16(IM_STEP_TAB + (uint32_t)stepi * 2u) >> ((bits - 1u) & 31u);
                    if (sign_var & 0xffffu) {
                        r = pred - delta;
                        if (!((int32_t)r > -32768)) r = 0xffff8000u;
                    } else {
                        r = delta + pred;
                        if (!((int32_t)r < 0x7fff)) r = 0x7fffu;
                    }
                }
                pred = r;
                GWR16(outp, (uint16_t)r);
                outp += stride;
                int32_t adj = (int8_t)GRD8(GRD32(IM_ADJ_PTRS + bits * 4u) + val);
                stepi += adj;
                if (stepi <= 0) stepi = 0;
                else if (stepi >= 0x58) stepi = 0x58;
                ebx = val;
                GWR32(ebp - 0x3cu, ns);              /* push ecx */
            }
            /* channel epilogue */
            GWR8(state + ch, (uint8_t)stepi);
            GWR16(predp, (uint16_t)pred);
            GWR32(ebp - 4u, pred);
            GWR32(ebp - 0x18u, outp);
            GWR32(ebp + 0xcu, (uint32_t)stepi);
            nch = (nch << 1) >> 1;
            ch++;
            predp += 2u;
            outp_ch += 2u;
            esi_reg = outp_ch;                       /* mov esi, [ebp-0xc]; add esi, 2 */
            eax = ch;
            ecx = nch;
            edx = (state & 0xffff0000u) | (pred & 0xffffu);
            if (ch >= nch) break;
        }
        GWR32(ebp - 8u, ch);
        GWR32(ebp - 0xcu, outp_ch);
        GWR32(ebp - 0x10u, predp);
        GWR32(ebp - 0x14u, cur);
        GWR32(ebp - 0x1cu, bitbuf);
        GWR32(ebp - 0x20u, bitpos);
        GWR32(ebp - 0x24u, sign_var);
        GWR32(ebp + 0x18u, nch);
        GWR8(ebp + 0x10u, bits_b);
        GWR8(ebp + 0x1cu, mask_b);
    }
    (void)ebx;
    cg->r[EAX] = eax;
    cg->r[ECX] = ecx;
    cg->r[EDX] = edx;
    cg->r[ESP] = esp0 + 4u;                          /* ret; EBX/ESI/EDI/EBP restored */
}

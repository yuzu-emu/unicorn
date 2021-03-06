/*
 *  ARM translation: AArch32 VFP instructions
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *  Copyright (c) 2005-2007 CodeSourcery
 *  Copyright (c) 2007 OpenedHand, Ltd.
 *  Copyright (c) 2019 Linaro, Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This file is intended to be included from translate.c; it uses
 * some macros and definitions provided by that file.
 * It might be possible to convert it to a standalone .c file eventually.
 */

/* Include the generated VFP decoder */
#include "decode-vfp.inc.c"
#include "decode-vfp-uncond.inc.c"

/*
 * The imm8 encodes the sign bit, enough bits to represent an exponent in
 * the range 01....1xx to 10....0xx, and the most significant 4 bits of
 * the mantissa; see VFPExpandImm() in the v8 ARM ARM.
 */
uint64_t vfp_expand_imm(int size, uint8_t imm8)
{
    uint64_t imm;

    switch (size) {
    case MO_64:
        imm = (extract32(imm8, 7, 1) ? 0x8000 : 0) |
            (extract32(imm8, 6, 1) ? 0x3fc0 : 0x4000) |
            extract32(imm8, 0, 6);
        imm <<= 48;
        break;
    case MO_32:
        imm = (extract32(imm8, 7, 1) ? 0x8000 : 0) |
            (extract32(imm8, 6, 1) ? 0x3e00 : 0x4000) |
            (extract32(imm8, 0, 6) << 3);
        imm <<= 16;
        break;
    case MO_16:
        imm = (extract32(imm8, 7, 1) ? 0x8000 : 0) |
            (extract32(imm8, 6, 1) ? 0x3000 : 0x4000) |
            (extract32(imm8, 0, 6) << 6);
        break;
    default:
        g_assert_not_reached();
    }
    return imm;
}

/*
 * Return the offset of a 16-bit half of the specified VFP single-precision
 * register. If top is true, returns the top 16 bits; otherwise the bottom
 * 16 bits.
 */
static inline long vfp_f16_offset(unsigned reg, bool top)
{
    long offs = vfp_reg_offset(false, reg);
#ifdef HOST_WORDS_BIGENDIAN
    if (!top) {
        offs += 2;
    }
#else
    if (top) {
        offs += 2;
    }
#endif
    return offs;
}

/*
 * Generate code for M-profile lazy FP state preservation if needed;
 * this corresponds to the pseudocode PreserveFPState() function.
 */
static void gen_preserve_fp_state(DisasContext *s)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (s->v7m_lspact) {
        /*
         * Lazy state saving affects external memory and also the NVIC,
         * so we must mark it as an IO operation for icount (and cause
         * this to be the last insn in the TB).
         */
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            s->base.is_jmp = DISAS_UPDATE_EXIT;
            gen_io_start(tcg_ctx);
        }
        gen_helper_v7m_preserve_fp_state(tcg_ctx, tcg_ctx->cpu_env);
        /*
         * If the preserve_fp_state helper doesn't throw an exception
         * then it will clear LSPACT; we don't need to repeat this for
         * any further FP insns in this TB.
         */
        s->v7m_lspact = false;
    }
}

/*
 * Check that VFP access is enabled. If it is, do the necessary
 * M-profile lazy-FP handling and then return true.
 * If not, emit code to generate an appropriate exception and
 * return false.
 * The ignore_vfp_enabled argument specifies that we should ignore
 * whether VFP is enabled via FPEXC[EN]: this should be true for FMXR/FMRX
 * accesses to FPSID, FPEXC, MVFR0, MVFR1, MVFR2, and false for all other insns.
 */
static bool full_vfp_access_check(DisasContext *s, bool ignore_vfp_enabled)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (s->fp_excp_el) {
        /* M-profile handled this earlier, in disas_m_nocp() */
        assert (!arm_dc_feature(s, ARM_FEATURE_M));
        gen_exception_insn(s, s->pc_curr, EXCP_UDEF,
                           syn_fp_access_trap(1, 0xe, false),
                           s->fp_excp_el);
        return false;
    }

    if (!s->vfp_enabled && !ignore_vfp_enabled) {
        assert(!arm_dc_feature(s, ARM_FEATURE_M));
        unallocated_encoding(s);
        return false;
    }

    if (arm_dc_feature(s, ARM_FEATURE_M)) {
        /* Handle M-profile lazy FP state mechanics */

        /* Trigger lazy-state preservation if necessary */
        gen_preserve_fp_state(s);

        /* Update ownership of FP context: set FPCCR.S to match current state */
        if (s->v8m_fpccr_s_wrong) {
            TCGv_i32 tmp;

            tmp = load_cpu_field(s, v7m.fpccr[M_REG_S]);
            if (s->v8m_secure) {
                tcg_gen_ori_i32(tcg_ctx, tmp, tmp, R_V7M_FPCCR_S_MASK);
            } else {
                tcg_gen_andi_i32(tcg_ctx, tmp, tmp, ~R_V7M_FPCCR_S_MASK);
            }
            store_cpu_field(s, tmp, v7m.fpccr[M_REG_S]);
            /* Don't need to do this for any further FP insns in this TB */
            s->v8m_fpccr_s_wrong = false;
        }

        if (s->v7m_new_fp_ctxt_needed) {
            /*
             * Create new FP context by updating CONTROL.FPCA, CONTROL.SFPA
             * and the FPSCR.
             */
            TCGv_i32 control, fpscr;
            uint32_t bits = R_V7M_CONTROL_FPCA_MASK;

            fpscr = load_cpu_field(s, v7m.fpdscr[s->v8m_secure]);
            gen_helper_vfp_set_fpscr(tcg_ctx, tcg_ctx->cpu_env, fpscr);
            tcg_temp_free_i32(tcg_ctx, fpscr);
            /*
             * We don't need to arrange to end the TB, because the only
             * parts of FPSCR which we cache in the TB flags are the VECLEN
             * and VECSTRIDE, and those don't exist for M-profile.
             */

            if (s->v8m_secure) {
                bits |= R_V7M_CONTROL_SFPA_MASK;
            }
            control = load_cpu_field(s, v7m.control[M_REG_S]);
            tcg_gen_ori_i32(tcg_ctx, control, control, bits);
            store_cpu_field(s, control, v7m.control[M_REG_S]);
            /* Don't need to do this for any further FP insns in this TB */
            s->v7m_new_fp_ctxt_needed = false;
        }
    }

    return true;
}

/*
 * The most usual kind of VFP access check, for everything except
 * FMXR/FMRX to the always-available special registers.
 */
static bool vfp_access_check(DisasContext *s)
{
    return full_vfp_access_check(s, false);
}

static bool trans_VSEL(DisasContext *s, arg_VSEL *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t rd, rn, rm;
    int sz = a->sz;

    if (!dc_isar_feature(aa32_vsel, s)) {
        return false;
    }

    if (sz == 3 && !dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    if (sz == 1 && !dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (sz == 3 && !dc_isar_feature(aa32_simd_r32, s) &&
        ((a->vm | a->vn | a->vd) & 0x10)) {
        return false;
    }

    rd = a->vd;
    rn = a->vn;
    rm = a->vm;

    if (!vfp_access_check(s)) {
        return true;
    }

    if (sz == 3) {
        TCGv_i64 frn, frm, dest;
        TCGv_i64 tmp, zero, zf, nf, vf;

        zero = tcg_const_i64(tcg_ctx, 0);

        frn = tcg_temp_new_i64(tcg_ctx);
        frm = tcg_temp_new_i64(tcg_ctx);
        dest = tcg_temp_new_i64(tcg_ctx);

        zf = tcg_temp_new_i64(tcg_ctx);
        nf = tcg_temp_new_i64(tcg_ctx);
        vf = tcg_temp_new_i64(tcg_ctx);

        tcg_gen_extu_i32_i64(tcg_ctx, zf, tcg_ctx->cpu_ZF);
        tcg_gen_ext_i32_i64(tcg_ctx, nf, tcg_ctx->cpu_NF);
        tcg_gen_ext_i32_i64(tcg_ctx, vf, tcg_ctx->cpu_VF);

        vfp_load_reg64(s, frn, rn);
        vfp_load_reg64(s, frm, rm);
        switch (a->cc) {
        case 0: /* eq: Z */
            tcg_gen_movcond_i64(tcg_ctx, TCG_COND_EQ, dest, zf, zero,
                                frn, frm);
            break;
        case 1: /* vs: V */
            tcg_gen_movcond_i64(tcg_ctx, TCG_COND_LT, dest, vf, zero,
                                frn, frm);
            break;
        case 2: /* ge: N == V -> N ^ V == 0 */
            tmp = tcg_temp_new_i64(tcg_ctx);
            tcg_gen_xor_i64(tcg_ctx, tmp, vf, nf);
            tcg_gen_movcond_i64(tcg_ctx, TCG_COND_GE, dest, tmp, zero,
                                frn, frm);
            tcg_temp_free_i64(tcg_ctx, tmp);
            break;
        case 3: /* gt: !Z && N == V */
            tcg_gen_movcond_i64(tcg_ctx, TCG_COND_NE, dest, zf, zero,
                                frn, frm);
            tmp = tcg_temp_new_i64(tcg_ctx);
            tcg_gen_xor_i64(tcg_ctx, tmp, vf, nf);
            tcg_gen_movcond_i64(tcg_ctx, TCG_COND_GE, dest, tmp, zero,
                                dest, frm);
            tcg_temp_free_i64(tcg_ctx, tmp);
            break;
        }
        vfp_store_reg64(s, dest, rd);
        tcg_temp_free_i64(tcg_ctx, frn);
        tcg_temp_free_i64(tcg_ctx, frm);
        tcg_temp_free_i64(tcg_ctx, dest);

        tcg_temp_free_i64(tcg_ctx, zf);
        tcg_temp_free_i64(tcg_ctx, nf);
        tcg_temp_free_i64(tcg_ctx, vf);

        tcg_temp_free_i64(tcg_ctx, zero);
    } else {
        TCGv_i32 frn, frm, dest;
        TCGv_i32 tmp, zero;

        zero = tcg_const_i32(tcg_ctx, 0);

        frn = tcg_temp_new_i32(tcg_ctx);
        frm = tcg_temp_new_i32(tcg_ctx);
        dest = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg32(s, frn, rn);
        vfp_load_reg32(s, frm, rm);
        switch (a->cc) {
        case 0: /* eq: Z */
            tcg_gen_movcond_i32(tcg_ctx, TCG_COND_EQ, dest, tcg_ctx->cpu_ZF, zero,
                                frn, frm);
            break;
        case 1: /* vs: V */
            tcg_gen_movcond_i32(tcg_ctx, TCG_COND_LT, dest, tcg_ctx->cpu_VF, zero,
                                frn, frm);
            break;
        case 2: /* ge: N == V -> N ^ V == 0 */
            tmp = tcg_temp_new_i32(tcg_ctx);
            tcg_gen_xor_i32(tcg_ctx, tmp, tcg_ctx->cpu_VF, tcg_ctx->cpu_NF);
            tcg_gen_movcond_i32(tcg_ctx, TCG_COND_GE, dest, tmp, zero,
                                frn, frm);
            tcg_temp_free_i32(tcg_ctx, tmp);
            break;
        case 3: /* gt: !Z && N == V */
            tcg_gen_movcond_i32(tcg_ctx, TCG_COND_NE, dest, tcg_ctx->cpu_ZF, zero,
                                frn, frm);
            tmp = tcg_temp_new_i32(tcg_ctx);
            tcg_gen_xor_i32(tcg_ctx, tmp, tcg_ctx->cpu_VF, tcg_ctx->cpu_NF);
            tcg_gen_movcond_i32(tcg_ctx, TCG_COND_GE, dest, tmp, zero,
                                dest, frm);
            tcg_temp_free_i32(tcg_ctx, tmp);
            break;
        }
        /* For fp16 the top half is always zeroes */
        if (sz == 1) {
            tcg_gen_andi_i32(tcg_ctx, dest, dest, 0xffff);
        }
        vfp_store_reg32(s, dest, rd);
        tcg_temp_free_i32(tcg_ctx, frn);
        tcg_temp_free_i32(tcg_ctx, frm);
        tcg_temp_free_i32(tcg_ctx, dest);

        tcg_temp_free_i32(tcg_ctx, zero);
    }

    return true;
}

/*
 * Table for converting the most common AArch32 encoding of
 * rounding mode to arm_fprounding order (which matches the
 * common AArch64 order); see ARM ARM pseudocode FPDecodeRM().
 */
static const uint8_t fp_decode_rm[] = {
    FPROUNDING_TIEAWAY,
    FPROUNDING_TIEEVEN,
    FPROUNDING_POSINF,
    FPROUNDING_NEGINF,
};

static bool trans_VRINT(DisasContext *s, arg_VRINT *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t rd, rm;
    int sz = a->sz;
    TCGv_ptr fpst;
    TCGv_i32 tcg_rmode;
    int rounding = fp_decode_rm[a->rm];

    if (!dc_isar_feature(aa32_vrint, s)) {
        return false;
    }

    if (sz == 3 && !dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    if (sz == 1 && !dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (sz == 3 && !dc_isar_feature(aa32_simd_r32, s) &&
        ((a->vm | a->vd) & 0x10)) {
        return false;
    }

    rd = a->vd;
    rm = a->vm;

    if (!vfp_access_check(s)) {
        return true;
    }

    if (sz == 1) {
        fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);
    } else {
        fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    }


    tcg_rmode = tcg_const_i32(tcg_ctx, arm_rmode_to_sf(rounding));
    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);

    if (sz == 3) {
        TCGv_i64 tcg_op;
        TCGv_i64 tcg_res;
        tcg_op = tcg_temp_new_i64(tcg_ctx);
        tcg_res = tcg_temp_new_i64(tcg_ctx);
        vfp_load_reg64(s, tcg_op, rm);
        gen_helper_rintd(tcg_ctx, tcg_res, tcg_op, fpst);
        vfp_store_reg64(s, tcg_res, rd);
        tcg_temp_free_i64(tcg_ctx, tcg_op);
        tcg_temp_free_i64(tcg_ctx, tcg_res);
    } else {
        TCGv_i32 tcg_op;
        TCGv_i32 tcg_res;
        tcg_op = tcg_temp_new_i32(tcg_ctx);
        tcg_res = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg32(s, tcg_op, rm);
        if (sz == 1) {
            gen_helper_rinth(tcg_ctx, tcg_res, tcg_op, fpst);
        } else {
            gen_helper_rints(tcg_ctx, tcg_res, tcg_op, fpst);
        }
        vfp_store_reg32(s, tcg_res, rd);
        tcg_temp_free_i32(tcg_ctx, tcg_op);
        tcg_temp_free_i32(tcg_ctx, tcg_res);
    }

    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);
    tcg_temp_free_i32(tcg_ctx, tcg_rmode);

    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

static bool trans_VCVT(DisasContext *s, arg_VCVT *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t rd, rm;
    int sz = a->sz;
    TCGv_ptr fpst;
    TCGv_i32 tcg_rmode, tcg_shift;
    int rounding = fp_decode_rm[a->rm];
    bool is_signed = a->op;

    if (!dc_isar_feature(aa32_vcvt_dr, s)) {
        return false;
    }

    if (sz == 3 && !dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    if (sz == 1 && !dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (sz == 3 && !dc_isar_feature(aa32_simd_r32, s) && (a->vm & 0x10)) {
        return false;
    }

    rd = a->vd;
    rm = a->vm;

    if (!vfp_access_check(s)) {
        return true;
    }

    if (sz == 1) {
        fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);
    } else {
        fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    }

    tcg_shift = tcg_const_i32(tcg_ctx, 0);

    tcg_rmode = tcg_const_i32(tcg_ctx, arm_rmode_to_sf(rounding));
    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);

    if (sz == 3) {
        TCGv_i64 tcg_double, tcg_res;
        TCGv_i32 tcg_tmp;
        tcg_double = tcg_temp_new_i64(tcg_ctx);
        tcg_res = tcg_temp_new_i64(tcg_ctx);
        tcg_tmp = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg64(s, tcg_double, rm);
        if (is_signed) {
            gen_helper_vfp_tosld(tcg_ctx, tcg_res, tcg_double, tcg_shift, fpst);
        } else {
            gen_helper_vfp_tould(tcg_ctx, tcg_res, tcg_double, tcg_shift, fpst);
        }
        tcg_gen_extrl_i64_i32(tcg_ctx, tcg_tmp, tcg_res);
        vfp_store_reg32(s, tcg_tmp, rd);
        tcg_temp_free_i32(tcg_ctx, tcg_tmp);
        tcg_temp_free_i64(tcg_ctx, tcg_res);
        tcg_temp_free_i64(tcg_ctx, tcg_double);
    } else {
        TCGv_i32 tcg_single, tcg_res;
        tcg_single = tcg_temp_new_i32(tcg_ctx);
        tcg_res = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg32(s, tcg_single, rm);
        if (sz == 1) {
            if (is_signed) {
                gen_helper_vfp_toslh(tcg_ctx, tcg_res, tcg_single, tcg_shift, fpst);
            } else {
                gen_helper_vfp_toulh(tcg_ctx, tcg_res, tcg_single, tcg_shift, fpst);
            }
        } else {
            if (is_signed) {
                gen_helper_vfp_tosls(tcg_ctx, tcg_res, tcg_single, tcg_shift, fpst);
            } else {
                gen_helper_vfp_touls(tcg_ctx, tcg_res, tcg_single, tcg_shift, fpst);
            }
        }
        vfp_store_reg32(s, tcg_res, rd);
        tcg_temp_free_i32(tcg_ctx, tcg_res);
        tcg_temp_free_i32(tcg_ctx, tcg_single);
    }

    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);
    tcg_temp_free_i32(tcg_ctx, tcg_rmode);

    tcg_temp_free_i32(tcg_ctx, tcg_shift);

    tcg_temp_free_ptr(tcg_ctx, fpst);

    return true;
}

static bool trans_VMOV_to_gp(DisasContext *s, arg_VMOV_to_gp *a)
{
    /* VMOV scalar to general purpose register */
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    /* SIZE == MO_32 is a VFP instruction; otherwise NEON.  */
    if (a->size == MO_32
        ? !dc_isar_feature(aa32_fpsp_v2, s)
        : !arm_dc_feature(s, ARM_FEATURE_NEON)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vn & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i32(tcg_ctx);
    read_neon_element32(s, tmp, a->vn, a->index, a->size | (a->u ? 0 : MO_SIGN));
    store_reg(s, a->rt, tmp);

    return true;
}

static bool trans_VMOV_from_gp(DisasContext *s, arg_VMOV_from_gp *a)
{
    /* VMOV general purpose register to scalar */
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    /* SIZE == MO_32 is a VFP instruction; otherwise NEON.  */
    if (a->size == MO_32
        ? !dc_isar_feature(aa32_fpsp_v2, s)
        : !arm_dc_feature(s, ARM_FEATURE_NEON)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vn & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = load_reg(s, a->rt);
    write_neon_element32(s, tmp, a->vn, a->index, a->size);
    tcg_temp_free_i32(tcg_ctx, tmp);

    return true;
}

static bool trans_VDUP(DisasContext *s, arg_VDUP *a)
{
    /* VDUP (general purpose register) */
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    int size, vec_size;

    if (!arm_dc_feature(s, ARM_FEATURE_NEON)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vn & 0x10)) {
        return false;
    }

    if (a->b && a->e) {
        return false;
    }

    if (a->q && (a->vn & 1)) {
        return false;
    }

    vec_size = a->q ? 16 : 8;
    if (a->b) {
        size = 0;
    } else if (a->e) {
        size = 1;
    } else {
        size = 2;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = load_reg(s, a->rt);
    tcg_gen_gvec_dup_i32(tcg_ctx, size, neon_full_reg_offset(a->vn),
                         vec_size, vec_size, tmp);
    tcg_temp_free_i32(tcg_ctx, tmp);

    return true;
}

/*
 * M-profile provides two different sets of instructions that can
 * access floating point system registers: VMSR/VMRS (which move
 * to/from a general purpose register) and VLDR/VSTR sysreg (which
 * move directly to/from memory). In some cases there are also side
 * effects which must happen after any write to memory (which could
 * cause an exception). So we implement the common logic for the
 * sysreg access in gen_M_fp_sysreg_write() and gen_M_fp_sysreg_read(),
 * which take pointers to callback functions which will perform the
 * actual "read/write general purpose register" and "read/write
 * memory" operations.
 */

/*
 * Emit code to store the sysreg to its final destination; frees the
 * TCG temp 'value' it is passed.
 */
typedef void fp_sysreg_storefn(DisasContext *s, void *opaque, TCGv_i32 value);
/*
 * Emit code to load the value to be copied to the sysreg; returns
 * a new TCG temporary
 */
typedef TCGv_i32 fp_sysreg_loadfn(DisasContext *s, void *opaque);

/* Common decode/access checks for fp sysreg read/write */
typedef enum FPSysRegCheckResult {
    FPSysRegCheckFailed, /* caller should return false */
    FPSysRegCheckDone, /* caller should return true */
    FPSysRegCheckContinue, /* caller should continue generating code */
} FPSysRegCheckResult;

static FPSysRegCheckResult fp_sysreg_checks(DisasContext *s, int regno)
{
    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return FPSysRegCheckFailed;
    }

    switch (regno) {
    case ARM_VFP_FPSCR:
    case QEMU_VFP_FPSCR_NZCV:
        break;
    case ARM_VFP_FPSCR_NZCVQC:
        if (!arm_dc_feature(s, ARM_FEATURE_V8_1M)) {
            return false;
        }
        break;
    case ARM_VFP_FPCXT_S:
    case ARM_VFP_FPCXT_NS:
        if (!arm_dc_feature(s, ARM_FEATURE_V8_1M)) {
            return false;
        }
        if (!s->v8m_secure) {
            return false;
        }
        break;
    default:
        return FPSysRegCheckFailed;
    }

    /*
     * FPCXT_NS is a special case: it has specific handling for
     * "current FP state is inactive", and must do the PreserveFPState()
     * but not the usual full set of actions done by ExecuteFPCheck().
     * So we don't call vfp_access_check() and the callers must handle this.
     */
    if (regno != ARM_VFP_FPCXT_NS && !vfp_access_check(s)) {
        return FPSysRegCheckDone;
    }
    return FPSysRegCheckContinue;
}

static void gen_branch_fpInactive(DisasContext *s, TCGCond cond,
                                  TCGLabel *label)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    /*
     * FPCXT_NS is a special case: it has specific handling for
     * "current FP state is inactive", and must do the PreserveFPState()
     * but not the usual full set of actions done by ExecuteFPCheck().
     * We don't have a TB flag that matches the fpInactive check, so we
     * do it at runtime as we don't expect FPCXT_NS accesses to be frequent.
     *
     * Emit code that checks fpInactive and does a conditional
     * branch to label based on it:
     *  if cond is TCG_COND_NE then branch if fpInactive != 0 (ie if inactive)
     *  if cond is TCG_COND_EQ then branch if fpInactive == 0 (ie if active)
     */
    assert(cond == TCG_COND_EQ || cond == TCG_COND_NE);

    /* fpInactive = FPCCR_NS.ASPEN == 1 && CONTROL.FPCA == 0 */
    TCGv_i32 aspen, fpca;
    aspen = load_cpu_field(s, v7m.fpccr[M_REG_NS]);
    fpca = load_cpu_field(s, v7m.control[M_REG_S]);
    tcg_gen_andi_i32(tcg_ctx, aspen, aspen, R_V7M_FPCCR_ASPEN_MASK);
    tcg_gen_xori_i32(tcg_ctx, aspen, aspen, R_V7M_FPCCR_ASPEN_MASK);
    tcg_gen_andi_i32(tcg_ctx, fpca, fpca, R_V7M_CONTROL_FPCA_MASK);
    tcg_gen_or_i32(tcg_ctx, fpca, fpca, aspen);
    tcg_gen_brcondi_i32(tcg_ctx, tcg_invert_cond(cond), fpca, 0, label);
    tcg_temp_free_i32(tcg_ctx, aspen);
    tcg_temp_free_i32(tcg_ctx, fpca);
}

static bool gen_M_fp_sysreg_write(DisasContext *s, int regno,
                                  fp_sysreg_loadfn *loadfn,
                                  void *opaque)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    /* Do a write to an M-profile floating point system register */
    TCGv_i32 tmp;
    TCGLabel *lab_end = NULL;

    switch (fp_sysreg_checks(s, regno)) {
    case FPSysRegCheckFailed:
        return false;
    case FPSysRegCheckDone:
        return true;
    case FPSysRegCheckContinue:
        break;
    }

    switch (regno) {
    case ARM_VFP_FPSCR:
        tmp = loadfn(s, opaque);
        gen_helper_vfp_set_fpscr(tcg_ctx, tcg_ctx->cpu_env, tmp);
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_lookup_tb(s);
        break;
    case ARM_VFP_FPSCR_NZCVQC:
    {
        TCGv_i32 fpscr;
        tmp = loadfn(s, opaque);
        /*
         * TODO: when we implement MVE, write the QC bit.
         * For non-MVE, QC is RES0.
         */
        tcg_gen_andi_i32(tcg_ctx, tmp, tmp, FPCR_NZCV_MASK);
        fpscr = load_cpu_field(s, vfp.xregs[ARM_VFP_FPSCR]);
        tcg_gen_andi_i32(tcg_ctx, fpscr, fpscr, ~FPCR_NZCV_MASK);
        tcg_gen_or_i32(tcg_ctx, fpscr, fpscr, tmp);
        store_cpu_field(s, fpscr, vfp.xregs[ARM_VFP_FPSCR]);
        tcg_temp_free_i32(tcg_ctx, tmp);
        break;
    }
    case ARM_VFP_FPCXT_NS:
        lab_end = gen_new_label(tcg_ctx);
        /* fpInactive case: write is a NOP, so branch to end */
        gen_branch_fpInactive(s, TCG_COND_NE, lab_end);
        /* !fpInactive: PreserveFPState(), and reads same as FPCXT_S */
        gen_preserve_fp_state(s);
        /* fall through */
    case ARM_VFP_FPCXT_S:
    {
        TCGv_i32 sfpa, control;
        /*
         * Set FPSCR and CONTROL.SFPA from value; the new FPSCR takes
         * bits [27:0] from value and zeroes bits [31:28].
         */
        tmp = loadfn(s, opaque);
        sfpa = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_shri_i32(tcg_ctx, sfpa, tmp, 31);
        control = load_cpu_field(s, v7m.control[M_REG_S]);
        tcg_gen_deposit_i32(tcg_ctx, control, control, sfpa,
                            R_V7M_CONTROL_SFPA_SHIFT, 1);
        store_cpu_field(s, control, v7m.control[M_REG_S]);
        tcg_gen_andi_i32(tcg_ctx, tmp, tmp, ~FPCR_NZCV_MASK);
        gen_helper_vfp_set_fpscr(tcg_ctx, tcg_ctx->cpu_env, tmp);
        tcg_temp_free_i32(tcg_ctx, tmp);
        tcg_temp_free_i32(tcg_ctx, sfpa);
        break;
    }
    default:
        g_assert_not_reached();
    }
    if (lab_end) {
        gen_set_label(tcg_ctx, lab_end);
    }
    return true;
}

static bool gen_M_fp_sysreg_read(DisasContext *s, int regno,
                                fp_sysreg_storefn *storefn,
                                void *opaque)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    /* Do a read from an M-profile floating point system register */
    TCGv_i32 tmp;
    TCGLabel *lab_end = NULL;
    bool lookup_tb = false;

    switch (fp_sysreg_checks(s, regno)) {
    case FPSysRegCheckFailed:
        return false;
    case FPSysRegCheckDone:
        return true;
    case FPSysRegCheckContinue:
        break;
    }

    switch (regno) {
    case ARM_VFP_FPSCR:
        tmp = tcg_temp_new_i32(tcg_ctx);
        gen_helper_vfp_get_fpscr(tcg_ctx, tmp, tcg_ctx->cpu_env);
        storefn(s, opaque, tmp);
        break;
    case ARM_VFP_FPSCR_NZCVQC:
        /*
         * TODO: MVE has a QC bit, which we probably won't store
         * in the xregs[] field. For non-MVE, where QC is RES0,
         * we can just fall through to the FPSCR_NZCV case.
         */
    case QEMU_VFP_FPSCR_NZCV:
        /*
         * Read just NZCV; this is a special case to avoid the
         * helper call for the "VMRS to CPSR.NZCV" insn.
         */
        tmp = load_cpu_field(s, vfp.xregs[ARM_VFP_FPSCR]);
        tcg_gen_andi_i32(tcg_ctx, tmp, tmp, FPCR_NZCV_MASK);
        storefn(s, opaque, tmp);
        break;
    case ARM_VFP_FPCXT_S:
    {
        TCGv_i32 control, sfpa, fpscr;
        /* Bits [27:0] from FPSCR, bit [31] from CONTROL.SFPA */
        tmp = tcg_temp_new_i32(tcg_ctx);
        sfpa = tcg_temp_new_i32(tcg_ctx);
        gen_helper_vfp_get_fpscr(tcg_ctx, tmp, tcg_ctx->cpu_env);
        tcg_gen_andi_i32(tcg_ctx, tmp, tmp, ~FPCR_NZCV_MASK);
        control = load_cpu_field(s, v7m.control[M_REG_S]);
        tcg_gen_andi_i32(tcg_ctx, sfpa, control, R_V7M_CONTROL_SFPA_MASK);
        tcg_gen_shli_i32(tcg_ctx, sfpa, sfpa, 31 - R_V7M_CONTROL_SFPA_SHIFT);
        tcg_gen_or_i32(tcg_ctx, tmp, tmp, sfpa);
        tcg_temp_free_i32(tcg_ctx, sfpa);
        /*
         * Store result before updating FPSCR etc, in case
         * it is a memory write which causes an exception.
         */
        storefn(s, opaque, tmp);
        /*
         * Now we must reset FPSCR from FPDSCR_NS, and clear
         * CONTROL.SFPA; so we'll end the TB here.
         */
        tcg_gen_andi_i32(tcg_ctx, control, control, ~R_V7M_CONTROL_SFPA_MASK);
        store_cpu_field(s, control, v7m.control[M_REG_S]);
        fpscr = load_cpu_field(s, v7m.fpdscr[M_REG_NS]);
        gen_helper_vfp_set_fpscr(tcg_ctx, tcg_ctx->cpu_env, fpscr);
        tcg_temp_free_i32(tcg_ctx, fpscr);
        lookup_tb = true;
        break;
    }
    case ARM_VFP_FPCXT_NS:
    {
        TCGv_i32 control, sfpa, fpscr, fpdscr, zero;
        TCGLabel *lab_active = gen_new_label(tcg_ctx);

        lookup_tb = true;

        gen_branch_fpInactive(s, TCG_COND_EQ, lab_active);
        /* fpInactive case: reads as FPDSCR_NS */
        TCGv_i32 tmp = load_cpu_field(s, v7m.fpdscr[M_REG_NS]);
        storefn(s, opaque, tmp);
        lab_end = gen_new_label(tcg_ctx);
        tcg_gen_br(tcg_ctx, lab_end);

        gen_set_label(tcg_ctx, lab_active);
        /* !fpInactive: Reads the same as FPCXT_S, but side effects differ */
        gen_preserve_fp_state(s);
        tmp = tcg_temp_new_i32(tcg_ctx);
        sfpa = tcg_temp_new_i32(tcg_ctx);
        fpscr = tcg_temp_new_i32(tcg_ctx);
        gen_helper_vfp_get_fpscr(tcg_ctx, fpscr, tcg_ctx->cpu_env);
        tcg_gen_andi_i32(tcg_ctx, tmp, fpscr, ~FPCR_NZCV_MASK);
        control = load_cpu_field(s, v7m.control[M_REG_S]);
        tcg_gen_andi_i32(tcg_ctx, sfpa, control, R_V7M_CONTROL_SFPA_MASK);
        tcg_gen_shli_i32(tcg_ctx, sfpa, sfpa, 31 - R_V7M_CONTROL_SFPA_SHIFT);
        tcg_gen_or_i32(tcg_ctx, tmp, tmp, sfpa);
        tcg_temp_free_i32(tcg_ctx, control);
        /* Store result before updating FPSCR, in case it faults */
        storefn(s, opaque, tmp);
        /* If SFPA is zero then set FPSCR from FPDSCR_NS */
        fpdscr = load_cpu_field(s, v7m.fpdscr[M_REG_NS]);
        zero = tcg_const_i32(tcg_ctx, 0);
        tcg_gen_movcond_i32(tcg_ctx, TCG_COND_EQ, fpscr, sfpa, zero, fpdscr, fpscr);
        gen_helper_vfp_set_fpscr(tcg_ctx, tcg_ctx->cpu_env, fpscr);
        tcg_temp_free_i32(tcg_ctx, zero);
        tcg_temp_free_i32(tcg_ctx, sfpa);
        tcg_temp_free_i32(tcg_ctx, fpdscr);
        tcg_temp_free_i32(tcg_ctx, fpscr);
        break;
    }
    default:
        g_assert_not_reached();
    }

    if (lab_end) {
        gen_set_label(tcg_ctx, lab_end);
    }
    if (lookup_tb) {
        gen_lookup_tb(s);
    }
    return true;
}

static void fp_sysreg_to_gpr(DisasContext *s, void *opaque, TCGv_i32 value)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    arg_VMSR_VMRS *a = opaque;

    if (a->rt == 15) {
        /* Set the 4 flag bits in the CPSR */
        gen_set_nzcv(s, value);
        tcg_temp_free_i32(tcg_ctx, value);
    } else {
        store_reg(s, a->rt, value);
    }
}

static TCGv_i32 gpr_to_fp_sysreg(DisasContext *s, void *opaque)
{
    arg_VMSR_VMRS *a = opaque;

    return load_reg(s, a->rt);
}

static bool gen_M_VMSR_VMRS(DisasContext *s, arg_VMSR_VMRS *a)
{
    /*
     * Accesses to R15 are UNPREDICTABLE; we choose to undef.
     * FPSCR -> r15 is a special case which writes to the PSR flags;
     * set a->reg to a special value to tell gen_M_fp_sysreg_read()
     * we only care about the top 4 bits of FPSCR there.
     */
    if (a->rt == 15) {
        if (a->l && a->reg == ARM_VFP_FPSCR) {
            a->reg = QEMU_VFP_FPSCR_NZCV;
        } else {
            return false;
        }
    }

    if (a->l) {
        /* VMRS, move FP system register to gp register */
        return gen_M_fp_sysreg_read(s, a->reg, fp_sysreg_to_gpr, a);
    } else {
        /* VMSR, move gp register to FP system register */
        return gen_M_fp_sysreg_write(s, a->reg, gpr_to_fp_sysreg, a);
    }
}

static bool trans_VMSR_VMRS(DisasContext *s, arg_VMSR_VMRS *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    bool ignore_vfp_enabled = false;

    if (arm_dc_feature(s, ARM_FEATURE_M)) {
        return gen_M_VMSR_VMRS(s, a);
    }

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    switch (a->reg) {
    case ARM_VFP_FPSID:
        /*
         * VFPv2 allows access to FPSID from userspace; VFPv3 restricts
         * all ID registers to privileged access only.
         */
        if (IS_USER(s) && dc_isar_feature(aa32_fpsp_v3, s)) {
            return false;
        }
        ignore_vfp_enabled = true;
        break;
    case ARM_VFP_MVFR0:
    case ARM_VFP_MVFR1:
        if (IS_USER(s) || !arm_dc_feature(s, ARM_FEATURE_MVFR)) {
            return false;
        }
        ignore_vfp_enabled = true;
        break;
    case ARM_VFP_MVFR2:
        if (IS_USER(s) || !arm_dc_feature(s, ARM_FEATURE_V8)) {
            return false;
        }
        ignore_vfp_enabled = true;
        break;
    case ARM_VFP_FPSCR:
        break;
    case ARM_VFP_FPEXC:
        if (IS_USER(s)) {
            return false;
        }
        ignore_vfp_enabled = true;
        break;
    case ARM_VFP_FPINST:
    case ARM_VFP_FPINST2:
        /* Not present in VFPv3 */
        if (IS_USER(s) || dc_isar_feature(aa32_fpsp_v3, s)) {
            return false;
        }
        break;
    default:
        return false;
    }

    if (!full_vfp_access_check(s, ignore_vfp_enabled)) {
        return true;
    }

    if (a->l) {
        /* VMRS, move VFP special register to gp register */
        switch (a->reg) {
        case ARM_VFP_MVFR0:
        case ARM_VFP_MVFR1:
        case ARM_VFP_MVFR2:
            if (s->current_el == 1) {
                TCGv_i32 tcg_reg, tcg_rt;

                gen_set_condexec(s);
                gen_set_pc_im(s, s->pc_curr);
                tcg_reg = tcg_const_i32(tcg_ctx, a->reg);
                tcg_rt = tcg_const_i32(tcg_ctx, a->rt);
                gen_helper_check_hcr_el2_trap(tcg_ctx, tcg_ctx->cpu_env, tcg_rt, tcg_reg);
                tcg_temp_free_i32(tcg_ctx, tcg_reg);
                tcg_temp_free_i32(tcg_ctx, tcg_rt);
            }
            /* fall through */
        case ARM_VFP_FPSID:
        case ARM_VFP_FPEXC:
        case ARM_VFP_FPINST:
        case ARM_VFP_FPINST2:
            tmp = load_cpu_field(s, vfp.xregs[a->reg]);
            break;
        case ARM_VFP_FPSCR:
            if (a->rt == 15) {
                tmp = load_cpu_field(s, vfp.xregs[ARM_VFP_FPSCR]);
                tcg_gen_andi_i32(tcg_ctx, tmp, tmp, FPCR_NZCV_MASK);
            } else {
                tmp = tcg_temp_new_i32(tcg_ctx);
                gen_helper_vfp_get_fpscr(tcg_ctx, tmp, tcg_ctx->cpu_env);
            }
            break;
        default:
            g_assert_not_reached();
        }

        if (a->rt == 15) {
            /* Set the 4 flag bits in the CPSR.  */
            gen_set_nzcv(s, tmp);
            tcg_temp_free_i32(tcg_ctx, tmp);
        } else {
            store_reg(s, a->rt, tmp);
        }
    } else {
        /* VMSR, move gp register to VFP special register */
        switch (a->reg) {
        case ARM_VFP_FPSID:
        case ARM_VFP_MVFR0:
        case ARM_VFP_MVFR1:
        case ARM_VFP_MVFR2:
            /* Writes are ignored.  */
            break;
        case ARM_VFP_FPSCR:
            tmp = load_reg(s, a->rt);
            gen_helper_vfp_set_fpscr(tcg_ctx, tcg_ctx->cpu_env, tmp);
            tcg_temp_free_i32(tcg_ctx, tmp);
            gen_lookup_tb(s);
            break;
        case ARM_VFP_FPEXC:
            /*
             * TODO: VFP subarchitecture support.
             * For now, keep the EN bit only
             */
            tmp = load_reg(s, a->rt);
            tcg_gen_andi_i32(tcg_ctx, tmp, tmp, 1 << 30);
            store_cpu_field(s, tmp, vfp.xregs[a->reg]);
            gen_lookup_tb(s);
            break;
        case ARM_VFP_FPINST:
        case ARM_VFP_FPINST2:
            tmp = load_reg(s, a->rt);
            store_cpu_field(s, tmp, vfp.xregs[a->reg]);
            break;
        default:
            g_assert_not_reached();
        }
    }

    return true;
}

static void fp_sysreg_to_memory(DisasContext *s, void *opaque, TCGv_i32 value)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    arg_vldr_sysreg *a = opaque;
    uint32_t offset = a->imm;
    TCGv_i32 addr;

    if (!a->a) {
        offset = - offset;
    }

    addr = load_reg(s, a->rn);
    if (a->p) {
        tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
    }

    if (s->v8m_stackcheck && a->rn == 13 && a->w) {
        gen_helper_v8m_stackcheck(tcg_ctx, tcg_ctx->cpu_env, addr);
    }

    gen_aa32_st_i32(s, value, addr, get_mem_index(s),
                    MO_UL | MO_ALIGN | s->be_data);
    tcg_temp_free_i32(tcg_ctx, value);

    if (a->w) {
        /* writeback */
        if (!a->p) {
            tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
        }
        store_reg(s, a->rn, addr);
    } else {
        tcg_temp_free_i32(tcg_ctx, addr);
    }
}

static TCGv_i32 memory_to_fp_sysreg(DisasContext *s, void *opaque)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    arg_vldr_sysreg *a = opaque;
    uint32_t offset = a->imm;
    TCGv_i32 addr;
    TCGv_i32 value = tcg_temp_new_i32(tcg_ctx);

    if (!a->a) {
        offset = - offset;
    }

    addr = load_reg(s, a->rn);
    if (a->p) {
        tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
    }

    if (s->v8m_stackcheck && a->rn == 13 && a->w) {
        gen_helper_v8m_stackcheck(tcg_ctx, tcg_ctx->cpu_env, addr);
    }

    gen_aa32_ld_i32(s, value, addr, get_mem_index(s),
                    MO_UL | MO_ALIGN | s->be_data);

    if (a->w) {
        /* writeback */
        if (!a->p) {
            tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
        }
        store_reg(s, a->rn, addr);
    } else {
        tcg_temp_free_i32(tcg_ctx, addr);
    }
    return value;
}

static bool trans_VLDR_sysreg(DisasContext *s, arg_vldr_sysreg *a)
{
    if (!arm_dc_feature(s, ARM_FEATURE_V8_1M)) {
        return false;
    }
    if (a->rn == 15) {
        return false;
    }
    return gen_M_fp_sysreg_write(s, a->reg, memory_to_fp_sysreg, a);
}

static bool trans_VSTR_sysreg(DisasContext *s, arg_vldr_sysreg *a)
{
    if (!arm_dc_feature(s, ARM_FEATURE_V8_1M)) {
        return false;
    }
    if (a->rn == 15) {
        return false;
    }
    return gen_M_fp_sysreg_read(s, a->reg, fp_sysreg_to_memory, a);
}

static bool trans_VMOV_half(DisasContext *s, arg_VMOV_single *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (a->rt == 15) {
        /* UNPREDICTABLE; we choose to UNDEF */
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    if (a->l) {
        /* VFP to general purpose register */
        tmp = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg32(s, tmp, a->vn);
        tcg_gen_andi_i32(tcg_ctx, tmp, tmp, 0xffff);
        store_reg(s, a->rt, tmp);
    } else {
        /* general purpose register to VFP */
        tmp = load_reg(s, a->rt);
        tcg_gen_andi_i32(tcg_ctx, tmp, tmp, 0xffff);
        vfp_store_reg32(s, tmp, a->vn);
        tcg_temp_free_i32(tcg_ctx, tmp);
    }

    return true;
}

static bool trans_VMOV_single(DisasContext *s, arg_VMOV_single *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    if (a->l) {
        /* VFP to general purpose register */
        tmp = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg32(s, tmp, a->vn);
        if (a->rt == 15) {
            /* Set the 4 flag bits in the CPSR.  */
            gen_set_nzcv(s, tmp);
            tcg_temp_free_i32(tcg_ctx, tmp);
        } else {
            store_reg(s, a->rt, tmp);
        }
    } else {
        /* general purpose register to VFP */
        tmp = load_reg(s, a->rt);
        vfp_store_reg32(s, tmp, a->vn);
        tcg_temp_free_i32(tcg_ctx, tmp);
    }

    return true;
}

static bool trans_VMOV_64_sp(DisasContext *s, arg_VMOV_64_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    /*
     * VMOV between two general-purpose registers and two single precision
     * floating point registers
     */
    if (!vfp_access_check(s)) {
        return true;
    }

    if (a->op) {
        /* fpreg to gpreg */
        tmp = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg32(s, tmp, a->vm);
        store_reg(s, a->rt, tmp);
        tmp = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg32(s, tmp, a->vm + 1);
        store_reg(s, a->rt2, tmp);
    } else {
        /* gpreg to fpreg */
        tmp = load_reg(s, a->rt);
        vfp_store_reg32(s, tmp, a->vm);
        tcg_temp_free_i32(tcg_ctx, tmp);
        tmp = load_reg(s, a->rt2);
        vfp_store_reg32(s, tmp, a->vm + 1);
        tcg_temp_free_i32(tcg_ctx, tmp);
    }

    return true;
}

static bool trans_VMOV_64_dp(DisasContext *s, arg_VMOV_64_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    /*
     * VMOV between two general-purpose registers and one double precision
     * floating point register.  Note that this does not require support
     * for double precision arithmetic.
     */
    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vm & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    if (a->op) {
        /* fpreg to gpreg */
        tmp = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg32(s, tmp, a->vm * 2);
        store_reg(s, a->rt, tmp);
        tmp = tcg_temp_new_i32(tcg_ctx);
        vfp_load_reg32(s, tmp, a->vm * 2 + 1);
        store_reg(s, a->rt2, tmp);
    } else {
        /* gpreg to fpreg */
        tmp = load_reg(s, a->rt);
        vfp_store_reg32(s, tmp, a->vm * 2);
        tcg_temp_free_i32(tcg_ctx, tmp);
        tmp = load_reg(s, a->rt2);
        vfp_store_reg32(s, tmp, a->vm * 2 + 1);
        tcg_temp_free_i32(tcg_ctx, tmp);
    }

    return true;
}

static bool trans_VLDR_VSTR_hp(DisasContext *s, arg_VLDR_VSTR_sp *a)
{
    uint32_t offset;
    TCGv_i32 addr, tmp;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    /* imm8 field is offset/2 for fp16, unlike fp32 and fp64 */
    offset = a->imm << 1;
    if (!a->u) {
        offset = -offset;
    }

    /* For thumb, use of PC is UNPREDICTABLE.  */
    addr = add_reg_for_lit(s, a->rn, offset);
    tmp = tcg_temp_new_i32(tcg_ctx);
    if (a->l) {
        gen_aa32_ld16u(s, tmp, addr, get_mem_index(s));
        vfp_store_reg32(s, tmp, a->vd);
    } else {
        vfp_load_reg32(s, tmp, a->vd);
        gen_aa32_st16(s, tmp, addr, get_mem_index(s));
    }
    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, addr);

    return true;
}

static bool trans_VLDR_VSTR_sp(DisasContext *s, arg_VLDR_VSTR_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t offset;
    TCGv_i32 addr, tmp;

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    offset = a->imm << 2;
    if (!a->u) {
        offset = -offset;
    }

    /* For thumb, use of PC is UNPREDICTABLE.  */
    addr = add_reg_for_lit(s, a->rn, offset);
    tmp = tcg_temp_new_i32(tcg_ctx);
    if (a->l) {
        gen_aa32_ld32u(s, tmp, addr, get_mem_index(s));
        vfp_store_reg32(s, tmp, a->vd);
    } else {
        vfp_load_reg32(s, tmp, a->vd);
        gen_aa32_st32(s, tmp, addr, get_mem_index(s));
    }
    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, addr);

    return true;
}

static bool trans_VLDR_VSTR_dp(DisasContext *s, arg_VLDR_VSTR_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t offset;
    TCGv_i32 addr;
    TCGv_i64 tmp;

    /* Note that this does not require support for double arithmetic.  */
    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vd & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    offset = a->imm << 2;
    if (!a->u) {
        offset = -offset;
    }

    /* For thumb, use of PC is UNPREDICTABLE.  */
    addr = add_reg_for_lit(s, a->rn, offset);
    tmp = tcg_temp_new_i64(tcg_ctx);
    if (a->l) {
        gen_aa32_ld64(s, tmp, addr, get_mem_index(s));
        vfp_store_reg64(s, tmp, a->vd);
    } else {
        vfp_load_reg64(s, tmp, a->vd);
        gen_aa32_st64(s, tmp, addr, get_mem_index(s));
    }
    tcg_temp_free_i64(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, addr);

    return true;
}

static bool trans_VLDM_VSTM_sp(DisasContext *s, arg_VLDM_VSTM_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t offset;
    TCGv_i32 addr, tmp;
    int i, n;

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    n = a->imm;

    if (n == 0 || (a->vd + n) > 32) {
        /*
         * UNPREDICTABLE cases for bad immediates: we choose to
         * UNDEF to avoid generating huge numbers of TCG ops
         */
        return false;
    }
    if (a->rn == 15 && a->w) {
        /* writeback to PC is UNPREDICTABLE, we choose to UNDEF */
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    /* For thumb, use of PC is UNPREDICTABLE.  */
    addr = add_reg_for_lit(s, a->rn, 0);
    if (a->p) {
        /* pre-decrement */
        tcg_gen_addi_i32(tcg_ctx, addr, addr, -(a->imm << 2));
    }

    if (s->v8m_stackcheck && a->rn == 13 && a->w) {
        /*
         * Here 'addr' is the lowest address we will store to,
         * and is either the old SP (if post-increment) or
         * the new SP (if pre-decrement). For post-increment
         * where the old value is below the limit and the new
         * value is above, it is UNKNOWN whether the limit check
         * triggers; we choose to trigger.
         */
        gen_helper_v8m_stackcheck(tcg_ctx, tcg_ctx->cpu_env, addr);
    }

    offset = 4;
    tmp = tcg_temp_new_i32(tcg_ctx);
    for (i = 0; i < n; i++) {
        if (a->l) {
            /* load */
            gen_aa32_ld32u(s, tmp, addr, get_mem_index(s));
            vfp_store_reg32(s, tmp, a->vd + i);
        } else {
            /* store */
            vfp_load_reg32(s, tmp, a->vd + i);
            gen_aa32_st32(s, tmp, addr, get_mem_index(s));
        }
        tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
    }
    tcg_temp_free_i32(tcg_ctx, tmp);
    if (a->w) {
        /* writeback */
        if (a->p) {
            offset = -offset * n;
            tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
        }
        store_reg(s, a->rn, addr);
    } else {
        tcg_temp_free_i32(tcg_ctx, addr);
    }

    return true;
}

static bool trans_VLDM_VSTM_dp(DisasContext *s, arg_VLDM_VSTM_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t offset;
    TCGv_i32 addr;
    TCGv_i64 tmp;
    int i, n;

    /* Note that this does not require support for double arithmetic.  */
    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    n = a->imm >> 1;

    if (n == 0 || (a->vd + n) > 32 || n > 16) {
        /*
         * UNPREDICTABLE cases for bad immediates: we choose to
         * UNDEF to avoid generating huge numbers of TCG ops
         */
        return false;
    }
    if (a->rn == 15 && a->w) {
        /* writeback to PC is UNPREDICTABLE, we choose to UNDEF */
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vd + n) > 16) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    /* For thumb, use of PC is UNPREDICTABLE.  */
    addr = add_reg_for_lit(s, a->rn, 0);
    if (a->p) {
        /* pre-decrement */
        tcg_gen_addi_i32(tcg_ctx, addr, addr, -(a->imm << 2));
    }

    if (s->v8m_stackcheck && a->rn == 13 && a->w) {
        /*
         * Here 'addr' is the lowest address we will store to,
         * and is either the old SP (if post-increment) or
         * the new SP (if pre-decrement). For post-increment
         * where the old value is below the limit and the new
         * value is above, it is UNKNOWN whether the limit check
         * triggers; we choose to trigger.
         */
        gen_helper_v8m_stackcheck(tcg_ctx, tcg_ctx->cpu_env, addr);
    }

    offset = 8;
    tmp = tcg_temp_new_i64(tcg_ctx);
    for (i = 0; i < n; i++) {
        if (a->l) {
            /* load */
            gen_aa32_ld64(s, tmp, addr, get_mem_index(s));
            vfp_store_reg64(s, tmp, a->vd + i);
        } else {
            /* store */
            vfp_load_reg64(s, tmp, a->vd + i);
            gen_aa32_st64(s, tmp, addr, get_mem_index(s));
        }
        tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
    }
    tcg_temp_free_i64(tcg_ctx, tmp);
    if (a->w) {
        /* writeback */
        if (a->p) {
            offset = -offset * n;
        } else if (a->imm & 1) {
            offset = 4;
        } else {
            offset = 0;
        }

        if (offset != 0) {
            tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
        }
        store_reg(s, a->rn, addr);
    } else {
        tcg_temp_free_i32(tcg_ctx, addr);
    }

    return true;
}

/*
 * Types for callbacks for do_vfp_3op_sp() and do_vfp_3op_dp().
 * The callback should emit code to write a value to vd. If
 * do_vfp_3op_{sp,dp}() was passed reads_vd then the TCGv vd
 * will contain the old value of the relevant VFP register;
 * otherwise it must be written to only.
 */
typedef void VFPGen3OpSPFn(TCGContext *, TCGv_i32 vd,
                           TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst);
typedef void VFPGen3OpDPFn(TCGContext *, TCGv_i64 vd,
                           TCGv_i64 vn, TCGv_i64 vm, TCGv_ptr fpst);

/*
 * Types for callbacks for do_vfp_2op_sp() and do_vfp_2op_dp().
 * The callback should emit code to write a value to vd (which
 * should be written to only).
 */
typedef void VFPGen2OpSPFn(TCGContext *, TCGv_i32 vd, TCGv_i32 vm);
typedef void VFPGen2OpDPFn(TCGContext *, TCGv_i64 vd, TCGv_i64 vm);

/*
 * Return true if the specified S reg is in a scalar bank
 * (ie if it is s0..s7)
 */
static inline bool vfp_sreg_is_scalar(int reg)
{
    return (reg & 0x18) == 0;
}

/*
 * Return true if the specified D reg is in a scalar bank
 * (ie if it is d0..d3 or d16..d19)
 */
static inline bool vfp_dreg_is_scalar(int reg)
{
    return (reg & 0xc) == 0;
}

/*
 * Advance the S reg number forwards by delta within its bank
 * (ie increment the low 3 bits but leave the rest the same)
 */
static inline int vfp_advance_sreg(int reg, int delta)
{
    return ((reg + delta) & 0x7) | (reg & ~0x7);
}

/*
 * Advance the D reg number forwards by delta within its bank
 * (ie increment the low 2 bits but leave the rest the same)
 */
static inline int vfp_advance_dreg(int reg, int delta)
{
    return ((reg + delta) & 0x3) | (reg & ~0x3);
}

/*
 * Perform a 3-operand VFP data processing instruction. fn is the
 * callback to do the actual operation; this function deals with the
 * code to handle looping around for VFP vector processing.
 */
static bool do_vfp_3op_sp(DisasContext *s, VFPGen3OpSPFn *fn,
                          int vd, int vn, int vm, bool reads_vd)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t delta_m = 0;
    uint32_t delta_d = 0;
    int veclen = s->vec_len;
    TCGv_i32 f0, f1, fd;
    TCGv_ptr fpst;

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    if (!dc_isar_feature(aa32_fpshvec, s) &&
        (veclen != 0 || s->vec_stride != 0)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    if (veclen > 0) {
        /* Figure out what type of vector operation this is.  */
        if (vfp_sreg_is_scalar(vd)) {
            /* scalar */
            veclen = 0;
        } else {
            delta_d = s->vec_stride + 1;

            if (vfp_sreg_is_scalar(vm)) {
                /* mixed scalar/vector */
                delta_m = 0;
            } else {
                /* vector */
                delta_m = delta_d;
            }
        }
    }

    f0 = tcg_temp_new_i32(tcg_ctx);
    f1 = tcg_temp_new_i32(tcg_ctx);
    fd = tcg_temp_new_i32(tcg_ctx);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);

    vfp_load_reg32(s, f0, vn);
    vfp_load_reg32(s, f1, vm);

    for (;;) {
        if (reads_vd) {
            vfp_load_reg32(s, fd, vd);
        }
        fn(tcg_ctx, fd, f0, f1, fpst);
        vfp_store_reg32(s, fd, vd);

        if (veclen == 0) {
            break;
        }

        /* Set up the operands for the next iteration */
        veclen--;
        vd = vfp_advance_sreg(vd, delta_d);
        vn = vfp_advance_sreg(vn, delta_d);
        vfp_load_reg32(s, f0, vn);
        if (delta_m) {
            vm = vfp_advance_sreg(vm, delta_m);
            vfp_load_reg32(s, f1, vm);
        }
    }

    tcg_temp_free_i32(tcg_ctx, f0);
    tcg_temp_free_i32(tcg_ctx, f1);
    tcg_temp_free_i32(tcg_ctx, fd);
    tcg_temp_free_ptr(tcg_ctx, fpst);

    return true;
}

static bool do_vfp_3op_hp(DisasContext *s, VFPGen3OpSPFn *fn,
                          int vd, int vn, int vm, bool reads_vd)
{
    /*
     * Do a half-precision operation. Functionally this is
     * the same as do_vfp_3op_sp(), except:
     *  - it uses the FPST_FPCR_F16
     *  - it doesn't need the VFP vector handling (fp16 is a
     *    v8 feature, and in v8 VFP vectors don't exist)
     *  - it does the aa32_fp16_arith feature test
     */
    TCGv_i32 f0, f1, fd;
    TCGv_ptr fpst;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (s->vec_len != 0 || s->vec_stride != 0) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    f0 = tcg_temp_new_i32(tcg_ctx);
    f1 = tcg_temp_new_i32(tcg_ctx);
    fd = tcg_temp_new_i32(tcg_ctx);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);

    vfp_load_reg32(s, f0, vn);
    vfp_load_reg32(s, f1, vm);

    if (reads_vd) {
        vfp_load_reg32(s, fd, vd);
    }
    fn(tcg_ctx, fd, f0, f1, fpst);
    vfp_store_reg32(s, fd, vd);

    tcg_temp_free_i32(tcg_ctx, f0);
    tcg_temp_free_i32(tcg_ctx, f1);
    tcg_temp_free_i32(tcg_ctx, fd);
    tcg_temp_free_ptr(tcg_ctx, fpst);

    return true;
}

static bool do_vfp_3op_dp(DisasContext *s, VFPGen3OpDPFn *fn,
                          int vd, int vn, int vm, bool reads_vd)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t delta_m = 0;
    uint32_t delta_d = 0;
    int veclen = s->vec_len;
    TCGv_i64 f0, f1, fd;
    TCGv_ptr fpst;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (!dc_isar_feature(aa32_simd_r32, s) && ((vd | vn | vm) & 0x10)) {
        return false;
    }

    if (!dc_isar_feature(aa32_fpshvec, s) &&
        (veclen != 0 || s->vec_stride != 0)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    if (veclen > 0) {
        /* Figure out what type of vector operation this is.  */
        if (vfp_dreg_is_scalar(vd)) {
            /* scalar */
            veclen = 0;
        } else {
            delta_d = (s->vec_stride >> 1) + 1;

            if (vfp_dreg_is_scalar(vm)) {
                /* mixed scalar/vector */
                delta_m = 0;
            } else {
                /* vector */
                delta_m = delta_d;
            }
        }
    }

    f0 = tcg_temp_new_i64(tcg_ctx);
    f1 = tcg_temp_new_i64(tcg_ctx);
    fd = tcg_temp_new_i64(tcg_ctx);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);

    vfp_load_reg64(s, f0, vn);
    vfp_load_reg64(s, f1, vm);

    for (;;) {
        if (reads_vd) {
            vfp_load_reg64(s, fd, vd);
        }
        fn(tcg_ctx, fd, f0, f1, fpst);
        vfp_store_reg64(s, fd, vd);

        if (veclen == 0) {
            break;
        }
        /* Set up the operands for the next iteration */
        veclen--;
        vd = vfp_advance_dreg(vd, delta_d);
        vn = vfp_advance_dreg(vn, delta_d);
        vfp_load_reg64(s, f0, vn);
        if (delta_m) {
            vm = vfp_advance_dreg(vm, delta_m);
            vfp_load_reg64(s, f1, vm);
        }
    }

    tcg_temp_free_i64(tcg_ctx, f0);
    tcg_temp_free_i64(tcg_ctx, f1);
    tcg_temp_free_i64(tcg_ctx, fd);
    tcg_temp_free_ptr(tcg_ctx, fpst);

    return true;
}

static bool do_vfp_2op_sp(DisasContext *s, VFPGen2OpSPFn *fn, int vd, int vm)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t delta_m = 0;
    uint32_t delta_d = 0;
    int veclen = s->vec_len;
    TCGv_i32 f0, fd;

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    if (!dc_isar_feature(aa32_fpshvec, s) &&
        (veclen != 0 || s->vec_stride != 0)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    if (veclen > 0) {
        /* Figure out what type of vector operation this is.  */
        if (vfp_sreg_is_scalar(vd)) {
            /* scalar */
            veclen = 0;
        } else {
            delta_d = s->vec_stride + 1;

            if (vfp_sreg_is_scalar(vm)) {
                /* mixed scalar/vector */
                delta_m = 0;
            } else {
                /* vector */
                delta_m = delta_d;
            }
        }
    }

    f0 = tcg_temp_new_i32(tcg_ctx);
    fd = tcg_temp_new_i32(tcg_ctx);

    vfp_load_reg32(s, f0, vm);

    for (;;) {
        fn(tcg_ctx, fd, f0);
        vfp_store_reg32(s, fd, vd);

        if (veclen == 0) {
            break;
        }

        if (delta_m == 0) {
            /* single source one-many */
            while (veclen--) {
                vd = vfp_advance_sreg(vd, delta_d);
                vfp_store_reg32(s, fd, vd);
            }
            break;
        }

        /* Set up the operands for the next iteration */
        veclen--;
        vd = vfp_advance_sreg(vd, delta_d);
        vm = vfp_advance_sreg(vm, delta_m);
        vfp_load_reg32(s, f0, vm);
    }

    tcg_temp_free_i32(tcg_ctx, f0);
    tcg_temp_free_i32(tcg_ctx, fd);

    return true;
}

static bool do_vfp_2op_hp(DisasContext *s, VFPGen2OpSPFn *fn, int vd, int vm)
{
    /*
     * Do a half-precision operation. Functionally this is
     * the same as do_vfp_2op_sp(), except:
     *  - it doesn't need the VFP vector handling (fp16 is a
     *    v8 feature, and in v8 VFP vectors don't exist)
     *  - it does the aa32_fp16_arith feature test
     */
    TCGv_i32 f0;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (s->vec_len != 0 || s->vec_stride != 0) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    f0 = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, f0, vm);
    fn(tcg_ctx, f0, f0);
    vfp_store_reg32(s, f0, vd);
    tcg_temp_free_i32(tcg_ctx, f0);

    return true;
}

static bool do_vfp_2op_dp(DisasContext *s, VFPGen2OpDPFn *fn, int vd, int vm)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t delta_m = 0;
    uint32_t delta_d = 0;
    int veclen = s->vec_len;
    TCGv_i64 f0, fd;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist */
    if (!dc_isar_feature(aa32_simd_r32, s) && ((vd | vm) & 0x10)) {
        return false;
    }

    if (!dc_isar_feature(aa32_fpshvec, s) &&
        (veclen != 0 || s->vec_stride != 0)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    if (veclen > 0) {
        /* Figure out what type of vector operation this is.  */
        if (vfp_dreg_is_scalar(vd)) {
            /* scalar */
            veclen = 0;
        } else {
            delta_d = (s->vec_stride >> 1) + 1;

            if (vfp_dreg_is_scalar(vm)) {
                /* mixed scalar/vector */
                delta_m = 0;
            } else {
                /* vector */
                delta_m = delta_d;
            }
        }
    }

    f0 = tcg_temp_new_i64(tcg_ctx);
    fd = tcg_temp_new_i64(tcg_ctx);

    vfp_load_reg64(s, f0, vm);

    for (;;) {
        fn(tcg_ctx, fd, f0);
        vfp_store_reg64(s, fd, vd);

        if (veclen == 0) {
            break;
        }

        if (delta_m == 0) {
            /* single source one-many */
            while (veclen--) {
                vd = vfp_advance_dreg(vd, delta_d);
                vfp_store_reg64(s, fd, vd);
            }
            break;
        }

        /* Set up the operands for the next iteration */
        veclen--;
        vd = vfp_advance_dreg(vd, delta_d);
        vd = vfp_advance_dreg(vm, delta_m);
        vfp_load_reg64(s, f0, vm);
    }

    tcg_temp_free_i64(tcg_ctx, f0);
    tcg_temp_free_i64(tcg_ctx, fd);

    return true;
}

static void gen_VMLA_hp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /* Note that order of inputs to the add matters for NaNs */
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    gen_helper_vfp_mulh(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_addh(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static bool trans_VMLA_hp(DisasContext *s, arg_VMLA_sp *a)
{
    return do_vfp_3op_hp(s, gen_VMLA_hp, a->vd, a->vn, a->vm, true);
}

static void gen_VMLA_sp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /* Note that order of inputs to the add matters for NaNs */
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    gen_helper_vfp_muls(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_adds(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static bool trans_VMLA_sp(DisasContext *s, arg_VMLA_sp *a)
{
    return do_vfp_3op_sp(s, gen_VMLA_sp, a->vd, a->vn, a->vm, true);
}

static void gen_VMLA_dp(TCGContext *tcg_ctx, TCGv_i64 vd, TCGv_i64 vn, TCGv_i64 vm, TCGv_ptr fpst)
{
    /* Note that order of inputs to the add matters for NaNs */
    TCGv_i64 tmp = tcg_temp_new_i64(tcg_ctx);

    gen_helper_vfp_muld(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_addd(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i64(tcg_ctx, tmp);
}

static bool trans_VMLA_dp(DisasContext *s, arg_VMLA_dp *a)
{
    return do_vfp_3op_dp(s, gen_VMLA_dp, a->vd, a->vn, a->vm, true);
}

static void gen_VMLS_hp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /*
     * VMLS: vd = vd + -(vn * vm)
     * Note that order of inputs to the add matters for NaNs.
     */
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    gen_helper_vfp_mulh(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_negh(tcg_ctx, tmp, tmp);
    gen_helper_vfp_addh(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static bool trans_VMLS_hp(DisasContext *s, arg_VMLS_sp *a)
{
    return do_vfp_3op_hp(s, gen_VMLS_hp, a->vd, a->vn, a->vm, true);
}

static void gen_VMLS_sp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /*
     * VMLS: vd = vd + -(vn * vm)
     * Note that order of inputs to the add matters for NaNs.
     */
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    gen_helper_vfp_muls(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_negs(tcg_ctx, tmp, tmp);
    gen_helper_vfp_adds(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static bool trans_VMLS_sp(DisasContext *s, arg_VMLS_sp *a)
{
    return do_vfp_3op_sp(s, gen_VMLS_sp, a->vd, a->vn, a->vm, true);
}

static void gen_VMLS_dp(TCGContext *tcg_ctx, TCGv_i64 vd, TCGv_i64 vn, TCGv_i64 vm, TCGv_ptr fpst)
{
    /*
     * VMLS: vd = vd + -(vn * vm)
     * Note that order of inputs to the add matters for NaNs.
     */
    TCGv_i64 tmp = tcg_temp_new_i64(tcg_ctx);

    gen_helper_vfp_muld(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_negd(tcg_ctx, tmp, tmp);
    gen_helper_vfp_addd(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i64(tcg_ctx, tmp);
}

static bool trans_VMLS_dp(DisasContext *s, arg_VMLS_dp *a)
{
    return do_vfp_3op_dp(s, gen_VMLS_dp, a->vd, a->vn, a->vm, true);
}

static void gen_VNMLS_hp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /*
     * VNMLS: -fd + (fn * fm)
     * Note that it isn't valid to replace (-A + B) with (B - A) or similar
     * plausible looking simplifications because this will give wrong results
     * for NaNs.
     */
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    gen_helper_vfp_mulh(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_negh(tcg_ctx, vd, vd);
    gen_helper_vfp_addh(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static bool trans_VNMLS_hp(DisasContext *s, arg_VNMLS_sp *a)
{
    return do_vfp_3op_hp(s, gen_VNMLS_hp, a->vd, a->vn, a->vm, true);
}

static void gen_VNMLS_sp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /*
     * VNMLS: -fd + (fn * fm)
     * Note that it isn't valid to replace (-A + B) with (B - A) or similar
     * plausible looking simplifications because this will give wrong results
     * for NaNs.
     */
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    gen_helper_vfp_muls(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_negs(tcg_ctx, vd, vd);
    gen_helper_vfp_adds(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static bool trans_VNMLS_sp(DisasContext *s, arg_VNMLS_sp *a)
{
    return do_vfp_3op_sp(s, gen_VNMLS_sp, a->vd, a->vn, a->vm, true);
}

static void gen_VNMLS_dp(TCGContext *tcg_ctx, TCGv_i64 vd, TCGv_i64 vn, TCGv_i64 vm, TCGv_ptr fpst)
{
    /*
     * VNMLS: -fd + (fn * fm)
     * Note that it isn't valid to replace (-A + B) with (B - A) or similar
     * plausible looking simplifications because this will give wrong results
     * for NaNs.
     */
    TCGv_i64 tmp = tcg_temp_new_i64(tcg_ctx);

    gen_helper_vfp_muld(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_negd(tcg_ctx, vd, vd);
    gen_helper_vfp_addd(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i64(tcg_ctx, tmp);
}

static bool trans_VNMLS_dp(DisasContext *s, arg_VNMLS_dp *a)
{
    return do_vfp_3op_dp(s, gen_VNMLS_dp, a->vd, a->vn, a->vm, true);
}

static void gen_VNMLA_hp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /* VNMLA: -fd + -(fn * fm) */
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    gen_helper_vfp_mulh(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_negh(tcg_ctx, tmp, tmp);
    gen_helper_vfp_negh(tcg_ctx, vd, vd);
    gen_helper_vfp_addh(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static bool trans_VNMLA_hp(DisasContext *s, arg_VNMLA_sp *a)
{
    return do_vfp_3op_hp(s, gen_VNMLA_hp, a->vd, a->vn, a->vm, true);
}

static void gen_VNMLA_sp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /* VNMLA: -fd + -(fn * fm) */
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    gen_helper_vfp_muls(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_negs(tcg_ctx, tmp, tmp);
    gen_helper_vfp_negs(tcg_ctx, vd, vd);
    gen_helper_vfp_adds(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static bool trans_VNMLA_sp(DisasContext *s, arg_VNMLA_sp *a)
{
    return do_vfp_3op_sp(s, gen_VNMLA_sp, a->vd, a->vn, a->vm, true);
}

static void gen_VNMLA_dp(TCGContext *tcg_ctx, TCGv_i64 vd, TCGv_i64 vn, TCGv_i64 vm, TCGv_ptr fpst)
{
    /* VNMLA: -fd + (fn * fm) */
    TCGv_i64 tmp = tcg_temp_new_i64(tcg_ctx);

    gen_helper_vfp_muld(tcg_ctx, tmp, vn, vm, fpst);
    gen_helper_vfp_negd(tcg_ctx, tmp, tmp);
    gen_helper_vfp_negd(tcg_ctx, vd, vd);
    gen_helper_vfp_addd(tcg_ctx, vd, vd, tmp, fpst);
    tcg_temp_free_i64(tcg_ctx, tmp);
}

static bool trans_VNMLA_dp(DisasContext *s, arg_VNMLA_dp *a)
{
    return do_vfp_3op_dp(s, gen_VNMLA_dp, a->vd, a->vn, a->vm, true);
}

static bool trans_VMUL_hp(DisasContext *s, arg_VMUL_sp *a)
{
    return do_vfp_3op_hp(s, gen_helper_vfp_mulh, a->vd, a->vn, a->vm, false);
}

static bool trans_VMUL_sp(DisasContext *s, arg_VMUL_sp *a)
{
    return do_vfp_3op_sp(s, gen_helper_vfp_muls, a->vd, a->vn, a->vm, false);
}

static bool trans_VMUL_dp(DisasContext *s, arg_VMUL_dp *a)
{
    return do_vfp_3op_dp(s, gen_helper_vfp_muld, a->vd, a->vn, a->vm, false);
}

static void gen_VNMUL_hp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /* VNMUL: -(fn * fm) */
    gen_helper_vfp_mulh(tcg_ctx, vd, vn, vm, fpst);
    gen_helper_vfp_negh(tcg_ctx, vd, vd);
}

static bool trans_VNMUL_hp(DisasContext *s, arg_VNMUL_sp *a)
{
    return do_vfp_3op_hp(s, gen_VNMUL_hp, a->vd, a->vn, a->vm, false);
}

static void gen_VNMUL_sp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vn, TCGv_i32 vm, TCGv_ptr fpst)
{
    /* VNMUL: -(fn * fm) */
    gen_helper_vfp_muls(tcg_ctx, vd, vn, vm, fpst);
    gen_helper_vfp_negs(tcg_ctx, vd, vd);
}

static bool trans_VNMUL_sp(DisasContext *s, arg_VNMUL_sp *a)
{
    return do_vfp_3op_sp(s, gen_VNMUL_sp, a->vd, a->vn, a->vm, false);
}

static void gen_VNMUL_dp(TCGContext *tcg_ctx, TCGv_i64 vd, TCGv_i64 vn, TCGv_i64 vm, TCGv_ptr fpst)
{
    /* VNMUL: -(fn * fm) */
    gen_helper_vfp_muld(tcg_ctx, vd, vn, vm, fpst);
    gen_helper_vfp_negd(tcg_ctx, vd, vd);
}

static bool trans_VNMUL_dp(DisasContext *s, arg_VNMUL_dp *a)
{
    return do_vfp_3op_dp(s, gen_VNMUL_dp, a->vd, a->vn, a->vm, false);
}

static bool trans_VADD_hp(DisasContext *s, arg_VADD_sp *a)
{
    return do_vfp_3op_hp(s, gen_helper_vfp_addh, a->vd, a->vn, a->vm, false);
}

static bool trans_VADD_sp(DisasContext *s, arg_VADD_sp *a)
{
    return do_vfp_3op_sp(s, gen_helper_vfp_adds, a->vd, a->vn, a->vm, false);
}

static bool trans_VADD_dp(DisasContext *s, arg_VADD_dp *a)
{
    return do_vfp_3op_dp(s, gen_helper_vfp_addd, a->vd, a->vn, a->vm, false);
}

static bool trans_VSUB_hp(DisasContext *s, arg_VSUB_sp *a)
{
    return do_vfp_3op_hp(s, gen_helper_vfp_subh, a->vd, a->vn, a->vm, false);
}

static bool trans_VSUB_sp(DisasContext *s, arg_VSUB_sp *a)
{
    return do_vfp_3op_sp(s, gen_helper_vfp_subs, a->vd, a->vn, a->vm, false);
}

static bool trans_VSUB_dp(DisasContext *s, arg_VSUB_dp *a)
{
    return do_vfp_3op_dp(s, gen_helper_vfp_subd, a->vd, a->vn, a->vm, false);
}

static bool trans_VDIV_hp(DisasContext *s, arg_VDIV_sp *a)
{
    return do_vfp_3op_hp(s, gen_helper_vfp_divh, a->vd, a->vn, a->vm, false);
}

static bool trans_VDIV_sp(DisasContext *s, arg_VDIV_sp *a)
{
    return do_vfp_3op_sp(s, gen_helper_vfp_divs, a->vd, a->vn, a->vm, false);
}

static bool trans_VDIV_dp(DisasContext *s, arg_VDIV_dp *a)
{
    return do_vfp_3op_dp(s, gen_helper_vfp_divd, a->vd, a->vn, a->vm, false);
}

static bool trans_VMINNM_hp(DisasContext *s, arg_VMINNM_sp *a)
{
    if (!dc_isar_feature(aa32_vminmaxnm, s)) {
        return false;
    }
    return do_vfp_3op_hp(s, gen_helper_vfp_minnumh,
                         a->vd, a->vn, a->vm, false);
}

static bool trans_VMAXNM_hp(DisasContext *s, arg_VMAXNM_sp *a)
{
    if (!dc_isar_feature(aa32_vminmaxnm, s)) {
        return false;
    }
    return do_vfp_3op_hp(s, gen_helper_vfp_maxnumh,
                         a->vd, a->vn, a->vm, false);
}

static bool trans_VMINNM_sp(DisasContext *s, arg_VMINNM_sp *a)
{
    if (!dc_isar_feature(aa32_vminmaxnm, s)) {
        return false;
    }
    return do_vfp_3op_sp(s, gen_helper_vfp_minnums,
                         a->vd, a->vn, a->vm, false);
}

static bool trans_VMAXNM_sp(DisasContext *s, arg_VMAXNM_sp *a)
{
    if (!dc_isar_feature(aa32_vminmaxnm, s)) {
        return false;
    }
    return do_vfp_3op_sp(s, gen_helper_vfp_maxnums,
                         a->vd, a->vn, a->vm, false);
}

static bool trans_VMINNM_dp(DisasContext *s, arg_VMINNM_dp *a)
{
    if (!dc_isar_feature(aa32_vminmaxnm, s)) {
        return false;
    }
    return do_vfp_3op_dp(s, gen_helper_vfp_minnumd,
                         a->vd, a->vn, a->vm, false);
}

static bool trans_VMAXNM_dp(DisasContext *s, arg_VMAXNM_dp *a)
{
    if (!dc_isar_feature(aa32_vminmaxnm, s)) {
        return false;
    }
    return do_vfp_3op_dp(s, gen_helper_vfp_maxnumd,
                         a->vd, a->vn, a->vm, false);
}

static bool do_vfm_hp(DisasContext *s, arg_VFMA_sp *a, bool neg_n, bool neg_d)
{
    /*
     * VFNMA : fd = muladd(-fd,  fn, fm)
     * VFNMS : fd = muladd(-fd, -fn, fm)
     * VFMA  : fd = muladd( fd,  fn, fm)
     * VFMS  : fd = muladd( fd, -fn, fm)
     *
     * These are fused multiply-add, and must be done as one floating
     * point operation with no rounding between the multiplication and
     * addition steps.  NB that doing the negations here as separate
     * steps is correct : an input NaN should come out with its sign
     * bit flipped if it is a negated-input.
     */
    TCGv_ptr fpst;
    TCGv_i32 vn, vm, vd;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    /*
     * Present in VFPv4 only, and only with the FP16 extension.
     * Note that we can't rely on the SIMDFMAC check alone, because
     * in a Neon-no-VFP core that ID register field will be non-zero.
     */
    if (!dc_isar_feature(aa32_fp16_arith, s) ||
        !dc_isar_feature(aa32_simdfmac, s) ||
        !dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    if (s->vec_len != 0 || s->vec_stride != 0) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vn = tcg_temp_new_i32(tcg_ctx);
    vm = tcg_temp_new_i32(tcg_ctx);
    vd = tcg_temp_new_i32(tcg_ctx);

    vfp_load_reg32(s, vn, a->vn);
    vfp_load_reg32(s, vm, a->vm);
    if (neg_n) {
        /* VFNMS, VFMS */
        gen_helper_vfp_negh(tcg_ctx, vn, vn);
    }
    vfp_load_reg32(s, vd, a->vd);
    if (neg_d) {
        /* VFNMA, VFNMS */
        gen_helper_vfp_negh(tcg_ctx, vd, vd);
    }
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);
    gen_helper_vfp_muladdh(tcg_ctx, vd, vn, vm, vd, fpst);
    vfp_store_reg32(s, vd, a->vd);

    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, vn);
    tcg_temp_free_i32(tcg_ctx, vm);
    tcg_temp_free_i32(tcg_ctx, vd);

    return true;
}

static bool do_vfm_sp(DisasContext *s, arg_VFMA_sp *a, bool neg_n, bool neg_d)
{
    /*
     * VFNMA : fd = muladd(-fd,  fn, fm)
     * VFNMS : fd = muladd(-fd, -fn, fm)
     * VFMA  : fd = muladd( fd,  fn, fm)
     * VFMS  : fd = muladd( fd, -fn, fm)
     *
     * These are fused multiply-add, and must be done as one floating
     * point operation with no rounding between the multiplication and
     * addition steps.  NB that doing the negations here as separate
     * steps is correct : an input NaN should come out with its sign
     * bit flipped if it is a negated-input.
     */
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 vn, vm, vd;

    /*
     * Present in VFPv4 only.
     * Note that we can't rely on the SIMDFMAC check alone, because
     * in a Neon-no-VFP core that ID register field will be non-zero.
     */
    if (!dc_isar_feature(aa32_simdfmac, s) ||
        !dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    /*
     * In v7A, UNPREDICTABLE with non-zero vector length/stride; from
     * v8A, must UNDEF. We choose to UNDEF for both v7A and v8A.
     */
    if (s->vec_len != 0 || s->vec_stride != 0) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vn = tcg_temp_new_i32(tcg_ctx);
    vm = tcg_temp_new_i32(tcg_ctx);
    vd = tcg_temp_new_i32(tcg_ctx);

    vfp_load_reg32(s, vn, a->vn);
    vfp_load_reg32(s, vm, a->vm);
    if (neg_n) {
        /* VFNMS, VFMS */
        gen_helper_vfp_negs(tcg_ctx, vn, vn);
    }
    vfp_load_reg32(s, vd, a->vd);
    if (neg_d) {
        /* VFNMA, VFNMS */
        gen_helper_vfp_negs(tcg_ctx, vd, vd);
    }
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    gen_helper_vfp_muladds(tcg_ctx, vd, vn, vm, vd, fpst);
    vfp_store_reg32(s, vd, a->vd);

    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, vn);
    tcg_temp_free_i32(tcg_ctx, vm);
    tcg_temp_free_i32(tcg_ctx, vd);

    return true;
}

static bool do_vfm_dp(DisasContext *s, arg_VFMA_dp *a, bool neg_n, bool neg_d)
{
    /*
     * VFNMA : fd = muladd(-fd,  fn, fm)
     * VFNMS : fd = muladd(-fd, -fn, fm)
     * VFMA  : fd = muladd( fd,  fn, fm)
     * VFMS  : fd = muladd( fd, -fn, fm)
     *
     * These are fused multiply-add, and must be done as one floating
     * point operation with no rounding between the multiplication and
     * addition steps.  NB that doing the negations here as separate
     * steps is correct : an input NaN should come out with its sign
     * bit flipped if it is a negated-input.
     */
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i64 vn, vm, vd;

    /*
     * Present in VFPv4 only.
     * Note that we can't rely on the SIMDFMAC check alone, because
     * in a Neon-no-VFP core that ID register field will be non-zero.
     */
    if (!dc_isar_feature(aa32_simdfmac, s) ||
        !dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    /*
     * In v7A, UNPREDICTABLE with non-zero vector length/stride; from
     * v8A, must UNDEF. We choose to UNDEF for both v7A and v8A.
     */
    if (s->vec_len != 0 || s->vec_stride != 0) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && ((a->vd | a->vn | a->vm) & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vn = tcg_temp_new_i64(tcg_ctx);
    vm = tcg_temp_new_i64(tcg_ctx);
    vd = tcg_temp_new_i64(tcg_ctx);

    vfp_load_reg64(s, vn, a->vn);
    vfp_load_reg64(s, vm, a->vm);
    if (neg_n) {
        /* VFNMS, VFMS */
        gen_helper_vfp_negd(tcg_ctx, vn, vn);
    }
    vfp_load_reg64(s, vd, a->vd);
    if (neg_d) {
        /* VFNMA, VFNMS */
        gen_helper_vfp_negd(tcg_ctx, vd, vd);
    }
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    gen_helper_vfp_muladdd(tcg_ctx, vd, vn, vm, vd, fpst);
    vfp_store_reg64(s, vd, a->vd);

    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i64(tcg_ctx, vn);
    tcg_temp_free_i64(tcg_ctx, vm);
    tcg_temp_free_i64(tcg_ctx, vd);

    return true;
}

#define MAKE_ONE_VFM_TRANS_FN(INSN, PREC, NEGN, NEGD)                   \
    static bool trans_##INSN##_##PREC(DisasContext *s,                  \
                                      arg_##INSN##_##PREC *a)           \
    {                                                                   \
        return do_vfm_##PREC(s, a, NEGN, NEGD);                         \
    }

#define MAKE_VFM_TRANS_FNS(PREC) \
    MAKE_ONE_VFM_TRANS_FN(VFMA, PREC, false, false) \
    MAKE_ONE_VFM_TRANS_FN(VFMS, PREC, true, false) \
    MAKE_ONE_VFM_TRANS_FN(VFNMA, PREC, false, true) \
    MAKE_ONE_VFM_TRANS_FN(VFNMS, PREC, true, true)

MAKE_VFM_TRANS_FNS(hp)
MAKE_VFM_TRANS_FNS(sp)
MAKE_VFM_TRANS_FNS(dp)

static bool trans_VMOV_imm_hp(DisasContext *s, arg_VMOV_imm_sp *a)
{
    TCGv_i32 fd;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (s->vec_len != 0 || s->vec_stride != 0) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    fd = tcg_const_i32(tcg_ctx, vfp_expand_imm(MO_16, a->imm));
    vfp_store_reg32(s, fd, a->vd);
    tcg_temp_free_i32(tcg_ctx, fd);
    return true;
}

static bool trans_VMOV_imm_sp(DisasContext *s, arg_VMOV_imm_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t delta_d = 0;
    int veclen = s->vec_len;
    TCGv_i32 fd;
    uint32_t vd;

    vd = a->vd;

    if (!dc_isar_feature(aa32_fpsp_v3, s)) {
        return false;
    }

    if (!dc_isar_feature(aa32_fpshvec, s) &&
        (veclen != 0 || s->vec_stride != 0)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    if (veclen > 0) {
        /* Figure out what type of vector operation this is.  */
        if (vfp_sreg_is_scalar(vd)) {
            /* scalar */
            veclen = 0;
        } else {
            delta_d = s->vec_stride + 1;
        }
    }

    fd = tcg_const_i32(tcg_ctx, vfp_expand_imm(MO_32, a->imm));

    for (;;) {
        vfp_store_reg32(s, fd, vd);

        if (veclen == 0) {
            break;
        }

        /* Set up the operands for the next iteration */
        veclen--;
        vd = vfp_advance_sreg(vd, delta_d);
    }

    tcg_temp_free_i32(tcg_ctx, fd);
    return true;
}

static bool trans_VMOV_imm_dp(DisasContext *s, arg_VMOV_imm_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    uint32_t delta_d = 0;
    int veclen = s->vec_len;
    TCGv_i64 fd;
    uint32_t vd;

    vd = a->vd;

    if (!dc_isar_feature(aa32_fpdp_v3, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && (vd & 0x10)) {
        return false;
    }

    if (!dc_isar_feature(aa32_fpshvec, s) &&
        (veclen != 0 || s->vec_stride != 0)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    if (veclen > 0) {
        /* Figure out what type of vector operation this is.  */
        if (vfp_dreg_is_scalar(vd)) {
            /* scalar */
            veclen = 0;
        } else {
            delta_d = (s->vec_stride >> 1) + 1;
        }
    }

    fd = tcg_const_i64(tcg_ctx, vfp_expand_imm(MO_64, a->imm));

    for (;;) {
        vfp_store_reg64(s, fd, vd);

        if (veclen == 0) {
            break;
        }

        /* Set up the operands for the next iteration */
        veclen--;
        vd = vfp_advance_dreg(vd, delta_d);
    }

    tcg_temp_free_i64(tcg_ctx, fd);
    return true;
}

#define DO_VFP_2OP(INSN, PREC, FN)                              \
    static bool trans_##INSN##_##PREC(DisasContext *s,          \
                                      arg_##INSN##_##PREC *a)   \
    {                                                           \
        return do_vfp_2op_##PREC(s, FN, a->vd, a->vm);          \
    }

DO_VFP_2OP(VMOV_reg, sp, tcg_gen_mov_i32)
DO_VFP_2OP(VMOV_reg, dp, tcg_gen_mov_i64)

DO_VFP_2OP(VABS, hp, gen_helper_vfp_absh)
DO_VFP_2OP(VABS, sp, gen_helper_vfp_abss)
DO_VFP_2OP(VABS, dp, gen_helper_vfp_absd)

DO_VFP_2OP(VNEG, hp, gen_helper_vfp_negh)
DO_VFP_2OP(VNEG, sp, gen_helper_vfp_negs)
DO_VFP_2OP(VNEG, dp, gen_helper_vfp_negd)

static void gen_VSQRT_hp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vm)
{
    gen_helper_vfp_sqrth(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
}

static void gen_VSQRT_sp(TCGContext *tcg_ctx, TCGv_i32 vd, TCGv_i32 vm)
{
    gen_helper_vfp_sqrts(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
}

static void gen_VSQRT_dp(TCGContext *tcg_ctx, TCGv_i64 vd, TCGv_i64 vm)
{
    gen_helper_vfp_sqrtd(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
}

DO_VFP_2OP(VSQRT, hp, gen_VSQRT_hp)
DO_VFP_2OP(VSQRT, sp, gen_VSQRT_sp)
DO_VFP_2OP(VSQRT, dp, gen_VSQRT_dp)

static bool trans_VCMP_hp(DisasContext *s, arg_VCMP_sp *a)
{
    TCGv_i32 vd, vm;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    /* Vm/M bits must be zero for the Z variant */
    if (a->z && a->vm != 0) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vd = tcg_temp_new_i32(tcg_ctx);
    vm = tcg_temp_new_i32(tcg_ctx);

    vfp_load_reg32(s, vd, a->vd);
    if (a->z) {
        tcg_gen_movi_i32(tcg_ctx, vm, 0);
    } else {
        vfp_load_reg32(s, vm, a->vm);
    }

    if (a->e) {
        gen_helper_vfp_cmpeh_a32(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
    } else {
        gen_helper_vfp_cmph_a32(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
    }

    tcg_temp_free_i32(tcg_ctx, vd);
    tcg_temp_free_i32(tcg_ctx, vm);

    return true;
}

static bool trans_VCMP_sp(DisasContext *s, arg_VCMP_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 vd, vm;

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    /* Vm/M bits must be zero for the Z variant */
    if (a->z && a->vm != 0) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vd = tcg_temp_new_i32(tcg_ctx);
    vm = tcg_temp_new_i32(tcg_ctx);

    vfp_load_reg32(s, vd, a->vd);
    if (a->z) {
        tcg_gen_movi_i32(tcg_ctx, vm, 0);
    } else {
        vfp_load_reg32(s, vm, a->vm);
    }

    if (a->e) {
        gen_helper_vfp_cmpes(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
    } else {
        gen_helper_vfp_cmps(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
    }

    tcg_temp_free_i32(tcg_ctx, vd);
    tcg_temp_free_i32(tcg_ctx, vm);

    return true;
}

static bool trans_VCMP_dp(DisasContext *s, arg_VCMP_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i64 vd, vm;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    /* Vm/M bits must be zero for the Z variant */
    if (a->z && a->vm != 0) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && ((a->vd | a->vm) & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vd = tcg_temp_new_i64(tcg_ctx);
    vm = tcg_temp_new_i64(tcg_ctx);

    vfp_load_reg64(s, vd, a->vd);
    if (a->z) {
        tcg_gen_movi_i64(tcg_ctx, vm, 0);
    } else {
        vfp_load_reg64(s, vm, a->vm);
    }

    if (a->e) {
        gen_helper_vfp_cmped(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
    } else {
        gen_helper_vfp_cmpd(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
    }

    tcg_temp_free_i64(tcg_ctx, vd);
    tcg_temp_free_i64(tcg_ctx, vm);

    return true;
}

static bool trans_VCVT_f32_f16(DisasContext *s, arg_VCVT_f32_f16 *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 ahp_mode;
    TCGv_i32 tmp;

    if (!dc_isar_feature(aa32_fp16_spconv, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    ahp_mode = get_ahp_flag(s);
    tmp = tcg_temp_new_i32(tcg_ctx);
    /* The T bit tells us if we want the low or high 16 bits of Vm */
    tcg_gen_ld16u_i32(tcg_ctx, tmp, tcg_ctx->cpu_env, vfp_f16_offset(a->vm, a->t));
    gen_helper_vfp_fcvt_f16_to_f32(tcg_ctx, tmp, tmp, fpst, ahp_mode);
    vfp_store_reg32(s, tmp, a->vd);
    tcg_temp_free_i32(tcg_ctx, ahp_mode);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return true;
}

static bool trans_VCVT_f64_f16(DisasContext *s, arg_VCVT_f64_f16 *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 ahp_mode;
    TCGv_i32 tmp;
    TCGv_i64 vd;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_fp16_dpconv, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vd  & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    ahp_mode = get_ahp_flag(s);
    tmp = tcg_temp_new_i32(tcg_ctx);
    /* The T bit tells us if we want the low or high 16 bits of Vm */
    tcg_gen_ld16u_i32(tcg_ctx, tmp, tcg_ctx->cpu_env, vfp_f16_offset(a->vm, a->t));
    vd = tcg_temp_new_i64(tcg_ctx);
    gen_helper_vfp_fcvt_f16_to_f64(tcg_ctx, vd, tmp, fpst, ahp_mode);
    vfp_store_reg64(s, vd, a->vd);
    tcg_temp_free_i32(tcg_ctx, ahp_mode);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i64(tcg_ctx, vd);
    return true;
}

static bool trans_VCVT_f16_f32(DisasContext *s, arg_VCVT_f16_f32 *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 ahp_mode;
    TCGv_i32 tmp;

    if (!dc_isar_feature(aa32_fp16_spconv, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    ahp_mode = get_ahp_flag(s);
    tmp = tcg_temp_new_i32(tcg_ctx);

    vfp_load_reg32(s, tmp, a->vm);
    gen_helper_vfp_fcvt_f32_to_f16(tcg_ctx, tmp, tmp, fpst, ahp_mode);
    tcg_gen_st16_i32(tcg_ctx, tmp, tcg_ctx->cpu_env, vfp_f16_offset(a->vd, a->t));
    tcg_temp_free_i32(tcg_ctx, ahp_mode);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return true;
}

static bool trans_VCVT_f16_f64(DisasContext *s, arg_VCVT_f16_f64 *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 ahp_mode;
    TCGv_i32 tmp;
    TCGv_i64 vm;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    if (!dc_isar_feature(aa32_fp16_dpconv, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vm  & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    ahp_mode = get_ahp_flag(s);
    tmp = tcg_temp_new_i32(tcg_ctx);
    vm = tcg_temp_new_i64(tcg_ctx);

    vfp_load_reg64(s, vm, a->vm);
    gen_helper_vfp_fcvt_f64_to_f16(tcg_ctx, tmp, vm, fpst, ahp_mode);
    tcg_temp_free_i64(tcg_ctx, vm);
    tcg_gen_st16_i32(tcg_ctx, tmp, tcg_ctx->cpu_env, vfp_f16_offset(a->vd, a->t));
    tcg_temp_free_i32(tcg_ctx, ahp_mode);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return true;
}

static bool trans_VRINTR_hp(DisasContext *s, arg_VRINTR_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 tmp;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, tmp, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);
    gen_helper_rinth(tcg_ctx, tmp, tmp, fpst);
    vfp_store_reg32(s, tmp, a->vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return true;
}

static bool trans_VRINTR_sp(DisasContext *s, arg_VRINTR_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 tmp;

    if (!dc_isar_feature(aa32_vrint, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, tmp, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    gen_helper_rints(tcg_ctx, tmp, tmp, fpst);
    vfp_store_reg32(s, tmp, a->vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return true;
}

static bool trans_VRINTR_dp(DisasContext *s, arg_VRINTR_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i64 tmp;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    if (!dc_isar_feature(aa32_vrint, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && ((a->vd | a->vm) & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i64(tcg_ctx);
    vfp_load_reg64(s, tmp, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    gen_helper_rintd(tcg_ctx, tmp, tmp, fpst);
    vfp_store_reg64(s, tmp, a->vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i64(tcg_ctx, tmp);
    return true;
}

static bool trans_VRINTZ_hp(DisasContext *s, arg_VRINTZ_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 tmp;
    TCGv_i32 tcg_rmode;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, tmp, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);
    tcg_rmode = tcg_const_i32(tcg_ctx, float_round_to_zero);
    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);
    gen_helper_rinth(tcg_ctx, tmp, tmp, fpst);
    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);
    vfp_store_reg32(s, tmp, a->vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tcg_rmode);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return true;
}

static bool trans_VRINTZ_sp(DisasContext *s, arg_VRINTZ_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 tmp;
    TCGv_i32 tcg_rmode;

    if (!dc_isar_feature(aa32_vrint, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, tmp, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    tcg_rmode = tcg_const_i32(tcg_ctx, float_round_to_zero);
    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);
    gen_helper_rints(tcg_ctx, tmp, tmp, fpst);
    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);
    vfp_store_reg32(s, tmp, a->vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tcg_rmode);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return true;
}

static bool trans_VRINTZ_dp(DisasContext *s, arg_VRINTZ_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i64 tmp;
    TCGv_i32 tcg_rmode;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    if (!dc_isar_feature(aa32_vrint, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && ((a->vd | a->vm) & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i64(tcg_ctx);
    vfp_load_reg64(s, tmp, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    tcg_rmode = tcg_const_i32(tcg_ctx, float_round_to_zero);
    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);
    gen_helper_rintd(tcg_ctx, tmp, tmp, fpst);
    gen_helper_set_rmode(tcg_ctx, tcg_rmode, tcg_rmode, fpst);
    vfp_store_reg64(s, tmp, a->vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i64(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, tcg_rmode);
    return true;
}

static bool trans_VRINTX_hp(DisasContext *s, arg_VRINTX_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 tmp;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, tmp, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);
    gen_helper_rinth_exact(tcg_ctx, tmp, tmp, fpst);
    vfp_store_reg32(s, tmp, a->vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return true;
}

static bool trans_VRINTX_sp(DisasContext *s, arg_VRINTX_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i32 tmp;

    if (!dc_isar_feature(aa32_vrint, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, tmp, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    gen_helper_rints_exact(tcg_ctx, tmp, tmp, fpst);
    vfp_store_reg32(s, tmp, a->vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return true;
}

static bool trans_VRINTX_dp(DisasContext *s, arg_VRINTX_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr fpst;
    TCGv_i64 tmp;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    if (!dc_isar_feature(aa32_vrint, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && ((a->vd | a->vm) & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    tmp = tcg_temp_new_i64(tcg_ctx);
    vfp_load_reg64(s, tmp, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    gen_helper_rintd_exact(tcg_ctx, tmp, tmp, fpst);
    vfp_store_reg64(s, tmp, a->vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    tcg_temp_free_i64(tcg_ctx, tmp);
    return true;
}

static bool trans_VCVT_sp(DisasContext *s, arg_VCVT_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i64 vd;
    TCGv_i32 vm;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vd & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vm = tcg_temp_new_i32(tcg_ctx);
    vd = tcg_temp_new_i64(tcg_ctx);
    vfp_load_reg32(s, vm, a->vm);
    gen_helper_vfp_fcvtds(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
    vfp_store_reg64(s, vd, a->vd);
    tcg_temp_free_i32(tcg_ctx, vm);
    tcg_temp_free_i64(tcg_ctx, vd);
    return true;
}

static bool trans_VCVT_dp(DisasContext *s, arg_VCVT_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i64 vm;
    TCGv_i32 vd;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vm & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vd = tcg_temp_new_i32(tcg_ctx);
    vm = tcg_temp_new_i64(tcg_ctx);
    vfp_load_reg64(s, vm, a->vm);
    gen_helper_vfp_fcvtsd(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
    vfp_store_reg32(s, vd, a->vd);
    tcg_temp_free_i32(tcg_ctx, vd);
    tcg_temp_free_i64(tcg_ctx, vm);
    return true;
}

static bool trans_VCVT_int_hp(DisasContext *s, arg_VCVT_int_sp *a)
{
    TCGv_i32 vm;
    TCGv_ptr fpst;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vm = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, vm, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);
    if (a->s) {
        /* i32 -> f16 */
        gen_helper_vfp_sitoh(tcg_ctx, vm, vm, fpst);
    } else {
        /* u32 -> f16 */
        gen_helper_vfp_uitoh(tcg_ctx, vm, vm, fpst);
    }
    vfp_store_reg32(s, vm, a->vd);
    tcg_temp_free_i32(tcg_ctx, vm);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

static bool trans_VCVT_int_sp(DisasContext *s, arg_VCVT_int_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 vm;
    TCGv_ptr fpst;

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vm = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, vm, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    if (a->s) {
        /* i32 -> f32 */
        gen_helper_vfp_sitos(tcg_ctx, vm, vm, fpst);
    } else {
        /* u32 -> f32 */
        gen_helper_vfp_uitos(tcg_ctx, vm, vm, fpst);
    }
    vfp_store_reg32(s, vm, a->vd);
    tcg_temp_free_i32(tcg_ctx, vm);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

static bool trans_VCVT_int_dp(DisasContext *s, arg_VCVT_int_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 vm;
    TCGv_i64 vd;
    TCGv_ptr fpst;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vd & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vm = tcg_temp_new_i32(tcg_ctx);
    vd = tcg_temp_new_i64(tcg_ctx);
    vfp_load_reg32(s, vm, a->vm);
    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    if (a->s) {
        /* i32 -> f64 */
        gen_helper_vfp_sitod(tcg_ctx, vd, vm, fpst);
    } else {
        /* u32 -> f64 */
        gen_helper_vfp_uitod(tcg_ctx, vd, vm, fpst);
    }
    vfp_store_reg64(s, vd, a->vd);
    tcg_temp_free_i32(tcg_ctx, vm);
    tcg_temp_free_i64(tcg_ctx, vd);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

static bool trans_VJCVT(DisasContext *s, arg_VJCVT *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 vd;
    TCGv_i64 vm;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    if (!dc_isar_feature(aa32_jscvt, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vm & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    vm = tcg_temp_new_i64(tcg_ctx);
    vd = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg64(s, vm, a->vm);
    gen_helper_vjcvt(tcg_ctx, vd, vm, tcg_ctx->cpu_env);
    vfp_store_reg32(s, vd, a->vd);
    tcg_temp_free_i64(tcg_ctx, vm);
    tcg_temp_free_i32(tcg_ctx, vd);
    return true;
}

static bool trans_VCVT_fix_hp(DisasContext *s, arg_VCVT_fix_sp *a)
{
    TCGv_i32 vd, shift;
    TCGv_ptr fpst;
    int frac_bits;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    frac_bits = (a->opc & 1) ? (32 - a->imm) : (16 - a->imm);

    vd = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, vd, a->vd);

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);
    shift = tcg_const_i32(tcg_ctx, frac_bits);

    /* Switch on op:U:sx bits */
    switch (a->opc) {
    case 0:
        gen_helper_vfp_shtoh_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 1:
        gen_helper_vfp_sltoh_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 2:
        gen_helper_vfp_uhtoh_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 3:
        gen_helper_vfp_ultoh_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 4:
        gen_helper_vfp_toshh_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 5:
        gen_helper_vfp_toslh_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 6:
        gen_helper_vfp_touhh_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 7:
        gen_helper_vfp_toulh_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    default:
        g_assert_not_reached();
    }

    vfp_store_reg32(s, vd, a->vd);
    tcg_temp_free_i32(tcg_ctx, vd);
    tcg_temp_free_i32(tcg_ctx, shift);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

static bool trans_VCVT_fix_sp(DisasContext *s, arg_VCVT_fix_sp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 vd, shift;
    TCGv_ptr fpst;
    int frac_bits;

    if (!dc_isar_feature(aa32_fpsp_v3, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    frac_bits = (a->opc & 1) ? (32 - a->imm) : (16 - a->imm);

    vd = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, vd, a->vd);

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    shift = tcg_const_i32(tcg_ctx, frac_bits);

    /* Switch on op:U:sx bits */
    switch (a->opc) {
    case 0:
        gen_helper_vfp_shtos_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 1:
        gen_helper_vfp_sltos_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 2:
        gen_helper_vfp_uhtos_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 3:
        gen_helper_vfp_ultos_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 4:
        gen_helper_vfp_toshs_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 5:
        gen_helper_vfp_tosls_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 6:
        gen_helper_vfp_touhs_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 7:
        gen_helper_vfp_touls_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    default:
        g_assert_not_reached();
    }

    vfp_store_reg32(s, vd, a->vd);
    tcg_temp_free_i32(tcg_ctx, vd);
    tcg_temp_free_i32(tcg_ctx, shift);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

static bool trans_VCVT_fix_dp(DisasContext *s, arg_VCVT_fix_dp *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i64 vd;
    TCGv_i32 shift;
    TCGv_ptr fpst;
    int frac_bits;

    if (!dc_isar_feature(aa32_fpdp_v3, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vd & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    frac_bits = (a->opc & 1) ? (32 - a->imm) : (16 - a->imm);

    vd = tcg_temp_new_i64(tcg_ctx);
    vfp_load_reg64(s, vd, a->vd);

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    shift = tcg_const_i32(tcg_ctx, frac_bits);

    /* Switch on op:U:sx bits */
    switch (a->opc) {
    case 0:
        gen_helper_vfp_shtod_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 1:
        gen_helper_vfp_sltod_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 2:
        gen_helper_vfp_uhtod_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 3:
        gen_helper_vfp_ultod_round_to_nearest(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 4:
        gen_helper_vfp_toshd_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 5:
        gen_helper_vfp_tosld_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 6:
        gen_helper_vfp_touhd_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    case 7:
        gen_helper_vfp_tould_round_to_zero(tcg_ctx, vd, vd, shift, fpst);
        break;
    default:
        g_assert_not_reached();
    }

    vfp_store_reg64(s, vd, a->vd);
    tcg_temp_free_i64(tcg_ctx, vd);
    tcg_temp_free_i32(tcg_ctx, shift);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

static bool trans_VCVT_hp_int(DisasContext *s, arg_VCVT_sp_int *a)
{
    TCGv_i32 vm;
    TCGv_ptr fpst;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR_F16);
    vm = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, vm, a->vm);

    if (a->s) {
        if (a->rz) {
            gen_helper_vfp_tosizh(tcg_ctx, vm, vm, fpst);
        } else {
            gen_helper_vfp_tosih(tcg_ctx, vm, vm, fpst);
        }
    } else {
        if (a->rz) {
            gen_helper_vfp_touizh(tcg_ctx, vm, vm, fpst);
        } else {
            gen_helper_vfp_touih(tcg_ctx, vm, vm, fpst);
        }
    }
    vfp_store_reg32(s, vm, a->vd);
    tcg_temp_free_i32(tcg_ctx, vm);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

static bool trans_VCVT_sp_int(DisasContext *s, arg_VCVT_sp_int *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 vm;
    TCGv_ptr fpst;

    if (!dc_isar_feature(aa32_fpsp_v2, s)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    vm = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, vm, a->vm);

    if (a->s) {
        if (a->rz) {
            gen_helper_vfp_tosizs(tcg_ctx, vm, vm, fpst);
        } else {
            gen_helper_vfp_tosis(tcg_ctx, vm, vm, fpst);
        }
    } else {
        if (a->rz) {
            gen_helper_vfp_touizs(tcg_ctx, vm, vm, fpst);
        } else {
            gen_helper_vfp_touis(tcg_ctx, vm, vm, fpst);
        }
    }
    vfp_store_reg32(s, vm, a->vd);
    tcg_temp_free_i32(tcg_ctx, vm);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

static bool trans_VCVT_dp_int(DisasContext *s, arg_VCVT_dp_int *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 vd;
    TCGv_i64 vm;
    TCGv_ptr fpst;

    if (!dc_isar_feature(aa32_fpdp_v2, s)) {
        return false;
    }

    /* UNDEF accesses to D16-D31 if they don't exist. */
    if (!dc_isar_feature(aa32_simd_r32, s) && (a->vm & 0x10)) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    fpst = fpstatus_ptr(tcg_ctx, FPST_FPCR);
    vm = tcg_temp_new_i64(tcg_ctx);
    vd = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg64(s, vm, a->vm);

    if (a->s) {
        if (a->rz) {
            gen_helper_vfp_tosizd(tcg_ctx, vd, vm, fpst);
        } else {
            gen_helper_vfp_tosid(tcg_ctx, vd, vm, fpst);
        }
    } else {
        if (a->rz) {
            gen_helper_vfp_touizd(tcg_ctx, vd, vm, fpst);
        } else {
            gen_helper_vfp_touid(tcg_ctx, vd, vm, fpst);
        }
    }
    vfp_store_reg32(s, vd, a->vd);
    tcg_temp_free_i32(tcg_ctx, vd);
    tcg_temp_free_i64(tcg_ctx, vm);
    tcg_temp_free_ptr(tcg_ctx, fpst);
    return true;
}

/*
 * Decode VLLDM and VLSTM are nonstandard because:
 *  * if there is no FPU then these insns must NOP in
 *    Secure state and UNDEF in Nonsecure state
 *  * if there is an FPU then these insns do not have
 *    the usual behaviour that vfp_access_check() provides of
 *    being controlled by CPACR/NSACR enable bits or the
 *    lazy-stacking logic.
 */
static bool trans_VLLDM_VLSTM(DisasContext *s, arg_VLLDM_VLSTM *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 fptr;

    if (!arm_dc_feature(s, ARM_FEATURE_M) ||
        !arm_dc_feature(s, ARM_FEATURE_V8)) {
        return false;
    }

    if (a->op) {
        /*
         * T2 encoding ({D0-D31} reglist): v8.1M and up. We choose not
         * to take the IMPDEF option to make memory accesses to the stack
         * slots that correspond to the D16-D31 registers (discarding
         * read data and writing UNKNOWN values), so for us the T2
         * encoding behaves identically to the T1 encoding.
         */
        if (!arm_dc_feature(s, ARM_FEATURE_V8_1M)) {
            return false;
        }
    } else {
        /*
         * T1 encoding ({D0-D15} reglist); undef if we have 32 Dregs.
         * This is currently architecturally impossible, but we add the
         * check to stay in line with the pseudocode. Note that we must
         * emit code for the UNDEF so it takes precedence over the NOCP.
         */
        if (dc_isar_feature(aa32_simd_r32, s)) {
            unallocated_encoding(s);
            return true;
        }
    }

    /*
     * If not secure, UNDEF. We must emit code for this
     * rather than returning false so that this takes
     * precedence over the m-nocp.decode NOCP fallback.
     */
    if (!s->v8m_secure) {
        unallocated_encoding(s);
        return true;
    }
    /* If no fpu, NOP. */
    if (!dc_isar_feature(aa32_vfp, s)) {
        return true;
    }

    fptr = load_reg(s, a->rn);
    if (a->l) {
        gen_helper_v7m_vlldm(tcg_ctx, tcg_ctx->cpu_env, fptr);
    } else {
        gen_helper_v7m_vlstm(tcg_ctx, tcg_ctx->cpu_env, fptr);
    }
    tcg_temp_free_i32(tcg_ctx, fptr);

    /* End the TB, because we have updated FP control bits */
    s->base.is_jmp = DISAS_UPDATE_EXIT;
    return true;
}

static bool trans_VSCCLRM(DisasContext *s, arg_VSCCLRM *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int btmreg, topreg;
    TCGv_i64 zero;
    TCGv_i32 aspen, sfpa;

    if (!dc_isar_feature(aa32_m_sec_state, s)) {
        /* Before v8.1M, fall through in decode to NOCP check */
        return false;
    }

    /* Explicitly UNDEF because this takes precedence over NOCP */
    if (!arm_dc_feature(s, ARM_FEATURE_M_MAIN) || !s->v8m_secure) {
        unallocated_encoding(s);
        return true;
    }

    if (!dc_isar_feature(aa32_vfp_simd, s)) {
        /* NOP if we have neither FP nor MVE */
        return true;
    }

    /*
     * If FPCCR.ASPEN != 0 && CONTROL_S.SFPA == 0 then there is no
     * active floating point context so we must NOP (without doing
     * any lazy state preservation or the NOCP check).
     */
    aspen = load_cpu_field(s, v7m.fpccr[M_REG_S]);
    sfpa = load_cpu_field(s, v7m.control[M_REG_S]);
    tcg_gen_andi_i32(tcg_ctx, aspen, aspen, R_V7M_FPCCR_ASPEN_MASK);
    tcg_gen_xori_i32(tcg_ctx, aspen, aspen, R_V7M_FPCCR_ASPEN_MASK);
    tcg_gen_andi_i32(tcg_ctx, sfpa, sfpa, R_V7M_CONTROL_SFPA_MASK);
    tcg_gen_or_i32(tcg_ctx, sfpa, sfpa, aspen);
    arm_gen_condlabel(s);
    tcg_gen_brcondi_i32(tcg_ctx, TCG_COND_EQ, sfpa, 0, s->condlabel);

    if (s->fp_excp_el != 0) {
        gen_exception_insn(s, s->pc_curr, EXCP_NOCP,
                           syn_uncategorized(), s->fp_excp_el);
        return true;
    }

    topreg = a->vd + a->imm - 1;
    btmreg = a->vd;

    /* Convert to Sreg numbers if the insn specified in Dregs */
    if (a->size == 3) {
        topreg = topreg * 2 + 1;
        btmreg *= 2;
    }

    if (topreg > 63 || (topreg > 31 && !(topreg & 1))) {
        /* UNPREDICTABLE: we choose to undef */
        unallocated_encoding(s);
        return true;
    }

    /* Silently ignore requests to clear D16-D31 if they don't exist */
    if (topreg > 31 && !dc_isar_feature(aa32_simd_r32, s)) {
        topreg = 31;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    /* Zero the Sregs from btmreg to topreg inclusive. */
    zero = tcg_const_i64(tcg_ctx, 0);
    if (btmreg & 1) {
        write_neon_element64(s, zero, btmreg >> 1, 1, MO_32);
        btmreg++;
    }
    for (; btmreg + 1 <= topreg; btmreg += 2) {
        write_neon_element64(s, zero, btmreg >> 1, 0, MO_64);
    }
    if (btmreg == topreg) {
        write_neon_element64(s, zero, btmreg >> 1, 0, MO_32);
        btmreg++;
    }
    assert(btmreg == topreg + 1);
    /* TODO: when MVE is implemented, zero VPR here */
    return true;
}

static bool trans_NOCP(DisasContext *s, arg_NOCP *a)
{
    /*
     * Handle M-profile early check for disabled coprocessor:
     * all we need to do here is emit the NOCP exception if
     * the coprocessor is disabled. Otherwise we return false
     * and the real VFP/etc decode will handle the insn.
     */
    assert(arm_dc_feature(s, ARM_FEATURE_M));

    if (a->cp == 11) {
        a->cp = 10;
    }
    if (arm_dc_feature(s, ARM_FEATURE_V8_1M) &&
        (a->cp == 8 || a->cp == 9 || a->cp == 14 || a->cp == 15)) {
        /* in v8.1M cp 8, 9, 14, 15 also are governed by the cp10 enable */
        a->cp = 10;
    }

    if (a->cp != 10) {
        gen_exception_insn(s, s->pc_curr, EXCP_NOCP,
                           syn_uncategorized(), default_exception_el(s));
        return true;
    }

    if (s->fp_excp_el != 0) {
        gen_exception_insn(s, s->pc_curr, EXCP_NOCP,
                           syn_uncategorized(), s->fp_excp_el);
        return true;
    }

    return false;
}

static bool trans_NOCP_8_1(DisasContext *s, arg_nocp *a)
{
    /* This range needs a coprocessor check for v8.1M and later only */
    if (!arm_dc_feature(s, ARM_FEATURE_V8_1M)) {
        return false;
    }
    return trans_NOCP(s, a);
}

static bool trans_VINS(DisasContext *s, arg_VINS *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 rd, rm;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (s->vec_len != 0 || s->vec_stride != 0) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    /* Insert low half of Vm into high half of Vd */
    rm = tcg_temp_new_i32(tcg_ctx);
    rd = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, rm, a->vm);
    vfp_load_reg32(s, rd, a->vd);
    tcg_gen_deposit_i32(tcg_ctx, rd, rd, rm, 16, 16);
    vfp_store_reg32(s, rd, a->vd);
    tcg_temp_free_i32(tcg_ctx, rm);
    tcg_temp_free_i32(tcg_ctx, rd);
    return true;
}

static bool trans_VMOVX(DisasContext *s, arg_VINS *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 rm;

    if (!dc_isar_feature(aa32_fp16_arith, s)) {
        return false;
    }

    if (s->vec_len != 0 || s->vec_stride != 0) {
        return false;
    }

    if (!vfp_access_check(s)) {
        return true;
    }

    /* Set Vd to high half of Vm */
    rm = tcg_temp_new_i32(tcg_ctx);
    vfp_load_reg32(s, rm, a->vm);
    tcg_gen_shri_i32(tcg_ctx, rm, rm, 16);
    vfp_store_reg32(s, rm, a->vd);
    tcg_temp_free_i32(tcg_ctx, rm);
    return true;
}

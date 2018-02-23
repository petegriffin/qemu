/*
 * AArch64 SVE translation
 *
 * Copyright (c) 2018 Linaro, Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "tcg-op.h"
#include "tcg-op-gvec.h"
#include "tcg-gvec-desc.h"
#include "qemu/log.h"
#include "arm_ldst.h"
#include "translate.h"
#include "internals.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/log.h"
#include "trace-tcg.h"
#include "translate-a64.h"

typedef void GVecGen2Fn(unsigned, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void GVecGen3Fn(unsigned, uint32_t, uint32_t,
                        uint32_t, uint32_t, uint32_t);

/*
 * Include the generated decoder.
 */

#include "decode-sve.inc.c"

/*
 * Implement all of the translator functions referenced by the decoder.
 */

/* Return the offset info CPUARMState of the predicate vector register Pn.
 * Note for this purpose, FFR is P16.  */
static inline int pred_full_reg_offset(DisasContext *s, int regno)
{
    return offsetof(CPUARMState, vfp.pregs[regno]);
}

/* Return the byte size of the whole predicate register, VL / 64.  */
static inline int pred_full_reg_size(DisasContext *s)
{
    return s->sve_len >> 3;
}

/* Round up the size of a predicate register to a size allowed by
 * the tcg vector infrastructure.  Any operation which uses this
 * size may assume that the bits above pred_full_reg_size are zero,
 * and must leave them the same way.
 *
 * Note that this is not needed for the vector registers as they
 * are always properly sized for tcg vectors.
 */
static int size_for_gvec(int size)
{
    if (size <= 8) {
        return 8;
    } else {
        return QEMU_ALIGN_UP(size, 16);
    }
}

static int pred_gvec_reg_size(DisasContext *s)
{
    return size_for_gvec(pred_full_reg_size(s));
}

/* Invoke a vector expander on two Zregs.  */
static void do_vector2_z(DisasContext *s, GVecGen2Fn *gvec_fn,
                         int esz, int rd, int rn)
{
    unsigned vsz = vec_full_reg_size(s);
    gvec_fn(esz, vec_full_reg_offset(s, rd),
            vec_full_reg_offset(s, rn), vsz, vsz);
}

/* Invoke a vector expander on three Zregs.  */
static void do_vector3_z(DisasContext *s, GVecGen3Fn *gvec_fn,
                         int esz, int rd, int rn, int rm)
{
    unsigned vsz = vec_full_reg_size(s);
    gvec_fn(esz, vec_full_reg_offset(s, rd), vec_full_reg_offset(s, rn),
            vec_full_reg_offset(s, rm), vsz, vsz);
}

/* Invoke a vector move on two Zregs.  */
static void do_mov_z(DisasContext *s, int rd, int rn)
{
    do_vector2_z(s, tcg_gen_gvec_mov, 0, rd, rn);
}

/* Invoke a vector expander on two Pregs.  */
static void do_vector2_p(DisasContext *s, GVecGen2Fn *gvec_fn,
                         int esz, int rd, int rn)
{
    unsigned psz = pred_gvec_reg_size(s);
    gvec_fn(esz, pred_full_reg_offset(s, rd),
            pred_full_reg_offset(s, rn), psz, psz);
}

/* Invoke a vector expander on three Pregs.  */
static void do_vector3_p(DisasContext *s, GVecGen3Fn *gvec_fn,
                         int esz, int rd, int rn, int rm)
{
    unsigned psz = pred_gvec_reg_size(s);
    gvec_fn(esz, pred_full_reg_offset(s, rd), pred_full_reg_offset(s, rn),
            pred_full_reg_offset(s, rm), psz, psz);
}

/* Invoke a vector operation on four Pregs.  */
static void do_vecop4_p(DisasContext *s, const GVecGen4 *gvec_op,
                        int rd, int rn, int rm, int rg)
{
    unsigned psz = pred_gvec_reg_size(s);
    tcg_gen_gvec_4(pred_full_reg_offset(s, rd), pred_full_reg_offset(s, rn),
                   pred_full_reg_offset(s, rm), pred_full_reg_offset(s, rg),
                   psz, psz, gvec_op);
}

/* Invoke a vector move on two Pregs.  */
static void do_mov_p(DisasContext *s, int rd, int rn)
{
    do_vector2_p(s, tcg_gen_gvec_mov, 0, rd, rn);
}

/* Set the cpu flags as per a return from an SVE helper.  */
static void do_pred_flags(TCGv_i32 t)
{
    tcg_gen_mov_i32(cpu_NF, t);
    tcg_gen_andi_i32(cpu_ZF, t, 2);
    tcg_gen_andi_i32(cpu_CF, t, 1);
    tcg_gen_movi_i32(cpu_VF, 0);
}

/* Subroutines computing the ARM PredTest psuedofunction.  */
static void do_predtest1(TCGv_i64 d, TCGv_i64 g)
{
    TCGv_i32 t = tcg_temp_new_i32();

    gen_helper_sve_predtest1(t, d, g);
    do_pred_flags(t);
    tcg_temp_free_i32(t);
}

static void do_predtest(DisasContext *s, int dofs, int gofs, int words)
{
    TCGv_ptr dptr = tcg_temp_new_ptr();
    TCGv_ptr gptr = tcg_temp_new_ptr();
    TCGv_i32 t;

    tcg_gen_addi_ptr(dptr, cpu_env, dofs);
    tcg_gen_addi_ptr(gptr, cpu_env, gofs);
    t = tcg_const_i32(words);

    gen_helper_sve_predtest(t, dptr, gptr, t);
    tcg_temp_free_ptr(dptr);
    tcg_temp_free_ptr(gptr);

    do_pred_flags(t);
    tcg_temp_free_i32(t);
}

/* For each element size, the bits within a predicate word that are active.  */
const uint64_t pred_esz_masks[4] = {
    0xffffffffffffffffull, 0x5555555555555555ull,
    0x1111111111111111ull, 0x0101010101010101ull
};

/*
 *** SVE Logical - Unpredicated Group
 */

static void trans_AND_zzz(DisasContext *s, arg_rrr_esz *a, uint32_t insn)
{
    do_vector3_z(s, tcg_gen_gvec_and, 0, a->rd, a->rn, a->rm);
}

static void trans_ORR_zzz(DisasContext *s, arg_rrr_esz *a, uint32_t insn)
{
    if (a->rn == a->rm) { /* MOV */
        do_mov_z(s, a->rd, a->rn);
    } else {
        do_vector3_z(s, tcg_gen_gvec_or, 0, a->rd, a->rn, a->rm);
    }
}

static void trans_EOR_zzz(DisasContext *s, arg_rrr_esz *a, uint32_t insn)
{
    do_vector3_z(s, tcg_gen_gvec_xor, 0, a->rd, a->rn, a->rm);
}

static void trans_BIC_zzz(DisasContext *s, arg_BIC_zzz *a, uint32_t insn)
{
    do_vector3_z(s, tcg_gen_gvec_andc, 0, a->rd, a->rn, a->rm);
}

/*
 *** SVE Integer Arithmetic - Binary Predicated Group
 */

static void do_zpzz_ool(DisasContext *s, arg_rprr_esz *a, gen_helper_gvec_4 *fn)
{
    unsigned vsz = vec_full_reg_size(s);
    if (fn == NULL) {
        unallocated_encoding(s);
        return;
    }
    tcg_gen_gvec_4_ool(vec_full_reg_offset(s, a->rd),
                       vec_full_reg_offset(s, a->rn),
                       vec_full_reg_offset(s, a->rm),
                       pred_full_reg_offset(s, a->pg),
                       vsz, vsz, 0, fn);
}

#define DO_ZPZZ(NAME, name) \
void trans_##NAME##_zpzz(DisasContext *s, arg_rprr_esz *a, uint32_t insn) \
{                                                                         \
    static gen_helper_gvec_4 * const fns[4] = {                           \
        gen_helper_sve_##name##_zpzz_b, gen_helper_sve_##name##_zpzz_h,   \
        gen_helper_sve_##name##_zpzz_s, gen_helper_sve_##name##_zpzz_d,   \
    };                                                                    \
    do_zpzz_ool(s, a, fns[a->esz]);                                       \
}

DO_ZPZZ(AND, and)
DO_ZPZZ(EOR, eor)
DO_ZPZZ(ORR, orr)
DO_ZPZZ(BIC, bic)

DO_ZPZZ(ADD, add)
DO_ZPZZ(SUB, sub)

DO_ZPZZ(SMAX, smax)
DO_ZPZZ(UMAX, umax)
DO_ZPZZ(SMIN, smin)
DO_ZPZZ(UMIN, umin)
DO_ZPZZ(SABD, sabd)
DO_ZPZZ(UABD, uabd)

DO_ZPZZ(MUL, mul)
DO_ZPZZ(SMULH, smulh)
DO_ZPZZ(UMULH, umulh)

void trans_SDIV_zpzz(DisasContext *s, arg_rprr_esz *a, uint32_t insn)
{
    static gen_helper_gvec_4 * const fns[4] = {
        NULL, NULL, gen_helper_sve_sdiv_zpzz_s, gen_helper_sve_sdiv_zpzz_d
    };
    do_zpzz_ool(s, a, fns[a->esz]);
}

void trans_UDIV_zpzz(DisasContext *s, arg_rprr_esz *a, uint32_t insn)
{
    static gen_helper_gvec_4 * const fns[4] = {
        NULL, NULL, gen_helper_sve_udiv_zpzz_s, gen_helper_sve_udiv_zpzz_d
    };
    do_zpzz_ool(s, a, fns[a->esz]);
}

#undef DO_ZPZZ

/*
 *** SVE Integer Reduction Group
 */

typedef void gen_helper_gvec_reduc(TCGv_i64, TCGv_ptr, TCGv_ptr, TCGv_i32);
static void do_vpz_ool(DisasContext *s, arg_rpr_esz *a,
                       gen_helper_gvec_reduc *fn)
{
    unsigned vsz = vec_full_reg_size(s);
    TCGv_ptr t_zn, t_pg;
    TCGv_i32 desc;
    TCGv_i64 temp;

    if (fn == 0) {
        unallocated_encoding(s);
        return;
    }

    desc = tcg_const_i32(simd_desc(vsz, vsz, 0));
    temp = tcg_temp_new_i64();
    t_zn = tcg_temp_new_ptr();
    t_pg = tcg_temp_new_ptr();

    tcg_gen_addi_ptr(t_zn, cpu_env, vec_full_reg_offset(s, a->rn));
    tcg_gen_addi_ptr(t_pg, cpu_env, pred_full_reg_offset(s, a->pg));
    fn(temp, t_zn, t_pg, desc);
    tcg_temp_free_ptr(t_zn);
    tcg_temp_free_ptr(t_pg);
    tcg_temp_free_i32(desc);

    write_fp_dreg(s, a->rd, temp);
    tcg_temp_free_i64(temp);
}

#define DO_VPZ(NAME, name) \
static void trans_##NAME(DisasContext *s, arg_rpr_esz *a, uint32_t insn) \
{                                                                        \
    static gen_helper_gvec_reduc * const fns[4] = {                      \
        gen_helper_sve_##name##_b, gen_helper_sve_##name##_h,            \
        gen_helper_sve_##name##_s, gen_helper_sve_##name##_d,            \
    };                                                                   \
    do_vpz_ool(s, a, fns[a->esz]);                                       \
}

DO_VPZ(ORV, orv)
DO_VPZ(ANDV, andv)
DO_VPZ(EORV, eorv)

DO_VPZ(UADDV, uaddv)
DO_VPZ(SMAXV, smaxv)
DO_VPZ(UMAXV, umaxv)
DO_VPZ(SMINV, sminv)
DO_VPZ(UMINV, uminv)

static void trans_SADDV(DisasContext *s, arg_rpr_esz *a, uint32_t insn)
{
    static gen_helper_gvec_reduc * const fns[4] = {
        gen_helper_sve_saddv_b, gen_helper_sve_saddv_h,
        gen_helper_sve_saddv_s, NULL
    };
    do_vpz_ool(s, a, fns[a->esz]);
}

#undef DO_VPZ

/*
 *** SVE Predicate Logical Operations Group
 */

static void do_pppp_flags(DisasContext *s, arg_rprr_s *a,
                          const GVecGen4 *gvec_op)
{
    unsigned psz = pred_gvec_reg_size(s);
    int dofs = pred_full_reg_offset(s, a->rd);
    int nofs = pred_full_reg_offset(s, a->rn);
    int mofs = pred_full_reg_offset(s, a->rm);
    int gofs = pred_full_reg_offset(s, a->pg);

    if (psz == 8) {
        /* Do the operation and the flags generation in temps.  */
        TCGv_i64 pd = tcg_temp_new_i64();
        TCGv_i64 pn = tcg_temp_new_i64();
        TCGv_i64 pm = tcg_temp_new_i64();
        TCGv_i64 pg = tcg_temp_new_i64();

        tcg_gen_ld_i64(pn, cpu_env, nofs);
        tcg_gen_ld_i64(pm, cpu_env, mofs);
        tcg_gen_ld_i64(pg, cpu_env, gofs);

        gvec_op->fni8(pd, pn, pm, pg);
        tcg_gen_st_i64(pd, cpu_env, dofs);

        do_predtest1(pd, pg);

        tcg_temp_free_i64(pd);
        tcg_temp_free_i64(pn);
        tcg_temp_free_i64(pm);
        tcg_temp_free_i64(pg);
    } else {
        /* The operation and flags generation is large.  The computation
         * of the flags depends on the original contents of the guarding
         * predicate.  If the destination overwrites the guarding predicate,
         * then the easiest way to get this right is to save a copy.
          */
        int tofs = gofs;
        if (a->rd == a->pg) {
            tofs = offsetof(CPUARMState, vfp.preg_tmp);
            tcg_gen_gvec_mov(0, tofs, gofs, psz, psz);
        }

        tcg_gen_gvec_4(dofs, nofs, mofs, gofs, psz, psz, gvec_op);
        do_predtest(s, dofs, tofs, psz / 8);
    }
}

static void gen_and_pg_i64(TCGv_i64 pd, TCGv_i64 pn, TCGv_i64 pm, TCGv_i64 pg)
{
    tcg_gen_and_i64(pd, pn, pm);
    tcg_gen_and_i64(pd, pd, pg);
}

static void gen_and_pg_vec(unsigned vece, TCGv_vec pd, TCGv_vec pn,
                           TCGv_vec pm, TCGv_vec pg)
{
    tcg_gen_and_vec(vece, pd, pn, pm);
    tcg_gen_and_vec(vece, pd, pd, pg);
}

static void trans_AND_pppp(DisasContext *s, arg_rprr_s *a, uint32_t insn)
{
    static const GVecGen4 op = {
        .fni8 = gen_and_pg_i64,
        .fniv = gen_and_pg_vec,
        .fno = gen_helper_sve_and_pppp,
        .prefer_i64 = TCG_TARGET_REG_BITS == 64,
    };
    if (a->s) {
        do_pppp_flags(s, a, &op);
    } else if (a->pg == a->rn && a->rn == a->rm) {
        do_mov_p(s, a->rd, a->rn);
    } else if (a->pg == a->rn || a->pg == a->rm) {
        do_vector3_p(s, tcg_gen_gvec_and, 0, a->rd, a->rn, a->rm);
    } else {
        do_vecop4_p(s, &op, a->rd, a->rn, a->rm, a->pg);
    }
}

static void gen_bic_pg_i64(TCGv_i64 pd, TCGv_i64 pn, TCGv_i64 pm, TCGv_i64 pg)
{
    tcg_gen_andc_i64(pd, pn, pm);
    tcg_gen_and_i64(pd, pd, pg);
}

static void gen_bic_pg_vec(unsigned vece, TCGv_vec pd, TCGv_vec pn,
                           TCGv_vec pm, TCGv_vec pg)
{
    tcg_gen_andc_vec(vece, pd, pn, pm);
    tcg_gen_and_vec(vece, pd, pd, pg);
}

static void trans_BIC_pppp(DisasContext *s, arg_rprr_s *a, uint32_t insn)
{
    static const GVecGen4 op = {
        .fni8 = gen_bic_pg_i64,
        .fniv = gen_bic_pg_vec,
        .fno = gen_helper_sve_bic_pppp,
        .prefer_i64 = TCG_TARGET_REG_BITS == 64,
    };
    if (a->s) {
        do_pppp_flags(s, a, &op);
    } else if (a->pg == a->rn) {
        do_vector3_p(s, tcg_gen_gvec_andc, 0, a->rd, a->rn, a->rm);
    } else {
        do_vecop4_p(s, &op, a->rd, a->rn, a->rm, a->pg);
    }
}

static void gen_eor_pg_i64(TCGv_i64 pd, TCGv_i64 pn, TCGv_i64 pm, TCGv_i64 pg)
{
    tcg_gen_xor_i64(pd, pn, pm);
    tcg_gen_and_i64(pd, pd, pg);
}

static void gen_eor_pg_vec(unsigned vece, TCGv_vec pd, TCGv_vec pn,
                           TCGv_vec pm, TCGv_vec pg)
{
    tcg_gen_xor_vec(vece, pd, pn, pm);
    tcg_gen_and_vec(vece, pd, pd, pg);
}

static void trans_EOR_pppp(DisasContext *s, arg_rprr_s *a, uint32_t insn)
{
    static const GVecGen4 op = {
        .fni8 = gen_eor_pg_i64,
        .fniv = gen_eor_pg_vec,
        .fno = gen_helper_sve_eor_pppp,
        .prefer_i64 = TCG_TARGET_REG_BITS == 64,
    };
    if (a->s) {
        do_pppp_flags(s, a, &op);
    } else {
        do_vecop4_p(s, &op, a->rd, a->rn, a->rm, a->pg);
    }
}

static void gen_sel_pg_i64(TCGv_i64 pd, TCGv_i64 pn, TCGv_i64 pm, TCGv_i64 pg)
{
    tcg_gen_and_i64(pn, pn, pg);
    tcg_gen_andc_i64(pm, pm, pg);
    tcg_gen_or_i64(pd, pn, pm);
}

static void gen_sel_pg_vec(unsigned vece, TCGv_vec pd, TCGv_vec pn,
                           TCGv_vec pm, TCGv_vec pg)
{
    tcg_gen_and_vec(vece, pn, pn, pg);
    tcg_gen_andc_vec(vece, pm, pm, pg);
    tcg_gen_or_vec(vece, pd, pn, pm);
}

static void trans_SEL_pppp(DisasContext *s, arg_rprr_s *a, uint32_t insn)
{
    static const GVecGen4 op = {
        .fni8 = gen_sel_pg_i64,
        .fniv = gen_sel_pg_vec,
        .fno = gen_helper_sve_sel_pppp,
        .prefer_i64 = TCG_TARGET_REG_BITS == 64,
    };
    if (a->s) {
        unallocated_encoding(s);
    } else {
        do_vecop4_p(s, &op, a->rd, a->rn, a->rm, a->pg);
    }
}

static void gen_orr_pg_i64(TCGv_i64 pd, TCGv_i64 pn, TCGv_i64 pm, TCGv_i64 pg)
{
    tcg_gen_or_i64(pd, pn, pm);
    tcg_gen_and_i64(pd, pd, pg);
}

static void gen_orr_pg_vec(unsigned vece, TCGv_vec pd, TCGv_vec pn,
                           TCGv_vec pm, TCGv_vec pg)
{
    tcg_gen_or_vec(vece, pd, pn, pm);
    tcg_gen_and_vec(vece, pd, pd, pg);
}

static void trans_ORR_pppp(DisasContext *s, arg_rprr_s *a, uint32_t insn)
{
    static const GVecGen4 op = {
        .fni8 = gen_orr_pg_i64,
        .fniv = gen_orr_pg_vec,
        .fno = gen_helper_sve_orr_pppp,
        .prefer_i64 = TCG_TARGET_REG_BITS == 64,
    };
    if (a->s) {
        do_pppp_flags(s, a, &op);
    } else if (a->pg == a->rn && a->rn == a->rm) {
        do_mov_p(s, a->rd, a->rn);
    } else {
        do_vecop4_p(s, &op, a->rd, a->rn, a->rm, a->pg);
    }
}

static void gen_orn_pg_i64(TCGv_i64 pd, TCGv_i64 pn, TCGv_i64 pm, TCGv_i64 pg)
{
    tcg_gen_orc_i64(pd, pn, pm);
    tcg_gen_and_i64(pd, pd, pg);
}

static void gen_orn_pg_vec(unsigned vece, TCGv_vec pd, TCGv_vec pn,
                           TCGv_vec pm, TCGv_vec pg)
{
    tcg_gen_orc_vec(vece, pd, pn, pm);
    tcg_gen_and_vec(vece, pd, pd, pg);
}

static void trans_ORN_pppp(DisasContext *s, arg_rprr_s *a, uint32_t insn)
{
    static const GVecGen4 op = {
        .fni8 = gen_orn_pg_i64,
        .fniv = gen_orn_pg_vec,
        .fno = gen_helper_sve_orn_pppp,
        .prefer_i64 = TCG_TARGET_REG_BITS == 64,
    };
    if (a->s) {
        do_pppp_flags(s, a, &op);
    } else {
        do_vecop4_p(s, &op, a->rd, a->rn, a->rm, a->pg);
    }
}

static void gen_nor_pg_i64(TCGv_i64 pd, TCGv_i64 pn, TCGv_i64 pm, TCGv_i64 pg)
{
    tcg_gen_or_i64(pd, pn, pm);
    tcg_gen_andc_i64(pd, pg, pd);
}

static void gen_nor_pg_vec(unsigned vece, TCGv_vec pd, TCGv_vec pn,
                           TCGv_vec pm, TCGv_vec pg)
{
    tcg_gen_or_vec(vece, pd, pn, pm);
    tcg_gen_andc_vec(vece, pd, pg, pd);
}

static void trans_NOR_pppp(DisasContext *s, arg_rprr_s *a, uint32_t insn)
{
    static const GVecGen4 op = {
        .fni8 = gen_nor_pg_i64,
        .fniv = gen_nor_pg_vec,
        .fno = gen_helper_sve_nor_pppp,
        .prefer_i64 = TCG_TARGET_REG_BITS == 64,
    };
    if (a->s) {
        do_pppp_flags(s, a, &op);
    } else {
        do_vecop4_p(s, &op, a->rd, a->rn, a->rm, a->pg);
    }
}

static void gen_nand_pg_i64(TCGv_i64 pd, TCGv_i64 pn, TCGv_i64 pm, TCGv_i64 pg)
{
    tcg_gen_and_i64(pd, pn, pm);
    tcg_gen_andc_i64(pd, pg, pd);
}

static void gen_nand_pg_vec(unsigned vece, TCGv_vec pd, TCGv_vec pn,
                           TCGv_vec pm, TCGv_vec pg)
{
    tcg_gen_and_vec(vece, pd, pn, pm);
    tcg_gen_andc_vec(vece, pd, pg, pd);
}

static void trans_NAND_pppp(DisasContext *s, arg_rprr_s *a, uint32_t insn)
{
    static const GVecGen4 op = {
        .fni8 = gen_nand_pg_i64,
        .fniv = gen_nand_pg_vec,
        .fno = gen_helper_sve_nand_pppp,
        .prefer_i64 = TCG_TARGET_REG_BITS == 64,
    };
    if (a->s) {
        do_pppp_flags(s, a, &op);
    } else {
        do_vecop4_p(s, &op, a->rd, a->rn, a->rm, a->pg);
    }
}

/*
 *** SVE Predicate Misc Group
 */

static void trans_PTEST(DisasContext *s, arg_PTEST *a, uint32_t insn)
{
    int nofs = pred_full_reg_offset(s, a->rn);
    int gofs = pred_full_reg_offset(s, a->pg);
    int words = DIV_ROUND_UP(pred_full_reg_size(s), 8);

    if (words == 1) {
        TCGv_i64 pn = tcg_temp_new_i64();
        TCGv_i64 pg = tcg_temp_new_i64();

        tcg_gen_ld_i64(pn, cpu_env, nofs);
        tcg_gen_ld_i64(pg, cpu_env, gofs);
        do_predtest1(pn, pg);

        tcg_temp_free_i64(pn);
        tcg_temp_free_i64(pg);
    } else {
        do_predtest(s, nofs, gofs, words);
    }
}

/* See the ARM pseudocode DecodePredCount.  */
static unsigned decode_pred_count(unsigned fullsz, int pattern, int esz)
{
    unsigned elements = fullsz >> esz;
    unsigned bound;

    switch (pattern) {
    case 0x0: /* POW2 */
        return pow2floor(elements);
    case 0x1: /* VL1 */
    case 0x2: /* VL2 */
    case 0x3: /* VL3 */
    case 0x4: /* VL4 */
    case 0x5: /* VL5 */
    case 0x6: /* VL6 */
    case 0x7: /* VL7 */
    case 0x8: /* VL8 */
        bound = pattern;
        break;
    case 0x9: /* VL16 */
    case 0xa: /* VL32 */
    case 0xb: /* VL64 */
    case 0xc: /* VL128 */
    case 0xd: /* VL256 */
        bound = 16 << (pattern - 9);
        break;
    case 0x1d: /* MUL4 */
        return elements - elements % 4;
    case 0x1e: /* MUL3 */
        return elements - elements % 3;
    case 0x1f: /* ALL */
        return elements;
    default:   /* #uimm5 */
        return 0;
    }
    return elements >= bound ? bound : 0;
}

static void trans_PTRUE(DisasContext *s, arg_PTRUE *a, uint32_t insn)
{
    unsigned fullsz = vec_full_reg_size(s);
    unsigned ofs = pred_full_reg_offset(s, a->rd);
    unsigned numelem, setsz, i;
    uint64_t word, lastword;
    TCGv_i64 t;

    numelem = decode_pred_count(fullsz, a->pat, a->esz);

    /* Determine what we must store into each bit, and how many.  */
    if (numelem == 0) {
        lastword = word = 0;
        setsz = fullsz;
    } else {
        setsz = numelem << a->esz;
        lastword = word = pred_esz_masks[a->esz];
        if (setsz % 64) {
            lastword &= ~(-1ull << (setsz % 64));
        }
    }

    t = tcg_temp_new_i64();
    if (fullsz <= 64) {
        tcg_gen_movi_i64(t, lastword);
        tcg_gen_st_i64(t, cpu_env, ofs);
        goto done;
    }

    if (word == lastword) {
        unsigned maxsz = size_for_gvec(fullsz / 8);
        unsigned oprsz = size_for_gvec(setsz / 8);

        if (oprsz * 8 == setsz) {
            tcg_gen_gvec_dup64i(ofs, oprsz, maxsz, word);
            goto done;
        }
        if (oprsz * 8 == setsz + 8) {
            tcg_gen_gvec_dup64i(ofs, oprsz, maxsz, word);
            tcg_gen_movi_i64(t, 0);
            tcg_gen_st_i64(t, cpu_env, ofs + oprsz - 8);
            goto done;
        }
    }

    setsz /= 8;
    fullsz /= 8;

    tcg_gen_movi_i64(t, word);
    for (i = 0; i < setsz; i += 8) {
        tcg_gen_st_i64(t, cpu_env, ofs + i);
    }
    if (lastword != word) {
        tcg_gen_movi_i64(t, lastword);
        tcg_gen_st_i64(t, cpu_env, ofs + i);
        i += 8;
    }
    if (i < fullsz) {
        tcg_gen_movi_i64(t, 0);
        for (; i < fullsz; i += 8) {
            tcg_gen_st_i64(t, cpu_env, ofs + i);
        }
    }

 done:
    tcg_temp_free_i64(t);

    /* PTRUES */
    if (a->s) {
        tcg_gen_movi_i32(cpu_NF, -(word != 0));
        tcg_gen_movi_i32(cpu_CF, word == 0);
        tcg_gen_movi_i32(cpu_VF, 0);
        tcg_gen_mov_i32(cpu_ZF, cpu_NF);
    }
}

static void do_pfirst_pnext(DisasContext *s, arg_rr_esz *a,
                            void (*gen_fn)(TCGv_i32, TCGv_ptr,
                                           TCGv_ptr, TCGv_i32))
{
    TCGv_ptr t_pd = tcg_temp_new_ptr();
    TCGv_ptr t_pg = tcg_temp_new_ptr();
    TCGv_i32 t;
    unsigned desc;

    desc = DIV_ROUND_UP(pred_full_reg_size(s), 8);
    desc = deposit32(desc, SIMD_DATA_SHIFT, 2, a->esz);

    tcg_gen_addi_ptr(t_pd, cpu_env, pred_full_reg_offset(s, a->rd));
    tcg_gen_addi_ptr(t_pg, cpu_env, pred_full_reg_offset(s, a->rn));
    t = tcg_const_i32(desc);

    gen_fn(t, t_pd, t_pg, t);
    tcg_temp_free_ptr(t_pd);
    tcg_temp_free_ptr(t_pg);

    do_pred_flags(t);
    tcg_temp_free_i32(t);
}

static void trans_PFIRST(DisasContext *s, arg_rr_esz *a, uint32_t insn)
{
    do_pfirst_pnext(s, a, gen_helper_sve_pfirst);
}

static void trans_PNEXT(DisasContext *s, arg_rr_esz *a, uint32_t insn)
{
    do_pfirst_pnext(s, a, gen_helper_sve_pnext);
}

/*
 *** SVE Memory - 32-bit Gather and Unsized Contiguous Group
 */

/* Subroutine loading a vector register at VOFS of LEN bytes.
 * The load should begin at the address Rn + IMM.
 */

#if UINTPTR_MAX == UINT32_MAX
# define ptr i32
#else
# define ptr i64
#endif

static void do_ldr(DisasContext *s, uint32_t vofs, uint32_t len,
                   int rn, int imm)
{
    uint32_t len_align = QEMU_ALIGN_DOWN(len, 8);
    uint32_t len_remain = len % 8;
    uint32_t nparts = len / 8 + ctpop8(len_remain);
    int midx = get_mem_index(s);
    TCGv_i64 addr, t0, t1;

    addr = tcg_temp_new_i64();
    t0 = tcg_temp_new_i64();

    /* Note that unpredicated load/store of vector/predicate registers
     * are defined as a stream of bytes, which equates to little-endian
     * operations on larger quantities.  There is no nice way to force
     * a little-endian load for aarch64_be-linux-user out of line.
     *
     * Attempt to keep code expansion to a minimum by limiting the
     * amount of unrolling done.
     */
    if (nparts <= 4) {
        int i;

        for (i = 0; i < len_align; i += 8) {
            tcg_gen_addi_i64(addr, cpu_reg_sp(s, rn), imm + i);
            tcg_gen_qemu_ld_i64(t0, addr, midx, MO_LEQ);
            tcg_gen_st_i64(t0, cpu_env, vofs + i);
        }
    } else {
        TCGLabel *loop = gen_new_label();
        TCGv_ptr i = TCGV_NAT_TO_PTR(glue(tcg_const_local_, ptr)(0));
        TCGv_ptr dest;

        gen_set_label(loop);

        /* Minimize the number of local temps that must be re-read from
         * the stack each iteration.  Instead, re-compute values other
         * than the loop counter.
         */
        dest = tcg_temp_new_ptr();
        tcg_gen_addi_ptr(dest, i, imm);
#if UINTPTR_MAX == UINT32_MAX
        tcg_gen_extu_i32_i64(addr, TCGV_PTR_TO_NAT(dest));
        tcg_gen_add_i64(addr, addr, cpu_reg_sp(s, rn));
#else
        tcg_gen_add_i64(addr, TCGV_PTR_TO_NAT(dest), cpu_reg_sp(s, rn));
#endif

        tcg_gen_qemu_ld_i64(t0, addr, midx, MO_LEQ);

        tcg_gen_add_ptr(dest, cpu_env, i);
        tcg_gen_addi_ptr(i, i, 8);
        tcg_gen_st_i64(t0, dest, vofs);
        tcg_temp_free_ptr(dest);

        glue(tcg_gen_brcondi_, ptr)(TCG_COND_LTU, TCGV_PTR_TO_NAT(i),
                                    len_align, loop);
        tcg_temp_free_ptr(i);
    }

    /* Predicate register loads can be any multiple of 2.
     * Note that we still store the entire 64-bit unit into cpu_env.
     */
    if (len_remain) {
        tcg_gen_addi_i64(addr, cpu_reg_sp(s, rn), imm + len_align);

        switch (len_remain) {
        case 2:
        case 4:
        case 8:
            tcg_gen_qemu_ld_i64(t0, addr, midx, MO_LE | ctz32(len_remain));
            break;

        case 6:
            t1 = tcg_temp_new_i64();
            tcg_gen_qemu_ld_i64(t0, addr, midx, MO_LEUL);
            tcg_gen_addi_i64(addr, addr, 4);
            tcg_gen_qemu_ld_i64(t1, addr, midx, MO_LEUW);
            tcg_gen_deposit_i64(t0, t0, t1, 32, 32);
            tcg_temp_free_i64(t1);
            break;

        default:
            g_assert_not_reached();
        }
        tcg_gen_st_i64(t0, cpu_env, vofs + len_align);
    }
    tcg_temp_free_i64(addr);
    tcg_temp_free_i64(t0);
}

#undef ptr

static void trans_LDR_zri(DisasContext *s, arg_rri *a, uint32_t insn)
{
    int size = vec_full_reg_size(s);
    do_ldr(s, vec_full_reg_offset(s, a->rd), size, a->rn, a->imm * size);
}

static void trans_LDR_pri(DisasContext *s, arg_rri *a, uint32_t insn)
{
    int size = pred_full_reg_size(s);
    do_ldr(s, pred_full_reg_offset(s, a->rd), size, a->rn, a->imm * size);
}

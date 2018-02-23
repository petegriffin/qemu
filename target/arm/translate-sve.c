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

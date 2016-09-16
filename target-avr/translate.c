/*
 * QEMU AVR CPU
 *
 * Copyright (c) 2016 Michael Rolnik
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
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/lgpl-2.1.html>
 */


#include "qemu/osdep.h"
#include "tcg/tcg.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "disas/disas.h"
#include "tcg-op.h"
#include "exec/cpu_ldst.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/log.h"

static TCGv_env cpu_env;

static TCGv cpu_pc;

static TCGv cpu_Cf;
static TCGv cpu_Zf;
static TCGv cpu_Nf;
static TCGv cpu_Vf;
static TCGv cpu_Sf;
static TCGv cpu_Hf;
static TCGv cpu_Tf;
static TCGv cpu_If;

static TCGv cpu_rampD;
static TCGv cpu_rampX;
static TCGv cpu_rampY;
static TCGv cpu_rampZ;

static TCGv cpu_r[32];
static TCGv cpu_eind;
static TCGv cpu_sp;

#define REG(x) (cpu_r[x])

enum {
    BS_NONE = 0, /* Nothing special (none of the below) */
    BS_STOP = 1, /* We want to stop translation for any reason */
    BS_BRANCH = 2, /* A branch condition is reached */
    BS_EXCP = 3, /* An exception condition is reached */
};

uint32_t get_opcode(uint8_t const *code, unsigned bitBase, unsigned bitSize);

typedef struct DisasContext DisasContext;
typedef struct InstInfo InstInfo;

typedef int (*translate_function_t)(DisasContext *ctx, uint32_t opcode);
struct InstInfo {
    target_long cpc;
    target_long npc;
    uint32_t opcode;
    translate_function_t translate;
    unsigned length;
};

/* This is the state at translation time. */
struct DisasContext {
    struct TranslationBlock *tb;
    CPUAVRState *env;

    InstInfo inst[2];/* two consecutive instructions */

    /* Routine used to access memory */
    int memidx;
    int bstate;
    int singlestep;
};

static void gen_goto_tb(DisasContext *ctx, int n, target_ulong dest)
{
    TranslationBlock *tb = ctx->tb;

    if (ctx->singlestep == 0) {
        tcg_gen_goto_tb(n);
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_exit_tb((uintptr_t)tb + n);
    } else {
        tcg_gen_movi_i32(cpu_pc, dest);
        gen_helper_debug(cpu_env);
        tcg_gen_exit_tb(0);
    }
}

#include "exec/gen-icount.h"
#include "translate-inst.h"
#include "translate-inst.inc.c"
#include "decode.inc.c"

void avr_translate_init(void)
{
    int i;
    static int done_init;

    if (done_init) {
        return;
    }
#define AVR_REG_OFFS(x) offsetof(CPUAVRState, x)
    cpu_env = tcg_global_reg_new_ptr(TCG_AREG0, "env");
    cpu_pc = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(pc_w), "pc");
    cpu_Cf = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(sregC), "Cf");
    cpu_Zf = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(sregZ), "Zf");
    cpu_Nf = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(sregN), "Nf");
    cpu_Vf = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(sregV), "Vf");
    cpu_Sf = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(sregS), "Sf");
    cpu_Hf = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(sregH), "Hf");
    cpu_Tf = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(sregT), "Tf");
    cpu_If = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(sregI), "If");
    cpu_rampD = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(rampD), "rampD");
    cpu_rampX = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(rampX), "rampX");
    cpu_rampY = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(rampY), "rampY");
    cpu_rampZ = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(rampZ), "rampZ");
    cpu_eind = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(eind), "eind");
    cpu_sp = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(sp), "sp");

    for (i = 0; i < 32; i++) {
        char name[16];

        sprintf(name, "r[%d]", i);

        cpu_r[i] = tcg_global_mem_new_i32(cpu_env, AVR_REG_OFFS(r[i]), name);
    }

    done_init = 1;
}

static void decode_opc(DisasContext *ctx, InstInfo *inst)
{
    /* PC points to words.  */
    inst->opcode = cpu_ldl_code(ctx->env, inst->cpc * 2);
    inst->length = 16;
    inst->translate = NULL;

    avr_decode(inst->cpc, &inst->length, inst->opcode, &inst->translate);

    if (inst->length == 16) {
        inst->npc = inst->cpc + 1;
        /* get opcode as 16bit value */
        inst->opcode = inst->opcode & 0x0000ffff;
    }
    if (inst->length == 32) {
        inst->npc = inst->cpc + 2;
        /* get opcode as 32bit value */
        inst->opcode = (inst->opcode << 16)
                     | (inst->opcode >> 16);
    }
}

/* generate intermediate code for basic block 'tb'. */
void gen_intermediate_code(CPUAVRState *env, struct TranslationBlock *tb)
{
    AVRCPU *cpu = avr_env_get_cpu(env);
    CPUState *cs = CPU(cpu);
    DisasContext ctx;
    target_ulong pc_start;
    int num_insns, max_insns;
    target_ulong cpc;
    target_ulong npc;

    pc_start = tb->pc / 2;
    ctx.tb = tb;
    ctx.env = env;
    ctx.memidx = 0;
    ctx.bstate = BS_NONE;
    ctx.singlestep = cs->singlestep_enabled;
    num_insns = 0;
    max_insns = tb->cflags & CF_COUNT_MASK;

    if (max_insns == 0) {
        max_insns = CF_COUNT_MASK;
    }
    if (max_insns > TCG_MAX_INSNS) {
        max_insns = TCG_MAX_INSNS;
    }
    if (tb->flags & TB_FLAGS_FULL_ACCESS) {
        /*
            this flag is set by ST/LD instruction
            we will regenerate it ONLY with mem/cpu memory access
            instead of mem access
        */
        max_insns = 1;
    }

    gen_tb_start(tb);

    /* decode first instruction */
    ctx.inst[0].cpc = pc_start;
    decode_opc(&ctx, &ctx.inst[0]);
    do {
        /* set curr/next PCs */
        cpc = ctx.inst[0].cpc;
        npc = ctx.inst[0].npc;

        /* decode next instruction */
        ctx.inst[1].cpc = ctx.inst[0].npc;
        decode_opc(&ctx, &ctx.inst[1]);

        /* translate current instruction */
        tcg_gen_insn_start(cpc);
        num_insns++;

        /*
         * this is due to some strange GDB behavior
         * let's assume main is has 0x100 address
         * b main   - sets a breakpoint to 0x00000100 address (code)
         * b *0x100 - sets a breakpoint to 0x00800100 address (data)
         */
        if (unlikely(cpu_breakpoint_test(cs, PHYS_BASE_CODE + cpc * 2, BP_ANY))
                 || cpu_breakpoint_test(cs, PHYS_BASE_DATA + cpc * 2, BP_ANY)) {
            tcg_gen_movi_i32(cpu_pc, cpc);
            gen_helper_debug(cpu_env);
            ctx.bstate = BS_EXCP;
            goto done_generating;
        }

        if (ctx.inst[0].translate) {
            ctx.bstate = ctx.inst[0].translate(&ctx, ctx.inst[0].opcode);
        }

        if (num_insns >= max_insns) {
            break; /* max translated instructions limit reached */
        }
        if (ctx.singlestep) {
            break; /* single step */
        }
        if ((cpc & (TARGET_PAGE_SIZE - 1)) == 0) {
            break; /* page boundary */
        }

        ctx.inst[0] = ctx.inst[1]; /* make next inst curr */
    } while (ctx.bstate == BS_NONE && !tcg_op_buf_full());

    if (tb->cflags & CF_LAST_IO) {
        gen_io_end();
    }

    if (ctx.singlestep) {
        if (ctx.bstate == BS_STOP || ctx.bstate == BS_NONE) {
            tcg_gen_movi_tl(cpu_pc, npc);
        }
        gen_helper_debug(cpu_env);
        tcg_gen_exit_tb(0);
    } else {
        switch (ctx.bstate) {
        case BS_STOP:
        case BS_NONE:
            gen_goto_tb(&ctx, 0, npc);
            break;
        case BS_EXCP:
            tcg_gen_exit_tb(0);
            break;
        default:
            break;
        }
    }

done_generating:
    gen_tb_end(tb, num_insns);

    tb->size = (npc - pc_start) * 2;
    tb->icount = num_insns;
}

void restore_state_to_opc(CPUAVRState *env, TranslationBlock *tb,
                            target_ulong *data)
{
    env->pc_w = data[0];
}

void avr_cpu_dump_state(CPUState *cs, FILE *f, fprintf_function cpu_fprintf,
                            int flags)
{
    AVRCPU *cpu = AVR_CPU(cs);
    CPUAVRState *env = &cpu->env;
    int i;

    cpu_fprintf(f, "\n");
    cpu_fprintf(f, "PC:    %06x\n", env->pc_w);
    cpu_fprintf(f, "SP:      %04x\n", env->sp);
    cpu_fprintf(f, "rampD:     %02x\n", env->rampD >> 16);
    cpu_fprintf(f, "rampX:     %02x\n", env->rampX >> 16);
    cpu_fprintf(f, "rampY:     %02x\n", env->rampY >> 16);
    cpu_fprintf(f, "rampZ:     %02x\n", env->rampZ >> 16);
    cpu_fprintf(f, "EIND:      %02x\n", env->eind);
    cpu_fprintf(f, "X:       %02x%02x\n", env->r[27], env->r[26]);
    cpu_fprintf(f, "Y:       %02x%02x\n", env->r[29], env->r[28]);
    cpu_fprintf(f, "Z:       %02x%02x\n", env->r[31], env->r[30]);
    cpu_fprintf(f, "SREG:    [ %c %c %c %c %c %c %c %c ]\n",
                        env->sregI ? 'I' : '-',
                        env->sregT ? 'T' : '-',
                        env->sregH ? 'H' : '-',
                        env->sregS ? 'S' : '-',
                        env->sregV ? 'V' : '-',
                        env->sregN ? '-' : 'N', /* Zf has negative logic */
                        env->sregZ ? 'Z' : '-',
                        env->sregC ? 'I' : '-');

    cpu_fprintf(f, "\n");
    for (i = 0; i < ARRAY_SIZE(env->r); i++) {
        cpu_fprintf(f, "R[%02d]:  %02x   ", i, env->r[i]);

        if ((i % 8) == 7) {
            cpu_fprintf(f, "\n");
        }
    }
    cpu_fprintf(f, "\n");
}

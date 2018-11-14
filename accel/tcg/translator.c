/*
 * Generic intermediate code generation.
 *
 * Copyright (C) 2016-2017 Lluís Vilanova <vilanova@ac.upc.edu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "tcg/tcg.h"
#include "tcg/tcg-op.h"
#include "exec/exec-all.h"
#include "exec/gen-icount.h"
#include "exec/log.h"
#include "exec/translator.h"
#include "exec/plugin-gen.h"

/* Pairs with tcg_clear_temp_count.
   To be called by #TranslatorOps.{translate_insn,tb_stop} if
   (1) the target is sufficiently clean to support reporting,
   (2) as and when all temporaries are known to be consumed.
   For most targets, (2) is at the end of translate_insn.  */
void translator_loop_temp_check(DisasContextBase *db)
{
    if (tcg_check_temp_count()) {
        qemu_log("warning: TCG temporary leaks before "
                 TARGET_FMT_lx "\n", db->pc_next);
    }
}

void translator_loop(const TranslatorOps *ops, DisasContextBase *db,
                     CPUState *cpu, TranslationBlock *tb)
{
    int bp_insn = 0;
    int insn_idx = 0;
    bool tb_trans_cb = false;
    bool first_pass = true; /* second pass otherwise */
    void *saved_dc = g_alloca(ops->ctx_size);
    /* tb->plugin_mask is a u32 */
    unsigned long plugin_mask = tb->plugin_mask;
    struct qemu_plugin_tb *plugin_tb = &tcg_ctx->plugin_tb;

    if (test_bit(QEMU_PLUGIN_EV_VCPU_TB_TRANS, &plugin_mask)) {
        tb_trans_cb = true;
        plugin_tb->cbs.n = 0;
        plugin_tb->n = 0;
        plugin_tb->vaddr = tb->pc;
        tcg_ctx->plugin_mem_cb = NULL;
    }

    /* Initialize DisasContext */
    db->tb = tb;
    db->pc_first = tb->pc;
    db->pc_next = db->pc_first;
    db->is_jmp = DISAS_NEXT;
    db->num_insns = 0;
    db->singlestep_enabled = cpu->singlestep_enabled;

    /* Instruction counting */
    db->max_insns = tb_cflags(db->tb) & CF_COUNT_MASK;
    if (db->max_insns == 0) {
        db->max_insns = CF_COUNT_MASK;
    }
    if (db->max_insns > TCG_MAX_INSNS) {
        db->max_insns = TCG_MAX_INSNS;
    }
    if (db->singlestep_enabled || singlestep) {
        db->max_insns = 1;
    }

 translate:
    tcg_func_start(tcg_ctx);

    /* See the "2-pass translation" comment below */
    if (tb_trans_cb) {
        void *dc = db;

        dc -= ops->ctx_base_offset;
        if (first_pass) {
            memcpy(saved_dc, dc, ops->ctx_size);
        } else {
            memcpy(dc, saved_dc, ops->ctx_size);
        }
    }

    ops->init_disas_context(db, cpu);
    tcg_debug_assert(db->is_jmp == DISAS_NEXT);  /* no early exit */

    /* Reset the temp count so that we can identify leaks */
    tcg_clear_temp_count();

    /* Start translating.  */
    gen_tb_start(db->tb);
    ops->tb_start(db, cpu);
    tcg_debug_assert(db->is_jmp == DISAS_NEXT);  /* no early exit */

    if (!first_pass && plugin_tb->cbs.n) {
        qemu_plugin_gen_vcpu_udata_callbacks(&plugin_tb->cbs);
    }

    while (true) {
        struct qemu_plugin_insn *plugin_insn = NULL;
        bool mem_helpers = false;

        /*
         * 2-pass translation.
         *
         * In the first pass we fully determine the TB.
         * If no plugins have subscribed to TB translation events, we're done.
         *
         * If they have, we first share with plugins a TB descriptor so
         * that plugins can subscribe to instruction-related events, e.g.
         * memory accesses of particular instructions, or TB execution.
         * With this info, which is kept in plugin_tb, we then do a second pass,
         * inserting the appropriate instrumentation into the translated TB.
         *
         * Since all translation state is kept in DisasContext, we copy it
         * before the first pass, and restore it before the second.
         */
        if (tb_trans_cb) {
            if (first_pass) {
                plugin_insn = qemu_plugin_tb_insn_get(plugin_tb);
                tcg_ctx->plugin_insn = plugin_insn;
                plugin_insn->vaddr = db->pc_next;
                g_assert(tcg_ctx->plugin_mem_cb == NULL);
            } else {
                struct qemu_plugin_insn *insn = &plugin_tb->insns[insn_idx++];

                tcg_ctx->plugin_insn = NULL;
                if (unlikely(insn->exec_cbs.n)) {
                    qemu_plugin_gen_vcpu_udata_callbacks(&insn->exec_cbs);
                }
                if (insn->mem_cbs.n) {
                    tcg_ctx->plugin_mem_cb = &insn->mem_cbs;
                    if (insn->calls_helpers) {
                        qemu_plugin_gen_enable_mem_helpers(&insn->mem_cbs);
                        mem_helpers = true;
                    }
                } else {
                    tcg_ctx->plugin_mem_cb = NULL;
                }
            }
        }
        db->num_insns++;
        ops->insn_start(db, cpu);
        tcg_debug_assert(db->is_jmp == DISAS_NEXT);  /* no early exit */

        /* Pass breakpoint hits to target for further processing */
        if (!db->singlestep_enabled
            && unlikely(!QTAILQ_EMPTY(&cpu->breakpoints))) {
            CPUBreakpoint *bp;
            QTAILQ_FOREACH(bp, &cpu->breakpoints, entry) {
                if (bp->pc == db->pc_next) {
                    if (ops->breakpoint_check(db, cpu, bp)) {
                        bp_insn = 1;
                        break;
                    }
                }
            }
            /* The breakpoint_check hook may use DISAS_TOO_MANY to indicate
               that only one more instruction is to be executed.  Otherwise
               it should use DISAS_NORETURN when generating an exception,
               but may use a DISAS_TARGET_* value for Something Else.  */
            if (db->is_jmp > DISAS_TOO_MANY) {
                break;
            }
        }

        /* Disassemble one instruction.  The translate_insn hook should
           update db->pc_next and db->is_jmp to indicate what should be
           done next -- either exiting this loop or locate the start of
           the next instruction.  */
        if (db->num_insns == db->max_insns
            && (tb_cflags(db->tb) & CF_LAST_IO)) {
            /* Accept I/O on the last instruction.  */
            gen_io_start();
            ops->translate_insn(db, cpu, plugin_insn);
            gen_io_end();
        } else {
            ops->translate_insn(db, cpu, plugin_insn);
        }

        if (unlikely(mem_helpers)) {
            qemu_plugin_gen_disable_mem_helpers();
        }

        /* Stop translation if translate_insn so indicated.  */
        if (db->is_jmp != DISAS_NEXT) {
            break;
        }

        /* Stop translation if the output buffer is full,
           or we have executed all of the allowed instructions.  */
        if (tcg_op_buf_full() || db->num_insns >= db->max_insns) {
            db->is_jmp = DISAS_TOO_MANY;
            break;
        }
    }

    if (tb_trans_cb && first_pass) {
        qemu_plugin_tb_trans_cb(cpu, plugin_tb);
        first_pass = false;
        goto translate;
    }

    /* Emit code to exit the TB, as indicated by db->is_jmp.  */
    ops->tb_stop(db, cpu);
    gen_tb_end(db->tb, db->num_insns - bp_insn);

    /* The disas_log hook may use these values rather than recompute.  */
    db->tb->size = db->pc_next - db->pc_first;
    db->tb->icount = db->num_insns;

#ifdef DEBUG_DISAS
    if (qemu_loglevel_mask(CPU_LOG_TB_IN_ASM)
        && qemu_log_in_addr_range(db->pc_first)) {
        qemu_log_lock();
        qemu_log("----------------\n");
        ops->disas_log(db, cpu);
        qemu_log("\n");
        qemu_log_unlock();
    }
#endif
}

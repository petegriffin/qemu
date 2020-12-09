/*
 * TCG specific prototypes for helpers
 *
 *  Copyright (c) 2003 Fabrice Bellard
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

#ifndef I386_HELPER_TCG_H
#define I386_HELPER_TCG_H

#include "exec/exec-all.h"

/* Maximum instruction code size */
#define TARGET_MAX_INSN_SIZE 16

/*
 * XXX: This value should match the one returned by CPUID
 * and in exec.c
 */
# if defined(TARGET_X86_64)
# define TCG_PHYS_ADDR_BITS 40
# else
# define TCG_PHYS_ADDR_BITS 36
# endif

#define PHYS_ADDR_MASK MAKE_64BIT_MASK(0, TCG_PHYS_ADDR_BITS)

/**
 * x86_cpu_do_interrupt:
 * @cpu: vCPU the interrupt is to be handled by.
 */
void x86_cpu_do_interrupt(CPUState *cpu);
bool x86_cpu_exec_interrupt(CPUState *cpu, int int_req);

/* helper.c */
bool x86_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                      MMUAccessType access_type, int mmu_idx,
                      bool probe, uintptr_t retaddr);

void breakpoint_handler(CPUState *cs);

/* n must be a constant to be efficient */
static inline target_long lshift(target_long x, int n)
{
    if (n >= 0) {
        return x << n;
    } else {
        return x >> (-n);
    }
}

/* float macros */
#define FT0    (env->ft0)
#define ST0    (env->fpregs[env->fpstt].d)
#define ST(n)  (env->fpregs[(env->fpstt + (n)) & 7].d)
#define ST1    ST(1)

/* translate.c */
void tcg_x86_init(void);

/* excp_helper.c */
void QEMU_NORETURN raise_exception(CPUX86State *env, int exception_index);
void QEMU_NORETURN raise_exception_ra(CPUX86State *env, int exception_index,
                                      uintptr_t retaddr);
void QEMU_NORETURN raise_exception_err(CPUX86State *env, int exception_index,
                                       int error_code);
void QEMU_NORETURN raise_exception_err_ra(CPUX86State *env, int exception_index,
                                          int error_code, uintptr_t retaddr);
void QEMU_NORETURN raise_interrupt(CPUX86State *nenv, int intno, int is_int,
                                   int error_code, int next_eip_addend);

/* cc_helper.c */
extern const uint8_t parity_table[256];

/*
 * NOTE: the translator must set DisasContext.cc_op to CC_OP_EFLAGS
 * after generating a call to a helper that uses this.
 */
static inline void cpu_load_eflags(CPUX86State *env, int eflags,
                                   int update_mask)
{
    CC_SRC = eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    CC_OP = CC_OP_EFLAGS;
    env->df = 1 - (2 * ((eflags >> 10) & 1));
    env->eflags = (env->eflags & ~update_mask) |
        (eflags & update_mask) | 0x2;
}

/* svm_helper.c */
void QEMU_NORETURN cpu_vmexit(CPUX86State *nenv, uint32_t exit_code,
                              uint64_t exit_info_1, uintptr_t retaddr);
void do_vmexit(CPUX86State *env, uint32_t exit_code, uint64_t exit_info_1);

/* seg_helper.c */
void do_interrupt_x86_hardirq(CPUX86State *env, int intno, int is_hw);

/* smm_helper.c */
void do_smm_enter(X86CPU *cpu);

#endif /* I386_HELPER_TCG_H */

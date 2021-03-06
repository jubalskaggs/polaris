/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)cpc_subr.c	1.15	05/06/08 SMI"

/*
 * x86-specific routines used by the CPU Performance counter driver.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/atomic.h>
#include <sys/regset.h>
#include <sys/privregs.h>
#include <sys/x86_archext.h>
#include <sys/cpuvar.h>
#include <sys/machcpuvar.h>
#include <sys/archsystm.h>
#include <sys/cpc_pcbe.h>
#include <sys/cpc_impl.h>
#include <sys/x_call.h>
#include <sys/cmn_err.h>
#include <sys/chip.h>
#include <sys/spl.h>
#include <io/pcplusmp/apic.h>

static const uint64_t allstopped = 0;
static kcpc_ctx_t *(*overflow_intr_handler)(caddr_t);

int kcpc_hw_overflow_intr_installed;		/* set by APIC code */
extern kcpc_ctx_t *kcpc_overflow_intr(caddr_t arg, uint64_t bitmap);

extern int kcpc_counts_include_idle; /* Project Private /etc/system variable */

void (*kcpc_hw_enable_cpc_intr)(void);		/* set by APIC code */

int
kcpc_hw_add_ovf_intr(kcpc_ctx_t *(*handler)(caddr_t))
{
	if (x86_type != X86_TYPE_P6)
		return (0);
	overflow_intr_handler = handler;
	return (ipltospl(APIC_PCINT_IPL));
}

void
kcpc_hw_rem_ovf_intr(void)
{
	overflow_intr_handler = NULL;
}

/*
 * Hook used on P4 systems to catch online/offline events.
 */
/*ARGSUSED*/
static int
kcpc_cpu_setup(cpu_setup_t what, int cpuid, void *arg)
{
	chip_t *chp = cpu[cpuid]->cpu_chip;

	if (what != CPU_ON)
		return (0);

	/*
	 * If any CPU-bound contexts exist, we don't need to invalidate
	 * anything, as no per-LWP contexts can coexist.
	 */
	if (kcpc_cpuctx)
		return (0);

	/*
	 * If this chip now has more than 1 active cpu, we must invalidate all
	 * contexts in the system.
	 */
	if (chp->chip_ncpu > 1)
		kcpc_invalidate_all();

	return (0);
}

static kmutex_t cpu_setup_lock;	/* protects setup_registered */
static int setup_registered;

void
kcpc_hw_init(cpu_t *cp)
{
	kthread_t		*t = cp->cpu_idle_thread;

	if (x86_feature & X86_HTT) {
		mutex_enter(&cpu_setup_lock);
		if (setup_registered == 0) {
			mutex_enter(&cpu_lock);
			register_cpu_setup_func(kcpc_cpu_setup, NULL);
			mutex_exit(&cpu_lock);
			setup_registered = 1;
		}
		mutex_exit(&cpu_setup_lock);
	}

	mutex_init(&cp->cpu_cpc_ctxlock, "cpu_cpc_ctxlock", MUTEX_DEFAULT, 0);

	if (kcpc_counts_include_idle)
		return;

	installctx(t, cp, kcpc_idle_save, kcpc_idle_restore,
	    NULL, NULL, NULL, NULL);
}

#define	BITS(v, u, l)	\
	(((v) >> (l)) & ((1 << (1 + (u) - (l))) - 1))

#define	PCBE_NAMELEN 30	/* Enough Room for pcbe.manuf.model.family.stepping */

/*
 * Examine the processor and load an appropriate PCBE.
 */
int
kcpc_hw_load_pcbe(void)
{
	return (kcpc_pcbe_tryload(cpuid_getvendorstr(CPU), cpuid_getfamily(CPU),
	    cpuid_getmodel(CPU), cpuid_getstep(CPU)));
}

static int
kcpc_remotestop_func(void)
{
	ASSERT(CPU->cpu_cpc_ctx != NULL);
	pcbe_ops->pcbe_allstop();
	atomic_or_uint(&CPU->cpu_cpc_ctx->kc_flags, KCPC_CTX_INVALID_STOPPED);

	return (0);
}

/*
 * Ensure the counters are stopped on the given processor.
 *
 * Callers must ensure kernel preemption is disabled.
 */
void
kcpc_remote_stop(cpu_t *cp)
{
	cpuset_t set;

	CPUSET_ZERO(set);

	CPUSET_ADD(set, cp->cpu_id);

	xc_sync(0, 0, 0, X_CALL_HIPRI, set, (xc_func_t)kcpc_remotestop_func);
}

/*
 * Called by the generic framework to check if it's OK to bind a set to a CPU.
 */
int
kcpc_hw_cpu_hook(processorid_t cpuid, ulong_t *kcpc_cpumap)
{
	cpu_t *p, *cpu;

	if ((x86_feature & X86_HTT) == 0)
		return (0);

	/*
	 * Only one logical CPU on each Pentium 4 HT CPU may be bound to at
	 * once.
	 *
	 * This loop is protected by holding cpu_lock, in order to properly
	 * access the cpu_t of the desired cpu. This also guarantees that the
	 * per chip cpu lists will not change whilst we look at them.
	 */
	mutex_enter(&cpu_lock);
	if ((cpu = cpu_get(cpuid)) == NULL) {
		mutex_exit(&cpu_lock);
		return (-1);
	}

	for (p = cpu->cpu_next_chip; p != cpu; p = p->cpu_next_chip) {
		if (BT_TEST(kcpc_cpumap, p->cpu_id)) {
			mutex_exit(&cpu_lock);
			return (-1);
		}
	}

	mutex_exit(&cpu_lock);
	return (0);
}

/*
 * Called by the generic framework to check if it's OK to bind a set to an LWP.
 */
int
kcpc_hw_lwp_hook(void)
{
	chip_t *p;

	if ((x86_feature & X86_HTT) == 0)
		return (0);

	/*
	 * Only one CPU per chip may be online.
	 */
	mutex_enter(&cpu_lock);
	p = CPU->cpu_chip;
	do {
		if (p->chip_ncpu > 1) {
			mutex_exit(&cpu_lock);
			return (-1);
		}
		p = p->chip_next;
	} while (p != CPU->cpu_chip);
	mutex_exit(&cpu_lock);
	return (0);
}

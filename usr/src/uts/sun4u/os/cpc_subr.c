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
 * sun4u common CPC subroutines.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/atomic.h>
#include <sys/thread.h>
#include <sys/regset.h>
#include <sys/archsystm.h>
#include <sys/machsystm.h>
#include <sys/cpc_impl.h>
#include <sys/cpc_ultra.h>
#include <sys/sunddi.h>
#include <sys/intr.h>
#include <sys/ivintr.h>
#include <sys/x_call.h>
#include <sys/cpuvar.h>
#include <sys/machcpuvar.h>
#include <sys/cpc_pcbe.h>
#include <sys/modctl.h>
#include <sys/sdt.h>

uint32_t		cpc_level15_inum;	/* used in interrupt.s */
int			cpc_has_overflow_intr;	/* set in cheetah.c */

extern kcpc_ctx_t *kcpc_overflow_intr(caddr_t arg, uint64_t bitmap);
extern int kcpc_counts_include_idle;

/*
 * Called on the boot CPU during startup.
 */
void
kcpc_hw_init(void)
{
	if (cpc_has_overflow_intr) {
		cpc_level15_inum = add_softintr(PIL_15,
		    kcpc_hw_overflow_intr, NULL);
	}

	/*
	 * Make sure the boot CPU gets set up.
	 */
	kcpc_hw_startup_cpu(CPU->cpu_flags);
}

/*
 * Prepare for CPC interrupts and install an idle thread CPC context.
 */
void
kcpc_hw_startup_cpu(ushort_t cpflags)
{
	cpu_t		*cp = CPU;
	kthread_t	*t = cp->cpu_idle_thread;

	ASSERT(t->t_bound_cpu == cp);

	if (cpc_has_overflow_intr && (cpflags & CPU_FROZEN) == 0) {
		int pstate_save = disable_vec_intr();

		ASSERT(cpc_level15_inum != 0);

		intr_enqueue_req(PIL_15, cpc_level15_inum);
		enable_vec_intr(pstate_save);
	}

	mutex_init(&cp->cpu_cpc_ctxlock, "cpu_cpc_ctxlock", MUTEX_DEFAULT, 0);

	if (kcpc_counts_include_idle)
		return;

	installctx(t, cp, kcpc_idle_save, kcpc_idle_restore, NULL, NULL,
	    NULL, NULL);
}

/*
 * Examine the processor and load an appropriate PCBE.
 */
int
kcpc_hw_load_pcbe(void)
{
	uint64_t	ver = ultra_getver();

	return (kcpc_pcbe_tryload(NULL, ULTRA_VER_MANUF(ver),
	    ULTRA_VER_IMPL(ver), ULTRA_VER_MASK(ver)));
}

/*ARGSUSED*/
static void
kcpc_remotestop_func(uint64_t arg1, uint64_t arg2)
{
	ASSERT(CPU->cpu_cpc_ctx != NULL);
	pcbe_ops->pcbe_allstop();
	atomic_or_uint(&CPU->cpu_cpc_ctx->kc_flags, KCPC_CTX_INVALID_STOPPED);
}

/*
 * Ensure the counters are stopped on the given processor.
 *
 * Callers must ensure kernel preemption is disabled.
 */
void
kcpc_remote_stop(cpu_t *cp)
{
	xc_one(cp->cpu_id, kcpc_remotestop_func, 0, 0);
}

/*ARGSUSED*/
int
kcpc_hw_cpu_hook(processorid_t cpuid, ulong_t *kcpc_cpumap)
{
	return (0);
}

int
kcpc_hw_lwp_hook(void)
{
	return (0);
}

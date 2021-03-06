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
 * Copyright 1996-2003 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)bootops.c	1.70	05/06/08 SMI"

/*
 * Implementation of the vestigial bootops vector for platforms using the
 * 1275-like boot interfaces.
 */

#include <sys/types.h>
#include <sys/bootconf.h>
#include <sys/param.h>
#include <sys/obpdefs.h>
#include <sys/promif.h>
#include <sys/salib.h>
#include <sys/boot.h>
#include <stddef.h>
#include "boot_plat.h"

#ifdef DEBUG
extern int debug;
#else
static const int debug = 0;
#endif

#define	dprintf		if (debug) printf

extern void	closeall(int);

/*
 * This is the number for this version of bootops, which is vestigial.
 * Standalones that require the old bootops will look in bootops.bsys_version,
 * see this number is higher than they expect and fail gracefully.
 * They can make this "peek" successfully even if they are ILP32 programs.
 */
int boot_version = BO_VERSION;

struct bootops bootops;

void
setup_bootops(void)
{
	/* sanity-check bsys_printf - old kernels need to fail with a message */
#if !defined(lint)
	if (offsetof(struct bootops, bsys_printf) != 60) {
		printf("boot: bsys_printf is at offset 0x%lx instead of 60\n"
		    "boot: this will likely make old kernels die without "
		    "printing a message.\n",
		    offsetof(struct bootops, bsys_printf));
	}
	/* sanity-check bsys_1275_call - if it moves, kernels cannot boot */
	if (offsetof(struct bootops, bsys_1275_call) != 24) {
		printf("boot: bsys_1275_call is at offset 0x%lx instead of 24\n"
			"boot: this will likely break the kernel\n",
		    offsetof(struct bootops, bsys_1275_call));
	}
#endif
	bootops.bsys_version = boot_version;
	bootops.bsys_1275_call = (uint32_t)boot1275_entry;
	/* so old kernels die with a message */
	bootops.bsys_printf = boot_fail_gracefully;
	if (0) print_services();

	if (!memlistpage) /* paranoia runs rampant */
		prom_panic("\nMemlistpage not setup yet.");
	/*
	 * The memory list should always be updated last.  The prom
	 * calls which are made to update a memory list may have the
	 * undesirable affect of claiming physical memory.  This may
	 * happen after the kernel has created its page free list.
	 * The kernel deals with this by comparing the n and n-1
	 * snapshots of memory.  Updating the memory available list
	 * last guarantees we will have a current, accurate snapshot.
	 * See bug #1260786.
	 */
	update_memlist("virtual-memory", "available", &vfreelistp);
	update_memlist("memory", "available", &pfreelistp);

	dprintf("\nPhysinstalled: ");
	if (debug) print_memlist(pinstalledp);
	dprintf("\nPhysfree: ");
	if (debug) print_memlist(pfreelistp);
	dprintf("\nVirtfree: ");
	if (debug) print_memlist(vfreelistp);
}

void
install_memlistptrs(void)
{

	/* prob only need 1 page for now */
	memlistextent = tablep - memlistpage;

	if (0) {
		dprintf("physinstalled = %p\n", (void *)pinstalledp);
		dprintf("physavail = %p\n", (void *)pfreelistp);
		dprintf("virtavail = %p\n", (void *)vfreelistp);
		dprintf("extent = 0x%lx\n", memlistextent);
	}
}

/*
 *      A word of explanation is in order.
 *      This routine is meant to be called during
 *      boot_release(), when the kernel is trying
 *      to ascertain the current state of memory
 *      so that it can use a memlist to walk itself
 *      thru kvm_init().
 */

void
update_memlist(char *name, char *prop, struct memlist **list)
{
	/* Just take another prom snapshot */
	*list = fill_memlists(name, prop, *list);
	install_memlistptrs();
}

/*
 *  This routine is meant to be called by the
 *  kernel to shut down all boot and prom activity.
 *  After this routine is called, PROM or boot IO is no
 *  longer possible, nor is memory allocation.
 */
void
kern_killboot(void)
{
	if (verbosemode) {
		dprintf("Entering boot_release()\n");
		dprintf("\nPhysinstalled: ");
		if (debug) print_memlist(pinstalledp);
		dprintf("\nPhysfree: ");
		if (debug) print_memlist(pfreelistp);
		dprintf("\nVirtfree: ");
		if (debug) print_memlist(vfreelistp);
	}
	if (debug) {
		dprintf("Calling quiesce_io()\n");
		prom_enter_mon();
	}

	/* close all open devices */
	closeall(1);

	/*
	 *  Now we take YAPS (yet another Prom snapshot) of
	 *  memory, just for safety sake.
	 *
	 * The memory list should always be updated last.  The prom
	 * calls which are made to update a memory list may have the
	 * undesirable affect of claiming physical memory.  This may
	 * happen after the kernel has created its page free list.
	 * The kernel deals with this by comparing the n and n-1
	 * snapshots of memory.  Updating the memory available list
	 * last guarantees we will have a current, accurate snapshot.
	 * See bug #1260786.
	 */
	update_memlist("virtual-memory", "available", &vfreelistp);
	update_memlist("memory", "available", &pfreelistp);

	if (verbosemode) {
		dprintf("physinstalled = %p\n", (void *)pinstalledp);
		dprintf("physavail = %p\n", (void *)pfreelistp);
		dprintf("virtavail = %p\n", (void *)vfreelistp);
		dprintf("extent = 0x%lx\n", memlistextent);
		dprintf("Leaving boot_release()\n");

		dprintf("Physinstalled: \n");
		if (debug)
			print_memlist(pinstalledp);

		dprintf("Physfree:\n");
		if (debug)
			print_memlist(pfreelistp);

		dprintf("Virtfree: \n");
		if (debug)
			print_memlist(vfreelistp);
	}

#ifdef DEBUG_MMU
	dump_mmu();
	prom_enter_mon();
#endif
}

void
boot_fail_gracefully(void)
{
	prom_panic(
	    "mismatched version of /boot interface: new boot, old kernel");
}

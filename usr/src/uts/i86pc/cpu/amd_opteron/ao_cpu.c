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
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)ao_cpu.c	1.1	06/02/11 SMI"

#include <sys/types.h>
#include <sys/chip.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/fm/protocol.h>

#include "ao.h"

/*
 * AMD Opteron CPU Subroutines
 *
 * The following three tunables are used to determine the scrubbing rates for
 * the D$, L2$, and DRAM hardware scrubbers.  The values range from 0x00-0x16
 * as described in BKDG 3.6.6 Scrub Control Register.  A value of zero disables
 * the scrubber.  Values above zero indicate rates in descending order.
 *
 * The current default values are used on several Sun systems.  In the future
 * this code should assign values dynamically based on memory sizing.  If you
 * tune these values manually be aware of the following architectural issue:
 * At present, Opteron can only survive certain kinds of multi-bit errors if
 * they are detected by the scrubbers.  Therefore in general we want these
 * values tuned as high as possible without impacting workload performance.
 */
uint32_t ao_scrub_rate_dcache = 8;	/* 64B every 5.12 us */
uint32_t ao_scrub_rate_l2cache = 9;	/* 64B every 10.2 us */
uint32_t ao_scrub_rate_dram = 0xd;	/* 64B every 163.8 us */

uint32_t ao_scrub_system;		/* debug stash for system's value */
uint32_t ao_scrub_bios;			/* debug stash for bios's value */
uint32_t ao_scrub_lo;			/* debug stash for system low addr */
uint32_t ao_scrub_hi;			/* debug stash for system high addr */

enum {
	AO_SCRUB_DEFAULT,		/* retain system default values */
	AO_SCRUB_FIXED,			/* assign ao_scrub_rate_* values */
	AO_SCRUB_MAX			/* assign max of system and tunables */
} ao_scrub_policy = AO_SCRUB_MAX;

nvlist_t *
ao_fmri_create(ao_data_t *ao, nv_alloc_t *nva)
{
	nvlist_t *nvl = fm_nvlist_create(nva);

	fm_fmri_hc_set(nvl, FM_HC_SCHEME_VERSION, NULL, NULL, 3,
	    "motherboard", 0,
	    "chip", ao->ao_cpu->cpu_chip->chip_id,
	    "cpu", chip_plat_get_clogid(ao->ao_cpu));

	return (nvl);
}

/*
 * Return the maximum scrubbing rate between r1 and r2, where r2 is extracted
 * from the specified 'cfg' register value using 'mask' and 'shift'.  If a
 * value is zero, scrubbing is off so return the opposite value.  Otherwise
 * the maximum rate is the smallest non-zero value of the two values.
 */
static uint32_t
ao_scrubber_max(uint32_t r1, uint32_t cfg, uint32_t mask, uint32_t shift)
{
	uint32_t r2 = (cfg & mask) >> shift;

	if (r1 != 0 && r2 != 0)
		return (MIN(r1, r2));

	return (r1 ? r1 : r2);
}

/*
 * Enable the chip-specific hardware scrubbers for the D$, L2$, and DRAM, and
 * return a boolean value indicating if we enabled the DRAM scrubber.  We set
 * the scrubber rate based on a set of tunables defined at the top of the file.
 * The 'base' parameter is the DRAM Base Address for this chip and is used to
 * determine where the scrubber starts.  The 'ilen' value is the IntvlEn field
 * from the DRAM configuration indicating the node-interleaving configuration.
 */
int
ao_scrubber_enable(void *data, uint64_t base, uint64_t ilen)
{
	ao_data_t *ao = data;
	chipid_t chipid = chip_plat_get_chipid(ao->ao_cpu);
	uint32_t scrubctl, lo, hi;
	int rv = 1;

	/*
	 * Read the initial scrubber configuration and save it for debugging.
	 * If ao_scrub_policy is DEFAULT, return immediately.  Otherwise we
	 * disable scrubbing activity while we fiddle with the configuration.
	 */
	scrubctl = ao_pcicfg_read(chipid, AMD_NB_FUNC, AMD_NB_REG_SCRUBCTL);
	cas32(&ao_scrub_bios, 0, scrubctl);

	if (ao_scrub_policy == AO_SCRUB_DEFAULT)
		return ((scrubctl & AMD_NB_SCRUBCTL_DRAM_MASK) != 0);

	scrubctl &= ~AMD_NB_SCRUBCTL_DRAM_MASK;
	scrubctl &= ~AMD_NB_SCRUBCTL_L2_MASK;
	scrubctl &= ~AMD_NB_SCRUBCTL_DC_MASK;

	ao_pcicfg_write(chipid, AMD_NB_FUNC, AMD_NB_REG_SCRUBCTL, scrubctl);

	/*
	 * Read the DRAM Scrub Address Low and High registers, clear their
	 * address fields, enable sequential-redirect mode, and update the
	 * address fields using the specified DRAM Base Address.
	 */
	lo = ao_pcicfg_read(chipid, AMD_NB_FUNC, AMD_NB_REG_SCRUBADDR_LO);
	hi = ao_pcicfg_read(chipid, AMD_NB_FUNC, AMD_NB_REG_SCRUBADDR_HI);

	lo &= ~AMD_NB_SCRUBADDR_LO_MASK;
	hi &= ~AMD_NB_SCRUBADDR_HI_MASK;

	lo |= AMD_NB_SCRUBADDR_MKLO(base) | AMD_NB_SCRUBADDR_LO_SCRUBREDIREN;
	hi |= AMD_NB_SCRUBADDR_MKHI(base);

	ao_scrub_lo = lo;
	ao_scrub_hi = hi;

	ao_pcicfg_write(chipid, AMD_NB_FUNC, AMD_NB_REG_SCRUBADDR_LO, lo);
	ao_pcicfg_write(chipid, AMD_NB_FUNC, AMD_NB_REG_SCRUBADDR_HI, hi);

	if (ao_scrub_rate_dcache > AMD_NB_SCRUBCTL_RATE_MAX) {
		cmn_err(CE_WARN, "ao_scrub_rate_dcache is too large; "
		    "resetting to 0x%x\n", AMD_NB_SCRUBCTL_RATE_MAX);
		ao_scrub_rate_dcache = AMD_NB_SCRUBCTL_RATE_MAX;
	}

	if (ao_scrub_rate_l2cache > AMD_NB_SCRUBCTL_RATE_MAX) {
		cmn_err(CE_WARN, "ao_scrub_rate_l2cache is too large; "
		    "resetting to 0x%x\n", AMD_NB_SCRUBCTL_RATE_MAX);
		ao_scrub_rate_l2cache = AMD_NB_SCRUBCTL_RATE_MAX;
	}

	if (ao_scrub_rate_dram > AMD_NB_SCRUBCTL_RATE_MAX) {
		cmn_err(CE_WARN, "ao_scrub_rate_dram is too large; "
		    "resetting to 0x%x\n", AMD_NB_SCRUBCTL_RATE_MAX);
		ao_scrub_rate_dram = AMD_NB_SCRUBCTL_RATE_MAX;
	}

	if (ao_scrub_policy == AO_SCRUB_MAX) {
		ao_scrub_rate_dcache =
		    ao_scrubber_max(ao_scrub_rate_dcache, ao_scrub_bios,
		    AMD_NB_SCRUBCTL_DC_MASK, AMD_NB_SCRUBCTL_DC_SHIFT);

		ao_scrub_rate_l2cache =
		    ao_scrubber_max(ao_scrub_rate_l2cache, ao_scrub_bios,
		    AMD_NB_SCRUBCTL_L2_MASK, AMD_NB_SCRUBCTL_L2_SHIFT);

		ao_scrub_rate_dram =
		    ao_scrubber_max(ao_scrub_rate_dram, ao_scrub_bios,
		    AMD_NB_SCRUBCTL_DRAM_MASK, AMD_NB_SCRUBCTL_DRAM_SHIFT);
	}

#ifdef OPTERON_ERRATUM_101
	/*
	 * If the DRAM Base Address register's IntlvEn field indicates that
	 * node interleaving is enabled, we must disable the DRAM scrubber
	 * and return zero to indicate that Solaris should use s/w instead.
	 */
	if (ilen != 0) {
		cmn_err(CE_CONT, "?Opteron DRAM scrubber disabled because "
		    "DRAM memory is node-interleaved");
		ao_scrub_rate_dram = 0;
		rv = 0;
	}
#endif
	scrubctl |= AMD_NB_MKSCRUBCTL(ao_scrub_rate_dcache,
	    ao_scrub_rate_l2cache, ao_scrub_rate_dram);

	ao_scrub_system = scrubctl;
	ao_pcicfg_write(chipid, AMD_NB_FUNC, AMD_NB_REG_SCRUBCTL, scrubctl);

	return (rv);
}

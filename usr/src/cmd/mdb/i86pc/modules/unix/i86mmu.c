/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
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

#pragma ident	"@(#)i86mmu.c	1.10	06/03/31 SMI"

/*
 * This part of the file contains the mdb support for dcmds:
 *	::memseg_list
 *	::page_num2pp
 * and walkers for:
 *	memseg - a memseg list walker for ::memseg_list
 *
 */

#include <sys/types.h>
#include <sys/machparam.h>
#include <sys/controlregs.h>
#include <vm/as.h>

#include <mdb/mdb_modapi.h>
#include <mdb/mdb_target.h>

#include <vm/page.h>
#include <vm/hat_i86.h>

struct pfn2pp {
	pfn_t pfn;
	page_t *pp;
};

static int do_va2pfn(uintptr_t, struct as *, int, physaddr_t *);
static void get_mmu(void);

int
platform_vtop(uintptr_t addr, struct as *asp, physaddr_t *pap)
{
	if (asp == NULL)
		return (DCMD_ERR);

	/*
	 * The kernel has to at least have made it thru mmu_init()
	 */
	get_mmu();
	if (mmu.num_level == 0)
		return (DCMD_ERR);

	return (do_va2pfn(addr, asp, 0, pap));
}


/*ARGSUSED*/
int
page_num2pp_cb(uintptr_t addr, void *ignored, uintptr_t *data)
{
	struct memseg ms, *msp = &ms;
	struct pfn2pp *p = (struct pfn2pp *)data;

	if (mdb_vread(msp, sizeof (struct memseg), addr) == -1) {
		mdb_warn("can't read memseg at %#lx", addr);
		return (DCMD_ERR);
	}

	if (p->pfn >= msp->pages_base && p->pfn < msp->pages_end) {
		p->pp = msp->pages + (p->pfn - msp->pages_base);
		return (WALK_DONE);
	}

	return (WALK_NEXT);
}

/*
 * ::page_num2pp dcmd
 */
/*ARGSUSED*/
int
page_num2pp(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	struct pfn2pp pfn2pp;
	page_t page;

	if ((flags & DCMD_ADDRSPEC) == 0) {
		mdb_warn("page frame number missing\n");
			return (DCMD_USAGE);
	}

	pfn2pp.pfn = (pfn_t)addr;
	pfn2pp.pp = NULL;

	if (mdb_walk("memseg", (mdb_walk_cb_t)page_num2pp_cb,
	    (void *)&pfn2pp) == -1) {
		mdb_warn("can't walk memseg");
		return (DCMD_ERR);
	}

	if (pfn2pp.pp == NULL)
		return (DCMD_ERR);

	mdb_printf("%x has page at %p\n", pfn2pp.pfn, pfn2pp.pp);

	if (mdb_vread(&page, sizeof (page_t),
	    (uintptr_t)pfn2pp.pp) == -1) {
		mdb_warn("can't read page at %p", &page);
		return (DCMD_ERR);
	}

	if (page.p_pagenum != pfn2pp.pfn) {
		mdb_warn("WARNING! Found page structure contains "
			"different pagenumber %x\n", page.p_pagenum);
	}

	return (DCMD_OK);
}





/*
 * ::memseg_list dcmd and walker to implement it.
 */
/*ARGSUSED*/
int
memseg_list(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	struct memseg ms;

	if (!(flags & DCMD_ADDRSPEC)) {
		if (mdb_pwalk_dcmd("memseg", "memseg_list",
		    0, NULL, 0) == -1) {
			mdb_warn("can't walk memseg");
			return (DCMD_ERR);
		}
		return (DCMD_OK);
	}

	if (DCMD_HDRSPEC(flags))
		mdb_printf("%<u>%?s %?s %?s %?s %?s%</u>\n", "ADDR",
			"PAGES", "EPAGES", "BASE", "END");

	if (mdb_vread(&ms, sizeof (struct memseg), addr) == -1) {
		mdb_warn("can't read memseg at %#lx", addr);
		return (DCMD_ERR);
	}

	mdb_printf("%0?lx %0?lx %0?lx %0?lx %0?lx\n", addr,
		ms.pages, ms.epages, ms.pages_base, ms.pages_end);

	return (DCMD_OK);
}

/*
 * walk the memseg structures
 */
int
memseg_walk_init(mdb_walk_state_t *wsp)
{
	if (wsp->walk_addr != NULL) {
		mdb_warn("memseg only supports global walks\n");
		return (WALK_ERR);
	}

	if (mdb_readvar(&wsp->walk_addr, "memsegs") == -1) {
		mdb_warn("symbol 'memsegs' not found");
		return (WALK_ERR);
	}

	wsp->walk_data = mdb_alloc(sizeof (struct memseg), UM_SLEEP);
	return (WALK_NEXT);

}

int
memseg_walk_step(mdb_walk_state_t *wsp)
{
	int status;

	if (wsp->walk_addr == 0) {
		return (WALK_DONE);
	}

	if (mdb_vread(wsp->walk_data, sizeof (struct memseg),
	    wsp->walk_addr) == -1) {
		mdb_warn("failed to read struct memseg at %p", wsp->walk_addr);
		return (WALK_DONE);
	}

	status = wsp->walk_callback(wsp->walk_addr, wsp->walk_data,
	    wsp->walk_cbdata);

	wsp->walk_addr = (uintptr_t)(((struct memseg *)wsp->walk_data)->next);

	return (status);
}

void
memseg_walk_fini(mdb_walk_state_t *wsp)
{
	mdb_free(wsp->walk_data, sizeof (struct memseg));
}

/*
 * HAT related dcmds:
 *
 * ::pte [-p XXXXXXXX] [-l 0/1/2/3]
 *
 * dcmd that interprets the -p argument as a page table entry and
 * prints it in more human readable form. The PTE is assumed to be in
 * a level 0 page table, unless -l specifies another level.
 *
 * ::vatopfn [-v] [-a as]
 *
 * Given a virtual address, returns the PFN, if any, mapped at the address.
 * -v shows the intermediate htable/page table entries used to resolve the
 * mapping. By default the virtual address is assumed to be in the kernel's
 * address space.  -a is used to specify a different address space.
 */

struct hat *khat;		/* value of kas.a_hat */
struct hat_mmu_info mmu;
uintptr_t kernelbase;

/*
 * read mmu parameters from kernel
 */
static void
get_mmu(void)
{
	struct as kas;

	if (mmu.num_level != 0)
		return;

	if (mdb_readsym(&mmu, sizeof (mmu), "mmu") == -1)
		mdb_warn("Can't use HAT information before mmu_init()\n");
	if (mdb_readsym(&kas, sizeof (kas), "kas") == -1)
		mdb_warn("Couldn't find kas - kernel's struct as\n");
	if (mdb_readsym(&kernelbase, sizeof (kernelbase), "kernelbase") == -1)
		mdb_warn("Couldn't find kernelbase\n");
	khat = kas.a_hat;
}

/*
 * Print a PTE in more human friendly way. The PTE is assumed to be in
 * a level 0 page table, unless -l specifies another level.
 *
 * The PTE value can be specified as the -p option, since on a 32 bit kernel
 * with PAE running it's larger than a uintptr_t.
 */
static int
do_pte_dcmd(int level, uint64_t pte)
{
	static char *attr[] = {
	    "wrback", "wrthru", "uncached", "uncached",
	    "wrback", "wrthru", "wrcombine", "uncached"};
	int pat_index = 0;

	mdb_printf("PTE=%llx: ", pte);
	if (PTE_GET(pte, mmu.pt_nx))
		mdb_printf("noexec ");

	mdb_printf("page=0x%llx ", PTE2PFN(pte, level));

	if (PTE_GET(pte, PT_NOCONSIST))
		mdb_printf("noconsist ");

	if (PTE_GET(pte, PT_NOSYNC))
		mdb_printf("nosync ");

	if (PTE_GET(pte, mmu.pt_global))
		mdb_printf("global ");

	if (level > 0 && PTE_GET(pte, PT_PAGESIZE))
		mdb_printf("largepage ");

	if (level > 0 && PTE_GET(pte, PT_MOD))
		mdb_printf("mod ");

	if (level > 0 && PTE_GET(pte, PT_REF))
		mdb_printf("ref ");

	if (PTE_GET(pte, PT_USER))
		mdb_printf("user ");

	if (PTE_GET(pte, PT_WRITABLE))
		mdb_printf("write ");

	/*
	 * Report non-standard cacheability
	 */
	pat_index = 0;
	if (level > 0) {
		if (PTE_GET(pte, PT_PAGESIZE) && PTE_GET(pte, PT_PAT_LARGE))
			pat_index += 4;
	} else {
		if (PTE_GET(pte, PT_PAT_4K))
			pat_index += 4;
	}

	if (PTE_GET(pte, PT_NOCACHE))
		pat_index += 2;

	if (PTE_GET(pte, PT_WRITETHRU))
		pat_index += 1;

	if (pat_index != 0)
		mdb_printf("%s", attr[pat_index]);

	if (PTE_GET(pte, PT_VALID) == 0)
		mdb_printf(" !VALID ");

	mdb_printf("\n");
	return (DCMD_OK);
}

/*
 * Print a PTE in more human friendly way. The PTE is assumed to be in
 * a level 0 page table, unless -l specifies another level.
 *
 * The PTE value can be specified as the -p option, since on a 32 bit kernel
 * with PAE running it's larger than a uintptr_t.
 */
/*ARGSUSED*/
int
pte_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	int level = 0;
	uint64_t pte = 0;
	char *level_str = NULL;
	char *pte_str = NULL;

	/*
	 * The kernel has to at least have made it thru mmu_init()
	 */
	get_mmu();
	if (mmu.num_level == 0)
		return (DCMD_ERR);

	if (mdb_getopts(argc, argv,
	    'p', MDB_OPT_STR, &pte_str,
	    'l', MDB_OPT_STR, &level_str) != argc)
		return (DCMD_USAGE);

	/*
	 * parse the PTE to decode, if it's 0, we don't do anything
	 */
	if (pte_str != NULL) {
		pte = mdb_strtoull(pte_str);
	} else {
		if ((flags & DCMD_ADDRSPEC) == 0)
			return (DCMD_USAGE);
		pte = addr;
	}
	if (pte == 0)
		return (DCMD_OK);

	/*
	 * parse the level if supplied
	 */
	if (level_str != NULL) {
		level = mdb_strtoull(level_str);
		if (level < 0 || level > mmu.max_level)
			return (DCMD_ERR);
	}

	return (do_pte_dcmd(level, pte));
}

static int
do_va2pfn(uintptr_t addr, struct as *asp, int print_level, physaddr_t *pap)
{
	struct as as;
	struct hat *hatp;
	struct hat hat;
	htable_t *ht;
	htable_t htable;
	uintptr_t base;
	int h;
	int level;
	int found = 0;
	x86pte_t pte;
	x86pte_t buf;
	x86pte32_t *pte32 = (x86pte32_t *)&buf;
	physaddr_t paddr;
	size_t len;

	if (asp != NULL) {
		if (mdb_vread(&as, sizeof (as), (uintptr_t)asp) == -1) {
			mdb_warn("Couldn't read struct as\n");
			return (DCMD_ERR);
		}
		hatp = as.a_hat;
	} else {
		hatp = khat;
	}

	/*
	 * read the hat and its hash table
	 */
	if (mdb_vread(&hat, sizeof (hat), (uintptr_t)hatp) == -1) {
		mdb_warn("Couldn't read struct hat\n");
		return (DCMD_ERR);
	}

	/*
	 * read the htable hashtable
	 */
	*pap = 0;
	for (level = 0; level <= mmu.max_level; ++level) {
		if (level == mmu.max_level)
			base = 0;
		else
			base = addr & mmu.level_mask[level + 1];

		for (h = 0; h < hat.hat_num_hash; ++h) {
			if (mdb_vread(&ht, sizeof (htable_t *),
			    (uintptr_t)(hat.hat_ht_hash + h)) == -1) {
				mdb_warn("Couldn't read htable\n");
				return (DCMD_ERR);
			}
			for (; ht != NULL; ht = htable.ht_next) {
				if (mdb_vread(&htable, sizeof (htable_t),
				    (uintptr_t)ht) == -1) {
					mdb_warn("Couldn't read htable\n");
					return (DCMD_ERR);
				}
				if (htable.ht_vaddr != base ||
				    htable.ht_level != level)
					continue;

				/*
				 * found - read the page table entry
				 */
				paddr = htable.ht_pfn << MMU_PAGESHIFT;
				paddr += ((addr - base) >>
				    mmu.level_shift[level]) <<
				    mmu.pte_size_shift;
				len = mdb_pread(&buf, mmu.pte_size, paddr);
				if (len != mmu.pte_size)
					return (DCMD_ERR);
				if (mmu.pte_size == sizeof (x86pte_t))
					pte = buf;
				else
					pte = *pte32;

				if (!found) {
					if (PTE_IS_LGPG(pte, level))
						paddr = pte & PT_PADDR_LGPG;
					else
						paddr = pte & PT_PADDR;
					paddr += addr & mmu.level_offset[level];
					*pap = paddr;
					found = 1;
				}
				if (print_level == 0)
					continue;
				mdb_printf("\tlevel=%d htable=%p pte=%llx\n",
				    level, ht, pte);
			}
		}
	}

done:
	if (!found)
		return (DCMD_ERR);
	return (DCMD_OK);
}

int
va2pfn_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uintptr_t addrspace;
	char *addrspace_str = NULL;
	uint64_t physaddr;
	int rc;

	/*
	 * The kernel has to at least have made it thru mmu_init()
	 */
	get_mmu();
	if (mmu.num_level == 0)
		return (DCMD_ERR);

	if (mdb_getopts(argc, argv,
	    'a', MDB_OPT_STR, &addrspace_str) != argc)
		return (DCMD_USAGE);

	if ((flags & DCMD_ADDRSPEC) == 0)
		return (DCMD_USAGE);

	/*
	 * parse the address space
	 */
	if (addrspace_str != NULL)
		addrspace = mdb_strtoull(addrspace_str);
	else
		addrspace = 0;

	rc = do_va2pfn(addr, (struct as *)addrspace, 1, &physaddr);

	if (rc == DCMD_OK)
		mdb_printf("Virtual %p maps Physical %llx\n", addr, physaddr);

	return (rc);
}

/*
 * Report all hat's that either use PFN as a page table or that map the page.
 */
static int
do_report_maps(pfn_t pfn)
{
	struct hat *hatp;
	struct hat hat;
	htable_t *ht;
	htable_t htable;
	uintptr_t base;
	int h;
	int level;
	int entry;
	x86pte_t pte;
	x86pte_t buf;
	x86pte32_t *pte32 = (x86pte32_t *)&buf;
	physaddr_t paddr;
	size_t len;

	/*
	 * The hats are kept in a list with khat at the head.
	 */
	for (hatp = khat; hatp != NULL; hatp = hat.hat_next) {
		/*
		 * read the hat and its hash table
		 */
		if (mdb_vread(&hat, sizeof (hat), (uintptr_t)hatp) == -1) {
			mdb_warn("Couldn't read struct hat\n");
			return (DCMD_ERR);
		}

		/*
		 * read the htable hashtable
		 */
		paddr = 0;
		for (h = 0; h < hat.hat_num_hash; ++h) {
			if (mdb_vread(&ht, sizeof (htable_t *),
			    (uintptr_t)(hat.hat_ht_hash + h)) == -1) {
				mdb_warn("Couldn't read htable\n");
				return (DCMD_ERR);
			}
			for (; ht != NULL; ht = htable.ht_next) {
				if (mdb_vread(&htable, sizeof (htable_t),
				    (uintptr_t)ht) == -1) {
					mdb_warn("Couldn't read htable\n");
					return (DCMD_ERR);
				}

				/*
				 * only report kernel addresses once
				 */
				if (hatp != khat &&
				    htable.ht_vaddr >= kernelbase)
					continue;

				/*
				 * Is the PFN a pagetable itself?
				 */
				if (htable.ht_pfn == pfn) {
					mdb_printf("Pagetable for "
					    "hat=%p htable=%p\n", hatp, ht);
					continue;
				}

				/*
				 * otherwise, examine page mappings
				 */
				level = htable.ht_level;
				if (level > mmu.max_page_level)
					continue;
				paddr = htable.ht_pfn << MMU_PAGESHIFT;
				for (entry = 0; entry < htable.ht_num_ptes;
				    ++entry) {

					base = htable.ht_vaddr + entry *
					    mmu.level_size[level];

					/*
					 * only report kernel addresses once
					 */
					if (hatp != khat &&
					    base >= kernelbase)
						continue;

					len = mdb_pread(&buf, mmu.pte_size,
					    paddr + entry * mmu.pte_size);
					if (len != mmu.pte_size)
						return (DCMD_ERR);
					if (mmu.pte_size == sizeof (x86pte_t))
						pte = buf;
					else
						pte = *pte32;

					if ((pte & PT_VALID) == 0)
						continue;
					if (level == 0 || !(pte & PT_PAGESIZE))
						pte &= PT_PADDR;
					else
						pte &= PT_PADDR_LGPG;
					if ((pte >> MMU_PAGESHIFT) != pfn)
						continue;
					mdb_printf("hat=%p maps addr=%p\n",
						hatp, (caddr_t)base);
				}
			}
		}
	}

done:
	return (DCMD_OK);
}

/*
 * given a PFN as its address argument, prints out the uses of it
 */
/*ARGSUSED*/
int
report_maps_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	/*
	 * The kernel has to at least have made it thru mmu_init()
	 */
	get_mmu();
	if (mmu.num_level == 0)
		return (DCMD_ERR);

	if ((flags & DCMD_ADDRSPEC) == 0)
		return (DCMD_USAGE);

	return (do_report_maps((pfn_t)addr));
}

/*
 * Dump the page table at the given PFN
 */
static int
do_ptable_dcmd(pfn_t pfn)
{
	struct hat *hatp;
	struct hat hat;
	htable_t *ht;
	htable_t htable;
	uintptr_t base;
	int h;
	int level;
	int entry;
	uintptr_t pagesize;
	x86pte_t pte;
	x86pte_t buf;
	x86pte32_t *pte32 = (x86pte32_t *)&buf;
	physaddr_t paddr;
	size_t len;

	/*
	 * The hats are kept in a list with khat at the head.
	 */
	for (hatp = khat; hatp != NULL; hatp = hat.hat_next) {
		/*
		 * read the hat and its hash table
		 */
		if (mdb_vread(&hat, sizeof (hat), (uintptr_t)hatp) == -1) {
			mdb_warn("Couldn't read struct hat\n");
			return (DCMD_ERR);
		}

		/*
		 * read the htable hashtable
		 */
		paddr = 0;
		for (h = 0; h < hat.hat_num_hash; ++h) {
			if (mdb_vread(&ht, sizeof (htable_t *),
			    (uintptr_t)(hat.hat_ht_hash + h)) == -1) {
				mdb_warn("Couldn't read htable\n");
				return (DCMD_ERR);
			}
			for (; ht != NULL; ht = htable.ht_next) {
				if (mdb_vread(&htable, sizeof (htable_t),
				    (uintptr_t)ht) == -1) {
					mdb_warn("Couldn't read htable\n");
					return (DCMD_ERR);
				}

				/*
				 * Is this the PFN for this htable
				 */
				if (htable.ht_pfn == pfn)
					goto found_it;
			}
		}
	}

found_it:
	if (htable.ht_pfn == pfn) {
		mdb_printf("htable=%p\n", ht);
		level = htable.ht_level;
		base = htable.ht_vaddr;
		pagesize = mmu.level_size[level];
	} else {
		mdb_printf("Unknown pagetable - assuming level/addr 0");
		level = 0;	/* assume level == 0 for PFN */
		base = 0;
		pagesize = MMU_PAGESIZE;
	}

	paddr = pfn << MMU_PAGESHIFT;
	for (entry = 0; entry < mmu.ptes_per_table; ++entry) {
		len = mdb_pread(&buf, mmu.pte_size,
		    paddr + entry * mmu.pte_size);
		if (len != mmu.pte_size)
			return (DCMD_ERR);
		if (mmu.pte_size == sizeof (x86pte_t))
			pte = buf;
		else
			pte = *pte32;

		if (pte == 0)
			continue;

		mdb_printf("[%3d] va=%p ", entry, base + entry * pagesize);
		do_pte_dcmd(level, pte);
	}

done:
	return (DCMD_OK);
}

/*
 * given a PFN as its address argument, prints out the uses of it
 */
/*ARGSUSED*/
int
ptable_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	/*
	 * The kernel has to at least have made it thru mmu_init()
	 */
	get_mmu();
	if (mmu.num_level == 0)
		return (DCMD_ERR);

	if ((flags & DCMD_ADDRSPEC) == 0)
		return (DCMD_USAGE);

	return (do_ptable_dcmd((pfn_t)addr));
}

/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef	_SYS_MMU_H
#define	_SYS_MMU_H

#pragma ident	"@(#)mmu.h	1.2	06/05/16 SMI"

#ifdef	__cplusplus
extern "C" {
#endif

#ifndef _ASM
#include <sys/types.h>
#endif
#include <sys/hypervisor_api.h>

/*
 * Definitions for the SOFT MMU
 */

#define	FAST_IMMU_MISS_TT	0x64
#define	FAST_DMMU_MISS_TT	0x68
#define	FAST_PROT_TT		0x6c

/*
 * Constants defining alternate spaces
 * and register layouts within them,
 * and a few other interesting assembly constants.
 */

/*
 * vaddr offsets of various registers
 */
#define	MMU_PCONTEXT		0x08 /* primary context number */
#define	MMU_SCONTEXT		0x10 /* secondary context number */

/*
 * Pseudo Synchronous Fault Status Register Layout
 *
 * IMMU and DMMU maintain their own pseudo SFSR Register
 *
 * +------------------------------------------------+
 * |       Reserved       |   Context   |     FT    |
 * +----------------------|-------------------------+
 *  63                  32 31         16 15         0
 *
 */
#define	SFSR_FT		0x0000FFFF	/* fault type mask */
#define	SFSR_CTX	0xFFFF0000	/* fault context mask */

/*
 * Definition of FT (Fault Type) bit field of sfsr.
 */
#define	FT_NONE		0x00
#define	FT_PRIV		MMFSA_F_PRIV	/* privilege violation */
#define	FT_SPEC_LD	MMFSA_F_SOPG	/* speculative ld to e page */
#define	FT_ATOMIC_NC	MMFSA_F_NCATM	/* atomic to nc page */
#define	FT_ILL_ALT	MMFSA_F_INVASI	/* illegal lda/sta */
#define	FT_NFO		MMFSA_F_NFO	/* normal access to nfo page */
#define	FT_RANGE	MMFSA_F_INVVA	/* dmmu or immu address out of range */
#define	FT_NEW_FMISS	MMFSA_F_FMISS	/* fast miss */
#define	FT_NEW_FPROT	MMFSA_F_FPROT	/* fast protection */
#define	FT_NEW_MISS	MMFSA_F_MISS	/* mmu miss */
#define	FT_NEW_INVRA	MMFSA_F_INVRA	/* invalid RA */
#define	FT_NEW_PROT	MMFSA_F_PROT	/* protection violation */
#define	FT_NEW_PRVACT	MMFSA_F_PRVACT	/* privileged action */
#define	FT_NEW_WPT	MMFSA_F_WPT	/* watchpoint hit */
#define	FT_NEW_UNALIGN	MMFSA_F_UNALIGN	/* unaligned access */
#define	FT_NEW_INVPGSZ	MMFSA_F_INVPGSZ	/* invalid page size */

#define	SFSR_FT_SHIFT	0	/* amt. to shift right to get flt type */
#define	SFSR_CTX_SHIFT	16	/* to shift right to get context */
#define	X_FAULT_TYPE(x)	(((x) & SFSR_FT) >> SFSR_FT_SHIFT)
#define	X_FAULT_CTX(x)	(((x) & SFSR_CTX) >> SFSR_CTX_SHIFT)

/*
 * MMU TAG TARGET register Layout
 *
 * +-----+---------+------+-------------------------+
 * | 000 | context |  --  | virtual address [63:22] |
 * +-----+---------+------+-------------------------+
 *  63 61 60	 48 47	42 41			   0
 */
#define	TTARGET_CTX_SHIFT	48
#define	TTARGET_VA_SHIFT	22

/*
 * MMU TAG ACCESS register Layout
 *
 * +-------------------------+------------------+
 * | virtual address [63:13] |  context [12:0]  |
 * +-------------------------+------------------+
 *  63			  13	12		0
 */
#define	TAGACC_CTX_MASK		0x1FFF
#define	TAGACC_SHIFT		13
#define	TAGACC_VADDR_MASK	(~TAGACC_CTX_MASK)
#define	TAGACC_CTX_LSHIFT	(64 - TAGACC_SHIFT)

/*
 * MMU PRIMARY/SECONDARY CONTEXT register
 */
#define	CTXREG_CTX_MASK		0x1FFF

/*
 * The kernel always runs in KCONTEXT, and no user mappings
 * are ever valid in it (so any user access pagefaults).
 */
#define	KCONTEXT	0

/*
 * FLUSH_ADDR is used in the flush instruction to guarantee stores to mmu
 * registers complete.  It is selected so it won't miss in the tlb.
 */
#define	FLUSH_ADDR	(KERNELBASE + 2 * MMU_PAGESIZE4M)

#define	MAX_NCTXS_BITS			16	/* sun4v max. contexts bits */
#define	MIN_NCTXS_BITS			2
#define	MAX_NCTXS	(1ull << MAX_NCTXS_BITS)

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_MMU_H */

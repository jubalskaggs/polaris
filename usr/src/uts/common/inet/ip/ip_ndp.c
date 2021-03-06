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

#pragma ident	"@(#)ip_ndp.c	1.53	06/08/23 SMI"

#include <sys/types.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsun.h>
#include <sys/sysmacros.h>
#include <sys/errno.h>
#include <sys/dlpi.h>
#include <sys/socket.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/debug.h>
#include <sys/vtrace.h>
#include <sys/kmem.h>
#include <sys/zone.h>
#include <sys/ethernet.h>
#include <sys/sdt.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>

#include <inet/common.h>
#include <inet/mi.h>
#include <inet/mib2.h>
#include <inet/nd.h>
#include <inet/ip.h>
#include <inet/ip_if.h>
#include <inet/ip_ire.h>
#include <inet/ip_rts.h>
#include <inet/ip6.h>
#include <inet/ip_ndp.h>
#include <inet/ipsec_impl.h>
#include <inet/ipsec_info.h>
#include <inet/sctp_ip.h>

/*
 * Function names with nce_ prefix are static while function
 * names with ndp_ prefix are used by rest of the IP.
 *
 * Lock ordering:
 *
 *	ndp_g_lock -> ill_lock -> nce_lock
 *
 * The ndp_g_lock protects the NCE hash (nce_hash_tbl, NCE_HASH_PTR) and
 * nce_next.  Nce_lock protects the contents of the NCE (particularly
 * nce_refcnt).
 */

static	boolean_t nce_cmp_ll_addr(const nce_t *nce, const uchar_t *new_ll_addr,
    uint32_t ll_addr_len);
static	void	nce_fastpath(nce_t *nce);
static	void	nce_ire_delete(nce_t *nce);
static	void	nce_ire_delete1(ire_t *ire, char *nce_arg);
static	void 	nce_set_ll(nce_t *nce, uchar_t *ll_addr);
static	nce_t	*nce_lookup_addr(ill_t *, const in6_addr_t *, nce_t *);
static	nce_t	*nce_lookup_mapping(ill_t *ill, const in6_addr_t *addr);
static	void	nce_make_mapping(nce_t *nce, uchar_t *addrpos,
    uchar_t *addr);
static	int	nce_set_multicast(ill_t *ill, const in6_addr_t *addr);
static	void	nce_queue_mp(nce_t *nce, mblk_t *mp);
static	void	nce_report1(nce_t *nce, uchar_t *mp_arg);
static	mblk_t	*nce_udreq_alloc(ill_t *ill);
static	void	nce_update(nce_t *nce, uint16_t new_state,
    uchar_t *new_ll_addr);
static	uint32_t	nce_solicit(nce_t *nce, mblk_t *mp);
static	boolean_t	nce_xmit(ill_t *ill, uint32_t operation,
    ill_t *hwaddr_ill, boolean_t use_lla_addr, const in6_addr_t *sender,
    const in6_addr_t *target, int flag);
extern void	th_trace_rrecord(th_trace_t *);
static	int	ndp_lookup_then_add_v6(ill_t *, uchar_t *,
    const in6_addr_t *, const in6_addr_t *, const in6_addr_t *,
    uint32_t, uint16_t, uint16_t, nce_t **, mblk_t *, mblk_t *);
static	int	ndp_lookup_then_add_v4(ill_t *, uchar_t *,
    const in_addr_t *, const in_addr_t *, const in_addr_t *,
    uint32_t, uint16_t, uint16_t, nce_t **, mblk_t *, mblk_t *);
static	int	ndp_add_v6(ill_t *, uchar_t *, const in6_addr_t *,
    const in6_addr_t *, const in6_addr_t *, uint32_t, uint16_t, uint16_t,
    nce_t **);
static	int	ndp_add_v4(ill_t *, uchar_t *, const in_addr_t *,
    const in_addr_t *, const in_addr_t *, uint32_t, uint16_t, uint16_t,
    nce_t **, mblk_t *, mblk_t *);


#ifdef NCE_DEBUG
void	nce_trace_inactive(nce_t *);
#endif

ndp_g_t ndp4, ndp6;

#define	NCE_HASH_PTR_V4(addr) \
	(&(ndp4.nce_hash_tbl[IRE_ADDR_HASH(addr, NCE_TABLE_SIZE)]))

#define	NCE_HASH_PTR_V6(addr) \
	(&(ndp6.nce_hash_tbl[NCE_ADDR_HASH_V6(addr, NCE_TABLE_SIZE)]))

int
ndp_add(ill_t *ill, uchar_t *hw_addr, const void *addr,
    const void *mask, const void *extract_mask,
    uint32_t hw_extract_start, uint16_t flags, uint16_t state,
    nce_t **newnce, mblk_t *fp_mp, mblk_t *res_mp)
{
	int status;

	if (ill->ill_isv6)
		status = ndp_add_v6(ill, hw_addr, (in6_addr_t *)addr,
		    (in6_addr_t *)mask, (in6_addr_t *)extract_mask,
		    hw_extract_start, flags, state, newnce);
	else
		status = ndp_add_v4(ill, hw_addr, (in_addr_t *)addr,
		    (in_addr_t *)mask, (in_addr_t *)extract_mask,
		    hw_extract_start, flags, state, newnce, fp_mp, res_mp);
	return (status);
}

/* Non-tunable probe interval, based on link capabilities */
#define	ILL_PROBE_INTERVAL(ill)	((ill)->ill_note_link ? 150 : 1500)

/*
 * NDP Cache Entry creation routine.
 * Mapped entries will never do NUD .
 * This routine must always be called with ndp6.ndp_g_lock held.
 * Prior to return, nce_refcnt is incremented.
 */
static int
ndp_add_v6(ill_t *ill, uchar_t *hw_addr, const in6_addr_t *addr,
    const in6_addr_t *mask, const in6_addr_t *extract_mask,
    uint32_t hw_extract_start, uint16_t flags, uint16_t state,
    nce_t **newnce)
{
	static	nce_t		nce_nil;
	nce_t		*nce;
	mblk_t		*mp;
	mblk_t		*template;
	nce_t		**ncep;
	int		err;
	boolean_t	dropped = B_FALSE;

	ASSERT(MUTEX_HELD(&ndp6.ndp_g_lock));
	ASSERT(ill != NULL && ill->ill_isv6);
	if (IN6_IS_ADDR_UNSPECIFIED(addr)) {
		ip0dbg(("ndp_add: no addr\n"));
		return (EINVAL);
	}
	if ((flags & ~NCE_EXTERNAL_FLAGS_MASK)) {
		ip0dbg(("ndp_add: flags = %x\n", (int)flags));
		return (EINVAL);
	}
	if (IN6_IS_ADDR_UNSPECIFIED(extract_mask) &&
	    (flags & NCE_F_MAPPING)) {
		ip0dbg(("ndp_add: extract mask zero for mapping"));
		return (EINVAL);
	}
	/*
	 * Allocate the mblk to hold the nce.
	 *
	 * XXX This can come out of a separate cache - nce_cache.
	 * We don't need the mp anymore as there are no more
	 * "qwriter"s
	 */
	mp = allocb(sizeof (nce_t), BPRI_MED);
	if (mp == NULL)
		return (ENOMEM);

	nce = (nce_t *)mp->b_rptr;
	mp->b_wptr = (uchar_t *)&nce[1];
	*nce = nce_nil;

	/*
	 * This one holds link layer address
	 */
	if (ill->ill_net_type == IRE_IF_RESOLVER) {
		template = nce_udreq_alloc(ill);
	} else {
		ASSERT((ill->ill_net_type == IRE_IF_NORESOLVER));
		ASSERT((ill->ill_resolver_mp != NULL));
		template = copyb(ill->ill_resolver_mp);
	}
	if (template == NULL) {
		freeb(mp);
		return (ENOMEM);
	}
	nce->nce_ill = ill;
	nce->nce_ipversion = IPV6_VERSION;
	nce->nce_flags = flags;
	nce->nce_state = state;
	nce->nce_pcnt = ND_MAX_UNICAST_SOLICIT;
	nce->nce_rcnt = ill->ill_xmit_count;
	nce->nce_addr = *addr;
	nce->nce_mask = *mask;
	nce->nce_extract_mask = *extract_mask;
	nce->nce_ll_extract_start = hw_extract_start;
	nce->nce_fp_mp = NULL;
	nce->nce_res_mp = template;
	if (state == ND_REACHABLE)
		nce->nce_last = TICK_TO_MSEC(lbolt64);
	else
		nce->nce_last = 0;
	nce->nce_qd_mp = NULL;
	nce->nce_mp = mp;
	if (hw_addr != NULL)
		nce_set_ll(nce, hw_addr);
	/* This one is for nce getting created */
	nce->nce_refcnt = 1;
	mutex_init(&nce->nce_lock, NULL, MUTEX_DEFAULT, NULL);
	if (nce->nce_flags & NCE_F_MAPPING) {
		ASSERT(IN6_IS_ADDR_MULTICAST(addr));
		ASSERT(!IN6_IS_ADDR_UNSPECIFIED(&nce->nce_mask));
		ASSERT(!IN6_IS_ADDR_UNSPECIFIED(&nce->nce_extract_mask));
		ncep = &ndp6.nce_mask_entries;
	} else {
		ncep = ((nce_t **)NCE_HASH_PTR_V6(*addr));
	}

#ifdef NCE_DEBUG
	bzero(nce->nce_trace, sizeof (th_trace_t *) * IP_TR_HASH_MAX);
#endif
	/*
	 * Atomically ensure that the ill is not CONDEMNED, before
	 * adding the NCE.
	 */
	mutex_enter(&ill->ill_lock);
	if (ill->ill_state_flags & ILL_CONDEMNED) {
		mutex_exit(&ill->ill_lock);
		freeb(mp);
		freeb(template);
		return (EINVAL);
	}
	if ((nce->nce_next = *ncep) != NULL)
		nce->nce_next->nce_ptpn = &nce->nce_next;
	*ncep = nce;
	nce->nce_ptpn = ncep;
	*newnce = nce;
	/* This one is for nce being used by an active thread */
	NCE_REFHOLD(*newnce);

	/* Bump up the number of nce's referencing this ill */
	ill->ill_nce_cnt++;
	mutex_exit(&ill->ill_lock);

	err = 0;
	if ((flags & NCE_F_PERMANENT) && state == ND_PROBE) {
		mutex_enter(&nce->nce_lock);
		mutex_exit(&ndp6.ndp_g_lock);
		nce->nce_pcnt = ND_MAX_UNICAST_SOLICIT;
		mutex_exit(&nce->nce_lock);
		dropped = nce_xmit(ill, ND_NEIGHBOR_SOLICIT, NULL, B_FALSE,
		    &ipv6_all_zeros, addr, NDP_PROBE);
		if (dropped) {
			mutex_enter(&nce->nce_lock);
			nce->nce_pcnt++;
			mutex_exit(&nce->nce_lock);
		}
		NDP_RESTART_TIMER(nce, ILL_PROBE_INTERVAL(ill));
		mutex_enter(&ndp6.ndp_g_lock);
		err = EINPROGRESS;
	} else if (flags & NCE_F_UNSOL_ADV) {
		/*
		 * We account for the transmit below by assigning one
		 * less than the ndd variable. Subsequent decrements
		 * are done in ndp_timer.
		 */
		mutex_enter(&nce->nce_lock);
		mutex_exit(&ndp6.ndp_g_lock);
		nce->nce_unsolicit_count = ip_ndp_unsolicit_count - 1;
		mutex_exit(&nce->nce_lock);
		dropped = nce_xmit(ill,
		    ND_NEIGHBOR_ADVERT,
		    ill,	/* ill to be used for extracting ill_nd_lla */
		    B_TRUE,	/* use ill_nd_lla */
		    addr,	/* Source and target of the advertisement pkt */
		    &ipv6_all_hosts_mcast, /* Destination of the packet */
		    NDP_ORIDE);
		mutex_enter(&nce->nce_lock);
		if (dropped)
			nce->nce_unsolicit_count++;
		if (nce->nce_unsolicit_count != 0) {
			nce->nce_timeout_id = timeout(ndp_timer, nce,
			    MSEC_TO_TICK(ip_ndp_unsolicit_interval));
		}
		mutex_exit(&nce->nce_lock);
		mutex_enter(&ndp6.ndp_g_lock);
	}
	/*
	 * If the hw_addr is NULL, typically for ND_INCOMPLETE nces, then
	 * we call nce_fastpath as soon as the nce is resolved in ndp_process.
	 * We call nce_fastpath from nce_update if the link layer address of
	 * the peer changes from nce_update
	 */
	if (hw_addr != NULL || ill->ill_net_type == IRE_IF_NORESOLVER)
		nce_fastpath(nce);
	return (err);
}

int
ndp_lookup_then_add(ill_t *ill, uchar_t *hw_addr, const void *addr,
    const void *mask, const void *extract_mask,
    uint32_t hw_extract_start, uint16_t flags, uint16_t state,
    nce_t **newnce, mblk_t *fp_mp, mblk_t *res_mp)
{
	int status;

	if (ill->ill_isv6) {
		status = ndp_lookup_then_add_v6(ill, hw_addr,
		    (in6_addr_t *)addr, (in6_addr_t *)mask,
		    (in6_addr_t *)extract_mask, hw_extract_start, flags,
		    state, newnce, fp_mp, res_mp);
	} else  {
		status = ndp_lookup_then_add_v4(ill, hw_addr,
		    (in_addr_t *)addr, (in_addr_t *)mask,
		    (in_addr_t *)extract_mask, hw_extract_start, flags,
		    state, newnce, fp_mp, res_mp);
	}

	return (status);
}

static int
ndp_lookup_then_add_v6(ill_t *ill, uchar_t *hw_addr, const in6_addr_t *addr,
    const in6_addr_t *mask, const in6_addr_t *extract_mask,
    uint32_t hw_extract_start, uint16_t flags, uint16_t state,
    nce_t **newnce, mblk_t *fp_mp, mblk_t *res_mp)
{
	int	err = 0;
	nce_t	*nce;

	ASSERT(ill != NULL && ill->ill_isv6);
	mutex_enter(&ndp6.ndp_g_lock);
	nce = *((nce_t **)NCE_HASH_PTR_V6(*addr)); /* head of v6 hash table */
	nce = nce_lookup_addr(ill, addr, nce);
	if (nce == NULL) {
		err = ndp_add(ill,
		    hw_addr,
		    addr,
		    mask,
		    extract_mask,
		    hw_extract_start,
		    flags,
		    state,
		    newnce,
		    fp_mp,
		    res_mp);
	} else {
		*newnce = nce;
		err = EEXIST;
	}
	mutex_exit(&ndp6.ndp_g_lock);
	return (err);
}

/*
 * Remove all the CONDEMNED nces from the appropriate hash table.
 * We create a private list of NCEs, these may have ires pointing
 * to them, so the list will be passed through to clean up dependent
 * ires and only then we can do NCE_REFRELE which can make NCE inactive.
 */
static void
nce_remove(ndp_g_t *ndp, nce_t *nce, nce_t **free_nce_list)
{
	nce_t *nce1;
	nce_t **ptpn;

	ASSERT(MUTEX_HELD(&ndp->ndp_g_lock));
	ASSERT(ndp->ndp_g_walker == 0);
	for (; nce; nce = nce1) {
		nce1 = nce->nce_next;
		mutex_enter(&nce->nce_lock);
		if (nce->nce_flags & NCE_F_CONDEMNED) {
			ptpn = nce->nce_ptpn;
			nce1 = nce->nce_next;
			if (nce1 != NULL)
				nce1->nce_ptpn = ptpn;
			*ptpn = nce1;
			nce->nce_ptpn = NULL;
			nce->nce_next = NULL;
			nce->nce_next = *free_nce_list;
			*free_nce_list = nce;
		}
		mutex_exit(&nce->nce_lock);
	}
}

/*
 * 1. Mark the nce CONDEMNED. This ensures that no new nce_lookup()
 *    will return this NCE. Also no new IREs will be created that
 *    point to this NCE (See ire_add_v6).  Also no new timeouts will
 *    be started (See NDP_RESTART_TIMER).
 * 2. Cancel any currently running timeouts.
 * 3. If there is an ndp walker, return. The walker will do the cleanup.
 *    This ensures that walkers see a consistent list of NCEs while walking.
 * 4. Otherwise remove the NCE from the list of NCEs
 * 5. Delete all IREs pointing to this NCE.
 */
void
ndp_delete(nce_t *nce)
{
	nce_t	**ptpn;
	nce_t	*nce1;
	int	ipversion = nce->nce_ipversion;
	ndp_g_t *ndp = (ipversion == IPV4_VERSION ? &ndp4 : &ndp6);

	/* Serialize deletes */
	mutex_enter(&nce->nce_lock);
	if (nce->nce_flags & NCE_F_CONDEMNED) {
		/* Some other thread is doing the delete */
		mutex_exit(&nce->nce_lock);
		return;
	}
	/*
	 * Caller has a refhold. Also 1 ref for being in the list. Thus
	 * refcnt has to be >= 2
	 */
	ASSERT(nce->nce_refcnt >= 2);
	nce->nce_flags |= NCE_F_CONDEMNED;
	mutex_exit(&nce->nce_lock);

	nce_fastpath_list_delete(nce);

	/*
	 * Cancel any running timer. Timeout can't be restarted
	 * since CONDEMNED is set. Can't hold nce_lock across untimeout.
	 * Passing invalid timeout id is fine.
	 */
	if (nce->nce_timeout_id != 0) {
		(void) untimeout(nce->nce_timeout_id);
		nce->nce_timeout_id = 0;
	}

	mutex_enter(&ndp->ndp_g_lock);
	if (nce->nce_ptpn == NULL) {
		/*
		 * The last ndp walker has already removed this nce from
		 * the list after we marked the nce CONDEMNED and before
		 * we grabbed the global lock.
		 */
		mutex_exit(&ndp->ndp_g_lock);
		return;
	}
	if (ndp->ndp_g_walker > 0) {
		/*
		 * Can't unlink. The walker will clean up
		 */
		ndp->ndp_g_walker_cleanup = B_TRUE;
		mutex_exit(&ndp->ndp_g_lock);
		return;
	}

	/*
	 * Now remove the nce from the list. NDP_RESTART_TIMER won't restart
	 * the timer since it is marked CONDEMNED.
	 */
	ptpn = nce->nce_ptpn;
	nce1 = nce->nce_next;
	if (nce1 != NULL)
		nce1->nce_ptpn = ptpn;
	*ptpn = nce1;
	nce->nce_ptpn = NULL;
	nce->nce_next = NULL;
	mutex_exit(&ndp->ndp_g_lock);

	nce_ire_delete(nce);
}

void
ndp_inactive(nce_t *nce)
{
	mblk_t		**mpp;
	ill_t		*ill;

	ASSERT(nce->nce_refcnt == 0);
	ASSERT(MUTEX_HELD(&nce->nce_lock));
	ASSERT(nce->nce_fastpath == NULL);

	/* Free all nce allocated messages */
	mpp = &nce->nce_first_mp_to_free;
	do {
		while (*mpp != NULL) {
			mblk_t  *mp;

			mp = *mpp;
			*mpp = mp->b_next;
			mp->b_next = NULL;
			mp->b_prev = NULL;
			freemsg(mp);
		}
	} while (mpp++ != &nce->nce_last_mp_to_free);

#ifdef NCE_DEBUG
	nce_trace_inactive(nce);
#endif

	ill = nce->nce_ill;
	mutex_enter(&ill->ill_lock);
	ill->ill_nce_cnt--;
	/*
	 * If the number of nce's associated with this ill have dropped
	 * to zero, check whether we need to restart any operation that
	 * is waiting for this to happen.
	 */
	if (ill->ill_nce_cnt == 0) {
		/* ipif_ill_refrele_tail drops the ill_lock */
		ipif_ill_refrele_tail(ill);
	} else {
		mutex_exit(&ill->ill_lock);
	}
	mutex_destroy(&nce->nce_lock);
	freeb(nce->nce_mp);
}

/*
 * ndp_walk routine.  Delete the nce if it is associated with the ill
 * that is going away.  Always called as a writer.
 */
void
ndp_delete_per_ill(nce_t *nce, uchar_t *arg)
{
	if ((nce != NULL) && nce->nce_ill == (ill_t *)arg) {
		ndp_delete(nce);
	}
}

/*
 * Walk a list of to be inactive NCEs and blow away all the ires.
 */
static void
nce_ire_delete_list(nce_t *nce)
{
	nce_t *nce_next;

	ASSERT(nce != NULL);
	while (nce != NULL) {
		nce_next = nce->nce_next;
		nce->nce_next = NULL;

		/*
		 * It is possible for the last ndp walker (this thread)
		 * to come here after ndp_delete has marked the nce CONDEMNED
		 * and before it has removed the nce from the fastpath list
		 * or called untimeout. So we need to do it here. It is safe
		 * for both ndp_delete and this thread to do it twice or
		 * even simultaneously since each of the threads has a
		 * reference on the nce.
		 */
		nce_fastpath_list_delete(nce);
		/*
		 * Cancel any running timer. Timeout can't be restarted
		 * since CONDEMNED is set. Can't hold nce_lock across untimeout.
		 * Passing invalid timeout id is fine.
		 */
		if (nce->nce_timeout_id != 0) {
			(void) untimeout(nce->nce_timeout_id);
			nce->nce_timeout_id = 0;
		}
		/*
		 * We might hit this func thus in the v4 case:
		 * ipif_down->ipif_ndp_down->ndp_walk
		 */

		if (nce->nce_ipversion == IPV4_VERSION) {
			ire_walk_ill_v4(MATCH_IRE_ILL | MATCH_IRE_TYPE,
			    IRE_CACHE, nce_ire_delete1,
			    (char *)nce, nce->nce_ill);
		} else {
			ASSERT(nce->nce_ipversion == IPV6_VERSION);
			ire_walk_ill_v6(MATCH_IRE_ILL | MATCH_IRE_TYPE,
			    IRE_CACHE, nce_ire_delete1,
			    (char *)nce, nce->nce_ill);
		}
		NCE_REFRELE_NOTR(nce);
		nce = nce_next;
	}
}

/*
 * Delete an ire when the nce goes away.
 */
/* ARGSUSED */
static void
nce_ire_delete(nce_t *nce)
{
	if (nce->nce_ipversion == IPV6_VERSION) {
		ire_walk_ill_v6(MATCH_IRE_ILL | MATCH_IRE_TYPE, IRE_CACHE,
		    nce_ire_delete1, (char *)nce, nce->nce_ill);
		NCE_REFRELE_NOTR(nce);
	} else {
		ire_walk_ill_v4(MATCH_IRE_ILL | MATCH_IRE_TYPE, IRE_CACHE,
		    nce_ire_delete1, (char *)nce, nce->nce_ill);
		NCE_REFRELE_NOTR(nce);
	}
}

/*
 * ire_walk routine used to delete every IRE that shares this nce
 */
static void
nce_ire_delete1(ire_t *ire, char *nce_arg)
{
	nce_t	*nce = (nce_t *)nce_arg;

	ASSERT(ire->ire_type == IRE_CACHE);

	if (ire->ire_nce == nce) {
		ASSERT(ire->ire_ipversion == nce->nce_ipversion);
		ire_delete(ire);
	}
}

/*
 * Restart DAD on given NCE.  Returns B_TRUE if DAD has been restarted.
 */
boolean_t
ndp_restart_dad(nce_t *nce)
{
	boolean_t started;
	boolean_t dropped;

	if (nce == NULL)
		return (B_FALSE);
	mutex_enter(&nce->nce_lock);
	if (nce->nce_state == ND_PROBE) {
		mutex_exit(&nce->nce_lock);
		started = B_TRUE;
	} else if (nce->nce_state == ND_REACHABLE) {
		nce->nce_state = ND_PROBE;
		nce->nce_pcnt = ND_MAX_UNICAST_SOLICIT - 1;
		mutex_exit(&nce->nce_lock);
		dropped = nce_xmit(nce->nce_ill, ND_NEIGHBOR_SOLICIT, NULL,
		    B_FALSE, &ipv6_all_zeros, &nce->nce_addr, NDP_PROBE);
		if (dropped) {
			mutex_enter(&nce->nce_lock);
			nce->nce_pcnt++;
			mutex_exit(&nce->nce_lock);
		}
		NDP_RESTART_TIMER(nce, ILL_PROBE_INTERVAL(nce->nce_ill));
		started = B_TRUE;
	} else {
		mutex_exit(&nce->nce_lock);
		started = B_FALSE;
	}
	return (started);
}

/*
 * IPv6 Cache entry lookup.  Try to find an nce matching the parameters passed.
 * If one is found, the refcnt on the nce will be incremented.
 */
nce_t *
ndp_lookup_v6(ill_t *ill, const in6_addr_t *addr, boolean_t caller_holds_lock)
{
	nce_t	*nce;

	ASSERT(ill != NULL && ill->ill_isv6);
	if (!caller_holds_lock) {
		mutex_enter(&ndp6.ndp_g_lock);
	}
	nce = *((nce_t **)NCE_HASH_PTR_V6(*addr)); /* head of v6 hash table */
	nce = nce_lookup_addr(ill, addr, nce);
	if (nce == NULL)
		nce = nce_lookup_mapping(ill, addr);
	if (!caller_holds_lock)
		mutex_exit(&ndp6.ndp_g_lock);
	return (nce);
}
/*
 * IPv4 Cache entry lookup.  Try to find an nce matching the parameters passed.
 * If one is found, the refcnt on the nce will be incremented.
 * Since multicast mappings are handled in arp, there are no nce_mcast_entries
 * so we skip the nce_lookup_mapping call.
 * XXX TODO: if the nce is found to be ND_STALE, ndp_delete it and return NULL
 */
nce_t *
ndp_lookup_v4(ill_t *ill, const in_addr_t *addr, boolean_t caller_holds_lock)
{
	nce_t	*nce;
	in6_addr_t addr6;

	if (!caller_holds_lock) {
		mutex_enter(&ndp4.ndp_g_lock);
	}
	nce = *((nce_t **)NCE_HASH_PTR_V4(*addr)); /* head of v6 hash table */
	IN6_IPADDR_TO_V4MAPPED(*addr, &addr6);
	nce = nce_lookup_addr(ill, &addr6, nce);
	if (!caller_holds_lock)
		mutex_exit(&ndp4.ndp_g_lock);
	return (nce);
}

/*
 * Cache entry lookup.  Try to find an nce matching the parameters passed.
 * Look only for exact entries (no mappings).  If an nce is found, increment
 * the hold count on that nce. The caller passes in the start of the
 * appropriate hash table, and must be holding the appropriate global
 * lock (ndp_g_lock).
 */
static nce_t *
nce_lookup_addr(ill_t *ill, const in6_addr_t *addr, nce_t *nce)
{
	ndp_g_t *ndp = (ill->ill_isv6 ? &ndp6 : &ndp4);

	ASSERT(ill != NULL);
	ASSERT(MUTEX_HELD(&ndp->ndp_g_lock));
	if (IN6_IS_ADDR_UNSPECIFIED(addr))
		return (NULL);
	for (; nce != NULL; nce = nce->nce_next) {
		if (nce->nce_ill == ill) {
			if (IN6_ARE_ADDR_EQUAL(&nce->nce_addr, addr) &&
			    IN6_ARE_ADDR_EQUAL(&nce->nce_mask,
			    &ipv6_all_ones)) {
				mutex_enter(&nce->nce_lock);
				if (!(nce->nce_flags & NCE_F_CONDEMNED)) {
					NCE_REFHOLD_LOCKED(nce);
					mutex_exit(&nce->nce_lock);
					break;
				}
				mutex_exit(&nce->nce_lock);
			}
		}
	}
	return (nce);
}

/*
 * Cache entry lookup.  Try to find an nce matching the parameters passed.
 * Look only for mappings.
 */
static nce_t *
nce_lookup_mapping(ill_t *ill, const in6_addr_t *addr)
{
	nce_t	*nce;

	ASSERT(ill != NULL && ill->ill_isv6);
	ASSERT(MUTEX_HELD(&ndp6.ndp_g_lock));
	if (!IN6_IS_ADDR_MULTICAST(addr))
		return (NULL);
	nce = ndp6.nce_mask_entries;
	for (; nce != NULL; nce = nce->nce_next)
		if (nce->nce_ill == ill &&
		    (V6_MASK_EQ(*addr, nce->nce_mask, nce->nce_addr))) {
			mutex_enter(&nce->nce_lock);
			if (!(nce->nce_flags & NCE_F_CONDEMNED)) {
				NCE_REFHOLD_LOCKED(nce);
				mutex_exit(&nce->nce_lock);
				break;
			}
			mutex_exit(&nce->nce_lock);
		}
	return (nce);
}

/*
 * Process passed in parameters either from an incoming packet or via
 * user ioctl.
 */
void
ndp_process(nce_t *nce, uchar_t *hw_addr, uint32_t flag, boolean_t is_adv)
{
	ill_t	*ill = nce->nce_ill;
	uint32_t hw_addr_len = ill->ill_nd_lla_len;
	mblk_t	*mp;
	boolean_t ll_updated = B_FALSE;
	boolean_t ll_changed;

	ASSERT(nce->nce_ipversion == IPV6_VERSION);
	/*
	 * No updates of link layer address or the neighbor state is
	 * allowed, when the cache is in NONUD state.  This still
	 * allows for responding to reachability solicitation.
	 */
	mutex_enter(&nce->nce_lock);
	if (nce->nce_state == ND_INCOMPLETE) {
		if (hw_addr == NULL) {
			mutex_exit(&nce->nce_lock);
			return;
		}
		nce_set_ll(nce, hw_addr);
		/*
		 * Update nce state and send the queued packets
		 * back to ip this time ire will be added.
		 */
		if (flag & ND_NA_FLAG_SOLICITED) {
			nce_update(nce, ND_REACHABLE, NULL);
		} else {
			nce_update(nce, ND_STALE, NULL);
		}
		mutex_exit(&nce->nce_lock);
		nce_fastpath(nce);
		mutex_enter(&nce->nce_lock);
		mp = nce->nce_qd_mp;
		nce->nce_qd_mp = NULL;
		mutex_exit(&nce->nce_lock);
		while (mp != NULL) {
			mblk_t *nxt_mp;

			nxt_mp = mp->b_next;
			mp->b_next = NULL;
			if (mp->b_prev != NULL) {
				ill_t   *inbound_ill;
				queue_t *fwdq = NULL;
				uint_t ifindex;

				ifindex = (uint_t)(uintptr_t)mp->b_prev;
				inbound_ill = ill_lookup_on_ifindex(ifindex,
				    B_TRUE, NULL, NULL, NULL, NULL);
				if (inbound_ill == NULL) {
					mp->b_prev = NULL;
					freemsg(mp);
					return;
				} else {
					fwdq = inbound_ill->ill_rq;
				}
				mp->b_prev = NULL;
				/*
				 * Send a forwarded packet back into ip_rput_v6
				 * just as in ire_send_v6().
				 * Extract the queue from b_prev (set in
				 * ip_rput_data_v6).
				 */
				if (fwdq != NULL) {
					/*
					 * Forwarded packets hop count will
					 * get decremented in ip_rput_data_v6
					 */
					put(fwdq, mp);
				} else {
					/*
					 * Send locally originated packets back
					 * into * ip_wput_v6.
					 */
					put(ill->ill_wq, mp);
				}
				ill_refrele(inbound_ill);
			} else {
				put(ill->ill_wq, mp);
			}
			mp = nxt_mp;
		}
		return;
	}
	ll_changed = nce_cmp_ll_addr(nce, hw_addr, hw_addr_len);
	if (!is_adv) {
		/* If this is a SOLICITATION request only */
		if (ll_changed)
			nce_update(nce, ND_STALE, hw_addr);
		mutex_exit(&nce->nce_lock);
		return;
	}
	if (!(flag & ND_NA_FLAG_OVERRIDE) && ll_changed) {
		/* If in any other state than REACHABLE, ignore */
		if (nce->nce_state == ND_REACHABLE) {
			nce_update(nce, ND_STALE, NULL);
		}
		mutex_exit(&nce->nce_lock);
		return;
	} else {
		if (ll_changed) {
			nce_update(nce, ND_UNCHANGED, hw_addr);
			ll_updated = B_TRUE;
		}
		if (flag & ND_NA_FLAG_SOLICITED) {
			nce_update(nce, ND_REACHABLE, NULL);
		} else {
			if (ll_updated) {
				nce_update(nce, ND_STALE, NULL);
			}
		}
		mutex_exit(&nce->nce_lock);
		if (!(flag & ND_NA_FLAG_ROUTER) && (nce->nce_flags &
		    NCE_F_ISROUTER)) {
			ire_t *ire;

			/*
			 * Router turned to host.  We need to remove the
			 * entry as well as any default route that may be
			 * using this as a next hop.  This is required by
			 * section 7.2.5 of RFC 2461.
			 */
			ire = ire_ftable_lookup_v6(&ipv6_all_zeros,
			    &ipv6_all_zeros, &nce->nce_addr, IRE_DEFAULT,
			    nce->nce_ill->ill_ipif, NULL, ALL_ZONES, 0, NULL,
			    MATCH_IRE_ILL | MATCH_IRE_TYPE | MATCH_IRE_GW |
			    MATCH_IRE_DEFAULT);
			if (ire != NULL) {
				ip_rts_rtmsg(RTM_DELETE, ire, 0);
				ire_delete(ire);
				ire_refrele(ire);
			}
			ndp_delete(nce);
		}
	}
}

/*
 * Pass arg1 to the pfi supplied, along with each nce in existence.
 * ndp_walk() places a REFHOLD on the nce and drops the lock when
 * walking the hash list.
 */
void
ndp_walk_common(ndp_g_t *ndp, ill_t *ill, pfi_t pfi, void *arg1,
    boolean_t trace)
{

	nce_t	*nce;
	nce_t	*nce1;
	nce_t	**ncep;
	nce_t	*free_nce_list = NULL;

	mutex_enter(&ndp->ndp_g_lock);
	/* Prevent ndp_delete from unlink and free of NCE */
	ndp->ndp_g_walker++;
	mutex_exit(&ndp->ndp_g_lock);
	for (ncep = ndp->nce_hash_tbl;
	    ncep < A_END(ndp->nce_hash_tbl); ncep++) {
		for (nce = *ncep; nce != NULL; nce = nce1) {
			nce1 = nce->nce_next;
			if (ill == NULL || nce->nce_ill == ill) {
				if (trace) {
					NCE_REFHOLD(nce);
					(*pfi)(nce, arg1);
					NCE_REFRELE(nce);
				} else {
					NCE_REFHOLD_NOTR(nce);
					(*pfi)(nce, arg1);
					NCE_REFRELE_NOTR(nce);
				}
			}
		}
	}
	for (nce = ndp->nce_mask_entries; nce != NULL; nce = nce1) {
		nce1 = nce->nce_next;
		if (ill == NULL || nce->nce_ill == ill) {
			if (trace) {
				NCE_REFHOLD(nce);
				(*pfi)(nce, arg1);
				NCE_REFRELE(nce);
			} else {
				NCE_REFHOLD_NOTR(nce);
				(*pfi)(nce, arg1);
				NCE_REFRELE_NOTR(nce);
			}
		}
	}
	mutex_enter(&ndp->ndp_g_lock);
	ndp->ndp_g_walker--;
	/*
	 * While NCE's are removed from global list they are placed
	 * in a private list, to be passed to nce_ire_delete_list().
	 * The reason is, there may be ires pointing to this nce
	 * which needs to cleaned up.
	 */
	if (ndp->ndp_g_walker_cleanup && ndp->ndp_g_walker == 0) {
		/* Time to delete condemned entries */
		for (ncep = ndp->nce_hash_tbl;
		    ncep < A_END(ndp->nce_hash_tbl); ncep++) {
			nce = *ncep;
			if (nce != NULL) {
				nce_remove(ndp, nce, &free_nce_list);
			}
		}
		nce = ndp->nce_mask_entries;
		if (nce != NULL) {
			nce_remove(ndp, nce, &free_nce_list);
		}
		ndp->ndp_g_walker_cleanup = B_FALSE;
	}
	mutex_exit(&ndp->ndp_g_lock);

	if (free_nce_list != NULL) {
		nce_ire_delete_list(free_nce_list);
	}
}

void
ndp_walk(ill_t *ill, pfi_t pfi, void *arg1)
{
	ndp_walk_common(&ndp4, ill, pfi, arg1, B_TRUE);
	ndp_walk_common(&ndp6, ill, pfi, arg1, B_TRUE);
}

/*
 * Prepend the zoneid using an ipsec_out_t for later use by functions like
 * ip_rput_v6() after neighbor discovery has taken place.  If the message
 * block already has a M_CTL at the front of it, then simply set the zoneid
 * appropriately.
 */
static mblk_t *
ndp_prepend_zone(mblk_t *mp, zoneid_t zoneid)
{
	mblk_t		*first_mp;
	ipsec_out_t	*io;

	ASSERT(zoneid != ALL_ZONES);
	if (mp->b_datap->db_type == M_CTL) {
		io = (ipsec_out_t *)mp->b_rptr;
		ASSERT(io->ipsec_out_type == IPSEC_OUT);
		io->ipsec_out_zoneid = zoneid;
		return (mp);
	}

	first_mp = ipsec_alloc_ipsec_out();
	if (first_mp == NULL)
		return (NULL);
	io = (ipsec_out_t *)first_mp->b_rptr;
	/* This is not a secure packet */
	io->ipsec_out_secure = B_FALSE;
	io->ipsec_out_zoneid = zoneid;
	first_mp->b_cont = mp;
	return (first_mp);
}

/*
 * Process resolve requests.  Handles both mapped entries
 * as well as cases that needs to be send out on the wire.
 * Lookup a NCE for a given IRE.  Regardless of whether one exists
 * or one is created, we defer making ire point to nce until the
 * ire is actually added at which point the nce_refcnt on the nce is
 * incremented.  This is done primarily to have symmetry between ire_add()
 * and ire_delete() which decrements the nce_refcnt, when an ire is deleted.
 */
int
ndp_resolver(ill_t *ill, const in6_addr_t *dst, mblk_t *mp, zoneid_t zoneid)
{
	nce_t		*nce;
	int		err = 0;
	uint32_t	ms;
	mblk_t		*mp_nce = NULL;

	ASSERT(ill != NULL);
	ASSERT(ill->ill_isv6);
	if (IN6_IS_ADDR_MULTICAST(dst)) {
		err = nce_set_multicast(ill, dst);
		return (err);
	}
	err = ndp_lookup_then_add(ill,
	    NULL,	/* No hardware address */
	    dst,
	    &ipv6_all_ones,
	    &ipv6_all_zeros,
	    0,
	    (ill->ill_flags & ILLF_NONUD) ? NCE_F_NONUD : 0,
	    ND_INCOMPLETE,
	    &nce,
	    NULL, /* let ndp_add figure out fastpath mp and dlureq_mp for v6 */
	    NULL);

	switch (err) {
	case 0:
		/*
		 * New cache entry was created. Make sure that the state
		 * is not ND_INCOMPLETE. It can be in some other state
		 * even before we send out the solicitation as we could
		 * get un-solicited advertisements.
		 *
		 * If this is an XRESOLV interface, simply return 0,
		 * since we don't want to solicit just yet.
		 */
		if (ill->ill_flags & ILLF_XRESOLV) {
			NCE_REFRELE(nce);
			return (0);
		}
		rw_enter(&ill_g_lock, RW_READER);
		mutex_enter(&nce->nce_lock);
		if (nce->nce_state != ND_INCOMPLETE) {
			mutex_exit(&nce->nce_lock);
			rw_exit(&ill_g_lock);
			NCE_REFRELE(nce);
			return (0);
		}
		mp_nce = ndp_prepend_zone(mp, zoneid);
		if (mp_nce == NULL) {
			/* The caller will free mp */
			mutex_exit(&nce->nce_lock);
			rw_exit(&ill_g_lock);
			ndp_delete(nce);
			NCE_REFRELE(nce);
			return (ENOMEM);
		}
		ms = nce_solicit(nce, mp_nce);
		rw_exit(&ill_g_lock);
		if (ms == 0) {
			/* The caller will free mp */
			if (mp_nce != mp)
				freeb(mp_nce);
			mutex_exit(&nce->nce_lock);
			ndp_delete(nce);
			NCE_REFRELE(nce);
			return (EBUSY);
		}
		mutex_exit(&nce->nce_lock);
		NDP_RESTART_TIMER(nce, (clock_t)ms);
		NCE_REFRELE(nce);
		return (EINPROGRESS);
	case EEXIST:
		/* Resolution in progress just queue the packet */
		mutex_enter(&nce->nce_lock);
		if (nce->nce_state == ND_INCOMPLETE) {
			mp_nce = ndp_prepend_zone(mp, zoneid);
			if (mp_nce == NULL) {
				err = ENOMEM;
			} else {
				nce_queue_mp(nce, mp_nce);
				err = EINPROGRESS;
			}
		} else {
			/*
			 * Any other state implies we have
			 * a nce but IRE needs to be added ...
			 * ire_add_v6() will take care of the
			 * the case when the nce becomes CONDEMNED
			 * before the ire is added to the table.
			 */
			err = 0;
		}
		mutex_exit(&nce->nce_lock);
		NCE_REFRELE(nce);
		break;
	default:
		ip1dbg(("ndp_resolver: Can't create NCE %d\n", err));
		break;
	}
	return (err);
}

/*
 * When there is no resolver, the link layer template is passed in
 * the IRE.
 * Lookup a NCE for a given IRE.  Regardless of whether one exists
 * or one is created, we defer making ire point to nce until the
 * ire is actually added at which point the nce_refcnt on the nce is
 * incremented.  This is done primarily to have symmetry between ire_add()
 * and ire_delete() which decrements the nce_refcnt, when an ire is deleted.
 */
int
ndp_noresolver(ill_t *ill, const in6_addr_t *dst)
{
	nce_t		*nce;
	int		err = 0;

	ASSERT(ill != NULL);
	ASSERT(ill->ill_isv6);
	if (IN6_IS_ADDR_MULTICAST(dst)) {
		err = nce_set_multicast(ill, dst);
		return (err);
	}

	err = ndp_lookup_then_add(ill,
	    NULL,	/* hardware address */
	    dst,
	    &ipv6_all_ones,
	    &ipv6_all_zeros,
	    0,
	    (ill->ill_flags & ILLF_NONUD) ? NCE_F_NONUD : 0,
	    ND_REACHABLE,
	    &nce,
	    NULL, /* let ndp_add figure out fp_mp/dlureq_mp for v6 */
	    NULL);

	switch (err) {
	case 0:
		/*
		 * Cache entry with a proper resolver cookie was
		 * created.
		 */
		NCE_REFRELE(nce);
		break;
	case EEXIST:
		err = 0;
		NCE_REFRELE(nce);
		break;
	default:
		ip1dbg(("ndp_noresolver: Can't create NCE %d\n", err));
		break;
	}
	return (err);
}

/*
 * For each interface an entry is added for the unspecified multicast group.
 * Here that mapping is used to form the multicast cache entry for a particular
 * multicast destination.
 */
static int
nce_set_multicast(ill_t *ill, const in6_addr_t *dst)
{
	nce_t		*mnce;	/* Multicast mapping entry */
	nce_t		*nce;
	uchar_t		*hw_addr = NULL;
	int		err = 0;

	ASSERT(ill != NULL);
	ASSERT(ill->ill_isv6);
	ASSERT(!(IN6_IS_ADDR_UNSPECIFIED(dst)));

	mutex_enter(&ndp6.ndp_g_lock);
	nce = *((nce_t **)NCE_HASH_PTR_V6(*dst));
	nce = nce_lookup_addr(ill, dst, nce);
	if (nce != NULL) {
		mutex_exit(&ndp6.ndp_g_lock);
		NCE_REFRELE(nce);
		return (0);
	}
	/* No entry, now lookup for a mapping this should never fail */
	mnce = nce_lookup_mapping(ill, dst);
	if (mnce == NULL) {
		/* Something broken for the interface. */
		mutex_exit(&ndp6.ndp_g_lock);
		return (ESRCH);
	}
	ASSERT(mnce->nce_flags & NCE_F_MAPPING);
	if (ill->ill_net_type == IRE_IF_RESOLVER) {
		/*
		 * For IRE_IF_RESOLVER a hardware mapping can be
		 * generated, for IRE_IF_NORESOLVER, resolution cookie
		 * in the ill is copied in ndp_add().
		 */
		hw_addr = kmem_alloc(ill->ill_nd_lla_len, KM_NOSLEEP);
		if (hw_addr == NULL) {
			mutex_exit(&ndp6.ndp_g_lock);
			NCE_REFRELE(mnce);
			return (ENOMEM);
		}
		nce_make_mapping(mnce, hw_addr, (uchar_t *)dst);
	}
	NCE_REFRELE(mnce);
	/*
	 * IRE_IF_NORESOLVER type simply copies the resolution
	 * cookie passed in.  So no hw_addr is needed.
	 */
	err = ndp_add(ill,
	    hw_addr,
	    dst,
	    &ipv6_all_ones,
	    &ipv6_all_zeros,
	    0,
	    NCE_F_NONUD,
	    ND_REACHABLE,
	    &nce,
	    NULL,
	    NULL);
	mutex_exit(&ndp6.ndp_g_lock);
	if (hw_addr != NULL)
		kmem_free(hw_addr, ill->ill_nd_lla_len);
	if (err != 0) {
		ip1dbg(("nce_set_multicast: create failed" "%d\n", err));
		return (err);
	}
	NCE_REFRELE(nce);
	return (0);
}

/*
 * Return the link layer address, and any flags of a nce.
 */
int
ndp_query(ill_t *ill, struct lif_nd_req *lnr)
{
	nce_t		*nce;
	in6_addr_t	*addr;
	sin6_t		*sin6;
	dl_unitdata_req_t	*dl;

	ASSERT(ill != NULL && ill->ill_isv6);
	sin6 = (sin6_t *)&lnr->lnr_addr;
	addr =  &sin6->sin6_addr;

	nce = ndp_lookup_v6(ill, addr, B_FALSE);
	if (nce == NULL)
		return (ESRCH);
	/* If in INCOMPLETE state, no link layer address is available yet */
	if (nce->nce_state == ND_INCOMPLETE)
		goto done;
	dl = (dl_unitdata_req_t *)nce->nce_res_mp->b_rptr;
	if (ill->ill_flags & ILLF_XRESOLV)
		lnr->lnr_hdw_len = dl->dl_dest_addr_length;
	else
		lnr->lnr_hdw_len = ill->ill_nd_lla_len;
	ASSERT(NCE_LL_ADDR_OFFSET(ill) + lnr->lnr_hdw_len <=
	    sizeof (lnr->lnr_hdw_addr));
	bcopy(nce->nce_res_mp->b_rptr + NCE_LL_ADDR_OFFSET(ill),
	    (uchar_t *)&lnr->lnr_hdw_addr, lnr->lnr_hdw_len);
	if (nce->nce_flags & NCE_F_ISROUTER)
		lnr->lnr_flags = NDF_ISROUTER_ON;
	if (nce->nce_flags & NCE_F_PROXY)
		lnr->lnr_flags |= NDF_PROXY_ON;
	if (nce->nce_flags & NCE_F_ANYCAST)
		lnr->lnr_flags |= NDF_ANYCAST_ON;
done:
	NCE_REFRELE(nce);
	return (0);
}

/*
 * Send Enable/Disable multicast reqs to driver.
 */
int
ndp_mcastreq(ill_t *ill, const in6_addr_t *addr, uint32_t hw_addr_len,
    uint32_t hw_addr_offset, mblk_t *mp)
{
	nce_t		*nce;
	uchar_t		*hw_addr;

	ASSERT(ill != NULL && ill->ill_isv6);
	ASSERT(ill->ill_net_type == IRE_IF_RESOLVER);
	hw_addr = mi_offset_paramc(mp, hw_addr_offset, hw_addr_len);
	if (hw_addr == NULL || !IN6_IS_ADDR_MULTICAST(addr)) {
		freemsg(mp);
		return (EINVAL);
	}
	mutex_enter(&ndp6.ndp_g_lock);
	nce = nce_lookup_mapping(ill, addr);
	if (nce == NULL) {
		mutex_exit(&ndp6.ndp_g_lock);
		freemsg(mp);
		return (ESRCH);
	}
	mutex_exit(&ndp6.ndp_g_lock);
	/*
	 * Update dl_addr_length and dl_addr_offset for primitives that
	 * have physical addresses as opposed to full saps
	 */
	switch (((union DL_primitives *)mp->b_rptr)->dl_primitive) {
	case DL_ENABMULTI_REQ:
		/* Track the state if this is the first enabmulti */
		if (ill->ill_dlpi_multicast_state == IDMS_UNKNOWN)
			ill->ill_dlpi_multicast_state = IDMS_INPROGRESS;
		ip1dbg(("ndp_mcastreq: ENABMULTI\n"));
		break;
	case DL_DISABMULTI_REQ:
		ip1dbg(("ndp_mcastreq: DISABMULTI\n"));
		break;
	default:
		NCE_REFRELE(nce);
		ip1dbg(("ndp_mcastreq: default\n"));
		return (EINVAL);
	}
	nce_make_mapping(nce, hw_addr, (uchar_t *)addr);
	NCE_REFRELE(nce);
	putnext(ill->ill_wq, mp);
	return (0);
}

/*
 * Send a neighbor solicitation.
 * Returns number of milliseconds after which we should either rexmit or abort.
 * Return of zero means we should abort.
 * The caller holds the nce_lock to protect nce_qd_mp and nce_rcnt.
 *
 * NOTE: This routine drops nce_lock (and later reacquires it) when sending
 * the packet.
 * NOTE: This routine does not consume mp.
 */
uint32_t
nce_solicit(nce_t *nce, mblk_t *mp)
{
	ill_t		*ill;
	ill_t		*src_ill;
	ip6_t		*ip6h;
	in6_addr_t	src;
	in6_addr_t	dst;
	ipif_t		*ipif;
	ip6i_t		*ip6i;
	boolean_t	dropped = B_FALSE;

	ASSERT(RW_READ_HELD(&ill_g_lock));
	ASSERT(MUTEX_HELD(&nce->nce_lock));
	ill = nce->nce_ill;
	ASSERT(ill != NULL);

	if (nce->nce_rcnt == 0) {
		return (0);
	}

	if (mp == NULL) {
		ASSERT(nce->nce_qd_mp != NULL);
		mp = nce->nce_qd_mp;
	} else {
		nce_queue_mp(nce, mp);
	}

	/* Handle ip_newroute_v6 giving us IPSEC packets */
	if (mp->b_datap->db_type == M_CTL)
		mp = mp->b_cont;

	ip6h = (ip6_t *)mp->b_rptr;
	if (ip6h->ip6_nxt == IPPROTO_RAW) {
		/*
		 * This message should have been pulled up already in
		 * ip_wput_v6. We can't do pullups here because the message
		 * could be from the nce_qd_mp which could have b_next/b_prev
		 * non-NULL.
		 */
		ip6i = (ip6i_t *)ip6h;
		ASSERT((mp->b_wptr - (uchar_t *)ip6i) >=
			    sizeof (ip6i_t) + IPV6_HDR_LEN);
		ip6h = (ip6_t *)(mp->b_rptr + sizeof (ip6i_t));
	}
	src = ip6h->ip6_src;
	/*
	 * If the src of outgoing packet is one of the assigned interface
	 * addresses use it, otherwise we will pick the source address below.
	 */
	src_ill = ill;
	if (!IN6_IS_ADDR_UNSPECIFIED(&src)) {
		if (ill->ill_group != NULL)
			src_ill = ill->ill_group->illgrp_ill;
		for (; src_ill != NULL; src_ill = src_ill->ill_group_next) {
			for (ipif = src_ill->ill_ipif; ipif != NULL;
			    ipif = ipif->ipif_next) {
				if (IN6_ARE_ADDR_EQUAL(&src,
				    &ipif->ipif_v6lcl_addr)) {
					break;
				}
			}
			if (ipif != NULL)
				break;
		}
		/*
		 * If no relevant ipif can be found, then it's not one of our
		 * addresses.  Reset to :: and let nce_xmit.  If an ipif can be
		 * found, but it's not yet done with DAD verification, then
		 * just postpone this transmission until later.
		 */
		if (src_ill == NULL)
			src = ipv6_all_zeros;
		else if (!ipif->ipif_addr_ready)
			return (ill->ill_reachable_retrans_time);
	}
	dst = nce->nce_addr;
	/*
	 * If source address is unspecified, nce_xmit will choose
	 * one for us and initialize the hardware address also
	 * appropriately.
	 */
	if (IN6_IS_ADDR_UNSPECIFIED(&src))
		src_ill = NULL;
	nce->nce_rcnt--;
	mutex_exit(&nce->nce_lock);
	rw_exit(&ill_g_lock);
	dropped = nce_xmit(ill, ND_NEIGHBOR_SOLICIT, src_ill, B_TRUE, &src,
	    &dst, 0);
	rw_enter(&ill_g_lock, RW_READER);
	mutex_enter(&nce->nce_lock);
	if (dropped)
		nce->nce_rcnt++;
	return (ill->ill_reachable_retrans_time);
}

/*
 * Attempt to recover an address on an interface that's been marked as a
 * duplicate.  Because NCEs are destroyed when the interface goes down, there's
 * no easy way to just probe the address and have the right thing happen if
 * it's no longer in use.  Instead, we just bring it up normally and allow the
 * regular interface start-up logic to probe for a remaining duplicate and take
 * us back down if necessary.
 * Neither DHCP nor temporary addresses arrive here; they're excluded by
 * ip_ndp_excl.
 */
/* ARGSUSED */
static void
ip_ndp_recover(ipsq_t *ipsq, queue_t *rq, mblk_t *mp, void *dummy_arg)
{
	ill_t	*ill = rq->q_ptr;
	ipif_t	*ipif;
	in6_addr_t *addr = (in6_addr_t *)mp->b_rptr;

	for (ipif = ill->ill_ipif; ipif != NULL; ipif = ipif->ipif_next) {
		/*
		 * We do not support recovery of proxy ARP'd interfaces,
		 * because the system lacks a complete proxy ARP mechanism.
		 */
		if ((ipif->ipif_flags & IPIF_POINTOPOINT) ||
		    !IN6_ARE_ADDR_EQUAL(&ipif->ipif_v6lcl_addr, addr)) {
			continue;
		}

		/*
		 * If we have already recovered, then ignore.
		 */
		mutex_enter(&ill->ill_lock);
		if (!(ipif->ipif_flags & IPIF_DUPLICATE)) {
			mutex_exit(&ill->ill_lock);
			continue;
		}

		ipif->ipif_flags &= ~IPIF_DUPLICATE;
		ill->ill_ipif_dup_count--;
		mutex_exit(&ill->ill_lock);
		ipif->ipif_was_dup = B_TRUE;

		if (ipif_ndp_up(ipif, addr, B_FALSE) != EINPROGRESS)
			(void) ipif_up_done_v6(ipif);
	}
	freeb(mp);
}

/*
 * Attempt to recover an IPv6 interface that's been shut down as a duplicate.
 * As long as someone else holds the address, the interface will stay down.
 * When that conflict goes away, the interface is brought back up.  This is
 * done so that accidental shutdowns of addresses aren't made permanent.  Your
 * server will recover from a failure.
 *
 * For DHCP and temporary addresses, recovery is not done in the kernel.
 * Instead, it's handled by user space processes (dhcpagent and in.ndpd).
 *
 * This function is entered on a timer expiry; the ID is in ipif_recovery_id.
 */
static void
ipif6_dup_recovery(void *arg)
{
	ipif_t *ipif = arg;

	ipif->ipif_recovery_id = 0;
	if (!(ipif->ipif_flags & IPIF_DUPLICATE))
		return;

	/* If the link is down, we'll retry this later */
	if (!(ipif->ipif_ill->ill_phyint->phyint_flags & PHYI_RUNNING))
		return;

	ndp_do_recovery(ipif);
}

/*
 * Perform interface recovery by forcing the duplicate interfaces up and
 * allowing the system to determine which ones should stay up.
 *
 * Called both by recovery timer expiry and link-up notification.
 */
void
ndp_do_recovery(ipif_t *ipif)
{
	ill_t *ill = ipif->ipif_ill;
	mblk_t *mp;

	mp = allocb(sizeof (ipif->ipif_v6lcl_addr), BPRI_MED);
	if (mp == NULL) {
		ipif->ipif_recovery_id = timeout(ipif6_dup_recovery,
		    ipif, MSEC_TO_TICK(ip_dup_recovery));
	} else {
		bcopy(&ipif->ipif_v6lcl_addr, mp->b_rptr,
		    sizeof (ipif->ipif_v6lcl_addr));
		ill_refhold(ill);
		(void) qwriter_ip(NULL, ill, ill->ill_rq, mp, ip_ndp_recover,
		    CUR_OP, B_FALSE);
	}
}

/*
 * Find the solicitation in the given message, and extract printable details
 * (MAC and IP addresses) from it.
 */
static nd_neighbor_solicit_t *
ip_ndp_find_solicitation(mblk_t *mp, mblk_t *dl_mp, ill_t *ill, char *hbuf,
    size_t hlen, char *sbuf, size_t slen, uchar_t **haddr)
{
	nd_neighbor_solicit_t *ns;
	ip6_t *ip6h;
	uchar_t *addr;
	int alen;

	alen = 0;
	ip6h = (ip6_t *)mp->b_rptr;
	if (dl_mp == NULL) {
		nd_opt_hdr_t *opt;
		int nslen;

		/*
		 * If it's from the fast-path, then it can't be a probe
		 * message, and thus must include the source linkaddr option.
		 * Extract that here.
		 */
		ns = (nd_neighbor_solicit_t *)((char *)ip6h + IPV6_HDR_LEN);
		nslen = mp->b_wptr - (uchar_t *)ns;
		if ((nslen -= sizeof (*ns)) > 0) {
			opt = ndp_get_option((nd_opt_hdr_t *)(ns + 1), nslen,
			    ND_OPT_SOURCE_LINKADDR);
			if (opt != NULL &&
			    opt->nd_opt_len * 8 - sizeof (*opt) >=
			    ill->ill_nd_lla_len) {
				addr = (uchar_t *)(opt + 1);
				alen = ill->ill_nd_lla_len;
			}
		}
		/*
		 * We cheat a bit here for the sake of printing usable log
		 * messages in the rare case where the reply we got was unicast
		 * without a source linkaddr option, and the interface is in
		 * fastpath mode.  (Sigh.)
		 */
		if (alen == 0 && ill->ill_type == IFT_ETHER &&
		    MBLKHEAD(mp) >= sizeof (struct ether_header)) {
			struct ether_header *pether;

			pether = (struct ether_header *)((char *)ip6h -
			    sizeof (*pether));
			addr = pether->ether_shost.ether_addr_octet;
			alen = ETHERADDRL;
		}
	} else {
		dl_unitdata_ind_t *dlu;

		dlu = (dl_unitdata_ind_t *)dl_mp->b_rptr;
		alen = dlu->dl_src_addr_length;
		if (alen > 0 && dlu->dl_src_addr_offset >= sizeof (*dlu) &&
		    dlu->dl_src_addr_offset + alen <= MBLKL(dl_mp)) {
			addr = dl_mp->b_rptr + dlu->dl_src_addr_offset;
			if (ill->ill_sap_length < 0) {
				alen += ill->ill_sap_length;
			} else {
				addr += ill->ill_sap_length;
				alen -= ill->ill_sap_length;
			}
		}
	}
	if (alen > 0) {
		*haddr = addr;
		(void) mac_colon_addr(addr, alen, hbuf, hlen);
	} else {
		*haddr = NULL;
		(void) strcpy(hbuf, "?");
	}
	ns = (nd_neighbor_solicit_t *)((char *)ip6h + IPV6_HDR_LEN);
	(void) inet_ntop(AF_INET6, &ns->nd_ns_target, sbuf, slen);
	return (ns);
}

/*
 * This is for exclusive changes due to NDP duplicate address detection
 * failure.
 */
/* ARGSUSED */
static void
ip_ndp_excl(ipsq_t *ipsq, queue_t *rq, mblk_t *mp, void *dummy_arg)
{
	ill_t	*ill = rq->q_ptr;
	ipif_t	*ipif;
	char ibuf[LIFNAMSIZ + 10];	/* 10 digits for logical i/f number */
	char hbuf[MAC_STR_LEN];
	char sbuf[INET6_ADDRSTRLEN];
	nd_neighbor_solicit_t *ns;
	mblk_t *dl_mp = NULL;
	uchar_t *haddr;

	if (DB_TYPE(mp) != M_DATA) {
		dl_mp = mp;
		mp = mp->b_cont;
	}
	ns = ip_ndp_find_solicitation(mp, dl_mp, ill, hbuf, sizeof (hbuf), sbuf,
	    sizeof (sbuf), &haddr);
	if (haddr != NULL &&
	    bcmp(haddr, ill->ill_phys_addr, ill->ill_phys_addr_length) == 0) {
		/*
		 * Ignore conflicts generated by misbehaving switches that just
		 * reflect our own messages back to us.
		 */
		goto ignore_conflict;
	}
	(void) strlcpy(ibuf, ill->ill_name, sizeof (ibuf));
	for (ipif = ill->ill_ipif; ipif != NULL; ipif = ipif->ipif_next) {

		if ((ipif->ipif_flags & IPIF_POINTOPOINT) ||
		    !IN6_ARE_ADDR_EQUAL(&ipif->ipif_v6lcl_addr,
		    &ns->nd_ns_target)) {
			continue;
		}

		/* If it's already marked, then don't do anything. */
		if (ipif->ipif_flags & IPIF_DUPLICATE)
			continue;

		/*
		 * If this is a failure during duplicate recovery, then don't
		 * complain.  It may take a long time to recover.
		 */
		if (!ipif->ipif_was_dup) {
			if (ipif->ipif_id != 0) {
				(void) snprintf(ibuf + ill->ill_name_length - 1,
				    sizeof (ibuf) - ill->ill_name_length + 1,
				    ":%d", ipif->ipif_id);
			}
			cmn_err(CE_WARN, "%s has duplicate address %s (in "
			    "use by %s); disabled", ibuf, sbuf, hbuf);
		}
		mutex_enter(&ill->ill_lock);
		ASSERT(!(ipif->ipif_flags & IPIF_DUPLICATE));
		ipif->ipif_flags |= IPIF_DUPLICATE;
		ill->ill_ipif_dup_count++;
		mutex_exit(&ill->ill_lock);
		(void) ipif_down(ipif, NULL, NULL);
		ipif_down_tail(ipif);
		if (!(ipif->ipif_flags & (IPIF_DHCPRUNNING|IPIF_TEMPORARY)) &&
		    ill->ill_net_type == IRE_IF_RESOLVER &&
		    ip_dup_recovery > 0)
			ipif->ipif_recovery_id = timeout(ipif6_dup_recovery,
			    ipif, MSEC_TO_TICK(ip_dup_recovery));
	}
ignore_conflict:
	if (dl_mp != NULL)
		freeb(dl_mp);
	freemsg(mp);
}

/*
 * Handle failure by tearing down the ipifs with the specified address.  Note
 * that tearing down the ipif also means deleting the nce through ipif_down, so
 * it's not possible to do recovery by just restarting the nce timer.  Instead,
 * we start a timer on the ipif.
 */
static void
ip_ndp_failure(ill_t *ill, mblk_t *mp, mblk_t *dl_mp, nce_t *nce)
{
	if ((mp = copymsg(mp)) != NULL) {
		if (dl_mp == NULL)
			dl_mp = mp;
		else if ((dl_mp = copyb(dl_mp)) != NULL)
			dl_mp->b_cont = mp;
		if (dl_mp == NULL) {
			freemsg(mp);
		} else {
			ill_refhold(ill);
			(void) qwriter_ip(NULL, ill, ill->ill_rq, dl_mp,
			    ip_ndp_excl, CUR_OP, B_FALSE);
		}
	}
	ndp_delete(nce);
}

/*
 * Handle a discovered conflict: some other system is advertising that it owns
 * one of our IP addresses.  We need to defend ourselves, or just shut down the
 * interface.
 */
static void
ip_ndp_conflict(ill_t *ill, mblk_t *mp, mblk_t *dl_mp, nce_t *nce)
{
	ipif_t *ipif;
	uint32_t now;
	uint_t maxdefense;
	uint_t defs;

	ipif = ipif_lookup_addr_v6(&nce->nce_addr, ill, ALL_ZONES, NULL, NULL,
	    NULL, NULL);
	if (ipif == NULL)
		return;
	/*
	 * First, figure out if this address is disposable.
	 */
	if (ipif->ipif_flags & (IPIF_DHCPRUNNING | IPIF_TEMPORARY))
		maxdefense = ip_max_temp_defend;
	else
		maxdefense = ip_max_defend;

	/*
	 * Now figure out how many times we've defended ourselves.  Ignore
	 * defenses that happened long in the past.
	 */
	now = gethrestime_sec();
	mutex_enter(&nce->nce_lock);
	if ((defs = nce->nce_defense_count) > 0 &&
	    now - nce->nce_defense_time > ip_defend_interval) {
		nce->nce_defense_count = defs = 0;
	}
	nce->nce_defense_count++;
	nce->nce_defense_time = now;
	mutex_exit(&nce->nce_lock);
	ipif_refrele(ipif);

	/*
	 * If we've defended ourselves too many times already, then give up and
	 * tear down the interface(s) using this address.  Otherwise, defend by
	 * sending out an unsolicited Neighbor Advertisement.
	 */
	if (defs >= maxdefense) {
		ip_ndp_failure(ill, mp, dl_mp, nce);
	} else {
		char hbuf[MAC_STR_LEN];
		char sbuf[INET6_ADDRSTRLEN];
		uchar_t *haddr;

		(void) ip_ndp_find_solicitation(mp, dl_mp, ill, hbuf,
		    sizeof (hbuf), sbuf, sizeof (sbuf), &haddr);
		cmn_err(CE_WARN, "node %s is using our IP address %s on %s",
		    hbuf, sbuf, ill->ill_name);
		(void) nce_xmit(ill, ND_NEIGHBOR_ADVERT, ill, B_FALSE,
		    &nce->nce_addr, &ipv6_all_hosts_mcast, NDP_ORIDE);
	}
}

static void
ndp_input_solicit(ill_t *ill, mblk_t *mp, mblk_t *dl_mp)
{
	nd_neighbor_solicit_t *ns;
	uint32_t	hlen = ill->ill_nd_lla_len;
	uchar_t		*haddr = NULL;
	icmp6_t		*icmp_nd;
	ip6_t		*ip6h;
	nce_t		*our_nce = NULL;
	in6_addr_t	target;
	in6_addr_t	src;
	int		len;
	int		flag = 0;
	nd_opt_hdr_t	*opt = NULL;
	boolean_t	bad_solicit = B_FALSE;
	mib2_ipv6IfIcmpEntry_t	*mib = ill->ill_icmp6_mib;

	ip6h = (ip6_t *)mp->b_rptr;
	icmp_nd = (icmp6_t *)(mp->b_rptr + IPV6_HDR_LEN);
	len = mp->b_wptr - mp->b_rptr - IPV6_HDR_LEN;
	src = ip6h->ip6_src;
	ns = (nd_neighbor_solicit_t *)icmp_nd;
	target = ns->nd_ns_target;
	if (IN6_IS_ADDR_MULTICAST(&target)) {
		if (ip_debug > 2) {
			/* ip1dbg */
			pr_addr_dbg("ndp_input_solicit: Target is"
			    " multicast! %s\n", AF_INET6, &target);
		}
		bad_solicit = B_TRUE;
		goto done;
	}
	if (len > sizeof (nd_neighbor_solicit_t)) {
		/* Options present */
		opt = (nd_opt_hdr_t *)&ns[1];
		len -= sizeof (nd_neighbor_solicit_t);
		if (!ndp_verify_optlen(opt, len)) {
			ip1dbg(("ndp_input_solicit: Bad opt len\n"));
			bad_solicit = B_TRUE;
			goto done;
		}
	}
	if (IN6_IS_ADDR_UNSPECIFIED(&src)) {
		/* Check to see if this is a valid DAD solicitation */
		if (!IN6_IS_ADDR_MC_SOLICITEDNODE(&ip6h->ip6_dst)) {
			if (ip_debug > 2) {
				/* ip1dbg */
				pr_addr_dbg("ndp_input_solicit: IPv6 "
				    "Destination is not solicited node "
				    "multicast %s\n", AF_INET6,
				    &ip6h->ip6_dst);
			}
			bad_solicit = B_TRUE;
			goto done;
		}
	}

	our_nce = ndp_lookup_v6(ill, &target, B_FALSE);
	/*
	 * If this is a valid Solicitation, a permanent
	 * entry should exist in the cache
	 */
	if (our_nce == NULL ||
	    !(our_nce->nce_flags & NCE_F_PERMANENT)) {
		ip1dbg(("ndp_input_solicit: Wrong target in NS?!"
		    "ifname=%s ", ill->ill_name));
		if (ip_debug > 2) {
			/* ip1dbg */
			pr_addr_dbg(" dst %s\n", AF_INET6, &target);
		}
		bad_solicit = B_TRUE;
		goto done;
	}

	/* At this point we should have a verified NS per spec */
	if (opt != NULL) {
		opt = ndp_get_option(opt, len, ND_OPT_SOURCE_LINKADDR);
		if (opt != NULL) {
			haddr = (uchar_t *)&opt[1];
			if (hlen > opt->nd_opt_len * 8 - sizeof (*opt) ||
			    hlen == 0) {
				ip1dbg(("ndp_input_advert: bad SLLA\n"));
				bad_solicit = B_TRUE;
				goto done;
			}
		}
	}

	/* Set override flag, it will be reset later if need be. */
	flag |= NDP_ORIDE;
	if (!IN6_IS_ADDR_MULTICAST(&ip6h->ip6_dst)) {
		flag |= NDP_UNICAST;
	}

	/*
	 * Create/update the entry for the soliciting node.
	 * or respond to outstanding queries, don't if
	 * the source is unspecified address.
	 */
	if (!IN6_IS_ADDR_UNSPECIFIED(&src)) {
		int	err;
		nce_t	*nnce;

		ASSERT(ill->ill_isv6);
		/*
		 * Regular solicitations *must* include the Source Link-Layer
		 * Address option.  Ignore messages that do not.
		 */
		if (haddr == NULL && IN6_IS_ADDR_MULTICAST(&ip6h->ip6_dst)) {
			ip1dbg(("ndp_input_solicit: source link-layer address "
			    "option missing with a specified source.\n"));
			bad_solicit = B_TRUE;
			goto done;
		}

		/*
		 * This is a regular solicitation.  If we're still in the
		 * process of verifying the address, then don't respond at all
		 * and don't keep track of the sender.
		 */
		if (our_nce->nce_state == ND_PROBE)
			goto done;

		/*
		 * If the solicitation doesn't have sender hardware address
		 * (legal for unicast solicitation), then process without
		 * installing the return NCE.  Either we already know it, or
		 * we'll be forced to look it up when (and if) we reply to the
		 * packet.
		 */
		if (haddr == NULL)
			goto no_source;

		err = ndp_lookup_then_add(ill,
		    haddr,
		    &src,	/* Soliciting nodes address */
		    &ipv6_all_ones,
		    &ipv6_all_zeros,
		    0,
		    0,
		    ND_STALE,
		    &nnce,
		    NULL,
		    NULL);
		switch (err) {
		case 0:
			/* done with this entry */
			NCE_REFRELE(nnce);
			break;
		case EEXIST:
			/*
			 * B_FALSE indicates this is not an
			 * an advertisement.
			 */
			ndp_process(nnce, haddr, 0, B_FALSE);
			NCE_REFRELE(nnce);
			break;
		default:
			ip1dbg(("ndp_input_solicit: Can't create NCE %d\n",
			    err));
			goto done;
		}
no_source:
		flag |= NDP_SOLICITED;
	} else {
		/*
		 * No source link layer address option should be present in a
		 * valid DAD request.
		 */
		if (haddr != NULL) {
			ip1dbg(("ndp_input_solicit: source link-layer address "
			    "option present with an unspecified source.\n"));
			bad_solicit = B_TRUE;
			goto done;
		}
		if (our_nce->nce_state == ND_PROBE) {
			/*
			 * Internally looped-back probes won't have DLPI
			 * attached to them.  External ones (which are sent by
			 * multicast) always will.  Just ignore our own
			 * transmissions.
			 */
			if (dl_mp != NULL) {
				/*
				 * If someone else is probing our address, then
				 * we've crossed wires.  Declare failure.
				 */
				ip_ndp_failure(ill, mp, dl_mp, our_nce);
			}
			goto done;
		}
		/*
		 * This is a DAD probe.  Multicast the advertisement to the
		 * all-nodes address.
		 */
		src = ipv6_all_hosts_mcast;
	}
	if (our_nce->nce_flags & NCE_F_ISROUTER)
		flag |= NDP_ISROUTER;
	if (our_nce->nce_flags & NCE_F_PROXY)
		flag &= ~NDP_ORIDE;
	/* Response to a solicitation */
	(void) nce_xmit(ill,
	    ND_NEIGHBOR_ADVERT,
	    ill,	/* ill to be used for extracting ill_nd_lla */
	    B_TRUE,	/* use ill_nd_lla */
	    &target,	/* Source and target of the advertisement pkt */
	    &src,	/* IP Destination (source of original pkt) */
	    flag);
done:
	if (bad_solicit)
		BUMP_MIB(mib, ipv6IfIcmpInBadNeighborSolicitations);
	if (our_nce != NULL)
		NCE_REFRELE(our_nce);
}

void
ndp_input_advert(ill_t *ill, mblk_t *mp, mblk_t *dl_mp)
{
	nd_neighbor_advert_t *na;
	uint32_t	hlen = ill->ill_nd_lla_len;
	uchar_t		*haddr = NULL;
	icmp6_t		*icmp_nd;
	ip6_t		*ip6h;
	nce_t		*dst_nce = NULL;
	in6_addr_t	target;
	nd_opt_hdr_t	*opt = NULL;
	int		len;
	mib2_ipv6IfIcmpEntry_t	*mib = ill->ill_icmp6_mib;

	ip6h = (ip6_t *)mp->b_rptr;
	icmp_nd = (icmp6_t *)(mp->b_rptr + IPV6_HDR_LEN);
	len = mp->b_wptr - mp->b_rptr - IPV6_HDR_LEN;
	na = (nd_neighbor_advert_t *)icmp_nd;
	if (IN6_IS_ADDR_MULTICAST(&ip6h->ip6_dst) &&
	    (na->nd_na_flags_reserved & ND_NA_FLAG_SOLICITED)) {
		ip1dbg(("ndp_input_advert: Target is multicast but the "
		    "solicited flag is not zero\n"));
		BUMP_MIB(mib, ipv6IfIcmpInBadNeighborAdvertisements);
		return;
	}
	target = na->nd_na_target;
	if (IN6_IS_ADDR_MULTICAST(&target)) {
		ip1dbg(("ndp_input_advert: Target is multicast!\n"));
		BUMP_MIB(mib, ipv6IfIcmpInBadNeighborAdvertisements);
		return;
	}
	if (len > sizeof (nd_neighbor_advert_t)) {
		opt = (nd_opt_hdr_t *)&na[1];
		if (!ndp_verify_optlen(opt,
		    len - sizeof (nd_neighbor_advert_t))) {
			ip1dbg(("ndp_input_advert: cannot verify SLLA\n"));
			BUMP_MIB(mib, ipv6IfIcmpInBadNeighborAdvertisements);
			return;
		}
		/* At this point we have a verified NA per spec */
		len -= sizeof (nd_neighbor_advert_t);
		opt = ndp_get_option(opt, len, ND_OPT_TARGET_LINKADDR);
		if (opt != NULL) {
			haddr = (uchar_t *)&opt[1];
			if (hlen > opt->nd_opt_len * 8 - sizeof (*opt) ||
			    hlen == 0) {
				ip1dbg(("ndp_input_advert: bad SLLA\n"));
				BUMP_MIB(mib,
				    ipv6IfIcmpInBadNeighborAdvertisements);
				return;
			}
		}
	}

	/*
	 * If this interface is part of the group look at all the
	 * ills in the group.
	 */
	rw_enter(&ill_g_lock, RW_READER);
	if (ill->ill_group != NULL)
		ill = ill->ill_group->illgrp_ill;

	for (; ill != NULL; ill = ill->ill_group_next) {
		mutex_enter(&ill->ill_lock);
		if (!ILL_CAN_LOOKUP(ill)) {
			mutex_exit(&ill->ill_lock);
			continue;
		}
		ill_refhold_locked(ill);
		mutex_exit(&ill->ill_lock);
		dst_nce = ndp_lookup_v6(ill, &target, B_FALSE);
		/* We have to drop the lock since ndp_process calls put* */
		rw_exit(&ill_g_lock);
		if (dst_nce != NULL) {
			if ((dst_nce->nce_flags & NCE_F_PERMANENT) &&
			    dst_nce->nce_state == ND_PROBE) {
				/*
				 * Someone else sent an advertisement for an
				 * address that we're trying to configure.
				 * Tear it down.  Note that dl_mp might be NULL
				 * if we're getting a unicast reply.  This
				 * isn't typically done (multicast is the norm
				 * in response to a probe), but ip_ndp_failure
				 * will handle the dl_mp == NULL case as well.
				 */
				ip_ndp_failure(ill, mp, dl_mp, dst_nce);
			} else if (dst_nce->nce_flags & NCE_F_PERMANENT) {
				/*
				 * Someone just announced one of our local
				 * addresses.  If it wasn't us, then this is a
				 * conflict.  Defend the address or shut it
				 * down.
				 */
				if (dl_mp != NULL &&
				    (haddr == NULL ||
				    nce_cmp_ll_addr(dst_nce, haddr,
				    ill->ill_nd_lla_len))) {
					ip_ndp_conflict(ill, mp, dl_mp,
					    dst_nce);
				}
			} else {
				if (na->nd_na_flags_reserved &
				    ND_NA_FLAG_ROUTER) {
					dst_nce->nce_flags |= NCE_F_ISROUTER;
				}
				/* B_TRUE indicates this an advertisement */
				ndp_process(dst_nce, haddr,
				    na->nd_na_flags_reserved, B_TRUE);
			}
			NCE_REFRELE(dst_nce);
		}
		rw_enter(&ill_g_lock, RW_READER);
		ill_refrele(ill);
	}
	rw_exit(&ill_g_lock);
}

/*
 * Process NDP neighbor solicitation/advertisement messages.
 * The checksum has already checked o.k before reaching here.
 */
void
ndp_input(ill_t *ill, mblk_t *mp, mblk_t *dl_mp)
{
	icmp6_t		*icmp_nd;
	ip6_t		*ip6h;
	int		len;
	mib2_ipv6IfIcmpEntry_t	*mib = ill->ill_icmp6_mib;


	if (!pullupmsg(mp, -1)) {
		ip1dbg(("ndp_input: pullupmsg failed\n"));
		BUMP_MIB(ill->ill_ip6_mib, ipv6InDiscards);
		goto done;
	}
	ip6h = (ip6_t *)mp->b_rptr;
	if (ip6h->ip6_hops != IPV6_MAX_HOPS) {
		ip1dbg(("ndp_input: hoplimit != IPV6_MAX_HOPS\n"));
		BUMP_MIB(mib, ipv6IfIcmpBadHoplimit);
		goto done;
	}
	/*
	 * NDP does not accept any extension headers between the
	 * IP header and the ICMP header since e.g. a routing
	 * header could be dangerous.
	 * This assumes that any AH or ESP headers are removed
	 * by ip prior to passing the packet to ndp_input.
	 */
	if (ip6h->ip6_nxt != IPPROTO_ICMPV6) {
		ip1dbg(("ndp_input: Wrong next header 0x%x\n",
		    ip6h->ip6_nxt));
		BUMP_MIB(mib, ipv6IfIcmpInErrors);
		goto done;
	}
	icmp_nd = (icmp6_t *)(mp->b_rptr + IPV6_HDR_LEN);
	ASSERT(icmp_nd->icmp6_type == ND_NEIGHBOR_SOLICIT ||
	    icmp_nd->icmp6_type == ND_NEIGHBOR_ADVERT);
	if (icmp_nd->icmp6_code != 0) {
		ip1dbg(("ndp_input: icmp6 code != 0 \n"));
		BUMP_MIB(mib, ipv6IfIcmpInErrors);
		goto done;
	}
	len = mp->b_wptr - mp->b_rptr - IPV6_HDR_LEN;
	/*
	 * Make sure packet length is large enough for either
	 * a NS or a NA icmp packet.
	 */
	if (len <  sizeof (struct icmp6_hdr) + sizeof (struct in6_addr)) {
		ip1dbg(("ndp_input: packet too short\n"));
		BUMP_MIB(mib, ipv6IfIcmpInErrors);
		goto done;
	}
	if (icmp_nd->icmp6_type == ND_NEIGHBOR_SOLICIT) {
		ndp_input_solicit(ill, mp, dl_mp);
	} else {
		ndp_input_advert(ill, mp, dl_mp);
	}
done:
	freemsg(mp);
}

/*
 * nce_xmit is called to form and transmit a ND solicitation or
 * advertisement ICMP packet.
 *
 * If the source address is unspecified and this isn't a probe (used for
 * duplicate address detection), an appropriate source address and link layer
 * address will be chosen here.  The link layer address option is included if
 * the source is specified (i.e., all non-probe packets), and omitted (per the
 * specification) otherwise.
 *
 * It returns B_FALSE only if it does a successful put() to the
 * corresponding ill's ill_wq otherwise returns B_TRUE.
 */
static boolean_t
nce_xmit(ill_t *ill, uint32_t operation, ill_t *hwaddr_ill,
    boolean_t use_nd_lla, const in6_addr_t *sender, const in6_addr_t *target,
    int flag)
{
	uint32_t	len;
	icmp6_t 	*icmp6;
	mblk_t		*mp;
	ip6_t		*ip6h;
	nd_opt_hdr_t	*opt;
	uint_t		plen;
	ip6i_t		*ip6i;
	ipif_t		*src_ipif = NULL;
	uint8_t		*hw_addr;

	/*
	 * If we have a unspecified source(sender) address, select a
	 * proper source address for the solicitation here itself so
	 * that we can initialize the h/w address correctly. This is
	 * needed for interface groups as source address can come from
	 * the whole group and the h/w address initialized from ill will
	 * be wrong if the source address comes from a different ill.
	 *
	 * Note that the NA never comes here with the unspecified source
	 * address. The following asserts that whenever the source
	 * address is specified, the haddr also should be specified.
	 */
	ASSERT(IN6_IS_ADDR_UNSPECIFIED(sender) || (hwaddr_ill != NULL));

	if (IN6_IS_ADDR_UNSPECIFIED(sender) && !(flag & NDP_PROBE)) {
		ASSERT(operation != ND_NEIGHBOR_ADVERT);
		/*
		 * Pick a source address for this solicitation, but
		 * restrict the selection to addresses assigned to the
		 * output interface (or interface group).  We do this
		 * because the destination will create a neighbor cache
		 * entry for the source address of this packet, so the
		 * source address had better be a valid neighbor.
		 */
		src_ipif = ipif_select_source_v6(ill, target, RESTRICT_TO_ILL,
		    IPV6_PREFER_SRC_DEFAULT, GLOBAL_ZONEID);
		if (src_ipif == NULL) {
			char buf[INET6_ADDRSTRLEN];

			ip1dbg(("nce_xmit: No source ipif for dst %s\n",
			    inet_ntop(AF_INET6, (char *)target, buf,
			    sizeof (buf))));
			return (B_TRUE);
		}
		sender = &src_ipif->ipif_v6src_addr;
		hwaddr_ill = src_ipif->ipif_ill;
	}

	/*
	 * Always make sure that the NS/NA packets don't get load
	 * spread. This is needed so that the probe packets sent
	 * by the in.mpathd daemon can really go out on the desired
	 * interface. Probe packets are made to go out on a desired
	 * interface by including a ip6i with ATTACH_IF flag. As these
	 * packets indirectly end up sending/receiving NS/NA packets
	 * (neighbor doing NUD), we have to make sure that NA
	 * also go out on the same interface.
	 */
	plen = (sizeof (nd_opt_hdr_t) + ill->ill_nd_lla_len + 7) / 8;
	len = IPV6_HDR_LEN + sizeof (ip6i_t) + sizeof (nd_neighbor_advert_t) +
	    plen * 8;
	mp = allocb(len,  BPRI_LO);
	if (mp == NULL) {
		if (src_ipif != NULL)
			ipif_refrele(src_ipif);
		return (B_TRUE);
	}
	bzero((char *)mp->b_rptr, len);
	mp->b_wptr = mp->b_rptr + len;

	ip6i = (ip6i_t *)mp->b_rptr;
	ip6i->ip6i_vcf = IPV6_DEFAULT_VERS_AND_FLOW;
	ip6i->ip6i_nxt = IPPROTO_RAW;
	ip6i->ip6i_flags = IP6I_ATTACH_IF | IP6I_HOPLIMIT;
	if (flag & NDP_PROBE)
		ip6i->ip6i_flags |= IP6I_UNSPEC_SRC;
	ip6i->ip6i_ifindex = ill->ill_phyint->phyint_ifindex;

	ip6h = (ip6_t *)(mp->b_rptr + sizeof (ip6i_t));
	ip6h->ip6_vcf = IPV6_DEFAULT_VERS_AND_FLOW;
	ip6h->ip6_plen = htons(len - IPV6_HDR_LEN - sizeof (ip6i_t));
	ip6h->ip6_nxt = IPPROTO_ICMPV6;
	ip6h->ip6_hops = IPV6_MAX_HOPS;
	ip6h->ip6_dst = *target;
	icmp6 = (icmp6_t *)&ip6h[1];

	opt = (nd_opt_hdr_t *)((uint8_t *)ip6h + IPV6_HDR_LEN +
	    sizeof (nd_neighbor_advert_t));

	if (operation == ND_NEIGHBOR_SOLICIT) {
		nd_neighbor_solicit_t *ns = (nd_neighbor_solicit_t *)icmp6;

		if (!(flag & NDP_PROBE))
			opt->nd_opt_type = ND_OPT_SOURCE_LINKADDR;
		ip6h->ip6_src = *sender;
		ns->nd_ns_target = *target;
		if (!(flag & NDP_UNICAST)) {
			/* Form multicast address of the target */
			ip6h->ip6_dst = ipv6_solicited_node_mcast;
			ip6h->ip6_dst.s6_addr32[3] |=
			    ns->nd_ns_target.s6_addr32[3];
		}
	} else {
		nd_neighbor_advert_t *na = (nd_neighbor_advert_t *)icmp6;

		ASSERT(!(flag & NDP_PROBE));
		opt->nd_opt_type = ND_OPT_TARGET_LINKADDR;
		ip6h->ip6_src = *sender;
		na->nd_na_target = *sender;
		if (flag & NDP_ISROUTER)
			na->nd_na_flags_reserved |= ND_NA_FLAG_ROUTER;
		if (flag & NDP_SOLICITED)
			na->nd_na_flags_reserved |= ND_NA_FLAG_SOLICITED;
		if (flag & NDP_ORIDE)
			na->nd_na_flags_reserved |= ND_NA_FLAG_OVERRIDE;
	}

	hw_addr = NULL;
	if (!(flag & NDP_PROBE)) {
		mutex_enter(&hwaddr_ill->ill_lock);
		hw_addr = use_nd_lla ? hwaddr_ill->ill_nd_lla :
		    hwaddr_ill->ill_phys_addr;
		if (hw_addr != NULL) {
			/* Fill in link layer address and option len */
			opt->nd_opt_len = (uint8_t)plen;
			bcopy(hw_addr, &opt[1], hwaddr_ill->ill_nd_lla_len);
		}
		mutex_exit(&hwaddr_ill->ill_lock);
	}
	if (hw_addr == NULL) {
		/* If there's no link layer address option, then strip it. */
		len -= plen * 8;
		mp->b_wptr = mp->b_rptr + len;
		ip6h->ip6_plen = htons(len - IPV6_HDR_LEN - sizeof (ip6i_t));
	}

	icmp6->icmp6_type = (uint8_t)operation;
	icmp6->icmp6_code = 0;
	/*
	 * Prepare for checksum by putting icmp length in the icmp
	 * checksum field. The checksum is calculated in ip_wput_v6.
	 */
	icmp6->icmp6_cksum = ip6h->ip6_plen;

	if (src_ipif != NULL)
		ipif_refrele(src_ipif);
	if (canput(ill->ill_wq)) {
		put(ill->ill_wq, mp);
		return (B_FALSE);
	}
	freemsg(mp);
	return (B_TRUE);
}

/*
 * Make a link layer address (does not include the SAP) from an nce.
 * To form the link layer address, use the last four bytes of ipv6
 * address passed in and the fixed offset stored in nce.
 */
static void
nce_make_mapping(nce_t *nce, uchar_t *addrpos, uchar_t *addr)
{
	uchar_t *mask, *to;
	ill_t	*ill = nce->nce_ill;
	int 	len;

	if (ill->ill_net_type == IRE_IF_NORESOLVER)
		return;
	ASSERT(nce->nce_res_mp != NULL);
	ASSERT(ill->ill_net_type == IRE_IF_RESOLVER);
	ASSERT(nce->nce_flags & NCE_F_MAPPING);
	ASSERT(!IN6_IS_ADDR_UNSPECIFIED(&nce->nce_extract_mask));
	ASSERT(addr != NULL);
	bcopy(nce->nce_res_mp->b_rptr + NCE_LL_ADDR_OFFSET(ill),
	    addrpos, ill->ill_nd_lla_len);
	len = MIN((int)ill->ill_nd_lla_len - nce->nce_ll_extract_start,
	    IPV6_ADDR_LEN);
	mask = (uchar_t *)&nce->nce_extract_mask;
	mask += (IPV6_ADDR_LEN - len);
	addr += (IPV6_ADDR_LEN - len);
	to = addrpos + nce->nce_ll_extract_start;
	while (len-- > 0)
		*to++ |= *mask++ & *addr++;
}

/*
 * Pass a cache report back out via NDD.
 */
/* ARGSUSED */
int
ndp_report(queue_t *q, mblk_t *mp, caddr_t arg, cred_t *ioc_cr)
{
	(void) mi_mpprintf(mp, "ifname      hardware addr    flags"
			"     proto addr/mask");
	ndp_walk(NULL, (pfi_t)nce_report1, (uchar_t *)mp);
	return (0);
}

/*
 * Add a single line to the NDP Cache Entry Report.
 */
static void
nce_report1(nce_t *nce, uchar_t *mp_arg)
{
	ill_t		*ill = nce->nce_ill;
	char		local_buf[INET6_ADDRSTRLEN];
	uchar_t		flags_buf[10];
	uint32_t	flags = nce->nce_flags;
	mblk_t		*mp = (mblk_t *)mp_arg;
	uchar_t		*h;
	uchar_t		*m = flags_buf;
	in6_addr_t	v6addr;

	/*
	 * Lock the nce to protect nce_res_mp from being changed
	 * if an external resolver address resolution completes
	 * while nce_res_mp is being accessed here.
	 *
	 * Deal with all address formats, not just Ethernet-specific
	 * In addition, make sure that the mblk has enough space
	 * before writing to it. If is doesn't, allocate a new one.
	 */
	if (nce->nce_ipversion == IPV4_VERSION)
		/* Don't include v4 nce_ts in NDP cache entry report */
		return;

	ASSERT(ill != NULL);
	v6addr = nce->nce_mask;
	if (flags & NCE_F_PERMANENT)
		*m++ = 'P';
	if (flags & NCE_F_ISROUTER)
		*m++ = 'R';
	if (flags & NCE_F_MAPPING)
		*m++ = 'M';
	*m = '\0';

	if (ill->ill_net_type == IRE_IF_RESOLVER) {
		size_t		addrlen;
		char		*addr_buf;
		dl_unitdata_req_t	*dl;

		mutex_enter(&nce->nce_lock);
		h = nce->nce_res_mp->b_rptr + NCE_LL_ADDR_OFFSET(ill);
		dl = (dl_unitdata_req_t *)nce->nce_res_mp->b_rptr;
		if (ill->ill_flags & ILLF_XRESOLV)
			addrlen = (3 * (dl->dl_dest_addr_length));
		else
			addrlen = (3 * (ill->ill_nd_lla_len));
		if (addrlen <= 0) {
			mutex_exit(&nce->nce_lock);
			(void) mi_mpprintf(mp,
			    "%8s %9s %5s %s/%d",
			    ill->ill_name,
			    "None",
			    (uchar_t *)&flags_buf,
			    inet_ntop(AF_INET6, (char *)&nce->nce_addr,
				(char *)local_buf, sizeof (local_buf)),
				ip_mask_to_plen_v6(&v6addr));
		} else {
			/*
			 * Convert the hardware/lla address to ascii
			 */
			addr_buf = kmem_zalloc(addrlen, KM_NOSLEEP);
			if (addr_buf == NULL) {
				mutex_exit(&nce->nce_lock);
				return;
			}
			(void) mac_colon_addr((uint8_t *)h,
			    (ill->ill_flags & ILLF_XRESOLV) ?
			    dl->dl_dest_addr_length : ill->ill_nd_lla_len,
			    addr_buf, addrlen);
			mutex_exit(&nce->nce_lock);
			(void) mi_mpprintf(mp, "%8s %17s %5s %s/%d",
			    ill->ill_name, addr_buf, (uchar_t *)&flags_buf,
			    inet_ntop(AF_INET6, (char *)&nce->nce_addr,
				(char *)local_buf, sizeof (local_buf)),
				ip_mask_to_plen_v6(&v6addr));
			kmem_free(addr_buf, addrlen);
		}
	} else {
		(void) mi_mpprintf(mp,
		    "%8s %9s %5s %s/%d",
		    ill->ill_name,
		    "None",
		    (uchar_t *)&flags_buf,
		    inet_ntop(AF_INET6, (char *)&nce->nce_addr,
			(char *)local_buf, sizeof (local_buf)),
			ip_mask_to_plen_v6(&v6addr));
	}
}

mblk_t *
nce_udreq_alloc(ill_t *ill)
{
	mblk_t	*template_mp = NULL;
	dl_unitdata_req_t *dlur;
	int	sap_length;

	ASSERT(ill->ill_isv6);

	sap_length = ill->ill_sap_length;
	template_mp = ip_dlpi_alloc(sizeof (dl_unitdata_req_t) +
	    ill->ill_nd_lla_len + ABS(sap_length), DL_UNITDATA_REQ);
	if (template_mp == NULL)
		return (NULL);

	dlur = (dl_unitdata_req_t *)template_mp->b_rptr;
	dlur->dl_priority.dl_min = 0;
	dlur->dl_priority.dl_max = 0;
	dlur->dl_dest_addr_length = ABS(sap_length) + ill->ill_nd_lla_len;
	dlur->dl_dest_addr_offset = sizeof (dl_unitdata_req_t);

	/* Copy in the SAP value. */
	NCE_LL_SAP_COPY(ill, template_mp);

	return (template_mp);
}

/*
 * NDP retransmit timer.
 * This timer goes off when:
 * a. It is time to retransmit NS for resolver.
 * b. It is time to send reachability probes.
 */
void
ndp_timer(void *arg)
{
	nce_t		*nce = arg;
	ill_t		*ill = nce->nce_ill;
	uint32_t	ms;
	char		addrbuf[INET6_ADDRSTRLEN];
	mblk_t		*mp;
	boolean_t	dropped = B_FALSE;

	/*
	 * The timer has to be cancelled by ndp_delete before doing the final
	 * refrele. So the NCE is guaranteed to exist when the timer runs
	 * until it clears the timeout_id. Before clearing the timeout_id
	 * bump up the refcnt so that we can continue to use the nce
	 */
	ASSERT(nce != NULL);

	/*
	 * Grab the ill_g_lock now itself to avoid lock order problems.
	 * nce_solicit needs ill_g_lock to be able to traverse ills
	 */
	rw_enter(&ill_g_lock, RW_READER);
	mutex_enter(&nce->nce_lock);
	NCE_REFHOLD_LOCKED(nce);
	nce->nce_timeout_id = 0;

	/*
	 * Check the reachability state first.
	 */
	switch (nce->nce_state) {
	case ND_DELAY:
		rw_exit(&ill_g_lock);
		nce->nce_state = ND_PROBE;
		mutex_exit(&nce->nce_lock);
		(void) nce_xmit(ill, ND_NEIGHBOR_SOLICIT, NULL, B_FALSE,
		    &ipv6_all_zeros, &nce->nce_addr, NDP_UNICAST);
		if (ip_debug > 3) {
			/* ip2dbg */
			pr_addr_dbg("ndp_timer: state for %s changed "
			    "to PROBE\n", AF_INET6, &nce->nce_addr);
		}
		NDP_RESTART_TIMER(nce, ill->ill_reachable_retrans_time);
		NCE_REFRELE(nce);
		return;
	case ND_PROBE:
		/* must be retransmit timer */
		rw_exit(&ill_g_lock);
		nce->nce_pcnt--;
		ASSERT(nce->nce_pcnt < ND_MAX_UNICAST_SOLICIT &&
		    nce->nce_pcnt >= -1);
		if (nce->nce_pcnt > 0) {
			/*
			 * As per RFC2461, the nce gets deleted after
			 * MAX_UNICAST_SOLICIT unsuccessful re-transmissions.
			 * Note that the first unicast solicitation is sent
			 * during the DELAY state.
			 */
			ip2dbg(("ndp_timer: pcount=%x dst %s\n",
			    nce->nce_pcnt, inet_ntop(AF_INET6, &nce->nce_addr,
			    addrbuf, sizeof (addrbuf))));
			mutex_exit(&nce->nce_lock);
			dropped = nce_xmit(ill, ND_NEIGHBOR_SOLICIT, NULL,
			    B_FALSE, &ipv6_all_zeros, &nce->nce_addr,
			    (nce->nce_flags & NCE_F_PERMANENT) ? NDP_PROBE :
			    NDP_UNICAST);
			if (dropped) {
				mutex_enter(&nce->nce_lock);
				nce->nce_pcnt++;
				mutex_exit(&nce->nce_lock);
			}
			NDP_RESTART_TIMER(nce, ILL_PROBE_INTERVAL(ill));
		} else if (nce->nce_pcnt < 0) {
			/* No hope, delete the nce */
			nce->nce_state = ND_UNREACHABLE;
			mutex_exit(&nce->nce_lock);
			if (ip_debug > 2) {
				/* ip1dbg */
				pr_addr_dbg("ndp_timer: Delete IRE for"
				    " dst %s\n", AF_INET6, &nce->nce_addr);
			}
			ndp_delete(nce);
		} else if (!(nce->nce_flags & NCE_F_PERMANENT)) {
			/* Wait RetransTimer, before deleting the entry */
			ip2dbg(("ndp_timer: pcount=%x dst %s\n",
			    nce->nce_pcnt, inet_ntop(AF_INET6,
			    &nce->nce_addr, addrbuf, sizeof (addrbuf))));
			mutex_exit(&nce->nce_lock);
			/* Wait one interval before killing */
			NDP_RESTART_TIMER(nce, ill->ill_reachable_retrans_time);
		} else if (ill->ill_phyint->phyint_flags & PHYI_RUNNING) {
			ipif_t *ipif;

			/*
			 * We're done probing, and we can now declare this
			 * address to be usable.  Let IP know that it's ok to
			 * use.
			 */
			nce->nce_state = ND_REACHABLE;
			mutex_exit(&nce->nce_lock);
			ipif = ipif_lookup_addr_v6(&nce->nce_addr, ill,
			    ALL_ZONES, NULL, NULL, NULL, NULL);
			if (ipif != NULL) {
				if (ipif->ipif_was_dup) {
					char ibuf[LIFNAMSIZ + 10];
					char sbuf[INET6_ADDRSTRLEN];

					ipif->ipif_was_dup = B_FALSE;
					(void) strlcpy(ibuf, ill->ill_name,
					    sizeof (ibuf));
					(void) inet_ntop(AF_INET6,
					    &ipif->ipif_v6lcl_addr,
					    sbuf, sizeof (sbuf));
					if (ipif->ipif_id != 0) {
						(void) snprintf(ibuf +
						    ill->ill_name_length - 1,
						    sizeof (ibuf) -
						    ill->ill_name_length + 1,
						    ":%d", ipif->ipif_id);
					}
					cmn_err(CE_NOTE, "recovered address "
					    "%s on %s", sbuf, ibuf);
				}
				if ((ipif->ipif_flags & IPIF_UP) &&
				    !ipif->ipif_addr_ready) {
					ip_rts_ifmsg(ipif);
					ip_rts_newaddrmsg(RTM_ADD, 0, ipif);
					sctp_update_ipif(ipif, SCTP_IPIF_UP);
				}
				ipif->ipif_addr_ready = 1;
				ipif_refrele(ipif);
			}
			/* Begin defending our new address */
			nce->nce_unsolicit_count = 0;
			dropped = nce_xmit(ill, ND_NEIGHBOR_ADVERT, ill,
			    B_FALSE, &nce->nce_addr, &ipv6_all_hosts_mcast,
			    NDP_ORIDE);
			if (dropped) {
				nce->nce_unsolicit_count = 1;
				NDP_RESTART_TIMER(nce,
				    ip_ndp_unsolicit_interval);
			} else if (ip_ndp_defense_interval != 0) {
				NDP_RESTART_TIMER(nce, ip_ndp_defense_interval);
			}
		} else {
			/*
			 * This is an address we're probing to be our own, but
			 * the ill is down.  Wait until it comes back before
			 * doing anything, but switch to reachable state so
			 * that the restart will work.
			 */
			nce->nce_state = ND_REACHABLE;
			mutex_exit(&nce->nce_lock);
		}
		NCE_REFRELE(nce);
		return;
	case ND_INCOMPLETE:
		/*
		 * Must be resolvers retransmit timer.
		 */
		for (mp = nce->nce_qd_mp; mp != NULL; mp = mp->b_next) {
			ip6i_t	*ip6i;
			ip6_t	*ip6h;
			mblk_t *data_mp;

			/*
			 * Walk the list of packets queued, and see if there
			 * are any multipathing probe packets. Such packets
			 * are always queued at the head. Since this is a
			 * retransmit timer firing, mark such packets as
			 * delayed in ND resolution. This info will be used
			 * in ip_wput_v6(). Multipathing probe packets will
			 * always have an ip6i_t. Once we hit a packet without
			 * it, we can break out of this loop.
			 */
			if (mp->b_datap->db_type == M_CTL)
				data_mp = mp->b_cont;
			else
				data_mp = mp;

			ip6h = (ip6_t *)data_mp->b_rptr;
			if (ip6h->ip6_nxt != IPPROTO_RAW)
				break;

			/*
			 * This message should have been pulled up already in
			 * ip_wput_v6. We can't do pullups here because the
			 * b_next/b_prev is non-NULL.
			 */
			ip6i = (ip6i_t *)ip6h;
			ASSERT((data_mp->b_wptr - (uchar_t *)ip6i) >=
			    sizeof (ip6i_t) + IPV6_HDR_LEN);

			/* Mark this packet as delayed due to ND resolution */
			if (ip6i->ip6i_flags & IP6I_DROP_IFDELAYED)
				ip6i->ip6i_flags |= IP6I_ND_DELAYED;
		}
		if (nce->nce_qd_mp != NULL) {
			ms = nce_solicit(nce, NULL);
			rw_exit(&ill_g_lock);
			if (ms == 0) {
				if (nce->nce_state != ND_REACHABLE) {
					mutex_exit(&nce->nce_lock);
					nce_resolv_failed(nce);
					ndp_delete(nce);
				} else {
					mutex_exit(&nce->nce_lock);
				}
			} else {
				mutex_exit(&nce->nce_lock);
				NDP_RESTART_TIMER(nce, (clock_t)ms);
			}
			NCE_REFRELE(nce);
			return;
		}
		mutex_exit(&nce->nce_lock);
		rw_exit(&ill_g_lock);
		NCE_REFRELE(nce);
		break;
	case ND_REACHABLE :
		rw_exit(&ill_g_lock);
		if (((nce->nce_flags & NCE_F_UNSOL_ADV) &&
		    nce->nce_unsolicit_count != 0) ||
		    ((nce->nce_flags & NCE_F_PERMANENT) &&
		    ip_ndp_defense_interval != 0)) {
			if (nce->nce_unsolicit_count > 0)
				nce->nce_unsolicit_count--;
			mutex_exit(&nce->nce_lock);
			dropped = nce_xmit(ill,
			    ND_NEIGHBOR_ADVERT,
			    ill,	/* ill to be used for hw addr */
			    B_FALSE,	/* use ill_phys_addr */
			    &nce->nce_addr,
			    &ipv6_all_hosts_mcast,
			    NDP_ORIDE);
			if (dropped) {
				mutex_enter(&nce->nce_lock);
				nce->nce_unsolicit_count++;
				mutex_exit(&nce->nce_lock);
			}
			if (nce->nce_unsolicit_count != 0) {
				NDP_RESTART_TIMER(nce,
				    ip_ndp_unsolicit_interval);
			} else {
				NDP_RESTART_TIMER(nce,
				    ip_ndp_defense_interval);
			}
		} else {
			mutex_exit(&nce->nce_lock);
		}
		NCE_REFRELE(nce);
		break;
	default:
		rw_exit(&ill_g_lock);
		mutex_exit(&nce->nce_lock);
		NCE_REFRELE(nce);
		break;
	}
}

/*
 * Set a link layer address from the ll_addr passed in.
 * Copy SAP from ill.
 */
static void
nce_set_ll(nce_t *nce, uchar_t *ll_addr)
{
	ill_t	*ill = nce->nce_ill;
	uchar_t	*woffset;

	ASSERT(ll_addr != NULL);
	/* Always called before fast_path_probe */
	ASSERT(nce->nce_fp_mp == NULL);
	if (ill->ill_sap_length != 0) {
		/*
		 * Copy the SAP type specified in the
		 * request into the xmit template.
		 */
		NCE_LL_SAP_COPY(ill, nce->nce_res_mp);
	}
	if (ill->ill_phys_addr_length > 0) {
		/*
		 * The bcopy() below used to be called for the physical address
		 * length rather than the link layer address length. For
		 * ethernet and many other media, the phys_addr and lla are
		 * identical.
		 * However, with xresolv interfaces being introduced, the
		 * phys_addr and lla are no longer the same, and the physical
		 * address may not have any useful meaning, so we use the lla
		 * for IPv6 address resolution and destination addressing.
		 *
		 * For PPP or other interfaces with a zero length
		 * physical address, don't do anything here.
		 * The bcopy() with a zero phys_addr length was previously
		 * a no-op for interfaces with a zero-length physical address.
		 * Using the lla for them would change the way they operate.
		 * Doing nothing in such cases preserves expected behavior.
		 */
		woffset = nce->nce_res_mp->b_rptr + NCE_LL_ADDR_OFFSET(ill);
		bcopy(ll_addr, woffset, ill->ill_nd_lla_len);
	}
}

static boolean_t
nce_cmp_ll_addr(const nce_t *nce, const uchar_t *ll_addr, uint32_t ll_addr_len)
{
	ill_t	*ill = nce->nce_ill;
	uchar_t	*ll_offset;

	ASSERT(nce->nce_res_mp != NULL);
	if (ll_addr == NULL)
		return (B_FALSE);
	ll_offset = nce->nce_res_mp->b_rptr + NCE_LL_ADDR_OFFSET(ill);
	if (bcmp(ll_addr, ll_offset, ll_addr_len) != 0)
		return (B_TRUE);
	return (B_FALSE);
}

/*
 * Updates the link layer address or the reachability state of
 * a cache entry.  Reset probe counter if needed.
 */
static void
nce_update(nce_t *nce, uint16_t new_state, uchar_t *new_ll_addr)
{
	ill_t	*ill = nce->nce_ill;
	boolean_t need_stop_timer = B_FALSE;
	boolean_t need_fastpath_update = B_FALSE;

	ASSERT(MUTEX_HELD(&nce->nce_lock));
	ASSERT(nce->nce_ipversion == IPV6_VERSION);
	/*
	 * If this interface does not do NUD, there is no point
	 * in allowing an update to the cache entry.  Although
	 * we will respond to NS.
	 * The only time we accept an update for a resolver when
	 * NUD is turned off is when it has just been created.
	 * Non-Resolvers will always be created as REACHABLE.
	 */
	if (new_state != ND_UNCHANGED) {
		if ((nce->nce_flags & NCE_F_NONUD) &&
		    (nce->nce_state != ND_INCOMPLETE))
			return;
		ASSERT((int16_t)new_state >= ND_STATE_VALID_MIN);
		ASSERT((int16_t)new_state <= ND_STATE_VALID_MAX);
		need_stop_timer = B_TRUE;
		if (new_state == ND_REACHABLE)
			nce->nce_last = TICK_TO_MSEC(lbolt64);
		else {
			/* We force NUD in this case */
			nce->nce_last = 0;
		}
		nce->nce_state = new_state;
		nce->nce_pcnt = ND_MAX_UNICAST_SOLICIT;
	}
	/*
	 * In case of fast path we need to free the the fastpath
	 * M_DATA and do another probe.  Otherwise we can just
	 * overwrite the DL_UNITDATA_REQ data, noting we'll lose
	 * whatever packets that happens to be transmitting at the time.
	 */
	if (new_ll_addr != NULL) {
		ASSERT(nce->nce_res_mp->b_rptr + NCE_LL_ADDR_OFFSET(ill) +
		    ill->ill_nd_lla_len <= nce->nce_res_mp->b_wptr);
		bcopy(new_ll_addr, nce->nce_res_mp->b_rptr +
		    NCE_LL_ADDR_OFFSET(ill), ill->ill_nd_lla_len);
		if (nce->nce_fp_mp != NULL) {
			freemsg(nce->nce_fp_mp);
			nce->nce_fp_mp = NULL;
		}
		need_fastpath_update = B_TRUE;
	}
	mutex_exit(&nce->nce_lock);
	if (need_stop_timer) {
		(void) untimeout(nce->nce_timeout_id);
		nce->nce_timeout_id = 0;
	}
	if (need_fastpath_update)
		nce_fastpath(nce);
	mutex_enter(&nce->nce_lock);
}

void
nce_queue_mp_common(nce_t *nce, mblk_t *mp, boolean_t head_insert)
{
	uint_t	count = 0;
	mblk_t  **mpp;

	ASSERT(MUTEX_HELD(&nce->nce_lock));

	for (mpp = &nce->nce_qd_mp; *mpp != NULL;
	    mpp = &(*mpp)->b_next) {
		if (++count >
		    nce->nce_ill->ill_max_buf) {
			mblk_t *tmp = nce->nce_qd_mp->b_next;

			nce->nce_qd_mp->b_next = NULL;
			nce->nce_qd_mp->b_prev = NULL;
			freemsg(nce->nce_qd_mp);
			nce->nce_qd_mp = tmp;
		}
	}
	/* put this on the list */
	if (head_insert) {
		mp->b_next = nce->nce_qd_mp;
		nce->nce_qd_mp = mp;
	} else {
		*mpp = mp;
	}
}

static void
nce_queue_mp(nce_t *nce, mblk_t *mp)
{
	boolean_t head_insert = B_FALSE;
	ip6_t	*ip6h;
	ip6i_t	*ip6i;
	mblk_t *data_mp;

	ASSERT(MUTEX_HELD(&nce->nce_lock));

	if (mp->b_datap->db_type == M_CTL)
		data_mp = mp->b_cont;
	else
		data_mp = mp;
	ip6h = (ip6_t *)data_mp->b_rptr;
	if (ip6h->ip6_nxt == IPPROTO_RAW) {
		/*
		 * This message should have been pulled up already in
		 * ip_wput_v6. We can't do pullups here because the message
		 * could be from the nce_qd_mp which could have b_next/b_prev
		 * non-NULL.
		 */
		ip6i = (ip6i_t *)ip6h;
		ASSERT((data_mp->b_wptr - (uchar_t *)ip6i) >=
			    sizeof (ip6i_t) + IPV6_HDR_LEN);
		/*
		 * Multipathing probe packets have IP6I_DROP_IFDELAYED set.
		 * This has 2 aspects mentioned below.
		 * 1. Perform head insertion in the nce_qd_mp for these packets.
		 * This ensures that next retransmit of ND solicitation
		 * will use the interface specified by the probe packet,
		 * for both NS and NA. This corresponds to the src address
		 * in the IPv6 packet. If we insert at tail, we will be
		 * depending on the packet at the head for successful
		 * ND resolution. This is not reliable, because the interface
		 * on which the NA arrives could be different from the interface
		 * on which the NS was sent, and if the receiving interface is
		 * failed, it will appear that the sending interface is also
		 * failed, causing in.mpathd to misdiagnose this as link
		 * failure.
		 * 2. Drop the original packet, if the ND resolution did not
		 * succeed in the first attempt. However we will create the
		 * nce and the ire, as soon as the ND resolution succeeds.
		 * We don't gain anything by queueing multiple probe packets
		 * and sending them back-to-back once resolution succeeds.
		 * It is sufficient to send just 1 packet after ND resolution
		 * succeeds. Since mpathd is sending down probe packets at a
		 * constant rate, we don't need to send the queued packet. We
		 * need to queue it only for NDP resolution. The benefit of
		 * dropping the probe packets that were delayed in ND
		 * resolution, is that in.mpathd will not see inflated
		 * RTT. If the ND resolution does not succeed within
		 * in.mpathd's failure detection time, mpathd may detect
		 * a failure, and it does not matter whether the packet
		 * was queued or dropped.
		 */
		if (ip6i->ip6i_flags & IP6I_DROP_IFDELAYED)
			head_insert = B_TRUE;
	}

	nce_queue_mp_common(nce, mp, head_insert);
}

/*
 * Called when address resolution failed due to a timeout.
 * Send an ICMP unreachable in response to all queued packets.
 */
void
nce_resolv_failed(nce_t *nce)
{
	mblk_t	*mp, *nxt_mp, *first_mp;
	char	buf[INET6_ADDRSTRLEN];
	ip6_t *ip6h;
	zoneid_t zoneid = GLOBAL_ZONEID;

	ip1dbg(("nce_resolv_failed: dst %s\n",
	    inet_ntop(AF_INET6, (char *)&nce->nce_addr, buf, sizeof (buf))));
	mutex_enter(&nce->nce_lock);
	mp = nce->nce_qd_mp;
	nce->nce_qd_mp = NULL;
	mutex_exit(&nce->nce_lock);
	while (mp != NULL) {
		nxt_mp = mp->b_next;
		mp->b_next = NULL;
		mp->b_prev = NULL;

		first_mp = mp;
		if (mp->b_datap->db_type == M_CTL) {
			ipsec_out_t *io = (ipsec_out_t *)mp->b_rptr;
			ASSERT(io->ipsec_out_type == IPSEC_OUT);
			zoneid = io->ipsec_out_zoneid;
			ASSERT(zoneid != ALL_ZONES);
			mp = mp->b_cont;
		}

		ip6h = (ip6_t *)mp->b_rptr;
		if (ip6h->ip6_nxt == IPPROTO_RAW) {
			ip6i_t *ip6i;
			/*
			 * This message should have been pulled up already
			 * in ip_wput_v6. ip_hdr_complete_v6 assumes that
			 * the header is pulled up.
			 */
			ip6i = (ip6i_t *)ip6h;
			ASSERT((mp->b_wptr - (uchar_t *)ip6i) >=
			    sizeof (ip6i_t) + IPV6_HDR_LEN);
			mp->b_rptr += sizeof (ip6i_t);
		}
		/*
		 * Ignore failure since icmp_unreachable_v6 will silently
		 * drop packets with an unspecified source address.
		 */
		(void) ip_hdr_complete_v6((ip6_t *)mp->b_rptr, zoneid);
		icmp_unreachable_v6(nce->nce_ill->ill_wq, first_mp,
		    ICMP6_DST_UNREACH_ADDR, B_FALSE, B_FALSE);
		mp = nxt_mp;
	}
}

/*
 * Called by SIOCSNDP* ioctl to add/change an nce entry
 * and the corresponding attributes.
 * Disallow states other than ND_REACHABLE or ND_STALE.
 */
int
ndp_sioc_update(ill_t *ill, lif_nd_req_t *lnr)
{
	sin6_t		*sin6;
	in6_addr_t	*addr;
	nce_t		*nce;
	int		err;
	uint16_t	new_flags = 0;
	uint16_t	old_flags = 0;
	int		inflags = lnr->lnr_flags;

	ASSERT(ill->ill_isv6);
	if ((lnr->lnr_state_create != ND_REACHABLE) &&
	    (lnr->lnr_state_create != ND_STALE))
		return (EINVAL);

	sin6 = (sin6_t *)&lnr->lnr_addr;
	addr = &sin6->sin6_addr;

	mutex_enter(&ndp6.ndp_g_lock);
	/* We know it can not be mapping so just look in the hash table */
	nce = *((nce_t **)NCE_HASH_PTR_V6(*addr));
	nce = nce_lookup_addr(ill, addr, nce);
	if (nce != NULL)
		new_flags = nce->nce_flags;

	switch (inflags & (NDF_ISROUTER_ON|NDF_ISROUTER_OFF)) {
	case NDF_ISROUTER_ON:
		new_flags |= NCE_F_ISROUTER;
		break;
	case NDF_ISROUTER_OFF:
		new_flags &= ~NCE_F_ISROUTER;
		break;
	case (NDF_ISROUTER_OFF|NDF_ISROUTER_ON):
		mutex_exit(&ndp6.ndp_g_lock);
		if (nce != NULL)
			NCE_REFRELE(nce);
		return (EINVAL);
	}

	switch (inflags & (NDF_ANYCAST_ON|NDF_ANYCAST_OFF)) {
	case NDF_ANYCAST_ON:
		new_flags |= NCE_F_ANYCAST;
		break;
	case NDF_ANYCAST_OFF:
		new_flags &= ~NCE_F_ANYCAST;
		break;
	case (NDF_ANYCAST_OFF|NDF_ANYCAST_ON):
		mutex_exit(&ndp6.ndp_g_lock);
		if (nce != NULL)
			NCE_REFRELE(nce);
		return (EINVAL);
	}

	switch (inflags & (NDF_PROXY_ON|NDF_PROXY_OFF)) {
	case NDF_PROXY_ON:
		new_flags |= NCE_F_PROXY;
		break;
	case NDF_PROXY_OFF:
		new_flags &= ~NCE_F_PROXY;
		break;
	case (NDF_PROXY_OFF|NDF_PROXY_ON):
		mutex_exit(&ndp6.ndp_g_lock);
		if (nce != NULL)
			NCE_REFRELE(nce);
		return (EINVAL);
	}

	if (nce == NULL) {
		err = ndp_add(ill,
		    (uchar_t *)lnr->lnr_hdw_addr,
		    addr,
		    &ipv6_all_ones,
		    &ipv6_all_zeros,
		    0,
		    new_flags,
		    lnr->lnr_state_create,
		    &nce,
		    NULL,
		    NULL);
		if (err != 0) {
			mutex_exit(&ndp6.ndp_g_lock);
			ip1dbg(("ndp_sioc_update: Can't create NCE %d\n", err));
			return (err);
		}
	}
	old_flags = nce->nce_flags;
	if (old_flags & NCE_F_ISROUTER && !(new_flags & NCE_F_ISROUTER)) {
		/*
		 * Router turned to host, delete all ires.
		 * XXX Just delete the entry, but we need to add too.
		 */
		nce->nce_flags &= ~NCE_F_ISROUTER;
		mutex_exit(&ndp6.ndp_g_lock);
		ndp_delete(nce);
		NCE_REFRELE(nce);
		return (0);
	}
	mutex_exit(&ndp6.ndp_g_lock);

	mutex_enter(&nce->nce_lock);
	nce->nce_flags = new_flags;
	mutex_exit(&nce->nce_lock);
	/*
	 * Note that we ignore the state at this point, which
	 * should be either STALE or REACHABLE.  Instead we let
	 * the link layer address passed in to determine the state
	 * much like incoming packets.
	 */
	ndp_process(nce, (uchar_t *)lnr->lnr_hdw_addr, 0, B_FALSE);
	NCE_REFRELE(nce);
	return (0);
}

/*
 * If the device driver supports it, we make nce_fp_mp to have
 * an M_DATA prepend.  Otherwise nce_fp_mp will be null.
 * The caller insures there is hold on nce for this function.
 * Note that since ill_fastpath_probe() copies the mblk there is
 * no need for the hold beyond this function.
 */
static void
nce_fastpath(nce_t *nce)
{
	ill_t	*ill = nce->nce_ill;
	int res;

	ASSERT(ill != NULL);
	if (nce->nce_fp_mp != NULL) {
		/* Already contains fastpath info */
		return;
	}
	if (nce->nce_res_mp != NULL) {
		nce_fastpath_list_add(nce);
		res = ill_fastpath_probe(ill, nce->nce_res_mp);
		/*
		 * EAGAIN is an indication of a transient error
		 * i.e. allocation failure etc. leave the nce in the list it
		 * will be updated when another probe happens for another ire
		 * if not it will be taken out of the list when the ire is
		 * deleted.
		 */

		if (res != 0 && res != EAGAIN)
			nce_fastpath_list_delete(nce);
	}
}

/*
 * Drain the list of nce's waiting for fastpath response.
 */
void
nce_fastpath_list_dispatch(ill_t *ill, boolean_t (*func)(nce_t *, void  *),
    void *arg)
{

	nce_t *next_nce;
	nce_t *current_nce;
	nce_t *first_nce;
	nce_t *prev_nce = NULL;

	ASSERT(ill != NULL && ill->ill_isv6);

	mutex_enter(&ill->ill_lock);
	first_nce = current_nce = (nce_t *)ill->ill_fastpath_list;
	while (current_nce != (nce_t *)&ill->ill_fastpath_list) {
		next_nce = current_nce->nce_fastpath;
		/*
		 * Take it off the list if we're flushing, or if the callback
		 * routine tells us to do so.  Otherwise, leave the nce in the
		 * fastpath list to handle any pending response from the lower
		 * layer.  We can't drain the list when the callback routine
		 * comparison failed, because the response is asynchronous in
		 * nature, and may not arrive in the same order as the list
		 * insertion.
		 */
		if (func == NULL || func(current_nce, arg)) {
			current_nce->nce_fastpath = NULL;
			if (current_nce == first_nce)
				ill->ill_fastpath_list = first_nce = next_nce;
			else
				prev_nce->nce_fastpath = next_nce;
		} else {
			/* previous element that is still in the list */
			prev_nce = current_nce;
		}
		current_nce = next_nce;
	}
	mutex_exit(&ill->ill_lock);
}

/*
 * Add nce to the nce fastpath list.
 */
void
nce_fastpath_list_add(nce_t *nce)
{
	ill_t *ill;

	ill = nce->nce_ill;
	ASSERT(ill != NULL && ill->ill_isv6);

	mutex_enter(&ill->ill_lock);
	mutex_enter(&nce->nce_lock);

	/*
	 * if nce has not been deleted and
	 * is not already in the list add it.
	 */
	if (!(nce->nce_flags & NCE_F_CONDEMNED) &&
	    (nce->nce_fastpath == NULL)) {
		nce->nce_fastpath = (nce_t *)ill->ill_fastpath_list;
		ill->ill_fastpath_list = nce;
	}

	mutex_exit(&nce->nce_lock);
	mutex_exit(&ill->ill_lock);
}

/*
 * remove nce from the nce fastpath list.
 */
void
nce_fastpath_list_delete(nce_t *nce)
{
	nce_t *nce_ptr;

	ill_t *ill;

	ill = nce->nce_ill;
	ASSERT(ill != NULL);
	if (!ill->ill_isv6)  {
		/*
		 * v4 nce_t's do not have nce_fastpath set.
		 */
		return;
	}

	mutex_enter(&ill->ill_lock);
	if (nce->nce_fastpath == NULL)
		goto done;

	ASSERT(ill->ill_fastpath_list != &ill->ill_fastpath_list);

	if (ill->ill_fastpath_list == nce) {
		ill->ill_fastpath_list = nce->nce_fastpath;
	} else {
		nce_ptr = ill->ill_fastpath_list;
		while (nce_ptr != (nce_t *)&ill->ill_fastpath_list) {
			if (nce_ptr->nce_fastpath == nce) {
				nce_ptr->nce_fastpath = nce->nce_fastpath;
				break;
			}
			nce_ptr = nce_ptr->nce_fastpath;
		}
	}

	nce->nce_fastpath = NULL;
done:
	mutex_exit(&ill->ill_lock);
}

/*
 * Update all NCE's that are not in fastpath mode and
 * have an nce_fp_mp that matches mp. mp->b_cont contains
 * the fastpath header.
 *
 * Returns TRUE if entry should be dequeued, or FALSE otherwise.
 */
boolean_t
ndp_fastpath_update(nce_t *nce, void *arg)
{
	mblk_t 	*mp, *fp_mp;
	uchar_t	*mp_rptr, *ud_mp_rptr;
	mblk_t	*ud_mp = nce->nce_res_mp;
	ptrdiff_t	cmplen;

	if (nce->nce_flags & NCE_F_MAPPING)
		return (B_TRUE);
	if ((nce->nce_fp_mp != NULL) || (ud_mp == NULL))
		return (B_TRUE);

	ip2dbg(("ndp_fastpath_update: trying\n"));
	mp = (mblk_t *)arg;
	mp_rptr = mp->b_rptr;
	cmplen = mp->b_wptr - mp_rptr;
	ASSERT(cmplen >= 0);
	ud_mp_rptr = ud_mp->b_rptr;
	/*
	 * The nce is locked here to prevent any other threads
	 * from accessing and changing nce_res_mp when the IPv6 address
	 * becomes resolved to an lla while we're in the middle
	 * of looking at and comparing the hardware address (lla).
	 * It is also locked to prevent multiple threads in nce_fastpath_update
	 * from examining nce_res_mp atthe same time.
	 */
	mutex_enter(&nce->nce_lock);
	if (ud_mp->b_wptr - ud_mp_rptr != cmplen ||
	    bcmp((char *)mp_rptr, (char *)ud_mp_rptr, cmplen) != 0) {
		mutex_exit(&nce->nce_lock);
		/*
		 * Don't take the ire off the fastpath list yet,
		 * since the response may come later.
		 */
		return (B_FALSE);
	}
	/* Matched - install mp as the fastpath mp */
	ip1dbg(("ndp_fastpath_update: match\n"));
	fp_mp = dupb(mp->b_cont);
	if (fp_mp != NULL) {
		nce->nce_fp_mp = fp_mp;
	}
	mutex_exit(&nce->nce_lock);
	return (B_TRUE);
}

/*
 * This function handles the DL_NOTE_FASTPATH_FLUSH notification from
 * driver.  Note that it assumes IP is exclusive...
 */
/* ARGSUSED */
void
ndp_fastpath_flush(nce_t *nce, char *arg)
{
	if (nce->nce_flags & NCE_F_MAPPING)
		return;
	/* No fastpath info? */
	if (nce->nce_fp_mp == NULL || nce->nce_res_mp == NULL)
		return;

	/* Just delete the NCE... */
	ndp_delete(nce);
}

/*
 * Return a pointer to a given option in the packet.
 * Assumes that option part of the packet have already been validated.
 */
nd_opt_hdr_t *
ndp_get_option(nd_opt_hdr_t *opt, int optlen, int opt_type)
{
	while (optlen > 0) {
		if (opt->nd_opt_type == opt_type)
			return (opt);
		optlen -= 8 * opt->nd_opt_len;
		opt = (struct nd_opt_hdr *)((char *)opt + 8 * opt->nd_opt_len);
	}
	return (NULL);
}

/*
 * Verify all option lengths present are > 0, also check to see
 * if the option lengths and packet length are consistent.
 */
boolean_t
ndp_verify_optlen(nd_opt_hdr_t *opt, int optlen)
{
	ASSERT(opt != NULL);
	while (optlen > 0) {
		if (opt->nd_opt_len == 0)
			return (B_FALSE);
		optlen -= 8 * opt->nd_opt_len;
		if (optlen < 0)
			return (B_FALSE);
		opt = (struct nd_opt_hdr *)((char *)opt + 8 * opt->nd_opt_len);
	}
	return (B_TRUE);
}

/*
 * ndp_walk function.
 * Free a fraction of the NCE cache entries.
 * A fraction of zero means to not free any in that category.
 */
void
ndp_cache_reclaim(nce_t *nce, char *arg)
{
	nce_cache_reclaim_t *ncr = (nce_cache_reclaim_t *)arg;
	uint_t	rand;

	if (nce->nce_flags & NCE_F_PERMANENT)
		return;

	rand = (uint_t)lbolt +
	    NCE_ADDR_HASH_V6(nce->nce_addr, NCE_TABLE_SIZE);
	if (ncr->ncr_host != 0 &&
	    (rand/ncr->ncr_host)*ncr->ncr_host == rand) {
		ndp_delete(nce);
		return;
	}
}

/*
 * ndp_walk function.
 * Count the number of NCEs that can be deleted.
 * These would be hosts but not routers.
 */
void
ndp_cache_count(nce_t *nce, char *arg)
{
	ncc_cache_count_t *ncc = (ncc_cache_count_t *)arg;

	if (nce->nce_flags & NCE_F_PERMANENT)
		return;

	ncc->ncc_total++;
	if (!(nce->nce_flags & NCE_F_ISROUTER))
		ncc->ncc_host++;
}

#ifdef NCE_DEBUG
th_trace_t *
th_trace_nce_lookup(nce_t *nce)
{
	int bucket_id;
	th_trace_t *th_trace;

	ASSERT(MUTEX_HELD(&nce->nce_lock));

	bucket_id = IP_TR_HASH(curthread);
	ASSERT(bucket_id < IP_TR_HASH_MAX);

	for (th_trace = nce->nce_trace[bucket_id]; th_trace != NULL;
	    th_trace = th_trace->th_next) {
		if (th_trace->th_id == curthread)
			return (th_trace);
	}
	return (NULL);
}

void
nce_trace_ref(nce_t *nce)
{
	int bucket_id;
	th_trace_t *th_trace;

	/*
	 * Attempt to locate the trace buffer for the curthread.
	 * If it does not exist, then allocate a new trace buffer
	 * and link it in list of trace bufs for this ipif, at the head
	 */
	ASSERT(MUTEX_HELD(&nce->nce_lock));

	if (nce->nce_trace_disable == B_TRUE)
		return;

	th_trace = th_trace_nce_lookup(nce);
	if (th_trace == NULL) {
		bucket_id = IP_TR_HASH(curthread);
		th_trace = (th_trace_t *)kmem_zalloc(sizeof (th_trace_t),
		    KM_NOSLEEP);
		if (th_trace == NULL) {
			nce->nce_trace_disable = B_TRUE;
			nce_trace_inactive(nce);
			return;
		}
		th_trace->th_id = curthread;
		th_trace->th_next = nce->nce_trace[bucket_id];
		th_trace->th_prev = &nce->nce_trace[bucket_id];
		if (th_trace->th_next != NULL)
			th_trace->th_next->th_prev = &th_trace->th_next;
		nce->nce_trace[bucket_id] = th_trace;
	}
	ASSERT(th_trace->th_refcnt < TR_BUF_MAX - 1);
	th_trace->th_refcnt++;
	th_trace_rrecord(th_trace);
}

void
nce_untrace_ref(nce_t *nce)
{
	th_trace_t *th_trace;

	ASSERT(MUTEX_HELD(&nce->nce_lock));

	if (nce->nce_trace_disable == B_TRUE)
		return;

	th_trace = th_trace_nce_lookup(nce);
	ASSERT(th_trace != NULL && th_trace->th_refcnt > 0);

	th_trace_rrecord(th_trace);
	th_trace->th_refcnt--;
}

void
nce_trace_inactive(nce_t *nce)
{
	th_trace_t *th_trace;
	int i;

	ASSERT(MUTEX_HELD(&nce->nce_lock));

	for (i = 0; i < IP_TR_HASH_MAX; i++) {
		while (nce->nce_trace[i] != NULL) {
			th_trace = nce->nce_trace[i];

			/* unlink th_trace and free it */
			nce->nce_trace[i] = th_trace->th_next;
			if (th_trace->th_next != NULL)
				th_trace->th_next->th_prev =
				    &nce->nce_trace[i];

			th_trace->th_next = NULL;
			th_trace->th_prev = NULL;
			kmem_free(th_trace, sizeof (th_trace_t));
		}
	}

}

/* ARGSUSED */
int
nce_thread_exit(nce_t *nce, caddr_t arg)
{
	th_trace_t	*th_trace;

	mutex_enter(&nce->nce_lock);
	th_trace = th_trace_nce_lookup(nce);

	if (th_trace == NULL) {
		mutex_exit(&nce->nce_lock);
		return (0);
	}

	ASSERT(th_trace->th_refcnt == 0);

	/* unlink th_trace and free it */
	*th_trace->th_prev = th_trace->th_next;
	if (th_trace->th_next != NULL)
		th_trace->th_next->th_prev = th_trace->th_prev;
	th_trace->th_next = NULL;
	th_trace->th_prev = NULL;
	kmem_free(th_trace, sizeof (th_trace_t));
	mutex_exit(&nce->nce_lock);
	return (0);
}
#endif

/*
 * Called when address resolution fails due to a timeout.
 * Send an ICMP unreachable in response to all queued packets.
 */
void
arp_resolv_failed(nce_t *nce)
{
	mblk_t	*mp, *nxt_mp, *first_mp;
	char	buf[INET6_ADDRSTRLEN];
	zoneid_t zoneid = GLOBAL_ZONEID;
	struct in_addr ipv4addr;

	IN6_V4MAPPED_TO_INADDR(&nce->nce_addr, &ipv4addr);
	ip3dbg(("arp_resolv_failed: dst %s\n",
	    inet_ntop(AF_INET, &ipv4addr, buf, sizeof (buf))));
	mutex_enter(&nce->nce_lock);
	mp = nce->nce_qd_mp;
	nce->nce_qd_mp = NULL;
	mutex_exit(&nce->nce_lock);

	while (mp != NULL) {
		nxt_mp = mp->b_next;
		mp->b_next = NULL;
		mp->b_prev = NULL;

		first_mp = mp;
		/*
		 * Send icmp unreachable messages
		 * to the hosts.
		 */
		(void) ip_hdr_complete((ipha_t *)mp->b_rptr, zoneid);
		ip3dbg(("arp_resolv_failed: Calling icmp_unreachable\n"));
		icmp_unreachable(nce->nce_ill->ill_wq, first_mp,
		    ICMP_HOST_UNREACHABLE);
		mp = nxt_mp;
	}
}

static int
ndp_lookup_then_add_v4(ill_t *ill, uchar_t *hw_addr, const in_addr_t *addr,
    const in_addr_t *mask, const in_addr_t *extract_mask,
    uint32_t hw_extract_start, uint16_t flags, uint16_t state,
    nce_t **newnce, mblk_t *fp_mp, mblk_t *res_mp)
{
	int	err = 0;
	nce_t	*nce;
	in6_addr_t addr6;

	mutex_enter(&ndp4.ndp_g_lock);
	nce = *((nce_t **)NCE_HASH_PTR_V4(*addr));
	IN6_IPADDR_TO_V4MAPPED(*addr, &addr6);
	nce = nce_lookup_addr(ill, &addr6, nce);
	if (nce == NULL) {
		err = ndp_add_v4(ill,
		    hw_addr,
		    addr,
		    mask,
		    extract_mask,
		    hw_extract_start,
		    flags,
		    state,
		    newnce,
		    fp_mp,
		    res_mp);
	} else {
		*newnce = nce;
		err = EEXIST;
	}
	mutex_exit(&ndp4.ndp_g_lock);
	return (err);
}

/*
 * NDP Cache Entry creation routine for IPv4.
 * Mapped entries are handled in arp.
 * This routine must always be called with ndp4.ndp_g_lock held.
 * Prior to return, nce_refcnt is incremented.
 */
static int
ndp_add_v4(ill_t *ill, uchar_t *hw_addr, const in_addr_t *addr,
    const in_addr_t *mask, const in_addr_t *extract_mask,
    uint32_t hw_extract_start, uint16_t flags, uint16_t state,
    nce_t **newnce, mblk_t *fp_mp, mblk_t *res_mp)
{
	static	nce_t		nce_nil;
	nce_t		*nce;
	mblk_t		*mp;
	mblk_t		*template;
	nce_t		**ncep;

	ASSERT(MUTEX_HELD(&ndp4.ndp_g_lock));
	ASSERT(ill != NULL);
	if ((flags & ~NCE_EXTERNAL_FLAGS_MASK)) {
		return (EINVAL);
	}
	ASSERT((flags & NCE_F_MAPPING) == 0);
	ASSERT(extract_mask == NULL);
	/*
	 * Allocate the mblk to hold the nce.
	 */
	mp = allocb(sizeof (nce_t), BPRI_MED);
	if (mp == NULL)
		return (ENOMEM);

	nce = (nce_t *)mp->b_rptr;
	mp->b_wptr = (uchar_t *)&nce[1];
	*nce = nce_nil;

	/*
	 * This one holds link layer address; if res_mp has been provided
	 * by the caller, accept it without any further checks. Otherwise,
	 * for V4, we fill it up with ill_resolver_mp here, then in
	 * in ire_arpresolve(), we fill it up with the ARP query
	 * once its formulated.
	 */
	if (res_mp != NULL) {
		template = res_mp;
	} else  {
		template = copyb(ill->ill_resolver_mp);
	}
	if (template == NULL) {
		freeb(mp);
		return (ENOMEM);
	}
	nce->nce_ill = ill;
	nce->nce_ipversion = IPV4_VERSION;
	nce->nce_flags = flags;
	nce->nce_state = state;
	nce->nce_pcnt = ND_MAX_UNICAST_SOLICIT;
	nce->nce_rcnt = ill->ill_xmit_count;
	IN6_IPADDR_TO_V4MAPPED(*addr, &nce->nce_addr);
	if (*mask == IP_HOST_MASK) {
		nce->nce_mask = ipv6_all_ones;
	} else  {
		IN6_IPADDR_TO_V4MAPPED(*mask, &nce->nce_mask);
	}
	nce->nce_extract_mask = ipv6_all_zeros;
	nce->nce_ll_extract_start = hw_extract_start;
	nce->nce_fp_mp = (fp_mp? fp_mp : NULL);
	nce->nce_res_mp = template;
	if (state == ND_REACHABLE)
		nce->nce_last = TICK_TO_MSEC(lbolt64);
	else
		nce->nce_last = 0;
	nce->nce_qd_mp = NULL;
	nce->nce_mp = mp;
	if (hw_addr != NULL)
		nce_set_ll(nce, hw_addr);
	/* This one is for nce getting created */
	nce->nce_refcnt = 1;
	mutex_init(&nce->nce_lock, NULL, MUTEX_DEFAULT, NULL);
	ncep = ((nce_t **)NCE_HASH_PTR_V4(*addr));

#ifdef NCE_DEBUG
	bzero(nce->nce_trace, sizeof (th_trace_t *) * IP_TR_HASH_MAX);
#endif
	/*
	 * Atomically ensure that the ill is not CONDEMNED, before
	 * adding the NCE.
	 */
	mutex_enter(&ill->ill_lock);
	if (ill->ill_state_flags & ILL_CONDEMNED) {
		mutex_exit(&ill->ill_lock);
		freeb(mp);
		if (res_mp == NULL) {
			/*
			 * template was locally allocated. need to free it.
			 */
			freeb(template);
		}
		return (EINVAL);
	}
	if ((nce->nce_next = *ncep) != NULL)
		nce->nce_next->nce_ptpn = &nce->nce_next;
	*ncep = nce;
	nce->nce_ptpn = ncep;
	*newnce = nce;
	/* This one is for nce being used by an active thread */
	NCE_REFHOLD(*newnce);

	/* Bump up the number of nce's referencing this ill */
	ill->ill_nce_cnt++;
	mutex_exit(&ill->ill_lock);
	return (0);
}

void
ndp_flush_qd_mp(nce_t *nce)
{
	mblk_t *qd_mp, *qd_next;

	ASSERT(MUTEX_HELD(&nce->nce_lock));
	qd_mp = nce->nce_qd_mp;
	nce->nce_qd_mp = NULL;
	while (qd_mp != NULL) {
		qd_next = qd_mp->b_next;
		qd_mp->b_next = NULL;
		qd_mp->b_prev = NULL;
		freemsg(qd_mp);
		qd_mp = qd_next;
	}
}

nce_t *
nce_reinit(nce_t *nce)
{
	nce_t *newnce = NULL;
	in_addr_t nce_addr, nce_mask;

	IN6_V4MAPPED_TO_IPADDR(&nce->nce_addr, nce_addr);
	IN6_V4MAPPED_TO_IPADDR(&nce->nce_mask, nce_mask);
	/*
	 * delete the old one. this will get rid of any ire's pointing
	 * at this nce.
	 */
	ndp_delete(nce);
	/*
	 * create a new nce with the same addr and mask.
	 */
	mutex_enter(&ndp4.ndp_g_lock);
	(void) ndp_add_v4(nce->nce_ill, NULL, &nce_addr, &nce_mask, NULL, 0, 0,
	    ND_INITIAL, &newnce, NULL, NULL);
	mutex_exit(&ndp4.ndp_g_lock);
	/*
	 * refrele the old nce.
	 */
	NCE_REFRELE(nce);
	return (newnce);
}

/*
 * ndp_walk routine to delete all entries that have a given destination or
 * gateway address and cached link layer (MAC) address.  This is used when ARP
 * informs us that a network-to-link-layer mapping may have changed.
 */
void
nce_delete_hw_changed(nce_t *nce, void *arg)
{
	nce_hw_map_t *hwm = arg;
	mblk_t *mp;
	dl_unitdata_req_t *dlu;
	uchar_t *macaddr;
	ill_t *ill;
	int saplen;
	ipaddr_t nce_addr;

	if (nce->nce_state != ND_REACHABLE)
		return;

	IN6_V4MAPPED_TO_IPADDR(&nce->nce_addr, nce_addr);
	if (nce_addr != hwm->hwm_addr)
		return;

	mutex_enter(&nce->nce_lock);
	if ((mp = nce->nce_res_mp) == NULL) {
		mutex_exit(&nce->nce_lock);
		return;
	}
	dlu = (dl_unitdata_req_t *)mp->b_rptr;
	macaddr = (uchar_t *)(dlu + 1);
	ill = nce->nce_ill;
	if ((saplen = ill->ill_sap_length) > 0)
		macaddr += saplen;
	else
		saplen = -saplen;

	/*
	 * If the hardware address is unchanged, then leave this one alone.
	 * Note that saplen == abs(saplen) now.
	 */
	if (hwm->hwm_hwlen == dlu->dl_dest_addr_length - saplen &&
	    bcmp(hwm->hwm_hwaddr, macaddr, hwm->hwm_hwlen) == 0) {
		mutex_exit(&nce->nce_lock);
		return;
	}
	mutex_exit(&nce->nce_lock);

	DTRACE_PROBE1(nce__hw__deleted, nce_t *, nce);
	ndp_delete(nce);
}

/*
 * This function verifies whether a given IPv4 address is potentially known to
 * the NCE subsystem.  If so, then ARP must not delete the corresponding ace_t,
 * so that it can continue to look for hardware changes on that address.
 */
boolean_t
ndp_lookup_ipaddr(in_addr_t addr)
{
	nce_t		*nce;
	struct in_addr	nceaddr;

	if (addr == INADDR_ANY)
		return (B_FALSE);

	mutex_enter(&ndp4.ndp_g_lock);
	nce = *(nce_t **)NCE_HASH_PTR_V4(addr);
	for (; nce != NULL; nce = nce->nce_next) {
		/* Note that only v4 mapped entries are in the table. */
		IN6_V4MAPPED_TO_INADDR(&nce->nce_addr, &nceaddr);
		if (addr == nceaddr.s_addr &&
		    IN6_ARE_ADDR_EQUAL(&nce->nce_mask, &ipv6_all_ones)) {
			/* Single flag check; no lock needed */
			if (!(nce->nce_flags & NCE_F_CONDEMNED))
				break;
		}
	}
	mutex_exit(&ndp4.ndp_g_lock);
	return (nce != NULL);
}

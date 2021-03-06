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

#ifndef _INET_IP_FTABLE_H
#define	_INET_IP_FTABLE_H

#pragma ident	"@(#)ip_ftable.h	1.1	06/08/09 SMI"

#ifdef	__cplusplus
extern "C" {
#endif

#ifdef 	_KERNEL

#include <net/radix.h>
#include <inet/common.h>
#include <inet/ip.h>

struct rt_entry {
	struct	radix_node rt_nodes[2];	/* tree glue, and other values */
	/*
	 * struct rt_entry must begin with a struct radix_node (or two!)
	 * to a 'struct rt_entry *'
	 */
	struct rt_sockaddr rt_dst;
	/*
	 * multiple routes to same dest/mask via varying gate/ifp are stored
	 * in the rt_irb bucket.
	 */
	irb_t rt_irb;
};

/* vehicle for passing args through rn_walktree */
struct rtfuncarg {
	pfv_t rt_func;
	char *rt_arg;
	uint_t rt_match_flags;
	uint_t rt_ire_type;
	ill_t  *rt_ill;
	zoneid_t rt_zoneid;
};
int rtfunc(struct radix_node *, void *);

typedef struct rt_entry rt_t;
typedef struct rtfuncarg rtf_t;

struct ts_label_s;
extern	ire_t	*ire_ftable_lookup(ipaddr_t, ipaddr_t, ipaddr_t, int,
    const ipif_t *, ire_t **, zoneid_t, uint32_t,
    const struct ts_label_s *, int);
extern	ire_t *ire_lookup_multi(ipaddr_t, zoneid_t);
extern	ire_t *ipif_lookup_multi_ire(ipif_t *, ipaddr_t);
extern	void ire_delete_host_redirects(ipaddr_t);
extern	ire_t *ire_ihandle_lookup_onlink(ire_t *);
extern	ire_t *ire_forward(ipaddr_t, boolean_t *, ire_t *, ire_t *,
    const struct ts_label_s *);
extern void	ire_ftable_walk(struct rt_entry *, uint_t, uint_t,
    ill_t *, zoneid_t, pfv_t, char *);
extern irb_t	*ire_get_bucket(ire_t *);
extern uint_t ifindex_lookup(const struct sockaddr *, zoneid_t);
extern int ipfil_sendpkt(const struct sockaddr *, mblk_t *, uint_t, zoneid_t);

extern struct radix_node_head *ip_ftable;

#define	IRB_REFHOLD_RN(rn)					\
	if ((rn->rn_flags & RNF_ROOT) == 0)			\
		IRB_REFHOLD(&((rt_t *)(rn))->rt_irb)

#define	IRB_REFRELE_RN(rn)					\
	if ((rn->rn_flags & RNF_ROOT) == 0)			\
		irb_refrele_ftable(&((rt_t *)(rn))->rt_irb);

#else

#define	IRB_REFHOLD_RN(rn)	/* */
#define	IRB_REFRELE_RN(rn)	/* */

#endif /* _KERNEL */

#ifdef	__cplusplus
}
#endif

#endif	/* _INET_IP_FTABLE_H */

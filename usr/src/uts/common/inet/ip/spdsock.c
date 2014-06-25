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

#pragma ident	"@(#)spdsock.c	1.24	05/10/05 SMI"

#include <sys/param.h>
#include <sys/types.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/strsun.h>
#include <sys/stropts.h>
#include <sys/vnode.h>
#include <sys/sysmacros.h>
#define	_SUN_TPI_VERSION 2
#include <sys/tihdr.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/mkdev.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/proc.h>
#include <sys/suntpi.h>
#include <sys/policy.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <net/pfkeyv2.h>
#include <net/pfpolicy.h>

#include <inet/common.h>
#include <netinet/ip6.h>
#include <inet/ip.h>
#include <inet/ip6.h>
#include <inet/mi.h>
#include <inet/nd.h>
#include <inet/optcom.h>
#include <inet/ipsec_info.h>
#include <inet/ipsec_impl.h>
#include <inet/spdsock.h>
#include <inet/sadb.h>

#include <sys/isa_defs.h>

/*
 * This is a transport provider for the PF_POLICY IPsec policy
 * management socket, which provides a management interface into the
 * SPD, allowing policy rules to be added, deleted, and queried.
 *
 * This effectively replaces the old private SIOC*IPSECONFIG ioctls
 * with an extensible interface which will hopefully be public some
 * day.
 *
 * See <net/pfpolicy.h> for more details on the protocol.
 *
 * We link against drv/ip and call directly into it to manipulate the
 * SPD; see ipsec_impl.h for the policy data structures and spd.c for
 * the code which maintains them.
 *
 * The MT model of this is QPAIR with the addition of some explicit
 * locking to protect system-wide policy data structures.
 */

static vmem_t *spdsock_vmem;		/* for minor numbers. */

#define	ALIGNED64(x) IS_P2ALIGNED((x), sizeof (uint64_t))

/* Default structure copied into T_INFO_ACK messages (from rts.c...) */
static struct T_info_ack spdsock_g_t_info_ack = {
	T_INFO_ACK,
	T_INFINITE,	/* TSDU_size. Maximum size messages. */
	T_INVALID,	/* ETSDU_size. No expedited data. */
	T_INVALID,	/* CDATA_size. No connect data. */
	T_INVALID,	/* DDATA_size. No disconnect data. */
	0,		/* ADDR_size. */
	0,		/* OPT_size. No user-settable options */
	64 * 1024,	/* TIDU_size. spdsock allows maximum size messages. */
	T_COTS,		/* SERV_type. spdsock supports connection oriented. */
	TS_UNBND,	/* CURRENT_state. This is set from spdsock_state. */
	(XPG4_1)	/* Provider flags */
};

/* Named Dispatch Parameter Management Structure */
typedef struct spdsockpparam_s {
	uint_t	spdsock_param_min;
	uint_t	spdsock_param_max;
	uint_t	spdsock_param_value;
	char *spdsock_param_name;
} spdsockparam_t;

/*
 * Table of NDD variables supported by spdsock. These are loaded into
 * spdsock_g_nd in spdsock_init_nd.
 * All of these are alterable, within the min/max values given, at run time.
 */
static	spdsockparam_t	spdsock_param_arr[] = {
	/* min	max	value	name */
	{ 4096, 65536,	8192,	"spdsock_xmit_hiwat"},
	{ 0,	65536,	1024,	"spdsock_xmit_lowat"},
	{ 4096, 65536,	8192,	"spdsock_recv_hiwat"},
	{ 65536, 1024*1024*1024, 256*1024,	"spdsock_max_buf"},
	{ 0,	3,	0,	"spdsock_debug"},
};
#define	spdsock_xmit_hiwat	spdsock_param_arr[0].spdsock_param_value
#define	spdsock_xmit_lowat	spdsock_param_arr[1].spdsock_param_value
#define	spdsock_recv_hiwat	spdsock_param_arr[2].spdsock_param_value
#define	spdsock_max_buf		spdsock_param_arr[3].spdsock_param_value
#define	spdsock_debug		spdsock_param_arr[4].spdsock_param_value

kmutex_t spdsock_param_lock;	/* Protects the NDD variables. */

/*
 * To save algorithm update messages that are processed only after IPsec
 * is loaded.
 */
static spd_ext_t *spdsock_extv_algs[SPD_EXT_MAX + 1];
static mblk_t *spdsock_mp_algs = NULL;
static boolean_t spdsock_algs_pending = B_FALSE;
static ipsec_alginfo_t *spdsock_algs[IPSEC_NALGTYPES][IPSEC_MAX_ALGS];
static ipsec_algs_exec_mode_t spdsock_algs_exec_mode[IPSEC_NALGTYPES];
static kmutex_t spdsock_alg_lock;

#define	ss0dbg(a)	printf a
/* NOTE:  != 0 instead of > 0 so lint doesn't complain. */
#define	ss1dbg(a)	if (spdsock_debug != 0) printf a
#define	ss2dbg(a)	if (spdsock_debug > 1) printf a
#define	ss3dbg(a)	if (spdsock_debug > 2) printf a

static IDP spdsock_g_nd;

static int spdsock_close(queue_t *);
static int spdsock_open(queue_t *, dev_t *, int, int, cred_t *);
static void spdsock_wput(queue_t *, mblk_t *);
static void spdsock_wsrv(queue_t *);
static void spdsock_rsrv(queue_t *);
static void spdsock_loadcheck(void *);
static void spdsock_merge_algs(void);

static struct module_info info = {
	5138, "spdsock", 1, INFPSZ, 512, 128
};

static struct qinit rinit = {
	NULL, (pfi_t)spdsock_rsrv, spdsock_open, spdsock_close,
	NULL, &info
};

static struct qinit winit = {
	(pfi_t)spdsock_wput, (pfi_t)spdsock_wsrv, NULL, NULL, NULL, &info
};

struct streamtab spdsockinfo = {
	&rinit, &winit
};

/* mapping from alg type to protocol number, as per RFC 2407 */
static const uint_t algproto[] = {
	PROTO_IPSEC_AH,
	PROTO_IPSEC_ESP,
};

#define	NALGPROTOS	(sizeof (algproto) / sizeof (algproto[0]))

/* mapping from kernel exec mode to spdsock exec mode */
static const uint_t execmodes[] = {
	SPD_ALG_EXEC_MODE_SYNC,
	SPD_ALG_EXEC_MODE_ASYNC
};

#define	NEXECMODES	(sizeof (execmodes) / sizeof (execmodes[0]))

/* ARGSUSED */
static int
spdsock_param_get(q, mp, cp, cr)
	queue_t	*q;
	mblk_t	*mp;
	caddr_t	cp;
	cred_t *cr;
{
	spdsockparam_t	*spdsockpa = (spdsockparam_t *)cp;
	uint_t value;

	mutex_enter(&spdsock_param_lock);
	value = spdsockpa->spdsock_param_value;
	mutex_exit(&spdsock_param_lock);

	(void) mi_mpprintf(mp, "%u", value);
	return (0);
}

/* This routine sets an NDD variable in a spdsockparam_t structure. */
/* ARGSUSED */
static int
spdsock_param_set(q, mp, value, cp, cr)
	queue_t	*q;
	mblk_t	*mp;
	char *value;
	caddr_t	cp;
	cred_t *cr;
{
	ulong_t	new_value;
	spdsockparam_t	*spdsockpa = (spdsockparam_t *)cp;

	/* Convert the value from a string into a long integer. */
	if (ddi_strtoul(value, NULL, 10, &new_value) != 0)
		return (EINVAL);

	mutex_enter(&spdsock_param_lock);
	/*
	 * Fail the request if the new value does not lie within the
	 * required bounds.
	 */
	if (new_value < spdsockpa->spdsock_param_min ||
	    new_value > spdsockpa->spdsock_param_max) {
		mutex_exit(&spdsock_param_lock);
		return (EINVAL);
	}

	/* Set the new value */
	spdsockpa->spdsock_param_value = new_value;
	mutex_exit(&spdsock_param_lock);

	return (0);
}

boolean_t
spdsock_ddi_init(void)
{
	spdsockparam_t *ssp = spdsock_param_arr;
	int count = A_CNT(spdsock_param_arr);

	if (!spdsock_g_nd) {
		for (; count-- > 0; ssp++) {
			if (ssp->spdsock_param_name != NULL &&
			    (ssp->spdsock_param_name[0] != '\0')) {
				if (!nd_load(&spdsock_g_nd,
				    ssp->spdsock_param_name,
				    spdsock_param_get, spdsock_param_set,
				    (caddr_t)ssp)) {
					nd_free(&spdsock_g_nd);
					return (B_FALSE);
				}
			}
		}
	}

	spdsock_max_optsize = optcom_max_optsize(
	    spdsock_opt_obj.odb_opt_des_arr, spdsock_opt_obj.odb_opt_arr_cnt);

	spdsock_vmem = vmem_create("spdsock", (void *)1, MAXMIN, 1,
	    NULL, NULL, NULL, 1, VM_SLEEP | VMC_IDENTIFIER);

	mutex_init(&spdsock_param_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&spdsock_alg_lock, NULL, MUTEX_DEFAULT, NULL);

	return (B_TRUE);
}

void
spdsock_ddi_destroy(void)
{
	vmem_destroy(spdsock_vmem);
	mutex_destroy(&spdsock_param_lock);
	mutex_destroy(&spdsock_alg_lock);
	nd_free(&spdsock_g_nd);
}

/*
 * NOTE: large quantities of this should be shared with keysock.
 * Would be nice to combine some of this into a common module, but
 * not possible given time pressures.
 */

/*
 * High-level reality checking of extensions.
 */
/* ARGSUSED */ /* XXX */
static boolean_t
ext_check(spd_ext_t *ext)
{

	return (B_TRUE);	/* For now... */
}



/* Return values for spdsock_get_ext(). */
#define	KGE_OK	0
#define	KGE_DUP	1
#define	KGE_UNK	2
#define	KGE_LEN	3
#define	KGE_CHK	4

/*
 * Parse basic extension headers and return in the passed-in pointer vector.
 * Return values include:
 *
 *	KGE_OK	Everything's nice and parsed out.
 *		If there are no extensions, place NULL in extv[0].
 *	KGE_DUP	There is a duplicate extension.
 *		First instance in appropriate bin.  First duplicate in
 *		extv[0].
 *	KGE_UNK	Unknown extension type encountered.  extv[0] contains
 *		unknown header.
 *	KGE_LEN	Extension length error.
 *	KGE_CHK	High-level reality check failed on specific extension.
 *
 * My apologies for some of the pointer arithmetic in here.  I'm thinking
 * like an assembly programmer, yet trying to make the compiler happy.
 */
static int
spdsock_get_ext(spd_ext_t *extv[], spd_msg_t *basehdr, uint_t msgsize)
{
	bzero(extv, sizeof (spd_ext_t *) * (SPD_EXT_MAX + 1));

	/* Use extv[0] as the "current working pointer". */

	extv[0] = (spd_ext_t *)(basehdr + 1);

	while (extv[0] < (spd_ext_t *)(((uint8_t *)basehdr) + msgsize)) {
		/* Check for unknown headers. */
		if (extv[0]->spd_ext_type == 0 ||
		    extv[0]->spd_ext_type > SPD_EXT_MAX)
			return (KGE_UNK);

		/*
		 * Check length.  Use uint64_t because extlen is in units
		 * of 64-bit words.  If length goes beyond the msgsize,
		 * return an error.  (Zero length also qualifies here.)
		 */
		if (extv[0]->spd_ext_len == 0 ||
		    (void *)((uint64_t *)extv[0] + extv[0]->spd_ext_len) >
		    (void *)((uint8_t *)basehdr + msgsize))
			return (KGE_LEN);

		/* Check for redundant headers. */
		if (extv[extv[0]->spd_ext_type] != NULL)
			return (KGE_DUP);

		/*
		 * Reality check the extension if possible at the spdsock
		 * level.
		 */
		if (!ext_check(extv[0]))
			return (KGE_CHK);

		/* If I make it here, assign the appropriate bin. */
		extv[extv[0]->spd_ext_type] = extv[0];

		/* Advance pointer (See above for uint64_t ptr reasoning.) */
		extv[0] = (spd_ext_t *)
		    ((uint64_t *)extv[0] + extv[0]->spd_ext_len);
	}

	/* Everything's cool. */

	/*
	 * If extv[0] == NULL, then there are no extension headers in this
	 * message.  Ensure that this is the case.
	 */
	if (extv[0] == (spd_ext_t *)(basehdr + 1))
		extv[0] = NULL;

	return (KGE_OK);
}

static const int bad_ext_diag[] = {
	SPD_DIAGNOSTIC_MALFORMED_LCLPORT,
	SPD_DIAGNOSTIC_MALFORMED_REMPORT,
	SPD_DIAGNOSTIC_MALFORMED_PROTO,
	SPD_DIAGNOSTIC_MALFORMED_LCLADDR,
	SPD_DIAGNOSTIC_MALFORMED_REMADDR,
	SPD_DIAGNOSTIC_MALFORMED_ACTION,
	SPD_DIAGNOSTIC_MALFORMED_RULE,
	SPD_DIAGNOSTIC_MALFORMED_RULESET,
	SPD_DIAGNOSTIC_MALFORMED_ICMP_TYPECODE
};

static const int dup_ext_diag[] = {
	SPD_DIAGNOSTIC_DUPLICATE_LCLPORT,
	SPD_DIAGNOSTIC_DUPLICATE_REMPORT,
	SPD_DIAGNOSTIC_DUPLICATE_PROTO,
	SPD_DIAGNOSTIC_DUPLICATE_LCLADDR,
	SPD_DIAGNOSTIC_DUPLICATE_REMADDR,
	SPD_DIAGNOSTIC_DUPLICATE_ACTION,
	SPD_DIAGNOSTIC_DUPLICATE_RULE,
	SPD_DIAGNOSTIC_DUPLICATE_RULESET,
	SPD_DIAGNOSTIC_DUPLICATE_ICMP_TYPECODE
};

/*
 * Transmit a PF_POLICY error message to the instance either pointed to
 * by ks, the instance with serial number serial, or more, depending.
 *
 * The faulty message (or a reasonable facsimile thereof) is in mp.
 * This function will free mp or recycle it for delivery, thereby causing
 * the stream head to free it.
 */
static void
spdsock_error(queue_t *q, mblk_t *mp, int error, int diagnostic)
{
	spd_msg_t *spmsg = (spd_msg_t *)mp->b_rptr;

	ASSERT(mp->b_datap->db_type == M_DATA);

	if (spmsg->spd_msg_type < SPD_MIN ||
	    spmsg->spd_msg_type > SPD_MAX)
		spmsg->spd_msg_type = SPD_RESERVED;

	/*
	 * Strip out extension headers.
	 */
	ASSERT(mp->b_rptr + sizeof (*spmsg) <= mp->b_datap->db_lim);
	mp->b_wptr = mp->b_rptr + sizeof (*spmsg);
	spmsg->spd_msg_len = SPD_8TO64(sizeof (spd_msg_t));
	spmsg->spd_msg_errno = (uint8_t)error;
	spmsg->spd_msg_diagnostic = (uint16_t)diagnostic;

	qreply(q, mp);
}

static void
spdsock_diag(queue_t *q, mblk_t *mp, int diagnostic)
{
	spdsock_error(q, mp, EINVAL, diagnostic);
}

static void
spd_echo(queue_t *q, mblk_t *mp)
{
	qreply(q, mp);
}

/* ARGSUSED */
static void
spdsock_flush(queue_t *q, ipsec_policy_head_t *iph,
    mblk_t *mp, spd_ext_t **extv)
{
	rw_enter(&iph->iph_lock, RW_WRITER);
	ipsec_polhead_flush(iph);
	rw_exit(&iph->iph_lock);

	spd_echo(q, mp);
}

static boolean_t
spdsock_ext_to_sel(spd_ext_t **extv, ipsec_selkey_t *sel, int *diag)
{
	bzero(sel, sizeof (*sel));

	if (extv[SPD_EXT_PROTO] != NULL) {
		struct spd_proto *pr =
		    (struct spd_proto *)extv[SPD_EXT_PROTO];
		sel->ipsl_proto = pr->spd_proto_number;
		sel->ipsl_valid |= IPSL_PROTOCOL;
	}
	if (extv[SPD_EXT_LCLPORT] != NULL) {
		struct spd_portrange *pr =
		    (struct spd_portrange *)extv[SPD_EXT_LCLPORT];
		sel->ipsl_lport = pr->spd_ports_minport;
		sel->ipsl_valid |= IPSL_LOCAL_PORT;
	}
	if (extv[SPD_EXT_REMPORT] != NULL) {
		struct spd_portrange *pr =
		    (struct spd_portrange *)extv[SPD_EXT_REMPORT];
		sel->ipsl_rport = pr->spd_ports_minport;
		sel->ipsl_valid |= IPSL_REMOTE_PORT;
	}

	if (extv[SPD_EXT_ICMP_TYPECODE] != NULL) {
		struct spd_typecode *tc=
		    (struct spd_typecode *)extv[SPD_EXT_ICMP_TYPECODE];

		sel->ipsl_valid |= IPSL_ICMP_TYPE;
		sel->ipsl_icmp_type = tc->spd_typecode_type;
		if (tc->spd_typecode_type_end < tc->spd_typecode_type)
			sel->ipsl_icmp_type_end = tc->spd_typecode_type;
		else
			sel->ipsl_icmp_type_end = tc->spd_typecode_type_end;

		if (tc->spd_typecode_code != 255) {
			sel->ipsl_valid |= IPSL_ICMP_CODE;
			sel->ipsl_icmp_code = tc->spd_typecode_code;
			if (tc->spd_typecode_code_end < tc->spd_typecode_code)
				sel->ipsl_icmp_code_end = tc->spd_typecode_code;
			else
				sel->ipsl_icmp_code_end =
				    tc->spd_typecode_code_end;
		}
	}
#define	ADDR2SEL(sel, extv, field, pfield, extn, bit)			      \
	if ((extv)[(extn)] != NULL) {					      \
		uint_t addrlen;						      \
		struct spd_address *ap = 				      \
			(struct spd_address *)((extv)[(extn)]); 	      \
		addrlen = (ap->spd_address_af == AF_INET6) ? 		      \
			IPV6_ADDR_LEN : IP_ADDR_LEN;			      \
		if (SPD_64TO8(ap->spd_address_len) < 			      \
			(addrlen + sizeof (*ap))) {			      \
			*diag = SPD_DIAGNOSTIC_BAD_ADDR_LEN;		      \
			return (B_FALSE);				      \
		}							      \
		bcopy((ap+1), &((sel)->field), addrlen);		      \
		(sel)->pfield = ap->spd_address_prefixlen;		      \
		(sel)->ipsl_valid |= (bit);				      \
		(sel)->ipsl_valid |= (ap->spd_address_af == AF_INET6) ?	      \
			IPSL_IPV6 : IPSL_IPV4;				      \
	}

	ADDR2SEL(sel, extv, ipsl_local, ipsl_local_pfxlen,
	    SPD_EXT_LCLADDR, IPSL_LOCAL_ADDR);
	ADDR2SEL(sel, extv, ipsl_remote, ipsl_remote_pfxlen,
	    SPD_EXT_REMADDR, IPSL_REMOTE_ADDR);

	if ((sel->ipsl_valid & (IPSL_IPV6|IPSL_IPV4)) ==
	    (IPSL_IPV6|IPSL_IPV4)) {
		*diag = SPD_DIAGNOSTIC_MIXED_AF;
		return (B_FALSE);
	}

#undef ADDR2SEL

	return (B_TRUE);
}

static boolean_t
spd_convert_type(uint32_t type, ipsec_act_t *act)
{
	switch (type) {
	case SPD_ACTTYPE_DROP:
		act->ipa_type = IPSEC_ACT_DISCARD;
		return (B_TRUE);

	case SPD_ACTTYPE_PASS:
		act->ipa_type = IPSEC_ACT_CLEAR;
		return (B_TRUE);

	case SPD_ACTTYPE_IPSEC:
		act->ipa_type = IPSEC_ACT_APPLY;
		return (B_TRUE);
	}
	return (B_FALSE);
}

static boolean_t
spd_convert_flags(uint32_t flags, ipsec_act_t *act)
{
	/*
	 * Note use of !! for boolean canonicalization.
	 */
	act->ipa_apply.ipp_use_ah = !!(flags & SPD_APPLY_AH);
	act->ipa_apply.ipp_use_esp = !!(flags & SPD_APPLY_ESP);
	act->ipa_apply.ipp_use_espa = !!(flags & SPD_APPLY_ESPA);
	act->ipa_apply.ipp_use_se = !!(flags & SPD_APPLY_SE);
	act->ipa_apply.ipp_use_unique = !!(flags & SPD_APPLY_UNIQUE);
	return (B_TRUE);
}

static void
spdsock_reset_act(ipsec_act_t *act)
{
	bzero(act, sizeof (*act));
	act->ipa_apply.ipp_espe_maxbits = IPSEC_MAX_KEYBITS;
	act->ipa_apply.ipp_espa_maxbits = IPSEC_MAX_KEYBITS;
	act->ipa_apply.ipp_ah_maxbits = IPSEC_MAX_KEYBITS;
}

/*
 * Sanity check action against reality, and shrink-wrap key sizes..
 */
static boolean_t
spdsock_check_action(ipsec_act_t *act, int *diag)
{
	if ((act->ipa_type != IPSEC_ACT_APPLY) &&
	    (act->ipa_apply.ipp_use_ah ||
		act->ipa_apply.ipp_use_esp ||
		act->ipa_apply.ipp_use_espa ||
		act->ipa_apply.ipp_use_se ||
		act->ipa_apply.ipp_use_unique)) {
		*diag = SPD_DIAGNOSTIC_ADD_INCON_FLAGS;
		return (B_FALSE);
	}
	if ((act->ipa_type == IPSEC_ACT_APPLY) &&
	    !act->ipa_apply.ipp_use_ah &&
	    !act->ipa_apply.ipp_use_esp) {
		*diag = SPD_DIAGNOSTIC_ADD_INCON_FLAGS;
		return (B_FALSE);
	}
	return (ipsec_check_action(act, diag));
}

/*
 * We may be short a few error checks here..
 */
static boolean_t
spdsock_ext_to_actvec(spd_ext_t **extv, ipsec_act_t **actpp, uint_t *nactp,
    int *diag)
{
	struct spd_ext_actions *sactp =
	    (struct spd_ext_actions *)extv[SPD_EXT_ACTION];
	ipsec_act_t act, *actp, *endactp;
	struct spd_attribute *attrp, *endattrp;
	uint64_t *endp;
	int nact;

	*actpp = NULL;
	*nactp = 0;

	if (sactp == NULL) {
		*diag = SPD_DIAGNOSTIC_NO_ACTION_EXT;
		return (B_FALSE);
	}

	/*
	 * Parse the "action" extension and convert into an action chain.
	 */

	nact = sactp->spd_actions_count;

	endp = (uint64_t *)sactp;
	endp += sactp->spd_actions_len;
	endattrp = (struct spd_attribute *)endp;

	actp = kmem_alloc(sizeof (*actp) * nact, KM_NOSLEEP);
	if (actp == NULL) {
		*diag = SPD_DIAGNOSTIC_ADD_NO_MEM;
		return (B_FALSE);
	}
	*actpp = actp;
	*nactp = nact;
	endactp = actp + nact;

	spdsock_reset_act(&act);
	attrp = (struct spd_attribute *)(&sactp[1]);

	for (; attrp < endattrp; attrp++) {
		switch (attrp->spd_attr_tag) {
		case SPD_ATTR_NOP:
			break;

		case SPD_ATTR_EMPTY:
			spdsock_reset_act(&act);
			break;

		case SPD_ATTR_END:
			attrp = endattrp;
			/* FALLTHRU */
		case SPD_ATTR_NEXT:
			if (actp >= endactp) {
				*diag = SPD_DIAGNOSTIC_ADD_WRONG_ACT_COUNT;
				goto fail;
			}
			if (!spdsock_check_action(&act, diag))
				goto fail;
			*actp++ = act;
			break;

		case SPD_ATTR_TYPE:
			if (!spd_convert_type(attrp->spd_attr_value, &act)) {
				*diag = SPD_DIAGNOSTIC_ADD_BAD_TYPE;
				goto fail;
			}
			break;

		case SPD_ATTR_FLAGS:
			if (!spd_convert_flags(attrp->spd_attr_value, &act)) {
				*diag = SPD_DIAGNOSTIC_ADD_BAD_FLAGS;
				goto fail;
			}
			break;

		case SPD_ATTR_AH_AUTH:
			act.ipa_apply.ipp_auth_alg = attrp->spd_attr_value;
			break;

		case SPD_ATTR_ESP_ENCR:
			act.ipa_apply.ipp_encr_alg = attrp->spd_attr_value;
			break;

		case SPD_ATTR_ESP_AUTH:
			act.ipa_apply.ipp_esp_auth_alg = attrp->spd_attr_value;
			break;

		case SPD_ATTR_ENCR_MINBITS:
			act.ipa_apply.ipp_espe_minbits = attrp->spd_attr_value;
			break;

		case SPD_ATTR_ENCR_MAXBITS:
			act.ipa_apply.ipp_espe_maxbits = attrp->spd_attr_value;
			break;

		case SPD_ATTR_AH_MINBITS:
			act.ipa_apply.ipp_ah_minbits = attrp->spd_attr_value;
			break;

		case SPD_ATTR_AH_MAXBITS:
			act.ipa_apply.ipp_ah_maxbits = attrp->spd_attr_value;
			break;

		case SPD_ATTR_ESPA_MINBITS:
			act.ipa_apply.ipp_espa_minbits = attrp->spd_attr_value;
			break;

		case SPD_ATTR_ESPA_MAXBITS:
			act.ipa_apply.ipp_espa_maxbits = attrp->spd_attr_value;
			break;

		case SPD_ATTR_LIFE_SOFT_TIME:
		case SPD_ATTR_LIFE_HARD_TIME:
		case SPD_ATTR_LIFE_SOFT_BYTES:
		case SPD_ATTR_LIFE_HARD_BYTES:
			break;

		case SPD_ATTR_KM_PROTO:
			act.ipa_apply.ipp_km_proto = attrp->spd_attr_value;
			break;

		case SPD_ATTR_KM_COOKIE:
			act.ipa_apply.ipp_km_cookie = attrp->spd_attr_value;
			break;

		case SPD_ATTR_REPLAY_DEPTH:
			act.ipa_apply.ipp_replay_depth = attrp->spd_attr_value;
			break;
		}
	}
	if (actp != endactp) {
		*diag = SPD_DIAGNOSTIC_ADD_WRONG_ACT_COUNT;
		goto fail;
	}

	return (B_TRUE);
fail:
	ipsec_actvec_free(*actpp, nact);
	*actpp = NULL;
	return (B_FALSE);
}

typedef struct
{
	ipsec_policy_t *pol;
	int dir;
} tmprule_t;

static int
mkrule(ipsec_policy_head_t *iph, struct spd_rule *rule,
    ipsec_selkey_t *sel, ipsec_act_t *actp, int nact, uint_t dir, uint_t af,
    tmprule_t **rp)
{
	ipsec_policy_t *pol;

	sel->ipsl_valid &= ~(IPSL_IPV6|IPSL_IPV4);
	sel->ipsl_valid |= af;

	pol = ipsec_policy_create(sel, actp, nact, rule->spd_rule_priority);
	if (pol == NULL)
		return (ENOMEM);

	(*rp)->pol = pol;
	(*rp)->dir = dir;
	(*rp)++;

	if (!ipsec_check_policy(iph, pol, dir))
		return (EEXIST);

	rule->spd_rule_index = pol->ipsp_index;
	return (0);
}

static int
mkrulepair(ipsec_policy_head_t *iph, struct spd_rule *rule,
    ipsec_selkey_t *sel, ipsec_act_t *actp, int nact, uint_t dir, uint_t afs,
    tmprule_t **rp)
{
	int error;

	if (afs & IPSL_IPV4) {
		error = mkrule(iph, rule, sel, actp, nact, dir, IPSL_IPV4, rp);
		if (error != 0)
			return (error);
	}
	if (afs & IPSL_IPV6) {
		error = mkrule(iph, rule, sel, actp, nact, dir, IPSL_IPV6, rp);
		if (error != 0)
			return (error);
	}
	return (0);
}


static void
spdsock_addrule(queue_t *q, ipsec_policy_head_t *iph,
    mblk_t *mp, spd_ext_t **extv)
{
	ipsec_selkey_t sel;
	ipsec_act_t *actp;
	uint_t nact;
	int diag, error, afs;
	struct spd_rule *rule = (struct spd_rule *)extv[SPD_EXT_RULE];
	tmprule_t rules[4], *rulep = &rules[0];

	if (rule == NULL) {
		spdsock_diag(q, mp, SPD_DIAGNOSTIC_NO_RULE_EXT);
		return;
	}

	if (rule->spd_rule_index != 0) {
		spdsock_diag(q, mp, SPD_DIAGNOSTIC_INVALID_RULE_INDEX);
		return;
	}

	if (!spdsock_ext_to_sel(extv, &sel, &diag)) {
		spdsock_diag(q, mp, diag);
		return;
	}

	if (!spdsock_ext_to_actvec(extv, &actp, &nact, &diag)) {
		spdsock_diag(q, mp, diag);
		return;
	}
	/*
	 * If no addresses were specified, add both.
	 */
	afs = sel.ipsl_valid & (IPSL_IPV6|IPSL_IPV4);
	if (afs == 0)
		afs = (IPSL_IPV6|IPSL_IPV4);

	rw_enter(&iph->iph_lock, RW_WRITER);

	if (rule->spd_rule_flags & SPD_RULE_FLAG_OUTBOUND) {
		error = mkrulepair(iph, rule, &sel, actp, nact,
		    IPSEC_TYPE_OUTBOUND, afs, &rulep);
		if (error != 0)
			goto fail;
	}

	if (rule->spd_rule_flags & SPD_RULE_FLAG_INBOUND) {
		error = mkrulepair(iph, rule, &sel, actp, nact,
		    IPSEC_TYPE_INBOUND, afs, &rulep);
		if (error != 0)
			goto fail;
	}

	while ((--rulep) >= &rules[0])
		ipsec_enter_policy(iph, rulep->pol, rulep->dir);

	rw_exit(&iph->iph_lock);

	ipsec_actvec_free(actp, nact);
	spd_echo(q, mp);
	return;

fail:
	rw_exit(&iph->iph_lock);
	while ((--rulep) >= &rules[0]) {
		IPPOL_REFRELE(rulep->pol);
	}
	ipsec_actvec_free(actp, nact);
	spdsock_error(q, mp, error, 0);
}

void
spdsock_deleterule(queue_t *q, ipsec_policy_head_t *iph,
    mblk_t *mp, spd_ext_t **extv)
{
	ipsec_selkey_t sel;
	struct spd_rule *rule = (struct spd_rule *)extv[SPD_EXT_RULE];
	int diag;

	if (rule == NULL) {
		spdsock_diag(q, mp, SPD_DIAGNOSTIC_NO_RULE_EXT);
		return;
	}

	if (rule->spd_rule_index != 0) {
		if (ipsec_policy_delete_index(iph, rule->spd_rule_index) != 0) {
			spdsock_error(q, mp, ESRCH, 0);
			return;
		}
	} else {
		if (!spdsock_ext_to_sel(extv, &sel, &diag)) {
			spdsock_diag(q, mp, diag);
			return;
		}

		if (rule->spd_rule_flags & SPD_RULE_FLAG_INBOUND) {
			if (!ipsec_policy_delete(iph, &sel,
			    IPSEC_TYPE_INBOUND))
				goto fail;
		}

		if (rule->spd_rule_flags & SPD_RULE_FLAG_OUTBOUND) {
			if (!ipsec_policy_delete(iph, &sel,
			    IPSEC_TYPE_OUTBOUND))
				goto fail;
		}
	}
	spd_echo(q, mp);
	return;
fail:
	spdsock_error(q, mp, ESRCH, 0);
}

void
spdsock_flip(queue_t *q, mblk_t *mp)
{
	ipsec_swap_policy();	/* can't fail */
	spd_echo(q, mp);
}

/*
 * Unimplemented feature
 */
/* ARGSUSED */
static void
spdsock_lookup(queue_t *q, ipsec_policy_head_t *iph,
    mblk_t *mp, spd_ext_t **extv)
{
	spdsock_error(q, mp, EINVAL, 0);
}


static mblk_t *
spdsock_dump_ruleset(mblk_t *req, ipsec_policy_head_t *iph,
    uint32_t count, uint16_t error)
{
	size_t len = sizeof (spd_ruleset_ext_t) + sizeof (spd_msg_t);
	spd_msg_t *msg;
	spd_ruleset_ext_t *ruleset;
	mblk_t *m = allocb(len, BPRI_HI);

	ASSERT(RW_READ_HELD(&iph->iph_lock));

	if (m == NULL) {
		return (NULL);
	}
	msg = (spd_msg_t *)m->b_rptr;
	ruleset = (spd_ruleset_ext_t *)(&msg[1]);

	m->b_wptr = (uint8_t *)&ruleset[1];

	*msg = *(spd_msg_t *)(req->b_rptr);
	msg->spd_msg_len = SPD_8TO64(len);
	msg->spd_msg_errno = error;

	ruleset->spd_ruleset_len = SPD_8TO64(sizeof (*ruleset));
	ruleset->spd_ruleset_type = SPD_EXT_RULESET;
	ruleset->spd_ruleset_count = count;
	ruleset->spd_ruleset_version = iph->iph_gen;
	return (m);
}

static mblk_t *
spdsock_dump_finish(spdsock_t *ss, int error)
{
	mblk_t *m;
	ipsec_policy_head_t *iph = ss->spdsock_dump_head;
	mblk_t *req = ss->spdsock_dump_req;

	rw_enter(&iph->iph_lock, RW_READER);
	m = spdsock_dump_ruleset(req, iph, ss->spdsock_dump_count, error);
	rw_exit(&iph->iph_lock);

	ss->spdsock_dump_req = NULL;
	freemsg(req);

	return (m);
}

/*
 * Rule encoding functions.
 * We do a two-pass encode.
 * If base != NULL, fill in encoded rule part starting at base+offset.
 * Always return "offset" plus length of to-be-encoded data.
 */
static uint_t
spdsock_encode_typecode(uint8_t *base, uint_t offset, uint8_t type,
    uint8_t type_end, uint8_t code, uint8_t code_end)
{
	struct spd_typecode *tcp;

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		tcp = (struct spd_typecode *)(base + offset);
		tcp->spd_typecode_len = SPD_8TO64(sizeof (*tcp));
		tcp->spd_typecode_exttype = SPD_EXT_ICMP_TYPECODE;
		tcp->spd_typecode_code = code;
		tcp->spd_typecode_type = type;
		tcp->spd_typecode_type_end = type_end;
		tcp->spd_typecode_code_end = code_end;
	}
	offset += sizeof (*tcp);

	ASSERT(ALIGNED64(offset));

	return (offset);
}

static uint_t
spdsock_encode_proto(uint8_t *base, uint_t offset, uint8_t proto)
{
	struct spd_proto *spp;

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		spp = (struct spd_proto *)(base + offset);
		spp->spd_proto_len = SPD_8TO64(sizeof (*spp));
		spp->spd_proto_exttype = SPD_EXT_PROTO;
		spp->spd_proto_number = proto;
		spp->spd_proto_reserved1 = 0;
		spp->spd_proto_reserved2 = 0;
	}
	offset += sizeof (*spp);

	ASSERT(ALIGNED64(offset));

	return (offset);
}

static uint_t
spdsock_encode_port(uint8_t *base, uint_t offset, uint16_t ext, uint16_t port)
{
	struct spd_portrange *spp;

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		spp = (struct spd_portrange *)(base + offset);
		spp->spd_ports_len = SPD_8TO64(sizeof (*spp));
		spp->spd_ports_exttype = ext;
		spp->spd_ports_minport = port;
		spp->spd_ports_maxport = port;
	}
	offset += sizeof (*spp);

	ASSERT(ALIGNED64(offset));

	return (offset);
}

static uint_t
spdsock_encode_addr(uint8_t *base, uint_t offset, uint16_t ext,
    const ipsec_selkey_t *sel, const ipsec_addr_t *addr, uint_t pfxlen)
{
	struct spd_address *sae;
	ipsec_addr_t *spdaddr;
	uint_t start = offset;
	uint_t addrlen;
	uint_t af;

	if (sel->ipsl_valid & IPSL_IPV4) {
		af = AF_INET;
		addrlen = IP_ADDR_LEN;
	} else {
		af = AF_INET6;
		addrlen = IPV6_ADDR_LEN;
	}

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		sae = (struct spd_address *)(base + offset);
		sae->spd_address_exttype = ext;
		sae->spd_address_af = af;
		sae->spd_address_prefixlen = pfxlen;
		sae->spd_address_reserved2 = 0;

		spdaddr = (ipsec_addr_t *)(&sae[1]);
		bcopy(addr, spdaddr, addrlen);
	}
	offset += sizeof (*sae);
	addrlen = roundup(addrlen, sizeof (uint64_t));
	offset += addrlen;

	ASSERT(ALIGNED64(offset));

	if (base != NULL)
		sae->spd_address_len = SPD_8TO64(offset - start);
	return (offset);
}

static uint_t
spdsock_encode_sel(uint8_t *base, uint_t offset, const ipsec_sel_t *sel)
{
	const ipsec_selkey_t *selkey = &sel->ipsl_key;

	if (selkey->ipsl_valid & IPSL_PROTOCOL)
		offset = spdsock_encode_proto(base, offset, selkey->ipsl_proto);
	if (selkey->ipsl_valid & IPSL_LOCAL_PORT)
		offset = spdsock_encode_port(base, offset, SPD_EXT_LCLPORT,
		    selkey->ipsl_lport);
	if (selkey->ipsl_valid & IPSL_REMOTE_PORT)
		offset = spdsock_encode_port(base, offset, SPD_EXT_REMPORT,
		    selkey->ipsl_rport);
	if (selkey->ipsl_valid & IPSL_REMOTE_ADDR)
		offset = spdsock_encode_addr(base, offset, SPD_EXT_REMADDR,
		    selkey, &selkey->ipsl_remote, selkey->ipsl_remote_pfxlen);
	if (selkey->ipsl_valid & IPSL_LOCAL_ADDR)
		offset = spdsock_encode_addr(base, offset, SPD_EXT_LCLADDR,
		    selkey, &selkey->ipsl_local, selkey->ipsl_local_pfxlen);
	if (selkey->ipsl_valid & IPSL_ICMP_TYPE) {
		offset = spdsock_encode_typecode(base, offset,
		    selkey->ipsl_icmp_type, selkey->ipsl_icmp_type_end,
		    (selkey->ipsl_valid & IPSL_ICMP_CODE) ?
			selkey->ipsl_icmp_code : 255,
		    (selkey->ipsl_valid & IPSL_ICMP_CODE) ?
			selkey->ipsl_icmp_code_end : 255);
	}
	return (offset);
}

static uint_t
spdsock_encode_actattr(uint8_t *base, uint_t offset, uint32_t tag,
    uint32_t value)
{
	struct spd_attribute *attr;

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		attr = (struct spd_attribute *)(base + offset);
		attr->spd_attr_tag = tag;
		attr->spd_attr_value = value;
	}
	offset += sizeof (struct spd_attribute);

	ASSERT(ALIGNED64(offset));

	return (offset);
}


#define	EMIT(t, v) offset = spdsock_encode_actattr(base, offset, (t), (v))

static uint_t
spdsock_encode_action(uint8_t *base, uint_t offset, const ipsec_action_t *ap)
{
	const struct ipsec_act *act = &(ap->ipa_act);
	uint_t flags;

	EMIT(SPD_ATTR_EMPTY, 0);
	switch (act->ipa_type) {
	case IPSEC_ACT_DISCARD:
	case IPSEC_ACT_REJECT:
		EMIT(SPD_ATTR_TYPE, SPD_ACTTYPE_DROP);
		break;
	case IPSEC_ACT_BYPASS:
	case IPSEC_ACT_CLEAR:
		EMIT(SPD_ATTR_TYPE, SPD_ACTTYPE_PASS);
		break;

	case IPSEC_ACT_APPLY:
		EMIT(SPD_ATTR_TYPE, SPD_ACTTYPE_IPSEC);
		flags = 0;
		if (act->ipa_apply.ipp_use_ah)
			flags |= SPD_APPLY_AH;
		if (act->ipa_apply.ipp_use_esp)
			flags |= SPD_APPLY_ESP;
		if (act->ipa_apply.ipp_use_espa)
			flags |= SPD_APPLY_ESPA;
		if (act->ipa_apply.ipp_use_se)
			flags |= SPD_APPLY_SE;
		if (act->ipa_apply.ipp_use_unique)
			flags |= SPD_APPLY_UNIQUE;
		EMIT(SPD_ATTR_FLAGS, flags);
		if (flags & SPD_APPLY_AH) {
			EMIT(SPD_ATTR_AH_AUTH, act->ipa_apply.ipp_auth_alg);
			EMIT(SPD_ATTR_AH_MINBITS,
			    act->ipa_apply.ipp_ah_minbits);
			EMIT(SPD_ATTR_AH_MAXBITS,
			    act->ipa_apply.ipp_ah_maxbits);
		}
		if (flags & SPD_APPLY_ESP) {
			EMIT(SPD_ATTR_ESP_ENCR, act->ipa_apply.ipp_encr_alg);
			EMIT(SPD_ATTR_ENCR_MINBITS,
			    act->ipa_apply.ipp_espe_minbits);
			EMIT(SPD_ATTR_ENCR_MAXBITS,
			    act->ipa_apply.ipp_espe_maxbits);
			if (flags & SPD_APPLY_ESPA) {
				EMIT(SPD_ATTR_ESP_AUTH,
				    act->ipa_apply.ipp_esp_auth_alg);
				EMIT(SPD_ATTR_ESPA_MINBITS,
				    act->ipa_apply.ipp_espa_minbits);
				EMIT(SPD_ATTR_ESPA_MAXBITS,
				    act->ipa_apply.ipp_espa_maxbits);
			}
		}
		if (act->ipa_apply.ipp_km_proto != 0)
			EMIT(SPD_ATTR_KM_PROTO, act->ipa_apply.ipp_km_proto);
		if (act->ipa_apply.ipp_km_cookie != 0)
			EMIT(SPD_ATTR_KM_PROTO, act->ipa_apply.ipp_km_cookie);
		if (act->ipa_apply.ipp_replay_depth != 0)
			EMIT(SPD_ATTR_REPLAY_DEPTH,
			    act->ipa_apply.ipp_replay_depth);
		/* Add more here */
		break;
	}

	return (offset);
}

static uint_t
spdsock_encode_action_list(uint8_t *base, uint_t offset,
    const ipsec_action_t *ap)
{
	struct spd_ext_actions *act;
	uint_t nact = 0;
	uint_t start = offset;

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		act = (struct spd_ext_actions *)(base + offset);
		act->spd_actions_len = 0;
		act->spd_actions_exttype = SPD_EXT_ACTION;
		act->spd_actions_count = 0;
		act->spd_actions_reserved = 0;
	}

	offset += sizeof (*act);

	ASSERT(ALIGNED64(offset));

	while (ap != NULL) {
		offset = spdsock_encode_action(base, offset, ap);
		ap = ap->ipa_next;
		nact++;
		if (ap != NULL) {
			EMIT(SPD_ATTR_NEXT, 0);
		}
	}
	EMIT(SPD_ATTR_END, 0);

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		act->spd_actions_count = nact;
		act->spd_actions_len = SPD_8TO64(offset - start);
	}

	return (offset);
}

#undef EMIT

/* ARGSUSED */
static uint_t
spdsock_rule_flags(uint_t dir, uint_t af)
{
	uint_t flags = 0;

	if (dir == IPSEC_TYPE_INBOUND)
		flags |= SPD_RULE_FLAG_INBOUND;
	if (dir == IPSEC_TYPE_OUTBOUND)
		flags |= SPD_RULE_FLAG_OUTBOUND;

	return (flags);
}


static uint_t
spdsock_encode_rule_head(uint8_t *base, uint_t offset,
    spd_msg_t *req, const ipsec_policy_t *rule, uint_t dir, uint_t af)
{
	struct spd_msg *spmsg;
	struct spd_rule *spr;

	uint_t start = offset;

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		spmsg = (struct spd_msg *)(base + offset);
		bzero(spmsg, sizeof (*spmsg));
		spmsg->spd_msg_version = PF_POLICY_V1;
		spmsg->spd_msg_type = SPD_DUMP;
		spmsg->spd_msg_seq = req->spd_msg_seq;
		spmsg->spd_msg_pid = req->spd_msg_pid;
	}
	offset += sizeof (struct spd_msg);

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		spr = (struct spd_rule *)(base + offset);
		spr->spd_rule_type = SPD_EXT_RULE;
		spr->spd_rule_priority = rule->ipsp_prio;
		spr->spd_rule_flags = spdsock_rule_flags(dir, af);
		spr->spd_rule_unused = 0;
		spr->spd_rule_len = SPD_8TO64(sizeof (*spr));
		spr->spd_rule_index = rule->ipsp_index;
	}
	offset += sizeof (struct spd_rule);
	offset = spdsock_encode_sel(base, offset, rule->ipsp_sel);
	offset = spdsock_encode_action_list(base, offset, rule->ipsp_act);

	ASSERT(ALIGNED64(offset));

	if (base != NULL) {
		spmsg->spd_msg_len = SPD_8TO64(offset - start);
	}
	return (offset);
}

/* ARGSUSED */
static mblk_t *
spdsock_encode_rule(mblk_t *req, const ipsec_policy_t *rule,
    uint_t dir, uint_t af)
{
	mblk_t *m;
	uint_t len;
	spd_msg_t *mreq = (spd_msg_t *)req->b_rptr;

	/*
	 * Figure out how much space we'll need.
	 */
	len = spdsock_encode_rule_head(NULL, 0, mreq, rule, dir, af);

	/*
	 * Allocate mblk.
	 */
	m = allocb(len, BPRI_HI);
	if (m == NULL)
		return (NULL);

	/*
	 * Fill it in..
	 */
	m->b_wptr = m->b_rptr + len;
	bzero(m->b_rptr, len);
	(void) spdsock_encode_rule_head(m->b_rptr, 0, mreq, rule, dir, af);
	return (m);
}

static ipsec_policy_t *
spdsock_dump_next_in_chain(spdsock_t *ss, ipsec_policy_head_t *iph,
    ipsec_policy_t *cur)
{
	ASSERT(RW_READ_HELD(&iph->iph_lock));

	ss->spdsock_dump_count++;
	ss->spdsock_dump_cur_rule = cur->ipsp_hash.hash_next;
	return (cur);
}

static ipsec_policy_t *
spdsock_dump_next_rule(spdsock_t *ss, ipsec_policy_head_t *iph)
{
	ipsec_policy_t *cur;
	ipsec_policy_root_t *ipr;
	int chain, nchains, type, af;

	ASSERT(RW_READ_HELD(&iph->iph_lock));

	cur = ss->spdsock_dump_cur_rule;

	if (cur != NULL)
		return (spdsock_dump_next_in_chain(ss, iph, cur));

	type = ss->spdsock_dump_cur_type;

next:
	chain = ss->spdsock_dump_cur_chain;
	ipr = &iph->iph_root[type];
	nchains = ipr->ipr_nchains;

	while (chain < nchains) {
		cur = ipr->ipr_hash[chain].hash_head;
		chain++;
		if (cur != NULL) {
			ss->spdsock_dump_cur_chain = chain;
			return (spdsock_dump_next_in_chain(ss, iph, cur));
		}
	}
	ss->spdsock_dump_cur_chain = nchains;

	af = ss->spdsock_dump_cur_af;
	while (af < IPSEC_NAF) {
		cur = ipr->ipr_nonhash[af];
		af++;
		if (cur != NULL) {
			ss->spdsock_dump_cur_af = af;
			return (spdsock_dump_next_in_chain(ss, iph, cur));
		}
	}

	type++;
	if (type >= IPSEC_NTYPES)
		return (NULL);

	ss->spdsock_dump_cur_chain = 0;
	ss->spdsock_dump_cur_type = type;
	ss->spdsock_dump_cur_af = IPSEC_AF_V4;
	goto next;

}

static mblk_t *
spdsock_dump_next_record(spdsock_t *ss)
{
	ipsec_policy_head_t *iph;
	ipsec_policy_t *rule;
	mblk_t *m;
	mblk_t *req = ss->spdsock_dump_req;

	iph = ss->spdsock_dump_head;

	ASSERT(iph != NULL);

	rw_enter(&iph->iph_lock, RW_READER);

	if (iph->iph_gen != ss->spdsock_dump_gen) {
		rw_exit(&iph->iph_lock);
		return (spdsock_dump_finish(ss, EAGAIN));
	}

	rule = spdsock_dump_next_rule(ss, iph);

	if (!rule) {
		rw_exit(&iph->iph_lock);
		return (spdsock_dump_finish(ss, 0));
	}

	m = spdsock_encode_rule(req, rule, ss->spdsock_dump_cur_type,
	    ss->spdsock_dump_cur_af);
	rw_exit(&iph->iph_lock);

	if (m == NULL)
		return (spdsock_dump_finish(ss, ENOMEM));
	return (m);
}

/*
 * Dump records until we run into flow-control back-pressure.
 */
static void
spdsock_dump_some(queue_t *q, spdsock_t *ss)
{
	mblk_t *m, *dataind;

	while ((ss->spdsock_dump_req != NULL) && canputnext(q)) {
		m = spdsock_dump_next_record(ss);
		if (m == NULL)
			return;
		dataind = allocb(sizeof (struct T_data_req), BPRI_HI);
		if (dataind == NULL) {
			freemsg(m);
			return;
		}
		dataind->b_cont = m;
		dataind->b_wptr += sizeof (struct T_data_req);
		((struct T_data_ind *)dataind->b_rptr)->PRIM_type = T_DATA_IND;
		((struct T_data_ind *)dataind->b_rptr)->MORE_flag = 0;
		dataind->b_datap->db_type = M_PROTO;
		putnext(q, dataind);
	}
}

/*
 * Start dumping.
 * Format a start-of-dump record, and set up the stream and kick the rsrv
 * procedure to continue the job..
 */
/* ARGSUSED */
static void
spdsock_dump(queue_t *q, ipsec_policy_head_t *iph,
    mblk_t *mp, spd_ext_t **extv)
{
	spdsock_t *ss = (spdsock_t *)q->q_ptr;
	mblk_t *mr;

	rw_enter(&iph->iph_lock, RW_READER);

	mr = spdsock_dump_ruleset(mp, iph, 0, 0);

	if (!mr) {
		rw_exit(&iph->iph_lock);
		spdsock_error(q, mp, ENOMEM, 0);
		return;
	}

	ss->spdsock_dump_req = mp;
	ss->spdsock_dump_head = iph;
	ss->spdsock_dump_gen = iph->iph_gen;
	ss->spdsock_dump_cur_type = 0;
	ss->spdsock_dump_cur_af = IPSEC_AF_V4;
	ss->spdsock_dump_cur_rule = NULL;
	ss->spdsock_dump_count = 0;
	ss->spdsock_dump_cur_chain = 0;
	rw_exit(&iph->iph_lock);

	qreply(q, mr);
	qenable(OTHERQ(q));
}

void
spdsock_clone(queue_t *q, mblk_t *mp)
{
	int error = ipsec_clone_system_policy();
	if (error != 0)
		spdsock_error(q, mp, error, 0);
	else
		spd_echo(q, mp);
}

/*
 * Process a SPD_ALGLIST request. The caller expects separate alg entries
 * for AH authentication, ESP authentication, and ESP encryption.
 * The same distinction is then used when setting the min and max key
 * sizes when defining policies.
 */

#define	SPDSOCK_AH_AUTH		0
#define	SPDSOCK_ESP_AUTH	1
#define	SPDSOCK_ESP_ENCR	2
#define	SPDSOCK_NTYPES		3

static const uint_t algattr[SPDSOCK_NTYPES] = {
	SPD_ATTR_AH_AUTH,
	SPD_ATTR_ESP_AUTH,
	SPD_ATTR_ESP_ENCR
};
static const uint_t minbitsattr[SPDSOCK_NTYPES] = {
	SPD_ATTR_AH_MINBITS,
	SPD_ATTR_ESPA_MINBITS,
	SPD_ATTR_ENCR_MINBITS
};
static const uint_t maxbitsattr[SPDSOCK_NTYPES] = {
	SPD_ATTR_AH_MAXBITS,
	SPD_ATTR_ESPA_MAXBITS,
	SPD_ATTR_ENCR_MAXBITS
};
static const uint_t defbitsattr[SPDSOCK_NTYPES] = {
	SPD_ATTR_AH_DEFBITS,
	SPD_ATTR_ESPA_DEFBITS,
	SPD_ATTR_ENCR_DEFBITS
};
static const uint_t incrbitsattr[SPDSOCK_NTYPES] = {
	SPD_ATTR_AH_INCRBITS,
	SPD_ATTR_ESPA_INCRBITS,
	SPD_ATTR_ENCR_INCRBITS
};

#define	ATTRPERALG	6	/* fixed attributes per algs */

void
spdsock_alglist(queue_t *q, mblk_t *mp)
{
	uint_t algtype;
	uint_t algidx;
	uint_t algcount;
	uint_t size;
	mblk_t *m;
	uint8_t *cur;
	spd_msg_t *msg;
	struct spd_ext_actions *act;
	struct spd_attribute *attr;

	mutex_enter(&alg_lock);

	/*
	 * The SPD client expects to receive separate entries for
	 * AH authentication and ESP authentication supported algorithms.
	 *
	 * Don't return the "any" algorithms, if defined, as no
	 * kernel policies can be set for these algorithms.
	 */
	algcount = 2 * ipsec_nalgs[IPSEC_ALG_AUTH] +
	    ipsec_nalgs[IPSEC_ALG_ENCR];

	if (ipsec_alglists[IPSEC_ALG_AUTH][SADB_AALG_NONE] != NULL)
		algcount--;
	if (ipsec_alglists[IPSEC_ALG_ENCR][SADB_EALG_NONE] != NULL)
		algcount--;

	/*
	 * For each algorithm, we encode:
	 * ALG / MINBITS / MAXBITS / DEFBITS / INCRBITS / {END, NEXT}
	 */

	size = sizeof (spd_msg_t) + sizeof (struct spd_ext_actions) +
	    ATTRPERALG * sizeof (struct spd_attribute) * algcount;

	ASSERT(ALIGNED64(size));

	m = allocb(size, BPRI_HI);
	if (m == NULL) {
		mutex_exit(&alg_lock);
		spdsock_error(q, mp, ENOMEM, 0);
		return;
	}

	m->b_wptr = m->b_rptr + size;
	cur = m->b_rptr;

	msg = (spd_msg_t *)cur;
	bcopy(mp->b_rptr, cur, sizeof (*msg));

	msg->spd_msg_len = SPD_8TO64(size);
	msg->spd_msg_errno = 0;
	msg->spd_msg_diagnostic = 0;

	cur += sizeof (*msg);

	act = (struct spd_ext_actions *)cur;
	cur += sizeof (*act);

	act->spd_actions_len = SPD_8TO64(size - sizeof (spd_msg_t));
	act->spd_actions_exttype = SPD_EXT_ACTION;
	act->spd_actions_count = algcount;
	act->spd_actions_reserved = 0;

	attr = (struct spd_attribute *)cur;

#define	EMIT(tag, value) {					\
		attr->spd_attr_tag = (tag); 			\
		attr->spd_attr_value = (value); 		\
		attr++;			  			\
	}

	/*
	 * If you change the number of EMIT's here, change
	 * ATTRPERALG above to match
	 */
#define	EMITALGATTRS(_type) {					\
		EMIT(algattr[_type], algid); 		/* 1 */	\
		EMIT(minbitsattr[_type], minbits);	/* 2 */	\
		EMIT(maxbitsattr[_type], maxbits);	/* 3 */	\
		EMIT(defbitsattr[_type], defbits);	/* 4 */	\
		EMIT(incrbitsattr[_type], incr);	/* 5 */	\
		EMIT(SPD_ATTR_NEXT, 0);			/* 6 */	\
	}

	for (algtype = 0; algtype < IPSEC_NALGTYPES; algtype++) {
		for (algidx = 0; algidx < ipsec_nalgs[algtype]; algidx++) {
			int algid = ipsec_sortlist[algtype][algidx];
			ipsec_alginfo_t *alg = ipsec_alglists[algtype][algid];
			uint_t minbits = alg->alg_minbits;
			uint_t maxbits = alg->alg_maxbits;
			uint_t defbits = alg->alg_default_bits;
			uint_t incr = alg->alg_increment;

			if (algtype == IPSEC_ALG_AUTH) {
				if (algid == SADB_AALG_NONE)
					continue;
				EMITALGATTRS(SPDSOCK_AH_AUTH);
				EMITALGATTRS(SPDSOCK_ESP_AUTH);
			} else {
				if (algid == SADB_EALG_NONE)
					continue;
				ASSERT(algtype == IPSEC_ALG_ENCR);
				EMITALGATTRS(SPDSOCK_ESP_ENCR);
			}
		}
	}

	mutex_exit(&alg_lock);

#undef EMITALGATTRS
#undef EMIT
#undef ATTRPERALG

	attr--;
	attr->spd_attr_tag = SPD_ATTR_END;

	freemsg(mp);
	qreply(q, m);
}

/*
 * Process a SPD_DUMPALGS request.
 */

#define	ATTRPERALG	7	/* fixed attributes per algs */

void
spdsock_dumpalgs(queue_t *q, mblk_t *mp)
{
	uint_t algtype;
	uint_t algidx;
	uint_t size;
	mblk_t *m;
	uint8_t *cur;
	spd_msg_t *msg;
	struct spd_ext_actions *act;
	struct spd_attribute *attr;
	ipsec_alginfo_t *alg;
	uint_t algid;
	uint_t i;
	uint_t alg_size;

	mutex_enter(&alg_lock);

	/*
	 * For each algorithm, we encode:
	 * ALG / MINBITS / MAXBITS / DEFBITS / INCRBITS / {END, NEXT}
	 *
	 * ALG_ID / ALG_PROTO / ALG_INCRBITS / ALG_NKEYSIZES / ALG_KEYSIZE*
	 * ALG_NBLOCKSIZES / ALG_BLOCKSIZE* / ALG_MECHNAME / {END, NEXT}
	 */

	/*
	 * Compute the size of the SPD message.
	 */
	size = sizeof (spd_msg_t) + sizeof (struct spd_ext_actions);

	for (algtype = 0; algtype < IPSEC_NALGTYPES; algtype++) {
		for (algidx = 0; algidx < ipsec_nalgs[algtype]; algidx++) {
			algid = ipsec_sortlist[algtype][algidx];
			alg = ipsec_alglists[algtype][algid];
			alg_size = sizeof (struct spd_attribute) *
			    (ATTRPERALG + alg->alg_nkey_sizes +
			    alg->alg_nblock_sizes) + CRYPTO_MAX_MECH_NAME;
			size += alg_size;
		}
	}

	ASSERT(ALIGNED64(size));

	m = allocb(size, BPRI_HI);
	if (m == NULL) {
		mutex_exit(&alg_lock);
		spdsock_error(q, mp, ENOMEM, 0);
		return;
	}

	m->b_wptr = m->b_rptr + size;
	cur = m->b_rptr;

	msg = (spd_msg_t *)cur;
	bcopy(mp->b_rptr, cur, sizeof (*msg));

	msg->spd_msg_len = SPD_8TO64(size);
	msg->spd_msg_errno = 0;
	msg->spd_msg_diagnostic = 0;

	cur += sizeof (*msg);

	act = (struct spd_ext_actions *)cur;
	cur += sizeof (*act);

	act->spd_actions_len = SPD_8TO64(size - sizeof (spd_msg_t));
	act->spd_actions_exttype = SPD_EXT_ACTION;
	act->spd_actions_count = ipsec_nalgs[IPSEC_ALG_AUTH] +
	    ipsec_nalgs[IPSEC_ALG_ENCR];
	act->spd_actions_reserved = 0;

	attr = (struct spd_attribute *)cur;

#define	EMIT(tag, value) {					\
		attr->spd_attr_tag = (tag); 			\
		attr->spd_attr_value = (value); 		\
		attr++;			  			\
	}

	for (algtype = 0; algtype < IPSEC_NALGTYPES; algtype++) {
		for (algidx = 0; algidx < ipsec_nalgs[algtype]; algidx++) {

			algid = ipsec_sortlist[algtype][algidx];
			alg = ipsec_alglists[algtype][algid];

			/*
			 * If you change the number of EMIT's here, change
			 * ATTRPERALG above to match
			 */
			EMIT(SPD_ATTR_ALG_ID, algid);
			EMIT(SPD_ATTR_ALG_PROTO, algproto[algtype]);
			EMIT(SPD_ATTR_ALG_INCRBITS, alg->alg_increment);

			EMIT(SPD_ATTR_ALG_NKEYSIZES, alg->alg_nkey_sizes);
			for (i = 0; i < alg->alg_nkey_sizes; i++)
				EMIT(SPD_ATTR_ALG_KEYSIZE,
				    alg->alg_key_sizes[i]);

			EMIT(SPD_ATTR_ALG_NBLOCKSIZES, alg->alg_nblock_sizes);
			for (i = 0; i < alg->alg_nblock_sizes; i++)
				EMIT(SPD_ATTR_ALG_BLOCKSIZE,
				    alg->alg_block_sizes[i]);

			EMIT(SPD_ATTR_ALG_MECHNAME, CRYPTO_MAX_MECH_NAME);
			bcopy(alg->alg_mech_name, attr, CRYPTO_MAX_MECH_NAME);
			attr = (struct spd_attribute *)((char *)attr +
			    CRYPTO_MAX_MECH_NAME);

			EMIT(SPD_ATTR_NEXT, 0);
		}
	}

	mutex_exit(&alg_lock);

#undef EMITALGATTRS
#undef EMIT
#undef ATTRPERALG

	attr--;
	attr->spd_attr_tag = SPD_ATTR_END;

	freemsg(mp);
	qreply(q, m);
}

/*
 * Do the actual work of processing an SPD_UPDATEALGS request. Can
 * be invoked either once IPsec is loaded on a cached request, or
 * when a request is received while IPsec is loaded.
 */
static void
spdsock_do_updatealg(spd_ext_t *extv[], int *diag)
{
	struct spd_ext_actions *actp;
	struct spd_attribute *attr, *endattr;
	uint64_t *start, *end;
	ipsec_alginfo_t *alg = NULL;
	ipsec_algtype_t alg_type = 0;
	boolean_t skip_alg = B_TRUE, doing_proto = B_FALSE;
	uint_t i, cur_key, cur_block, algid;

	*diag = -1;
	ASSERT(MUTEX_HELD(&spdsock_alg_lock));

	/* parse the message, building the list of algorithms */

	actp = (struct spd_ext_actions *)extv[SPD_EXT_ACTION];
	if (actp == NULL) {
		*diag = SPD_DIAGNOSTIC_NO_ACTION_EXT;
		return;
	}

	start = (uint64_t *)actp;
	end = (start + actp->spd_actions_len);
	endattr = (struct spd_attribute *)end;
	attr = (struct spd_attribute *)&actp[1];

	bzero(spdsock_algs, IPSEC_NALGTYPES * IPSEC_MAX_ALGS *
	    sizeof (ipsec_alginfo_t *));

	alg = kmem_zalloc(sizeof (*alg), KM_SLEEP);

#define	ALG_KEY_SIZES(a)   (((a)->alg_nkey_sizes + 1) * sizeof (uint16_t))
#define	ALG_BLOCK_SIZES(a) (((a)->alg_nblock_sizes + 1) * sizeof (uint16_t))

	while (attr < endattr) {
		switch (attr->spd_attr_tag) {
		case SPD_ATTR_NOP:
		case SPD_ATTR_EMPTY:
			break;
		case SPD_ATTR_END:
			attr = endattr;
			/* FALLTHRU */
		case SPD_ATTR_NEXT:
			if (doing_proto) {
				doing_proto = B_FALSE;
				break;
			}
			if (skip_alg) {
				ipsec_alg_free(alg);
			} else {
				ipsec_alg_free(
				    spdsock_algs[alg_type][alg->alg_id]);
				spdsock_algs[alg_type][alg->alg_id] = alg;
			}
			alg = kmem_zalloc(sizeof (*alg), KM_SLEEP);
			break;

		case SPD_ATTR_ALG_ID:
			if (attr->spd_attr_value >= IPSEC_MAX_ALGS) {
				ss1dbg(("spdsock_do_updatealg: "
				    "invalid alg id %d\n",
				    attr->spd_attr_value));
				*diag = SPD_DIAGNOSTIC_ALG_ID_RANGE;
				goto bail;
			}
			alg->alg_id = attr->spd_attr_value;
			break;

		case SPD_ATTR_ALG_PROTO:
			/* find the alg type */
			for (i = 0; i < NALGPROTOS; i++)
				if (algproto[i] == attr->spd_attr_value)
					break;
			skip_alg = (i == NALGPROTOS);
			if (!skip_alg)
				alg_type = i;
			break;

		case SPD_ATTR_ALG_INCRBITS:
			alg->alg_increment = attr->spd_attr_value;
			break;

		case SPD_ATTR_ALG_NKEYSIZES:
			if (alg->alg_key_sizes != NULL) {
				kmem_free(alg->alg_key_sizes,
				    ALG_KEY_SIZES(alg));
			}
			alg->alg_nkey_sizes = attr->spd_attr_value;
			/*
			 * Allocate room for the trailing zero key size
			 * value as well.
			 */
			alg->alg_key_sizes = kmem_zalloc(ALG_KEY_SIZES(alg),
			    KM_SLEEP);
			cur_key = 0;
			break;

		case SPD_ATTR_ALG_KEYSIZE:
			if (alg->alg_key_sizes == NULL ||
			    cur_key >= alg->alg_nkey_sizes) {
				ss1dbg(("spdsock_do_updatealg: "
					"too many key sizes\n"));
				*diag = SPD_DIAGNOSTIC_ALG_NUM_KEY_SIZES;
				goto bail;
			}
			alg->alg_key_sizes[cur_key++] = attr->spd_attr_value;
			break;

		case SPD_ATTR_ALG_NBLOCKSIZES:
			if (alg->alg_block_sizes != NULL) {
				kmem_free(alg->alg_block_sizes,
				    ALG_BLOCK_SIZES(alg));
			}
			alg->alg_nblock_sizes = attr->spd_attr_value;
			/*
			 * Allocate room for the trailing zero block size
			 * value as well.
			 */
			alg->alg_block_sizes = kmem_zalloc(ALG_BLOCK_SIZES(alg),
			    KM_SLEEP);
			cur_block = 0;
			break;

		case SPD_ATTR_ALG_BLOCKSIZE:
			if (alg->alg_block_sizes == NULL ||
			    cur_block >= alg->alg_nblock_sizes) {
				ss1dbg(("spdsock_do_updatealg: "
					"too many block sizes\n"));
				*diag = SPD_DIAGNOSTIC_ALG_NUM_BLOCK_SIZES;
				goto bail;
			}
			alg->alg_block_sizes[cur_block++] =
			    attr->spd_attr_value;
			break;

		case SPD_ATTR_ALG_MECHNAME: {
			char *mech_name;

			if (attr->spd_attr_value > CRYPTO_MAX_MECH_NAME) {
				ss1dbg(("spdsock_do_updatealg: "
					"mech name too long\n"));
				*diag = SPD_DIAGNOSTIC_ALG_MECH_NAME_LEN;
				goto bail;
			}
			mech_name = (char *)(attr + 1);
			bcopy(mech_name, alg->alg_mech_name,
			    attr->spd_attr_value);
			alg->alg_mech_name[CRYPTO_MAX_MECH_NAME-1] = '\0';
			attr = (struct spd_attribute *)((char *)attr +
			    attr->spd_attr_value);
			break;
		}

		case SPD_ATTR_PROTO_ID:
			doing_proto = B_TRUE;
			for (i = 0; i < NALGPROTOS; i++) {
				if (algproto[i] == attr->spd_attr_value) {
					alg_type = i;
					break;
				}
			}
			break;

		case SPD_ATTR_PROTO_EXEC_MODE:
			if (!doing_proto)
				break;
			for (i = 0; i < NEXECMODES; i++) {
				if (execmodes[i] == attr->spd_attr_value) {
					spdsock_algs_exec_mode[alg_type] = i;
					break;
				}
			}
			break;
		}
		attr++;
	}

#undef	ALG_KEY_SIZES
#undef	ALG_BLOCK_SIZES

	/* update the algorithm tables */
	spdsock_merge_algs();
bail:
	/* cleanup */
	ipsec_alg_free(alg);
	for (alg_type = 0; alg_type < IPSEC_NALGTYPES; alg_type++)
		for (algid = 0; algid < IPSEC_MAX_ALGS; algid++)
			if (spdsock_algs[alg_type][algid] != NULL)
				ipsec_alg_free(spdsock_algs[alg_type][algid]);
}

/*
 * Process an SPD_UPDATEALGS request. If IPsec is not loaded, queue
 * the request until IPsec loads. If IPsec is loaded, act on it
 * immediately.
 */

static void
spdsock_updatealg(queue_t *q, mblk_t *mp, spd_ext_t *extv[])
{
	if (!ipsec_loaded()) {
		/*
		 * IPsec is not loaded, save request and return nicely,
		 * the message will be processed once IPsec loads.
		 */
		mblk_t *new_mp;

		/* last update message wins */
		if ((new_mp = copymsg(mp)) == NULL) {
			spdsock_error(q, mp, ENOMEM, 0);
			return;
		}
		mutex_enter(&spdsock_alg_lock);
		bcopy(extv, spdsock_extv_algs,
		    sizeof (spd_ext_t *) * (SPD_EXT_MAX + 1));
		if (spdsock_mp_algs != NULL)
			freemsg(spdsock_mp_algs);
		spdsock_mp_algs = mp;
		spdsock_algs_pending = B_TRUE;
		mutex_exit(&spdsock_alg_lock);

		spd_echo(q, new_mp);
	} else {
		/*
		 * IPsec is loaded, act on the message immediately.
		 */
		int diag;

		mutex_enter(&spdsock_alg_lock);
		spdsock_do_updatealg(extv, &diag);
		mutex_exit(&spdsock_alg_lock);
		if (diag == -1)
			spd_echo(q, mp);
		else
			spdsock_diag(q, mp, diag);
	}
}

static void
spdsock_parse(queue_t *q, mblk_t *mp)
{
	spd_msg_t *spmsg;
	spd_ext_t *extv[SPD_EXT_MAX + 1];
	uint_t msgsize;
	ipsec_policy_head_t *iph;

	/* Make sure nothing's below me. */
	ASSERT(WR(q)->q_next == NULL);

	spmsg = (spd_msg_t *)mp->b_rptr;

	msgsize = SPD_64TO8(spmsg->spd_msg_len);

	if (msgdsize(mp) != msgsize) {
		/*
		 * Message len incorrect w.r.t. actual size.  Send an error
		 * (EMSGSIZE).	It may be necessary to massage things a
		 * bit.	 For example, if the spd_msg_type is hosed,
		 * I need to set it to SPD_RESERVED to get delivery to
		 * do the right thing.	Then again, maybe just letting
		 * the error delivery do the right thing.
		 */
		ss2dbg(("mblk (%lu) and base (%d) message sizes don't jibe.\n",
		    msgdsize(mp), msgsize));
		spdsock_error(q, mp, EMSGSIZE, SPD_DIAGNOSTIC_NONE);
		return;
	}

	if (msgsize > (uint_t)(mp->b_wptr - mp->b_rptr)) {

		/* Get all message into one mblk. */
		if (pullupmsg(mp, -1) == 0) {
			/*
			 * Something screwy happened.
			 */
			ss3dbg(("spdsock_parse: pullupmsg() failed.\n"));
			return;
		} else {
			spmsg = (spd_msg_t *)mp->b_rptr;
		}
	}

	switch (spdsock_get_ext(extv, spmsg, msgsize)) {
	case KGE_DUP:
		/* Handle duplicate extension. */
		ss1dbg(("Got duplicate extension of type %d.\n",
		    extv[0]->spd_ext_type));
		spdsock_diag(q, mp, dup_ext_diag[extv[0]->spd_ext_type]);
		return;
	case KGE_UNK:
		/* Handle unknown extension. */
		ss1dbg(("Got unknown extension of type %d.\n",
		    extv[0]->spd_ext_type));
		spdsock_diag(q, mp, SPD_DIAGNOSTIC_UNKNOWN_EXT);
		return;
	case KGE_LEN:
		/* Length error. */
		ss1dbg(("Length %d on extension type %d overrun or 0.\n",
		    extv[0]->spd_ext_len, extv[0]->spd_ext_type));
		spdsock_diag(q, mp, SPD_DIAGNOSTIC_BAD_EXTLEN);
		return;
	case KGE_CHK:
		/* Reality check failed. */
		ss1dbg(("Reality check failed on extension type %d.\n",
		    extv[0]->spd_ext_type));
		spdsock_diag(q, mp, bad_ext_diag[extv[0]->spd_ext_type]);
		return;
	default:
		/* Default case is no errors. */
		break;
	}

	/*
	 * Which rule set are we operating on today?
	 */

	switch (spmsg->spd_msg_spdid) {
	case SPD_ACTIVE:
		iph = ipsec_system_policy();
		break;

	case SPD_STANDBY:
		iph = ipsec_inactive_policy();
		break;

	default:
		spdsock_diag(q, mp, SPD_DIAGNOSTIC_BAD_SPDID);
		return;
	}

	/*
	 * Special-case SPD_UPDATEALGS so as not to load IPsec.
	 */
	if (!ipsec_loaded() && spmsg->spd_msg_type != SPD_UPDATEALGS) {
		spdsock_t *ss = (spdsock_t *)q->q_ptr;

		ASSERT(ss != NULL);
		ipsec_loader_loadnow();
		ss->spdsock_timeout_arg = mp;
		ss->spdsock_timeout = qtimeout(q, spdsock_loadcheck,
		    q, LOADCHECK_INTERVAL);
		return;
	}

	switch (spmsg->spd_msg_type) {
	case SPD_UPDATEALGS:
		spdsock_updatealg(q, mp, extv);
		return;
	case SPD_FLUSH:
		spdsock_flush(q, iph, mp, extv);
		return;

	case SPD_ADDRULE:
		spdsock_addrule(q, iph, mp, extv);
		return;

	case SPD_DELETERULE:
		spdsock_deleterule(q, iph, mp, extv);
		return;

	case SPD_FLIP:
		spdsock_flip(q, mp);
		return;

	case SPD_LOOKUP:
		spdsock_lookup(q, iph, mp, extv);
		return;

	case SPD_DUMP:
		spdsock_dump(q, iph, mp, extv);
		return;

	case SPD_CLONE:
		spdsock_clone(q, mp);
		return;

	case SPD_ALGLIST:
		spdsock_alglist(q, mp);
		return;

	case SPD_DUMPALGS:
		spdsock_dumpalgs(q, mp);
		return;

	default:
		spdsock_diag(q, mp, SPD_DIAGNOSTIC_BAD_MSG_TYPE);
		return;
	}
}

/*
 * If an algorithm mapping was received before IPsec was loaded, process it.
 * Called from the IPsec loader.
 */
void
spdsock_update_pending_algs(void)
{
	mutex_enter(&spdsock_alg_lock);
	if (spdsock_algs_pending) {
		int diag;
		spdsock_do_updatealg(spdsock_extv_algs, &diag);
		spdsock_algs_pending = B_FALSE;
	}
	mutex_exit(&spdsock_alg_lock);
}

static void
spdsock_loadcheck(void *arg)
{
	queue_t *q = (queue_t *)arg;
	spdsock_t *ss = (spdsock_t *)q->q_ptr;
	mblk_t *mp;

	ASSERT(ss != NULL);

	ss->spdsock_timeout = 0;
	mp = ss->spdsock_timeout_arg;
	ASSERT(mp != NULL);
	ss->spdsock_timeout_arg = NULL;
	if (ipsec_failed())
		spdsock_error(q, mp, EPROTONOSUPPORT, 0);
	else
		spdsock_parse(q, mp);
}

/*
 * Copy relevant state bits.
 */
static void
spdsock_copy_info(struct T_info_ack *tap, spdsock_t *ss)
{
	*tap = spdsock_g_t_info_ack;
	tap->CURRENT_state = ss->spdsock_state;
	tap->OPT_size = spdsock_max_optsize;
}

/*
 * This routine responds to T_CAPABILITY_REQ messages.  It is called by
 * spdsock_wput.  Much of the T_CAPABILITY_ACK information is copied from
 * spdsock_g_t_info_ack.  The current state of the stream is copied from
 * spdsock_state.
 */
static void
spdsock_capability_req(queue_t *q, mblk_t *mp)
{
	spdsock_t *ss = (spdsock_t *)q->q_ptr;
	t_uscalar_t cap_bits1;
	struct T_capability_ack	*tcap;

	cap_bits1 = ((struct T_capability_req *)mp->b_rptr)->CAP_bits1;

	mp = tpi_ack_alloc(mp, sizeof (struct T_capability_ack),
		mp->b_datap->db_type, T_CAPABILITY_ACK);
	if (mp == NULL)
		return;

	tcap = (struct T_capability_ack *)mp->b_rptr;
	tcap->CAP_bits1 = 0;

	if (cap_bits1 & TC1_INFO) {
		spdsock_copy_info(&tcap->INFO_ack, ss);
		tcap->CAP_bits1 |= TC1_INFO;
	}

	qreply(q, mp);
}

/*
 * This routine responds to T_INFO_REQ messages. It is called by
 * spdsock_wput_other.
 * Most of the T_INFO_ACK information is copied from spdsock_g_t_info_ack.
 * The current state of the stream is copied from spdsock_state.
 */
static void
spdsock_info_req(q, mp)
	queue_t	*q;
	mblk_t	*mp;
{
	mp = tpi_ack_alloc(mp, sizeof (struct T_info_ack), M_PCPROTO,
	    T_INFO_ACK);
	if (mp == NULL)
		return;
	spdsock_copy_info((struct T_info_ack *)mp->b_rptr,
	    (spdsock_t *)q->q_ptr);
	qreply(q, mp);
}

/*
 * spdsock_err_ack. This routine creates a
 * T_ERROR_ACK message and passes it
 * upstream.
 */
static void
spdsock_err_ack(q, mp, t_error, sys_error)
	queue_t	*q;
	mblk_t	*mp;
	int	t_error;
	int	sys_error;
{
	if ((mp = mi_tpi_err_ack_alloc(mp, t_error, sys_error)) != NULL)
		qreply(q, mp);
}

/*
 * This routine retrieves the current status of socket options.
 * It returns the size of the option retrieved.
 */
/* ARGSUSED */
int
spdsock_opt_get(queue_t *q, int level, int name, uchar_t *ptr)
{
	int *i1 = (int *)ptr;

	switch (level) {
	case SOL_SOCKET:
		switch (name) {
		case SO_TYPE:
			*i1 = SOCK_RAW;
			break;
		/*
		 * The following two items can be manipulated,
		 * but changing them should do nothing.
		 */
		case SO_SNDBUF:
			*i1 = (int)q->q_hiwat;
			break;
		case SO_RCVBUF:
			*i1 = (int)(RD(q)->q_hiwat);
			break;
		}
		break;
	default:
		return (0);
	}
	return (sizeof (int));
}

/*
 * This routine sets socket options.
 */
/* ARGSUSED */
int
spdsock_opt_set(queue_t *q, uint_t mgmt_flags, int level, int name,
    uint_t inlen, uchar_t *invalp, uint_t *outlenp, uchar_t *outvalp,
    void *thisdg_attrs, cred_t *cr, mblk_t *mblk)
{
	int *i1 = (int *)invalp;

	switch (level) {
	case SOL_SOCKET:
		switch (name) {
		case SO_SNDBUF:
			if (*i1 > spdsock_max_buf)
				return (ENOBUFS);
			q->q_hiwat = *i1;
			break;
		case SO_RCVBUF:
			if (*i1 > spdsock_max_buf)
				return (ENOBUFS);
			RD(q)->q_hiwat = *i1;
			(void) mi_set_sth_hiwat(RD(q), *i1);
			break;
		}
		break;
	}
	return (0);
}


/*
 * Handle STREAMS messages.
 */
static void
spdsock_wput_other(queue_t *q, mblk_t *mp)
{
	struct iocblk *iocp;
	int error;

	switch (mp->b_datap->db_type) {
	case M_PROTO:
	case M_PCPROTO:
		if ((mp->b_wptr - mp->b_rptr) < sizeof (long)) {
			ss3dbg((
			    "spdsock_wput_other: Not big enough M_PROTO\n"));
			freemsg(mp);
			return;
		}
		switch (((union T_primitives *)mp->b_rptr)->type) {
		case T_CAPABILITY_REQ:
			spdsock_capability_req(q, mp);
			return;
		case T_INFO_REQ:
			spdsock_info_req(q, mp);
			return;
		case T_SVR4_OPTMGMT_REQ:
			(void) svr4_optcom_req(q, mp, DB_CREDDEF(mp, kcred),
			    &spdsock_opt_obj);
			return;
		case T_OPTMGMT_REQ:
			(void) tpi_optcom_req(q, mp, DB_CREDDEF(mp, kcred),
			    &spdsock_opt_obj);
			return;
		case T_DATA_REQ:
		case T_EXDATA_REQ:
		case T_ORDREL_REQ:
			/* Illegal for spdsock. */
			freemsg(mp);
			(void) putnextctl1(RD(q), M_ERROR, EPROTO);
			return;
		default:
			/* Not supported by spdsock. */
			spdsock_err_ack(q, mp, TNOTSUPPORT, 0);
			return;
		}
	case M_IOCTL:
		iocp = (struct iocblk *)mp->b_rptr;
		error = EINVAL;

		switch (iocp->ioc_cmd) {
		case ND_SET:
		case ND_GET:
			if (nd_getset(q, spdsock_g_nd, mp)) {
				qreply(q, mp);
				return;
			} else
				error = ENOENT;
			/* FALLTHRU */
		default:
			miocnak(q, mp, 0, error);
			return;
		}
	case M_FLUSH:
		if (*mp->b_rptr & FLUSHW) {
			flushq(q, FLUSHALL);
			*mp->b_rptr &= ~FLUSHW;
		}
		if (*mp->b_rptr & FLUSHR) {
			qreply(q, mp);
			return;
		}
		/* Else FALLTHRU */
	}

	/* If fell through, just black-hole the message. */
	freemsg(mp);
}

static void
spdsock_wput(queue_t *q, mblk_t *mp)
{
	uint8_t *rptr = mp->b_rptr;
	mblk_t *mp1;
	spdsock_t *ss = (spdsock_t *)q->q_ptr;

	/*
	 * If we're dumping, defer processing other messages until the
	 * dump completes.
	 */
	if (ss->spdsock_dump_req != NULL) {
		if (!putq(q, mp))
			freemsg(mp);
		return;
	}

	switch (mp->b_datap->db_type) {
	case M_DATA:
		/*
		 * Silently discard.
		 */
		ss2dbg(("raw M_DATA in spdsock.\n"));
		freemsg(mp);
		return;
	case M_PROTO:
	case M_PCPROTO:
		if ((mp->b_wptr - rptr) >= sizeof (struct T_data_req)) {
			if (((union T_primitives *)rptr)->type == T_DATA_REQ) {
				if ((mp1 = mp->b_cont) == NULL) {
					/* No data after T_DATA_REQ. */
					ss2dbg(("No data after DATA_REQ.\n"));
					freemsg(mp);
					return;
				}
				freeb(mp);
				mp = mp1;
				ss2dbg(("T_DATA_REQ\n"));
				break;	/* Out of switch. */
			}
		}
		/* FALLTHRU */
	default:
		ss3dbg(("In default wput case (%d %d).\n",
		    mp->b_datap->db_type, ((union T_primitives *)rptr)->type));
		spdsock_wput_other(q, mp);
		return;
	}

	/* I now have a PF_POLICY message in an M_DATA block. */
	spdsock_parse(q, mp);
}

/*
 * Device open procedure, called when new queue pair created.
 * We are passed the read-side queue.
 */
/* ARGSUSED */
static int
spdsock_open(queue_t *q, dev_t *devp, int flag, int sflag, cred_t *credp)
{
	spdsock_t *ss;
	queue_t *oq = OTHERQ(q);
	minor_t ssminor;

	if (secpolicy_net_config(credp, B_FALSE) != 0)
		return (EPERM);

	if (q->q_ptr != NULL)
		return (0);  /* Re-open of an already open instance. */

	if (sflag & MODOPEN)
		return (EINVAL);

	ss2dbg(("Made it into PF_POLICY socket open.\n"));

	ssminor = (minor_t)(uintptr_t)vmem_alloc(spdsock_vmem, 1, VM_NOSLEEP);
	if (ssminor == 0)
		return (ENOMEM);

	ss = kmem_zalloc(sizeof (spdsock_t), KM_NOSLEEP);
	if (ss == NULL) {
		vmem_free(spdsock_vmem, (void *)(uintptr_t)ssminor, 1);
		return (ENOMEM);
	}

	ss->spdsock_minor = ssminor;
	ss->spdsock_state = TS_UNBND;
	ss->spdsock_dump_req = NULL;

	q->q_ptr = ss;
	oq->q_ptr = ss;

	q->q_hiwat = spdsock_recv_hiwat;

	oq->q_hiwat = spdsock_xmit_hiwat;
	oq->q_lowat = spdsock_xmit_lowat;

	qprocson(q);
	(void) mi_set_sth_hiwat(q, spdsock_recv_hiwat);

	*devp = makedevice(getmajor(*devp), ss->spdsock_minor);
	return (0);
}

/*
 * Read-side service procedure, invoked when we get back-enabled
 * when buffer space becomes available.
 *
 * Dump another chunk if we were dumping before; when we finish, kick
 * the write-side queue in case it's waiting for read queue space.
 */
void
spdsock_rsrv(queue_t *q)
{
	spdsock_t *ss = q->q_ptr;

	if (ss->spdsock_dump_req != NULL)
		spdsock_dump_some(q, ss);

	if (ss->spdsock_dump_req == NULL)
		qenable(OTHERQ(q));
}

/*
 * Write-side service procedure, invoked when we defer processing
 * if another message is received while a dump is in progress.
 */
void
spdsock_wsrv(queue_t *q)
{
	spdsock_t *ss = q->q_ptr;
	mblk_t *mp;

	if (ss->spdsock_dump_req != NULL) {
		qenable(OTHERQ(q));
		return;
	}

	while ((mp = getq(q)) != NULL) {
		if (ipsec_loaded()) {
			spdsock_wput(q, mp);
			if (ss->spdsock_dump_req != NULL)
				return;
		} else if (!ipsec_failed()) {
			(void) putq(q, mp);
		} else {
			spdsock_error(q, mp, EPFNOSUPPORT, 0);
		}
	}
}

static int
spdsock_close(queue_t *q)
{
	spdsock_t *ss = q->q_ptr;

	qprocsoff(q);

	/* Safe assumption. */
	ASSERT(ss != NULL);

	if (ss->spdsock_timeout != 0)
		(void) quntimeout(q, ss->spdsock_timeout);

	ss3dbg(("Driver close, PF_POLICY socket is going away.\n"));

	vmem_free(spdsock_vmem, (void *)(uintptr_t)ss->spdsock_minor, 1);

	kmem_free(ss, sizeof (spdsock_t));
	return (0);
}

/*
 * Merge the IPsec algorithms tables with the received algorithm information.
 */
void
spdsock_merge_algs(void)
{
	ipsec_alginfo_t *alg, *oalg;
	ipsec_algtype_t algtype;
	uint_t algidx, algid, nalgs;
	crypto_mech_name_t *mechs;
	uint_t mech_count, mech_idx;

	ASSERT(MUTEX_HELD(&spdsock_alg_lock));

	/*
	 * Get the list of supported mechanisms from the crypto framework.
	 * If a mechanism is supported by KCF, resolve its mechanism
	 * id and mark it as being valid. This operation must be done
	 * without holding alg_lock, since it can cause a provider
	 * module to be loaded and the provider notification callback to
	 * be invoked.
	 */
	mechs = crypto_get_mech_list(&mech_count, KM_SLEEP);
	for (algtype = 0; algtype < IPSEC_NALGTYPES; algtype++) {
		for (algid = 0; algid < IPSEC_MAX_ALGS; algid++) {
			int algflags = 0;
			crypto_mech_type_t mt = CRYPTO_MECHANISM_INVALID;

			if ((alg = spdsock_algs[algtype][algid]) == NULL)
				continue;

			/*
			 * The NULL encryption algorithm is a special
			 * case because there are no mechanisms, yet
			 * the algorithm is still valid.
			 */
			if (alg->alg_id == SADB_EALG_NULL) {
				alg->alg_mech_type = CRYPTO_MECHANISM_INVALID;
				alg->alg_flags = ALG_FLAG_VALID;
				continue;
			}

			for (mech_idx = 0; mech_idx < mech_count; mech_idx++) {
				if (strncmp(alg->alg_mech_name, mechs[mech_idx],
				    CRYPTO_MAX_MECH_NAME) == 0) {
					mt = crypto_mech2id(alg->alg_mech_name);
					ASSERT(mt != CRYPTO_MECHANISM_INVALID);
					algflags = ALG_FLAG_VALID;
					break;
				}
			}
			alg->alg_mech_type = mt;
			alg->alg_flags = algflags;
		}
	}

	mutex_enter(&alg_lock);

	/*
	 * For each algorithm currently defined, check if it is
	 * present in the new tables created from the SPD_UPDATEALGS
	 * message received from user-space.
	 * Delete the algorithm entries that are currently defined
	 * but not part of the new tables.
	 */
	for (algtype = 0; algtype < IPSEC_NALGTYPES; algtype++) {
		nalgs = ipsec_nalgs[algtype];
		for (algidx = 0; algidx < nalgs; algidx++) {
			algid = ipsec_sortlist[algtype][algidx];
			if (spdsock_algs[algtype][algid] == NULL)
				ipsec_alg_unreg(algtype, algid);
		}
	}

	/*
	 * For each algorithm we just received, check if it is
	 * present in the currently defined tables. If it is, swap
	 * the entry with the one we just allocated.
	 * If the new algorithm is not in the current tables,
	 * add it.
	 */
	for (algtype = 0; algtype < IPSEC_NALGTYPES; algtype++) {
		for (algid = 0; algid < IPSEC_MAX_ALGS; algid++) {
			if ((alg = spdsock_algs[algtype][algid]) == NULL)
				continue;

			if ((oalg = ipsec_alglists[algtype][algid]) == NULL) {
				/*
				 * New algorithm, add it to the algorithm
				 * table.
				 */
				ipsec_alg_reg(algtype, alg);
			} else {
				/*
				 * Algorithm is already in the table. Swap
				 * the existing entry with the new one.
				 */
				ipsec_alg_fix_min_max(alg, algtype);
				ipsec_alglists[algtype][algid] = alg;
				ipsec_alg_free(oalg);
			}
			spdsock_algs[algtype][algid] = NULL;
		}
	}

	for (algtype = 0; algtype < IPSEC_NALGTYPES; algtype++)
		ipsec_algs_exec_mode[algtype] = spdsock_algs_exec_mode[algtype];

	mutex_exit(&alg_lock);

	crypto_free_mech_list(mechs, mech_count);

	ipsecah_algs_changed();
	ipsecesp_algs_changed();
}
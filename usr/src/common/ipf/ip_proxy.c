/*
 * Copyright (C) 1997-2003 by Darren Reed.
 *
 * See the IPFILTER.LICENCE file for details on licencing.
 *
 * Copyright 2003 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)ip_proxy.c	1.1	03/12/15 SMI"

#if defined(KERNEL) || defined(_KERNEL)
# undef KERNEL
# undef _KERNEL
# define        KERNEL	1
# define        _KERNEL	1
#endif
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/fcntl.h>
#if !defined(_KERNEL) && !defined(__KERNEL__)
# include <stdio.h>
# include <string.h>
# include <stdlib.h>
# include <ctype.h>
# define _KERNEL
# ifdef __OpenBSD__
struct file;
# endif
# include <sys/uio.h>
# undef _KERNEL
#endif
#include <sys/protosw.h>
#include <sys/socket.h>
#if defined(_KERNEL)
# if !defined(__NetBSD__) && !defined(sun) && !defined(__osf__) && \
     !defined(__OpenBSD__) && !defined(__hpux)
#  include <sys/ctype.h>
# endif
# include <sys/systm.h>
# if !defined(__SVR4) && !defined(__svr4__)
#  include <sys/mbuf.h>
# endif
#endif
#if defined(_KERNEL) && (__FreeBSD_version >= 220000)
# include <sys/filio.h>
# include <sys/fcntl.h>
# if (__FreeBSD_version >= 300000) && !defined(IPFILTER_LKM)
#  include "opt_ipfilter.h"
# endif
#else
# include <sys/ioctl.h>
#endif
#if defined(__SVR4) || defined(__svr4__)
# include <sys/byteorder.h>
# ifdef _KERNEL
#  include <sys/dditypes.h>
# endif
# include <sys/stream.h>
# include <sys/kmem.h>
#endif
#if __FreeBSD__ > 2
# include <sys/queue.h>
#endif
#include <net/if.h>
#ifdef sun
# include <net/af.h>
#endif
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcpip.h>

#if SOLARIS2 >= 10
#include "ip_compat.h"
#include "ip_fil.h"
#include "ip_nat.h"
#include "ip_state.h"
#include "ip_proxy.h"
#else
#include "netinet/ip_compat.h"
#include "netinet/ip_fil.h"
#include "netinet/ip_nat.h"
#include "netinet/ip_state.h"
#include "netinet/ip_proxy.h"
#endif 

#if (__FreeBSD_version >= 300000)
# include <sys/malloc.h>
#endif

#if !defined(lint)
static const char rcsid[] = "@(#)$Id: ip_proxy.c,v 2.49 2003/06/14 02:56:40 darrenr Exp $";
#endif


static int appr_fixseqack __P((fr_info_t *, ip_t *, ap_session_t *, int ));


#define	AP_SESS_SIZE	53

#if SOLARIS2 >= 10
#include "ip_ftp_pxy.c"
#if defined(_KERNEL)
#include "ip_irc_pxy.c"
#include "ip_rcmd_pxy.c"
#include "ip_raudio_pxy.c"
#include "ip_h323_pxy.c"
#ifdef	IPFILTER_PRO
#include "ip_msnrpc_pxy.c"
#endif
#include "ip_netbios_pxy.c"
#endif
#include "ip_ipsec_pxy.c"
#include "ip_rpcb_pxy.c"
#else
#include "netinet/ip_ftp_pxy.c"
#if defined(_KERNEL)
#include "netinet/ip_irc_pxy.c"
#include "netinet/ip_rcmd_pxy.c"
#include "netinet/ip_raudio_pxy.c"
#include "netinet/ip_h323_pxy.c"
#ifdef	IPFILTER_PRO
#include "netinet/ip_msnrpc_pxy.c"
#endif
#include "netinet/ip_netbios_pxy.c"
#endif
#include "netinet/ip_ipsec_pxy.c"
#include "netinet/ip_rpcb_pxy.c"
#endif

ap_session_t	*ap_sess_tab[AP_SESS_SIZE];
ap_session_t	*ap_sess_list = NULL;
aproxy_t	*ap_proxylist = NULL;
aproxy_t	ap_proxies[] = {
#ifdef	IPF_FTP_PROXY
	{ NULL, "ftp", (char)IPPROTO_TCP, 0, 0, ippr_ftp_init, ippr_ftp_fini,
	  ippr_ftp_new, NULL, ippr_ftp_in, ippr_ftp_out, NULL },
#endif
#ifdef	IPF_IRC_PROXY
	{ NULL, "irc", (char)IPPROTO_TCP, 0, 0, ippr_irc_init, ippr_irc_fini,
	  ippr_irc_new, NULL, NULL, ippr_irc_out, NULL, NULL },
#endif
#ifdef	IPF_RCMD_PROXY
	{ NULL, "rcmd", (char)IPPROTO_TCP, 0, 0, ippr_rcmd_init, ippr_rcmd_fini,
	  ippr_rcmd_new, NULL, ippr_rcmd_in, ippr_rcmd_out, NULL, NULL },
#endif
#ifdef	IPF_RAUDIO_PROXY
	{ NULL, "raudio", (char)IPPROTO_TCP, 0, 0, ippr_raudio_init, ippr_raudio_fini,
	  ippr_raudio_new, NULL, ippr_raudio_in, ippr_raudio_out, NULL, NULL },
#endif
#ifdef	IPF_MSNRPC_PROXY
	{ NULL, "msnrpc", (char)IPPROTO_TCP, 0, 0, ippr_msnrpc_init, ippr_msnrpc_fini,
	  ippr_msnrpc_new, NULL, ippr_msnrpc_in, ippr_msnrpc_out, NULL, NULL },
#endif
#ifdef	IPF_NETBIOS_PROXY
	{ NULL, "netbios", (char)IPPROTO_UDP, 0, 0, ippr_netbios_init, ippr_netbios_fini,
	  NULL, NULL, NULL, ippr_netbios_out, NULL, NULL },
#endif
#ifdef	IPF_IPSEC_PROXY
	{ NULL, "ipsec", (char)IPPROTO_UDP, 0, 0,
	  ippr_ipsec_init, ippr_ipsec_fini, ippr_ipsec_new, ippr_ipsec_del,
	  NULL, ippr_ipsec_out, NULL, NULL },
#endif
#ifdef  IPF_H323_PROXY
	{ NULL, "h323", (char)IPPROTO_TCP, 0, 0, ippr_h323_init, ippr_h323_fini,
	  ippr_h323_new, ippr_h323_del, ippr_h323_in, NULL, NULL },
	{ NULL, "h245", (char)IPPROTO_TCP, 0, 0, NULL, NULL,
	  ippr_h245_new, NULL, NULL, ippr_h245_out, NULL },
#endif
#ifdef	IPF_RPCB_PROXY
# if 0
	{ NULL, "rpcbt", (char)IPPROTO_TCP, 0, 0,
	  ippr_rpcb_init, ippr_rpcb_fini, ippr_rpcb_new, ippr_rpcb_del,
	  ippr_rpcb_in, ippr_rpcb_out, NULL, NULL },
# endif
	{ NULL, "rpcbu", (char)IPPROTO_UDP, 0, 0,
	  ippr_rpcb_init, ippr_rpcb_fini, ippr_rpcb_new, ippr_rpcb_del,
	  ippr_rpcb_in, ippr_rpcb_out, NULL, NULL },
#endif
	{ NULL, "", '\0', 0, 0, NULL, NULL, NULL, NULL }
};

/*
 * Dynamically add a new kernel proxy.  Ensure that it is unique in the
 * collection compiled in and dynamically added.
 */
int appr_add(ap)
aproxy_t *ap;
{
	aproxy_t *a;

	for (a = ap_proxies; a->apr_p; a++)
		if ((a->apr_p == ap->apr_p) &&
		    !strncmp(a->apr_label, ap->apr_label,
			     sizeof(ap->apr_label)))
			return -1;

	for (a = ap_proxylist; a->apr_p; a = a->apr_next)
		if ((a->apr_p == ap->apr_p) &&
		    !strncmp(a->apr_label, ap->apr_label,
			     sizeof(ap->apr_label)))
			return -1;
	ap->apr_next = ap_proxylist;
	ap_proxylist = ap;
	if (ap->apr_init != NULL)
		return (*ap->apr_init)();
	return 0;
}


/*
 * Check to see if the proxy this control request has come through for
 * exists, and if it does and it has a control function then invoke that
 * control function.
 */
int appr_ctl(ctl)
ap_ctl_t *ctl;
{
	aproxy_t *a;
	int error;

	a = appr_lookup(ctl->apc_p, ctl->apc_label);
	if (a == NULL)
		error = ESRCH;
	else if (a->apr_ctl == NULL)
		error = ENXIO;
	else
		error = (*a->apr_ctl)(a, ctl);
	return error;
}


/*
 * Delete a proxy that has been added dynamically from those available.
 * If it is in use, return 1 (do not destroy NOW), not in use 0 or -1
 * if it cannot be matched.
 */
int appr_del(ap)
aproxy_t *ap;
{
	aproxy_t *a, **app;

	for (app = &ap_proxylist; ((a = *app) != NULL); app = &a->apr_next)
		if (a == ap) {
			a->apr_flags |= APR_DELETE;
			*app = a->apr_next;
			if (ap->apr_ref != 0)
				return 1;
			return 0;
		}
	return -1;
}


/*
 * Return 1 if the packet is a good match against a proxy, else 0.
 */
int appr_ok(fin, tcp, nat)
fr_info_t *fin;
tcphdr_t *tcp;
ipnat_t *nat;
{
	aproxy_t *apr = nat->in_apr;
	u_short dport = nat->in_dport;

	if ((apr == NULL) || (apr->apr_flags & APR_DELETE) ||
	    (fin->fin_p != apr->apr_p))
		return 0;
	if ((tcp == NULL) && dport)
		return 0;
	return 1;
}


int appr_ioctl(data, cmd, mode)
caddr_t data;
int mode;
#if defined(__NetBSD__) || defined(__OpenBSD__) || (__FreeBSD_version >= 300003)
u_long cmd;
#else
int cmd;
#endif
{
	ap_ctl_t ctl;
	caddr_t ptr;
	int error;

	mode = mode;	/* LINT */

	switch (cmd)
	{
	case SIOCPROXY :
		(void) BCOPYIN(data, &ctl, sizeof(ctl));
		ptr = NULL;

		if (ctl.apc_dsize > 0) {
			KMALLOCS(ptr, caddr_t, ctl.apc_dsize);
			if (ptr == NULL)
				error = ENOMEM;
			else {
				error = copyinptr(ctl.apc_data, ptr,
						  ctl.apc_dsize);
				if (error == 0)
					ctl.apc_data = ptr;
			}
		} else {
			ctl.apc_data = NULL;
			error = 0;
		}

		if (error == 0)
			error = appr_ctl(&ctl);

		if (ctl.apc_dsize > 0 && ptr != NULL && ctl.apc_data == ptr) {
			KFREES(ptr, ctl.apc_dsize);
		}
		break;

	default :
		error = EINVAL;
	}
	return error;
}


/*
 * If a proxy has a match function, call that to do extended packet
 * matching.
 */
int appr_match(fin, nat)
fr_info_t *fin;
nat_t *nat;
{
	aproxy_t *apr;
	ipnat_t *ipn;

	ipn = nat->nat_ptr;
	if (ipn == NULL)
		return -1;
	apr = ipn->in_apr;
	if ((apr == NULL) || (apr->apr_flags & APR_DELETE) ||
	    (nat->nat_aps == NULL))
		return -1;
	if (apr->apr_match != NULL)
		if ((*apr->apr_match)(fin, nat->nat_aps, nat) != 0)
			return -1;
	return 0;
}


/*
 * Allocate a new application proxy structure and fill it in with the
 * relevant details.  call the init function once complete, prior to
 * returning.
 */
int appr_new(fin, nat)
fr_info_t *fin;
nat_t *nat;
{
	register ap_session_t *aps;
	aproxy_t *apr;

	if ((nat->nat_ptr == NULL) || (nat->nat_aps != NULL))
		return -1;

	apr = nat->nat_ptr->in_apr;

	if (!apr || (apr->apr_flags & APR_DELETE) ||
	    (fin->fin_p != apr->apr_p))
		return -1;

	KMALLOC(aps, ap_session_t *);
	if (!aps)
		return -1;
	bzero((char *)aps, sizeof(*aps));
	aps->aps_p = fin->fin_p;
	aps->aps_data = NULL;
	aps->aps_apr = apr;
	aps->aps_psiz = 0;
	if (apr->apr_new != NULL)
		if ((*apr->apr_new)(fin, aps, nat) == -1) {
			if ((aps->aps_data != NULL) && (aps->aps_psiz != 0)) {
				KFREES(aps->aps_data, aps->aps_psiz);
			}
			KFREE(aps);
			return -1;
		}
	aps->aps_nat = nat;
	aps->aps_next = ap_sess_list;
	ap_sess_list = aps;
	nat->nat_aps = aps;

	return 0;
}


/*
 * Check to see if a packet should be passed through an active proxy routine
 * if one has been setup for it.  We don't need to check the checksum here if
 * IPFILTER_CKSUM is defined because if it is, a failed check causes FI_BAD
 * to be set.
 */
int appr_check(fin, nat)
fr_info_t *fin;
nat_t *nat;
{
#if SOLARIS && defined(_KERNEL) && (SOLARIS2 >= 6) 
#if defined(ICK_VALID)
	mb_t *m;
#endif
	int dosum = 1;
#endif
	tcphdr_t *tcp = NULL;
	udphdr_t *udp = NULL;
	ap_session_t *aps;
	aproxy_t *apr;
	ip_t *ip;
	short rv;
	int err;
#if !defined(_KERNEL) || defined(MENTAT) || defined(__sgi)
	u_32_t s1, s2, sd;
#endif

	if (fin->fin_flx & FI_BAD)
		return -1;

#ifndef IPFILTER_CKSUM
	if ((fin->fin_out == 0) && (fr_checkl4sum(fin) == -1)) {
# if PROXY_DEBUG || !defined(_KERNEL)
		printf("proxy l4 checksum failure\n");
# endif
		if (fin->fin_p == IPPROTO_TCP)
			frstats[fin->fin_out].fr_tcpbad++;
		return -1;
	}
#endif

	aps = nat->nat_aps;
	if ((aps != NULL) && (aps->aps_p == fin->fin_p)) {
		/*
		 * If there is data in this packet to be proxied then try and
		 * get it all into the one buffer, else drop it.
		 */
		if ((fin->fin_dlen > 0) && !(fin->fin_flx & FI_COALESCE))
			if (fr_coalesce(fin) == -1)
				return -1;
		ip = fin->fin_ip;

		switch (fin->fin_p)
		{
		case IPPROTO_TCP :
			tcp = (tcphdr_t *)fin->fin_dp;

#if SOLARIS && defined(_KERNEL) && (SOLARIS2 >= 6) && defined(ICK_VALID)
			m = fin->fin_qfm;
			if (dohwcksum && (m->b_ick_flag == ICK_VALID))
				dosum = 0;
#endif
			/*
			 * Don't bother the proxy with these...or in fact,
			 * should we free up proxy stuff when seen?
			 */
			if ((fin->fin_tcpf & TH_RST) != 0)
				break;
			/*FALLTHROUGH*/
		case IPPROTO_UDP :
			udp = (udphdr_t *)fin->fin_dp;
			break;
		default :
			break;
		}

		apr = aps->aps_apr;
		err = 0;
		if (fin->fin_out != 0) {
			if (apr->apr_outpkt != NULL)
				err = (*apr->apr_outpkt)(fin, aps, nat);
		} else {
			if (apr->apr_inpkt != NULL)
				err = (*apr->apr_inpkt)(fin, aps, nat);
		}

		rv = APR_EXIT(err);
		if (rv == 1) {
#if PROXY_DEBUG || !defined(_KERNEL)
			printf("proxy says bad packet received\n");
#endif
			return -1;
		}
		if (rv == 2) {
#if PROXY_DEBUG || !defined(_KERNEL)
			printf("proxy says free app proxy data\n");
#endif
			appr_free(apr);
			nat->nat_aps = NULL;
			return -1;
		}

		/*
		 * If err != 0 then the data size of the packet has changed
		 * so we need to recalculate the header checksums for the
		 * packet.
		 */
#if !defined(_KERNEL) || defined(MENTAT) || defined(__sgi)
		if (err != 0) {
			short adjlen = err & 0xffff;

			s1 = LONG_SUM(ip->ip_len - adjlen);
			s2 = LONG_SUM(ip->ip_len);
			CALC_SUMD(s1, s2, sd);
			fix_outcksum(fin, &ip->ip_sum, sd);
		}
#endif

		/*
		 * For TCP packets, we may need to adjust the sequence and
		 * acknowledgement numbers to reflect changes in size of the
		 * data stream.
		 *
		 * For both TCP and UDP, recalculate the layer 4 checksum,
		 * regardless, as we can't tell (here) if data has been
		 * changed or not.
		 */
		if (tcp != NULL) {
			err = appr_fixseqack(fin, ip, aps, APR_INC(err));
#if SOLARIS && defined(_KERNEL) && (SOLARIS2 >= 6)
			if (dosum)
				tcp->th_sum = fr_cksum(fin->fin_qfm, ip,
						       IPPROTO_TCP, tcp);
#else
			tcp->th_sum = fr_cksum(fin->fin_m, ip,
					       IPPROTO_TCP, tcp);
#endif
		} else if ((udp != NULL) && (udp->uh_sum != 0)) {
#if SOLARIS && defined(_KERNEL) && (SOLARIS2 >= 6)
			if (dosum)
				udp->uh_sum = fr_cksum(fin->fin_qfm, ip,
						       IPPROTO_UDP, udp);
#else
			udp->uh_sum = fr_cksum(fin->fin_m, ip,
					       IPPROTO_UDP, udp);
#endif
		}
		aps->aps_bytes += fin->fin_plen;
		aps->aps_pkts++;
		return 1;
	}
	return 0;
}


/*
 * Search for an proxy by the protocol it is being used with and its name.
 */
aproxy_t *appr_lookup(pr, name)
u_int pr;
char *name;
{
	aproxy_t *ap;

	for (ap = ap_proxies; ap->apr_p; ap++)
		if ((ap->apr_p == pr) &&
		    !strncmp(name, ap->apr_label, sizeof(ap->apr_label))) {
			ap->apr_ref++;
			return ap;
		}

	for (ap = ap_proxylist; ap; ap = ap->apr_next)
		if ((ap->apr_p == pr) &&
		    !strncmp(name, ap->apr_label, sizeof(ap->apr_label))) {
			ap->apr_ref++;
			return ap;
		}
	return NULL;
}


void appr_free(ap)
aproxy_t *ap;
{
	ap->apr_ref--;
}


void aps_free(aps)
ap_session_t *aps;
{
	ap_session_t *a, **ap;
	aproxy_t *apr;

	if (!aps)
		return;

	for (ap = &ap_sess_list; ((a = *ap) != NULL); ap = &a->aps_next)
		if (a == aps) {
			*ap = a->aps_next;
			break;
		}

	apr = aps->aps_apr;
	if ((apr != NULL) && (apr->apr_del != NULL))
		(*apr->apr_del)(aps);

	if ((aps->aps_data != NULL) && (aps->aps_psiz != 0))
		KFREES(aps->aps_data, aps->aps_psiz);
	KFREE(aps);
}


/*
 * returns 2 if ack or seq number in TCP header is changed, returns 0 otherwise
 */
static int appr_fixseqack(fin, ip, aps, inc)
fr_info_t *fin;
ip_t *ip;
ap_session_t *aps;
int inc;
{
	int sel, ch = 0, out, nlen;
	u_32_t seq1, seq2;
	tcphdr_t *tcp;
	short inc2;

	tcp = (tcphdr_t *)fin->fin_dp;
	out = fin->fin_out;
	/*
	 * ip_len has already been adjusted by 'inc'.
	 */
	nlen = ip->ip_len;
	nlen -= (IP_HL(ip) << 2) + (TCP_OFF(tcp) << 2);

	inc2 = inc;
	inc = (int)inc2;

	if (out != 0) {
		seq1 = (u_32_t)ntohl(tcp->th_seq);
		sel = aps->aps_sel[out];

		/* switch to other set ? */
		if ((aps->aps_seqmin[!sel] > aps->aps_seqmin[sel]) &&
		    (seq1 > aps->aps_seqmin[!sel])) {
#if PROXY_DEBUG
			printf("proxy out switch set seq %d -> %d %x > %x\n",
				sel, !sel, seq1, aps->aps_seqmin[!sel]);
#endif
			sel = aps->aps_sel[out] = !sel;
		}

		if (aps->aps_seqoff[sel]) {
			seq2 = aps->aps_seqmin[sel] - aps->aps_seqoff[sel];
			if (seq1 > seq2) {
				seq2 = aps->aps_seqoff[sel];
				seq1 += seq2;
				tcp->th_seq = htonl(seq1);
				ch = 1;
			}
		}

		if (inc && (seq1 > aps->aps_seqmin[!sel])) {
			aps->aps_seqmin[sel] = seq1 + nlen - 1;
			aps->aps_seqoff[sel] = aps->aps_seqoff[sel] + inc;
#if PROXY_DEBUG
			printf("proxy seq set %d at %x to %d + %d\n", sel,
				aps->aps_seqmin[sel], aps->aps_seqoff[sel],
				inc);
#endif
		}

		/***/

		seq1 = ntohl(tcp->th_ack);
		sel = aps->aps_sel[1 - out];

		/* switch to other set ? */
		if ((aps->aps_ackmin[!sel] > aps->aps_ackmin[sel]) &&
		    (seq1 > aps->aps_ackmin[!sel])) {
#if PROXY_DEBUG
			printf("proxy out switch set ack %d -> %d %x > %x\n",
				sel, !sel, seq1, aps->aps_ackmin[!sel]);
#endif
			sel = aps->aps_sel[1 - out] = !sel;
		}

		if (aps->aps_ackoff[sel] && (seq1 > aps->aps_ackmin[sel])) {
			seq2 = aps->aps_ackoff[sel];
			tcp->th_ack = htonl(seq1 - seq2);
			ch = 1;
		}
	} else {
		seq1 = ntohl(tcp->th_seq);
		sel = aps->aps_sel[out];

		/* switch to other set ? */
		if ((aps->aps_ackmin[!sel] > aps->aps_ackmin[sel]) &&
		    (seq1 > aps->aps_ackmin[!sel])) {
#if PROXY_DEBUG
			printf("proxy in switch set ack %d -> %d %x > %x\n",
				sel, !sel, seq1, aps->aps_ackmin[!sel]);
#endif
			sel = aps->aps_sel[out] = !sel;
		}

		if (aps->aps_ackoff[sel]) {
			seq2 = aps->aps_ackmin[sel] - aps->aps_ackoff[sel];
			if (seq1 > seq2) {
				seq2 = aps->aps_ackoff[sel];
				seq1 += seq2;
				tcp->th_seq = htonl(seq1);
				ch = 1;
			}
		}

		if (inc && (seq1 > aps->aps_ackmin[!sel])) {
			aps->aps_ackmin[!sel] = seq1 + nlen - 1;
			aps->aps_ackoff[!sel] = aps->aps_ackoff[sel] + inc;
#if PROXY_DEBUG
			printf("proxy ack set %d at %x to %d + %d\n", !sel,
				aps->aps_seqmin[!sel], aps->aps_seqoff[sel],
				inc);
#endif
		}

		/***/

		seq1 = ntohl(tcp->th_ack);
		sel = aps->aps_sel[1 - out];

		/* switch to other set ? */
		if ((aps->aps_seqmin[!sel] > aps->aps_seqmin[sel]) &&
		    (seq1 > aps->aps_seqmin[!sel])) {
#if PROXY_DEBUG
			printf("proxy in switch set seq %d -> %d %x > %x\n",
				sel, !sel, seq1, aps->aps_seqmin[!sel]);
#endif
			sel = aps->aps_sel[1 - out] = !sel;
		}

		if (aps->aps_seqoff[sel] != 0) {
#if PROXY_DEBUG
			printf("sel %d seqoff %d seq1 %x seqmin %x\n", sel,
				aps->aps_seqoff[sel], seq1,
				aps->aps_seqmin[sel]);
#endif
			if (seq1 > aps->aps_seqmin[sel]) {
				seq2 = aps->aps_seqoff[sel];
				tcp->th_ack = htonl(seq1 - seq2);
				ch = 1;
			}
		}
	}
#if PROXY_DEBUG
	printf("appr_fixseqack: seq %lx ack %lx\n", ntohl(tcp->th_seq),
		ntohl(tcp->th_ack));
#endif
	return ch ? 2 : 0;
}


/*
 * Initialise hook for kernel application proxies.
 * Call the initialise routine for all the compiled in kernel proxies.
 */
int appr_init()
{
	aproxy_t *ap;
	int err = 0;

	for (ap = ap_proxies; ap->apr_p; ap++) {
		if (ap->apr_init != NULL) {
			err = (*ap->apr_init)();
			if (err != 0)
				break;
		}
	}
	return err;
}


/*
 * Unload hook for kernel application proxies.
 * Call the finialise routine for all the compiled in kernel proxies.
 */
void appr_unload()
{
	aproxy_t *ap;

	for (ap = ap_proxies; ap->apr_p; ap++)
		if (ap->apr_fini != NULL)
			(*ap->apr_fini)();
	for (ap = ap_proxylist; ap; ap = ap->apr_next)
		if (ap->apr_fini != NULL)
			(*ap->apr_fini)();
}
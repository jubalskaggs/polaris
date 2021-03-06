/*
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * No ISC copyright for this file.
 */

#pragma ident	"@(#)res_sendsigned.c	1.3	04/09/28 SMI"

#include "port_before.h"
#include "fd_setsize.h"

#include <sys/types.h>
#include <sys/param.h>

#include <netinet/in.h>
#include <arpa/nameser.h>
#include <arpa/inet.h>

#include <isc/dst.h>

#include <errno.h>
#include <netdb.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "port_after.h"

#define DEBUG
#include "res_debug.h"


/* res_nsendsigned */
int
res_nsendsigned(res_state statp, const u_char *msg, int msglen,
		ns_tsig_key *key, u_char *answer, int anslen)
{
	res_state nstatp;
	DST_KEY *dstkey;
	int usingTCP = 0;
	u_char *newmsg;
	int newmsglen, bufsize, siglen;
	u_char sig[64];
	HEADER *hp;
	time_t tsig_time;
	int ret;

	dst_init();

	nstatp = (res_state) malloc(sizeof(*statp));
	if (nstatp == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	memcpy(nstatp, statp, sizeof(*statp));

	bufsize = msglen + 1024;
	newmsg = (u_char *) malloc(bufsize);
	if (newmsg == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	memcpy(newmsg, msg, msglen);
	newmsglen = msglen;

	if (ns_samename(key->alg, NS_TSIG_ALG_HMAC_MD5) != 1)
		dstkey = NULL;
	else
		dstkey = dst_buffer_to_key(key->name, KEY_HMAC_MD5,
					   NS_KEY_TYPE_AUTH_ONLY,
					   NS_KEY_PROT_ANY,
					   key->data, key->len);
	if (dstkey == NULL) {
		errno = EINVAL;
		free(nstatp);
		free(newmsg);
		return (-1);
	}

	nstatp->nscount = 1;
	siglen = sizeof(sig);
	ret = ns_sign(newmsg, &newmsglen, bufsize, NOERROR, dstkey, NULL, 0,
		      sig, &siglen, 0);
	if (ret < 0) {
		free (nstatp);
		free (newmsg);
		dst_free_key(dstkey);
		if (ret == NS_TSIG_ERROR_NO_SPACE)
			errno  = EMSGSIZE;
		else if (ret == -1)
			errno  = EINVAL;
		return (ret);
	}

	if (newmsglen > PACKETSZ || (nstatp->options & RES_IGNTC))
		usingTCP = 1;
	if (usingTCP == 0)
		nstatp->options |= RES_IGNTC;
	else
		nstatp->options |= RES_USEVC;

retry:

	ret = res_nsend(nstatp, newmsg, newmsglen, answer, anslen);
	if (ret < 0) {
		free (nstatp);
		free (newmsg);
		dst_free_key(dstkey);
		return (ret);
	}

	anslen = ret;
	ret = ns_verify(answer, &anslen, dstkey, sig, siglen,
			NULL, NULL, &tsig_time, nstatp->options & RES_KEEPTSIG);
	if (ret != 0) {
#ifdef	ORIGINAL_ISC_CODE
		Dprint(nstatp->pfcode & RES_PRF_REPLY,
		       (stdout, ";; TSIG invalid (%s)\n", p_rcode(ret)));
#else
		Dprint(nstatp->pfcode & RES_PRF_REPLY,
		       (stdout, ";; TSIG invalid (%s)\n", p_rcode(-ret)));
#endif
		free (nstatp);
		free (newmsg);
		dst_free_key(dstkey);
		if (ret == -1)
			errno = EINVAL;
		else
			errno = ENOTTY;
		return (-1);
	}
	Dprint(nstatp->pfcode & RES_PRF_REPLY, (stdout, ";; TSIG ok\n"));

	hp = (HEADER *) answer;
	if (hp->tc && usingTCP == 0) {
		nstatp->options &= ~RES_IGNTC;
		usingTCP = 1;
		goto retry;
	}

	free (nstatp);
	free (newmsg);
	dst_free_key(dstkey);
	return (anslen);
}

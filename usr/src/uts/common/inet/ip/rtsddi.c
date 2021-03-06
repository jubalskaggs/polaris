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
 * Copyright 1992-2002 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)rtsddi.c	1.25	05/06/08 SMI"

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <inet/common.h>
#include <inet/ip.h>

#define	INET_NAME	"rts"
#define	INET_STRTAB	rtsinfo
#define	INET_MODDESC	"PF_ROUTE socket STREAMS module 1.25"
#define	INET_DEVDESC	"PF_ROUTE socket STREAMS driver 1.25"
#define	INET_DEVMINOR	IPV4_MINOR
#define	INET_DEVMTFLAGS	IP_DEVMTFLAGS	/* since as a driver we're ip */
#define	INET_MODMTFLAGS	(D_MP|D_MTQPAIR|D_MTOUTPERIM|D_MTOCEXCL|D_SYNCSTR)

#include "../inetddi.c"

extern void rts_ddi_init(void);

int
_init(void)
{
	INET_BECOME_IP();

	/*
	 * Note: After mod_install succeeds, another thread can enter
	 * therefore all initialization is done before it.
	 */
	rts_ddi_init();
	return (mod_install(&modlinkage));
}

int
_fini(void)
{
	return (mod_remove(&modlinkage));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

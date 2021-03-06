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
/* Copyright (c) 1990 Mentat Inc. */

#pragma ident	"@(#)tcpddi.c	1.53	05/10/22 SMI"

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <inet/common.h>
#include <inet/ip.h>

#define	INET_NAME	"tcp"
#define	INET_STRTAB	tcpinfo
#define	INET_DEVDESC	"TCP STREAMS driver 1.53"
#define	INET_MODDESC	"TCP STREAMS module 1.53"
#define	INET_DEVMINOR	TCP_MINOR
/*
 * Note that unlike UDP, TCP uses synchronous STREAMS only
 * for TCP Fusion (loopback); this is why we don't define
 * D_SYNCSTR here.  Since TCP as a module is used only for
 * SNMP purposes, we define _D_DIRECT for device instance.
 */
#define	INET_DEVMTFLAGS	(D_MP|_D_DIRECT)
#define	INET_MODMTFLAGS	D_MP

#include "../inetddi.c"

int
_init(void)
{
	/*
	 * device initialization occurs in ipddi.c:_init()
	 * (i.e. it must be called before this routine)
	 */
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

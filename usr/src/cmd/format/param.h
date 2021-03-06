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
 * Copyright (c) 1991-2001 by Sun Microsystems, Inc.
 */

#ifndef	_PARAM_H
#define	_PARAM_H

#pragma ident	"@(#)param.h	1.9	05/06/08 SMI"

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * This file contains declarations of miscellaneous parameters.
 */
#ifndef	SECSIZE
#define	SECSIZE		DEV_BSIZE
#endif

#define	MAX_CYLS	(0x10000 - 1)		/* max legal cylinder count */
#define	MAX_HEADS	(0x10000 - 1)		/* max legal head count */
#define	MAX_SECTS	(0x10000 - 1)		/* max legal sector count */

#define	MIN_RPM		2000			/* min legal rpm */
#define	AVG_RPM		3600			/* default rpm */
#define	MAX_RPM		76000			/* max legal rpm */

#define	MIN_BPS		512			/* min legal bytes/sector */
#define	AVG_BPS		600			/* default bytes/sector */
#define	MAX_BPS		1000			/* max legal bytes/sector */

#define	INFINITY	0x7fffffff		/* a big number */

#ifdef	__cplusplus
}
#endif

#endif	/* _PARAM_H */

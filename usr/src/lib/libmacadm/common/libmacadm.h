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

#ifndef _LIBMACADM_H
#define	_LIBMACADM_H

#pragma ident	"@(#)libmacadm.h	1.3	05/07/30 SMI"

#include <sys/types.h>
#include <sys/mac.h>

#ifdef	__cplusplus
extern "C" {
#endif

extern int	macadm_walk(void (*)(void *, const char *),
    void *, boolean_t);

#ifdef	__cplusplus
}
#endif

#endif	/* _LIBMACADM_H */

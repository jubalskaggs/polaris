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
 * Copyright 1991 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)mincore.c	1.3	05/06/08 SMI"

/* mincore.c  SMI 12/14/90  */
#include <errno.h>
#include <syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define INCORE 1;	/* return only the incore status bit */

extern int errno;

int mincore(addr, len, vec)
caddr_t	addr;
int	len;
char	*vec;
{
	int i;

	if (len <0) {
		errno = EINVAL;
		return(-1);
	}
		
	if(_syscall(SYS_mincore, addr, len, vec) == 0) {
		for (i=0; i< len/getpagesize(); i++) {
			vec[i] &= INCORE;
		}
	}
}

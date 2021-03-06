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
 * Copyright (c) 1999 by Sun Microsystems, Inc.
 * All rights reserved.
 */

#pragma ident	"@(#)readv.c	1.6	05/06/08 SMI"

#include "../common/compat.h"
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <syscall.h>
#include <sys/uio.h>

/*
 * If reading from the utmp file, map the data to the SunOS 4.1
 * format on the fly. 
 */
extern void to_utmp(char *, char *, int);

int
readv(int fd, struct iovec *iov, int iovcnt)
{
	return (bc_readv(fd, iov, iovcnt));
}

int
bc_readv(int fd, struct iovec *iov, int iovcnt)
{
	int ret, off;
	int i, size, total = 0;
	char *nbuf;
	
	if (fd_get(fd) != -1) {
		for (i = 0; i < iovcnt; i++) {
			size = getmodsize(iov[i].iov_len,
			    sizeof (struct compat_utmp),
			    sizeof (struct utmpx));

			if ((nbuf = (void *)malloc(size)) == NULL) {
				fprintf(stderr, "readv: malloc failed\n");
				exit(-1);
			}
		
			if ((ret = _read(fd, nbuf, size)) == -1) {
				free(nbuf);
				return (-1);
			}

			total += ret;

			to_utmp(iov[i].iov_base, nbuf, ret);

			ret = getmodsize(ret, sizeof (struct utmpx), 
			    sizeof (struct compat_utmp));

			free(nbuf);
		}
		return (total);
	}

	return (_readv(fd, iov, iovcnt));
}

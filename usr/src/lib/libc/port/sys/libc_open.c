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

/*	Copyright (c) 1988 AT&T	*/
/*	  All Rights Reserved  	*/

#pragma ident	"@(#)libc_open.c	1.14	05/11/23 SMI"

#include "synonyms.h"
#include <sys/mkdev.h>
#include <limits.h>
#include <stdarg.h>
#include <unistd.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/stropts.h>
#include <sys/stream.h>
#include <sys/ptms.h>

#if !defined(_LP64)
extern int __open64(const char *fname, int oflag, mode_t mode);
#endif

extern	int __xpg4; /* defined in port/gen/xpg4.c; 0 if not xpg4/xpg4v2 */

extern int __open(const char *fname, int oflag, mode_t mode);

static void push_module(int fd);
static int isptsfd(int fd);
static void itoa(int i, char *ptr);

int
_open(const char *fname, int oflag, ...)
{
	mode_t mode;
	int fd;
	va_list ap;

	va_start(ap, oflag);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	/*
	 * XPG4v2 requires that open of a slave pseudo terminal device
	 * provides the process with an interface that is identical to
	 * the terminal interface. For a more detailed discussion,
	 * see bugid 4025044.
	 */
	fd = __open(fname, oflag, mode);
	if (__xpg4 != 0 && fd >= 0 && isptsfd(fd))
		push_module(fd);
	return (fd);
}

#if !defined(_LP64)
/*
 * The 32-bit APIs to large files require this interposition.
 * The 64-bit APIs just fall back to _open() above.
 */
int
_open64(const char *fname, int oflag, ...)
{
	mode_t mode;
	int fd;
	va_list ap;

	va_start(ap, oflag);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	/*
	 * XPG4v2 requires that open of a slave pseudo terminal device
	 * provides the process with an interface that is identical to
	 * the terminal interface. For a more detailed discussion,
	 * see bugid 4025044.
	 */
	fd = __open64(fname, oflag, mode);
	if (__xpg4 != 0 && fd >= 0 && isptsfd(fd))
		push_module(fd);
	return (fd);
}
#endif	/* !_LP64 */

/*
 * Check if the file matches an entry in the /dev/pts directory.
 * Be careful to preserve errno.
 */
static int
isptsfd(int fd)
{
	char buf[TTYNAME_MAX];
	struct stat64 fsb, stb;
	int oerrno = errno;
	int rval = 0;

	if (fstat64(fd, &fsb) == 0 && S_ISCHR(fsb.st_mode)) {
		(void) strcpy(buf, "/dev/pts/");
		itoa(minor(fsb.st_rdev), buf+strlen(buf));
		if (stat64(buf, &stb) == 0)
			rval = (stb.st_rdev == fsb.st_rdev);
	}
	errno = oerrno;
	return (rval);
}

/*
 * Converts a number to a string (null terminated).
 */
static void
itoa(int i, char *ptr)
{
	int dig = 0;
	int tempi;

	tempi = i;
	do {
		dig++;
		tempi /= 10;
	} while (tempi);

	ptr += dig;
	*ptr = '\0';
	while (--dig >= 0) {
		*(--ptr) = i % 10 + '0';
		i /= 10;
	}
}

/*
 * Push modules to provide tty semantics
 */
static void
push_module(int fd)
{
	struct strioctl istr;
	int oerrno = errno;

	istr.ic_cmd = PTSSTTY;
	istr.ic_len = 0;
	istr.ic_timout = 0;
	istr.ic_dp = NULL;
	if (ioctl(fd, I_STR, &istr) != -1) {
		(void) ioctl(fd, __I_PUSH_NOCTTY, "ptem");
		(void) ioctl(fd, __I_PUSH_NOCTTY, "ldterm");
		(void) ioctl(fd, __I_PUSH_NOCTTY, "ttcompat");
		istr.ic_cmd = PTSSTTY;
		istr.ic_len = 0;
		istr.ic_timout = 0;
		istr.ic_dp = NULL;
		(void) ioctl(fd, I_STR, &istr);
	}
	errno = oerrno;
}

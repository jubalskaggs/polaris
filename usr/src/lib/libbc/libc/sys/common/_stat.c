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

#pragma ident	"@(#)_stat.c	1.12	05/06/08 SMI"

#include <sys/errno.h>
#include <sys/syscall.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/param.h>
#include "compat.h"
#include "s5sysmacros.h"

#define ST_FSTYPSZ	16	/* array size for file system type name */

struct ts {
	long	tv_sec;		/* seconds */
	long	tv_nsec;	/* nanoseconds */
};

struct n_stat {
        unsigned long   st_dev;
        long            st_pad1[3];     /* reserved for network id */
        unsigned long   st_ino;
        unsigned long   st_mode;
        unsigned long   st_nlink;
        long            st_uid;
        long            st_gid;
        unsigned long   st_rdev;
        long            st_pad2[2];
        long            st_size;
        long            st_pad3;        /* future off_t expansion */
        struct ts       st_atim;
        struct ts       st_mtim;
        struct ts       st_ctim;
        long            st_blksize;
        long            st_blocks;
        char            st_fstype[ST_FSTYPSZ];
        long            st_pad4[8];     /* expansion area */
 
};

static void cpstatbuf(struct stat *, struct n_stat *);

int
fstat(int fd, struct stat *buf)
{
	return (bc_fstat(fd, buf));
}

int
bc_fstat(int fd, struct stat *buf)
{
	int ret;
	struct n_stat nb;
	extern int errno;	

	if (buf == 0) {
		errno = EFAULT;
		return (-1);
	}
		
	if ((ret = _syscall(SYS_fstat, fd, &nb)) == -1)
		return (ret);

	cpstatbuf(buf, &nb);
	if (fd_get(fd) != -1) {
		buf->st_size = getmodsize(buf->st_size, 
			sizeof (struct utmpx), sizeof(struct compat_utmp));
	}

	return (ret);
}

int
stat_com(int sysnum, char *path, struct stat *buf)
{
	int fd, ret;
	struct n_stat nb;

	if (strcmp(path, "/etc/mtab") == 0) {
/*
 * stat the real mnttab, or the "parsed" mtab
 * created by open?
 *
 * for now, stat the real mnttab.
 */

/*
 *		fd = open_mnt("/etc/mnttab", "mtab", O_RDONLY);
 *		ret = fstat(fd, buf);
 *		close(fd);
 *		return(ret);
 */
		ret = stat_com(sysnum, "/etc/mnttab", buf);
		return(ret);
	} else if (strcmp(path, "/etc/fstab") == 0) {
		fd = open_mnt("/etc/vfstab", "fstab", O_RDONLY);
		ret = fstat(fd, buf);
		close(fd);
		return(ret);
	} else if (strcmp(path, "/var/adm/utmp") == 0 ||
	    strcmp(path, "/var/adm/wtmp") == 0 ||
	    strcmp(path, "/etc/utmp") == 0) {
		char pathx[MAXPATHLEN+100];
		if (strcmp(path, "/etc/utmp") == 0)
			strcpy(pathx, "/var/adm/utmpx");
		else {
			strcpy(pathx, path);
			strcat(pathx, "x");
		}
		if ((ret = _syscall(sysnum, pathx, &nb)) == -1)
			return(-1);
		else {
			buf->st_size = getmodsize(buf->st_size, 
		       	    sizeof(struct compat_utmp), sizeof(struct utmpx));
			cpstatbuf(buf, &nb);
			return(ret);
		}
	} else if (_strstr(path, "/lib/locale/") != 0) {
		fd = open(path, O_RDONLY);
		ret = fstat(fd, buf);
		close(fd);
		return(ret);
	} else {
		if ((ret = _syscall(sysnum, path, &nb)) != -1)
			cpstatbuf(buf, &nb);
		return(ret);
	}
}


/*
 * Common code to copy xstat buf to BSD style buf
 */
static void
cpstatbuf(struct stat *bsdbuf, struct n_stat *nbuf)
{
        bsdbuf->st_dev = (dev_t) cmpdev(nbuf->st_dev);
        bsdbuf->st_ino = nbuf->st_ino;
        bsdbuf->st_mode = (unsigned short) nbuf->st_mode;
        bsdbuf->st_nlink = (short) nbuf->st_nlink;

	if ((unsigned long)nbuf->st_uid > 0xffff)
		bsdbuf->st_uid = 60001;	/* UID_NOBODY */
	else
        	bsdbuf->st_uid = (uid_t) nbuf->st_uid;

	if ((unsigned long)nbuf->st_gid > 0xffff)
		bsdbuf->st_gid = 60001;	/* GID_NOBODY */
	else
        	bsdbuf->st_gid = (gid_t) nbuf->st_gid;

        bsdbuf->st_rdev = (dev_t) cmpdev(nbuf->st_rdev);
        bsdbuf->st_size = nbuf->st_size;
        bsdbuf->st_atime = nbuf->st_atim.tv_sec;
        bsdbuf->st_mtime = nbuf->st_mtim.tv_sec;
        bsdbuf->st_ctime = nbuf->st_ctim.tv_sec;
        bsdbuf->st_blksize = nbuf->st_blksize;
        bsdbuf->st_blocks = nbuf->st_blocks;
}

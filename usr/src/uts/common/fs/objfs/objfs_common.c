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
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)objfs_common.c	1.2	05/06/08 SMI"

#include <sys/errno.h>
#include <sys/file.h>
#include <sys/objfs.h>
#include <sys/objfs_impl.h>

/*
 * For directories, make sure we are using large-file aware interfaces and we
 * aren't trying to open it writeable.
 */
/* ARGSUSED */
int
objfs_dir_open(vnode_t **cpp, int flag, cred_t *cr)
{
	if ((flag & (FOFFMAX | FWRITE)) != FOFFMAX)
		return (EINVAL);

	return (0);
}

/*
 * For all vnodes which have no cleanup to do at close time.
 */
/* ARGSUSED */
int
objfs_common_close(vnode_t *vp, int flag, int count, offset_t off, cred_t *cr)
{
	return (0);
}

/*
 * For directories, ensure we're not open for writting.
 */
/* ARGSUSED */
int
objfs_dir_access(vnode_t *vp, int mode, int flags, cred_t *cr)
{
	if (mode & VWRITE)
		return (EACCES);

	return (0);
}

/*
 * Fills in common fields for getattr().
 */
int
objfs_common_getattr(vnode_t *vp, vattr_t *vap)
{
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_rdev = 0;
	vap->va_blksize = DEV_BSIZE;
	vap->va_nblocks = howmany(vap->va_size, vap->va_blksize);
	vap->va_seq = 0;
	vap->va_fsid = vp->v_vfsp->vfs_dev;

	return (0);
}

/*
 * Returns the number objects currently loaded in the system.
 */
int
objfs_nobjs(void)
{
	int count = 0;
	struct modctl *mp;

	mutex_enter(&mod_lock);
	mp = &modules;

	do {
		if (mp->mod_loaded)
			count++;
	} while ((mp = mp->mod_next) != &modules);

	mutex_exit(&mod_lock);

	return (count);
}

/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
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
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)zfs_ctldir.c	1.10	06/07/22 SMI"

/*
 * ZFS control directory (a.k.a. ".zfs")
 *
 * This directory provides a common location for all ZFS meta-objects.
 * Currently, this is only the 'snapshot' directory, but this may expand in the
 * future.  The elements are built using the GFS primitives, as the hierarchy
 * does not actually exist on disk.
 *
 * For 'snapshot', we don't want to have all snapshots always mounted, because
 * this would take up a huge amount of space in /etc/mnttab.  We have three
 * types of objects:
 *
 * 	ctldir ------> snapshotdir -------> snapshot
 *                                             |
 *                                             |
 *                                             V
 *                                         mounted fs
 *
 * The 'snapshot' node contains just enough information to lookup '..' and act
 * as a mountpoint for the snapshot.  Whenever we lookup a specific snapshot, we
 * perform an automount of the underlying filesystem and return the
 * corresponding vnode.
 *
 * All mounts are handled automatically by the kernel, but unmounts are
 * (currently) handled from user land.  The main reason is that there is no
 * reliable way to auto-unmount the filesystem when it's "no longer in use".
 * When the user unmounts a filesystem, we call zfsctl_unmount(), which
 * unmounts any snapshots within the snapshot directory.
 */

#include <fs/fs_subr.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>
#include <sys/gfs.h>
#include <sys/stat.h>
#include <sys/dmu.h>
#include <sys/mount.h>

typedef struct {
	char		*se_name;
	vnode_t		*se_root;
	avl_node_t	se_node;
} zfs_snapentry_t;

static int
snapentry_compare(const void *a, const void *b)
{
	const zfs_snapentry_t *sa = a;
	const zfs_snapentry_t *sb = b;
	int ret = strcmp(sa->se_name, sb->se_name);

	if (ret < 0)
		return (-1);
	else if (ret > 0)
		return (1);
	else
		return (0);
}

vnodeops_t *zfsctl_ops_root;
vnodeops_t *zfsctl_ops_snapdir;
vnodeops_t *zfsctl_ops_snapshot;

static const fs_operation_def_t zfsctl_tops_root[];
static const fs_operation_def_t zfsctl_tops_snapdir[];
static const fs_operation_def_t zfsctl_tops_snapshot[];

static vnode_t *zfsctl_mknode_snapdir(vnode_t *);
static vnode_t *zfsctl_snapshot_mknode(vnode_t *, uint64_t objset);

static gfs_opsvec_t zfsctl_opsvec[] = {
	{ ".zfs", zfsctl_tops_root, &zfsctl_ops_root },
	{ ".zfs/snapshot", zfsctl_tops_snapdir, &zfsctl_ops_snapdir },
	{ ".zfs/snapshot/vnode", zfsctl_tops_snapshot, &zfsctl_ops_snapshot },
	{ NULL }
};

typedef struct zfsctl_node {
	gfs_dir_t	zc_gfs_private;
	uint64_t	zc_id;
	timestruc_t	zc_cmtime;	/* ctime and mtime, always the same */
} zfsctl_node_t;

typedef struct zfsctl_snapdir {
	zfsctl_node_t	sd_node;
	kmutex_t	sd_lock;
	avl_tree_t	sd_snaps;
} zfsctl_snapdir_t;

/*
 * Root directory elements.  We have only a single static entry, 'snapshot'.
 */
static gfs_dirent_t zfsctl_root_entries[] = {
	{ "snapshot", zfsctl_mknode_snapdir, GFS_CACHE_VNODE },
	{ NULL }
};

/* include . and .. in the calculation */
#define	NROOT_ENTRIES	((sizeof (zfsctl_root_entries) / \
    sizeof (gfs_dirent_t)) + 1)


/*
 * Initialize the various GFS pieces we'll need to create and manipulate .zfs
 * directories.  This is called from the ZFS init routine, and initializes the
 * vnode ops vectors that we'll be using.
 */
void
zfsctl_init(void)
{
	VERIFY(gfs_make_opsvec(zfsctl_opsvec) == 0);
}

void
zfsctl_fini(void)
{
	/*
	 * Remove vfsctl vnode ops
	 */
	if (zfsctl_ops_root)
		vn_freevnodeops(zfsctl_ops_root);
	if (zfsctl_ops_snapdir)
		vn_freevnodeops(zfsctl_ops_snapdir);
	if (zfsctl_ops_snapshot)
		vn_freevnodeops(zfsctl_ops_snapshot);

	zfsctl_ops_root = NULL;
	zfsctl_ops_snapdir = NULL;
	zfsctl_ops_snapshot = NULL;
}

/*
 * Return the inode number associated with the 'snapshot' directory.
 */
/* ARGSUSED */
static ino64_t
zfsctl_root_inode_cb(vnode_t *vp, int index)
{
	ASSERT(index == 0);
	return (ZFSCTL_INO_SNAPDIR);
}

/*
 * Create the '.zfs' directory.  This directory is cached as part of the VFS
 * structure.  This results in a hold on the vfs_t.  The code in zfs_umount()
 * therefore checks against a vfs_count of 2 instead of 1.  This reference
 * is removed when the ctldir is destroyed in the unmount.
 */
void
zfsctl_create(zfsvfs_t *zfsvfs)
{
	vnode_t *vp, *rvp;
	zfsctl_node_t *zcp;

	ASSERT(zfsvfs->z_ctldir == NULL);

	vp = gfs_root_create(sizeof (zfsctl_node_t), zfsvfs->z_vfs,
	    zfsctl_ops_root, ZFSCTL_INO_ROOT, zfsctl_root_entries,
	    zfsctl_root_inode_cb, MAXNAMELEN, NULL, NULL);
	zcp = vp->v_data;
	zcp->zc_id = ZFSCTL_INO_ROOT;

	VERIFY(VFS_ROOT(zfsvfs->z_vfs, &rvp) == 0);
	ZFS_TIME_DECODE(&zcp->zc_cmtime, VTOZ(rvp)->z_phys->zp_crtime);
	VN_RELE(rvp);

	/*
	 * We're only faking the fact that we have a root of a filesystem for
	 * the sake of the GFS interfaces.  Undo the flag manipulation it did
	 * for us.
	 */
	vp->v_flag &= ~(VROOT | VNOCACHE | VNOMAP | VNOSWAP | VNOMOUNT);

	zfsvfs->z_ctldir = vp;
}

/*
 * Destroy the '.zfs' directory.  Only called when the filesystem is unmounted.
 * There might still be more references if we were force unmounted, but only
 * new zfs_inactive() calls can occur and they don't reference .zfs
 */
void
zfsctl_destroy(zfsvfs_t *zfsvfs)
{
	VN_RELE(zfsvfs->z_ctldir);
	zfsvfs->z_ctldir = NULL;
}

/*
 * Given a root znode, retrieve the associated .zfs directory.
 * Add a hold to the vnode and return it.
 */
vnode_t *
zfsctl_root(znode_t *zp)
{
	ASSERT(zfs_has_ctldir(zp));
	VN_HOLD(zp->z_zfsvfs->z_ctldir);
	return (zp->z_zfsvfs->z_ctldir);
}

/*
 * Common open routine.  Disallow any write access.
 */
/* ARGSUSED */
static int
zfsctl_common_open(vnode_t **vpp, int flags, cred_t *cr)
{
	if (flags & FWRITE)
		return (EACCES);

	return (0);
}

/*
 * Common close routine.  Nothing to do here.
 */
/* ARGSUSED */
static int
zfsctl_common_close(vnode_t *vpp, int flags, int count, offset_t off,
    cred_t *cr)
{
	return (0);
}

/*
 * Common access routine.  Disallow writes.
 */
/* ARGSUSED */
static int
zfsctl_common_access(vnode_t *vp, int mode, int flags, cred_t *cr)
{
	if (mode & VWRITE)
		return (EACCES);

	return (0);
}

/*
 * Common getattr function.  Fill in basic information.
 */
static void
zfsctl_common_getattr(vnode_t *vp, vattr_t *vap)
{
	zfsctl_node_t	*zcp = vp->v_data;
	timestruc_t	now;

	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_rdev = 0;
	/*
	 * We are a purly virtual object, so we have no
	 * blocksize or allocated blocks.
	 */
	vap->va_blksize = 0;
	vap->va_nblocks = 0;
	vap->va_seq = 0;
	vap->va_fsid = vp->v_vfsp->vfs_dev;
	vap->va_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP |
	    S_IROTH | S_IXOTH;
	vap->va_type = VDIR;
	/*
	 * We live in the now (for atime).
	 */
	gethrestime(&now);
	vap->va_atime = now;
	vap->va_mtime = vap->va_ctime = zcp->zc_cmtime;
}

static int
zfsctl_common_fid(vnode_t *vp, fid_t *fidp)
{
	zfsvfs_t	*zfsvfs = vp->v_vfsp->vfs_data;
	zfsctl_node_t	*zcp = vp->v_data;
	uint64_t	object = zcp->zc_id;
	zfid_short_t	*zfid;
	int		i;

	ZFS_ENTER(zfsvfs);

	if (fidp->fid_len < SHORT_FID_LEN) {
		fidp->fid_len = SHORT_FID_LEN;
		ZFS_EXIT(zfsvfs);
		return (ENOSPC);
	}

	zfid = (zfid_short_t *)fidp;

	zfid->zf_len = SHORT_FID_LEN;

	for (i = 0; i < sizeof (zfid->zf_object); i++)
		zfid->zf_object[i] = (uint8_t)(object >> (8 * i));

	/* .zfs znodes always have a generation number of 0 */
	for (i = 0; i < sizeof (zfid->zf_gen); i++)
		zfid->zf_gen[i] = 0;

	ZFS_EXIT(zfsvfs);
	return (0);
}

/*
 * .zfs inode namespace
 *
 * We need to generate unique inode numbers for all files and directories
 * within the .zfs pseudo-filesystem.  We use the following scheme:
 *
 * 	ENTRY			ZFSCTL_INODE
 * 	.zfs			1
 * 	.zfs/snapshot		2
 * 	.zfs/snapshot/<snap>	objectid(snap)
 */

#define	ZFSCTL_INO_SNAP(id)	(id)

/*
 * Get root directory attributes.
 */
/* ARGSUSED */
static int
zfsctl_root_getattr(vnode_t *vp, vattr_t *vap, int flags, cred_t *cr)
{
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;

	ZFS_ENTER(zfsvfs);
	vap->va_nodeid = ZFSCTL_INO_ROOT;
	vap->va_nlink = vap->va_size = NROOT_ENTRIES;

	zfsctl_common_getattr(vp, vap);
	ZFS_EXIT(zfsvfs);

	return (0);
}

/*
 * Special case the handling of "..".
 */
/* ARGSUSED */
int
zfsctl_root_lookup(vnode_t *dvp, char *nm, vnode_t **vpp, pathname_t *pnp,
    int flags, vnode_t *rdir, cred_t *cr)
{
	zfsvfs_t *zfsvfs = dvp->v_vfsp->vfs_data;
	int err;

	ZFS_ENTER(zfsvfs);

	if (strcmp(nm, "..") == 0) {
		err = VFS_ROOT(dvp->v_vfsp, vpp);
	} else {
		err = gfs_dir_lookup(dvp, nm, vpp);
	}

	ZFS_EXIT(zfsvfs);

	return (err);
}

static const fs_operation_def_t zfsctl_tops_root[] = {
	{ VOPNAME_OPEN,		zfsctl_common_open			},
	{ VOPNAME_CLOSE,	zfsctl_common_close			},
	{ VOPNAME_IOCTL,	fs_inval				},
	{ VOPNAME_GETATTR,	zfsctl_root_getattr			},
	{ VOPNAME_ACCESS,	zfsctl_common_access			},
	{ VOPNAME_READDIR,	gfs_vop_readdir				},
	{ VOPNAME_LOOKUP,	zfsctl_root_lookup			},
	{ VOPNAME_SEEK,		fs_seek					},
	{ VOPNAME_INACTIVE,	(fs_generic_func_p) gfs_vop_inactive	},
	{ VOPNAME_FID,		zfsctl_common_fid			},
	{ NULL }
};

static int
zfsctl_snapshot_zname(vnode_t *vp, const char *name, int len, char *zname)
{
	objset_t *os = ((zfsvfs_t *)((vp)->v_vfsp->vfs_data))->z_os;

	dmu_objset_name(os, zname);
	if (strlen(zname) + 1 + strlen(name) >= len)
		return (ENAMETOOLONG);
	(void) strcat(zname, "@");
	(void) strcat(zname, name);
	return (0);
}

static int
zfsctl_unmount_snap(vnode_t *dvp, const char *name, int force, cred_t *cr)
{
	zfsctl_snapdir_t *sdp = dvp->v_data;
	zfs_snapentry_t search, *sep;
	avl_index_t where;
	int err;

	ASSERT(MUTEX_HELD(&sdp->sd_lock));

	search.se_name = (char *)name;
	if ((sep = avl_find(&sdp->sd_snaps, &search, &where)) == NULL)
		return (ENOENT);

	ASSERT(vn_ismntpt(sep->se_root));

	/* this will be dropped by dounmount() */
	if ((err = vn_vfswlock(sep->se_root)) != 0)
		return (err);

	VN_HOLD(sep->se_root);
	err = dounmount(vn_mountedvfs(sep->se_root), force, kcred);
	if (err) {
		VN_RELE(sep->se_root);
		return (err);
	}
	ASSERT(sep->se_root->v_count == 1);
	gfs_vop_inactive(sep->se_root, cr);

	avl_remove(&sdp->sd_snaps, sep);
	kmem_free(sep->se_name, strlen(sep->se_name) + 1);
	kmem_free(sep, sizeof (zfs_snapentry_t));

	return (0);
}


static void
zfsctl_rename_snap(zfsctl_snapdir_t *sdp, zfs_snapentry_t *sep, const char *nm)
{
	avl_index_t where;
	vfs_t *vfsp;
	refstr_t *pathref;
	char newpath[MAXNAMELEN];
	char *tail;

	ASSERT(MUTEX_HELD(&sdp->sd_lock));
	ASSERT(sep != NULL);

	vfsp = vn_mountedvfs(sep->se_root);
	ASSERT(vfsp != NULL);

	vfs_lock_wait(vfsp);

	/*
	 * Change the name in the AVL tree.
	 */
	avl_remove(&sdp->sd_snaps, sep);
	kmem_free(sep->se_name, strlen(sep->se_name) + 1);
	sep->se_name = kmem_alloc(strlen(nm) + 1, KM_SLEEP);
	(void) strcpy(sep->se_name, nm);
	VERIFY(avl_find(&sdp->sd_snaps, sep, &where) == NULL);
	avl_insert(&sdp->sd_snaps, sep, where);

	/*
	 * Change the current mountpoint info:
	 * 	- update the tail of the mntpoint path
	 *	- update the tail of the resource path
	 */
	pathref = vfs_getmntpoint(vfsp);
	(void) strncpy(newpath, refstr_value(pathref), sizeof (newpath));
	VERIFY((tail = strrchr(newpath, '/')) != NULL);
	*(tail+1) = '\0';
	ASSERT3U(strlen(newpath) + strlen(nm), <, sizeof (newpath));
	(void) strcat(newpath, nm);
	refstr_rele(pathref);
	vfs_setmntpoint(vfsp, newpath);

	pathref = vfs_getresource(vfsp);
	(void) strncpy(newpath, refstr_value(pathref), sizeof (newpath));
	VERIFY((tail = strrchr(newpath, '@')) != NULL);
	*(tail+1) = '\0';
	ASSERT3U(strlen(newpath) + strlen(nm), <, sizeof (newpath));
	(void) strcat(newpath, nm);
	refstr_rele(pathref);
	vfs_setresource(vfsp, newpath);

	vfs_unlock(vfsp);
}

static int
zfsctl_snapdir_rename(vnode_t *sdvp, char *snm, vnode_t *tdvp, char *tnm,
    cred_t *cr)
{
	zfsctl_snapdir_t *sdp = sdvp->v_data;
	zfs_snapentry_t search, *sep;
	avl_index_t where;
	char from[MAXNAMELEN], to[MAXNAMELEN];
	int err;

	err = zfsctl_snapshot_zname(sdvp, snm, MAXNAMELEN, from);
	if (err)
		return (err);
	err = zfs_secpolicy_write(from, NULL, cr);
	if (err)
		return (err);

	/*
	 * Cannot move snapshots out of the snapdir.
	 */
	if (sdvp != tdvp)
		return (EINVAL);

	if (strcmp(snm, tnm) == 0)
		return (0);

	err = zfsctl_snapshot_zname(tdvp, tnm, MAXNAMELEN, to);
	if (err)
		return (err);

	mutex_enter(&sdp->sd_lock);

	search.se_name = (char *)snm;
	if ((sep = avl_find(&sdp->sd_snaps, &search, &where)) == NULL) {
		mutex_exit(&sdp->sd_lock);
		return (ENOENT);
	}

	err = dmu_objset_rename(from, to);
	if (err == 0)
		zfsctl_rename_snap(sdp, sep, tnm);

	mutex_exit(&sdp->sd_lock);

	return (err);
}

/* ARGSUSED */
static int
zfsctl_snapdir_remove(vnode_t *dvp, char *name, vnode_t *cwd, cred_t *cr)
{
	zfsctl_snapdir_t *sdp = dvp->v_data;
	char snapname[MAXNAMELEN];
	int err;

	err = zfsctl_snapshot_zname(dvp, name, MAXNAMELEN, snapname);
	if (err)
		return (err);
	err = zfs_secpolicy_write(snapname, NULL, cr);
	if (err)
		return (err);

	mutex_enter(&sdp->sd_lock);

	err = zfsctl_unmount_snap(dvp, name, 0, cr);
	if (err) {
		mutex_exit(&sdp->sd_lock);
		return (err);
	}

	err = dmu_objset_destroy(snapname);

	mutex_exit(&sdp->sd_lock);

	return (err);
}

/*
 * Lookup entry point for the 'snapshot' directory.  Try to open the
 * snapshot if it exist, creating the pseudo filesystem vnode as necessary.
 * Perform a mount of the associated dataset on top of the vnode.
 */
/* ARGSUSED */
static int
zfsctl_snapdir_lookup(vnode_t *dvp, char *nm, vnode_t **vpp, pathname_t *pnp,
    int flags, vnode_t *rdir, cred_t *cr)
{
	zfsctl_snapdir_t *sdp = dvp->v_data;
	objset_t *snap;
	char snapname[MAXNAMELEN];
	char *mountpoint;
	zfs_snapentry_t *sep, search;
	struct mounta margs;
	vfs_t *vfsp;
	size_t mountpoint_len;
	avl_index_t where;
	zfsvfs_t *zfsvfs = dvp->v_vfsp->vfs_data;
	int err;

	ASSERT(dvp->v_type == VDIR);

	if (gfs_lookup_dot(vpp, dvp, zfsvfs->z_ctldir, nm) == 0)
		return (0);

	/*
	 * If we get a recursive call, that means we got called
	 * from the domount() code while it was trying to look up the
	 * spec (which looks like a local path for zfs).  We need to
	 * add some flag to domount() to tell it not to do this lookup.
	 */
	if (MUTEX_HELD(&sdp->sd_lock))
		return (ENOENT);

	ZFS_ENTER(zfsvfs);

	mutex_enter(&sdp->sd_lock);
	search.se_name = (char *)nm;
	if ((sep = avl_find(&sdp->sd_snaps, &search, &where)) != NULL) {
		*vpp = sep->se_root;
		VN_HOLD(*vpp);
		err = traverse(vpp);
		if (err) {
			VN_RELE(*vpp);
			*vpp = NULL;
		} else if (*vpp == sep->se_root) {
			/*
			 * The snapshot was unmounted behind our backs,
			 * try to remount it.
			 */
			goto domount;
		}
		mutex_exit(&sdp->sd_lock);
		ZFS_EXIT(zfsvfs);
		return (err);
	}

	/*
	 * The requested snapshot is not currently mounted, look it up.
	 */
	err = zfsctl_snapshot_zname(dvp, nm, MAXNAMELEN, snapname);
	if (err) {
		mutex_exit(&sdp->sd_lock);
		ZFS_EXIT(zfsvfs);
		return (err);
	}
	if (dmu_objset_open(snapname, DMU_OST_ZFS,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &snap) != 0) {
		mutex_exit(&sdp->sd_lock);
		ZFS_EXIT(zfsvfs);
		return (ENOENT);
	}

	sep = kmem_alloc(sizeof (zfs_snapentry_t), KM_SLEEP);
	sep->se_name = kmem_alloc(strlen(nm) + 1, KM_SLEEP);
	(void) strcpy(sep->se_name, nm);
	*vpp = sep->se_root = zfsctl_snapshot_mknode(dvp, dmu_objset_id(snap));
	avl_insert(&sdp->sd_snaps, sep, where);

	dmu_objset_close(snap);
domount:
	mountpoint_len = strlen(refstr_value(dvp->v_vfsp->vfs_mntpt)) +
	    strlen("/.zfs/snapshot/") + strlen(nm) + 1;
	mountpoint = kmem_alloc(mountpoint_len, KM_SLEEP);
	(void) snprintf(mountpoint, mountpoint_len, "%s/.zfs/snapshot/%s",
	    refstr_value(dvp->v_vfsp->vfs_mntpt), nm);

	margs.spec = snapname;
	margs.dir = mountpoint;
	margs.flags = MS_SYSSPACE | MS_NOMNTTAB;
	margs.fstype = "zfs";
	margs.dataptr = NULL;
	margs.datalen = 0;
	margs.optptr = NULL;
	margs.optlen = 0;

	err = domount("zfs", &margs, *vpp, kcred, &vfsp);
	kmem_free(mountpoint, mountpoint_len);

	if (err == 0) {
		/*
		 * Return the mounted root rather than the covered mount point.
		 */
		VFS_RELE(vfsp);
		err = traverse(vpp);
	}

	if (err == 0) {
		/*
		 * Fix up the root vnode.
		 */
		ASSERT(VTOZ(*vpp)->z_zfsvfs != zfsvfs);
		VTOZ(*vpp)->z_zfsvfs->z_parent = zfsvfs;
		(*vpp)->v_vfsp = zfsvfs->z_vfs;
		(*vpp)->v_flag &= ~VROOT;
	}
	mutex_exit(&sdp->sd_lock);
	ZFS_EXIT(zfsvfs);

	/*
	 * If we had an error, drop our hold on the vnode and
	 * zfsctl_snapshot_inactive() will clean up.
	 */
	if (err) {
		VN_RELE(*vpp);
		*vpp = NULL;
	}
	return (err);
}

/* ARGSUSED */
static int
zfsctl_snapdir_readdir_cb(vnode_t *vp, struct dirent64 *dp, int *eofp,
    offset_t *offp, offset_t *nextp, void *data)
{
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;
	char snapname[MAXNAMELEN];
	uint64_t id, cookie;

	ZFS_ENTER(zfsvfs);

	cookie = *offp;
	if (dmu_snapshot_list_next(zfsvfs->z_os, MAXNAMELEN, snapname, &id,
	    &cookie) == ENOENT) {
		*eofp = 1;
		ZFS_EXIT(zfsvfs);
		return (0);
	}

	(void) strcpy(dp->d_name, snapname);
	dp->d_ino = ZFSCTL_INO_SNAP(id);
	*nextp = cookie;

	ZFS_EXIT(zfsvfs);

	return (0);
}

vnode_t *
zfsctl_mknode_snapdir(vnode_t *pvp)
{
	vnode_t *vp;
	zfsctl_snapdir_t *sdp;

	vp = gfs_dir_create(sizeof (zfsctl_snapdir_t), pvp,
	    zfsctl_ops_snapdir, NULL, NULL, MAXNAMELEN,
	    zfsctl_snapdir_readdir_cb, NULL);
	sdp = vp->v_data;
	sdp->sd_node.zc_id = ZFSCTL_INO_SNAPDIR;
	sdp->sd_node.zc_cmtime = ((zfsctl_node_t *)pvp->v_data)->zc_cmtime;
	mutex_init(&sdp->sd_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&sdp->sd_snaps, snapentry_compare,
	    sizeof (zfs_snapentry_t), offsetof(zfs_snapentry_t, se_node));
	return (vp);
}

/* ARGSUSED */
static int
zfsctl_snapdir_getattr(vnode_t *vp, vattr_t *vap, int flags, cred_t *cr)
{
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;
	zfsctl_snapdir_t *sdp = vp->v_data;

	ZFS_ENTER(zfsvfs);
	zfsctl_common_getattr(vp, vap);
	vap->va_nodeid = gfs_file_inode(vp);
	vap->va_nlink = vap->va_size = avl_numnodes(&sdp->sd_snaps) + 2;
	ZFS_EXIT(zfsvfs);

	return (0);
}

/* ARGSUSED */
static void
zfsctl_snapdir_inactive(vnode_t *vp, cred_t *cr)
{
	zfsctl_snapdir_t *sdp = vp->v_data;
	void *private;

	private = gfs_dir_inactive(vp);
	if (private != NULL) {
		ASSERT(avl_numnodes(&sdp->sd_snaps) == 0);
		mutex_destroy(&sdp->sd_lock);
		avl_destroy(&sdp->sd_snaps);
		kmem_free(private, sizeof (zfsctl_snapdir_t));
	}
}

static const fs_operation_def_t zfsctl_tops_snapdir[] = {
	{ VOPNAME_OPEN,		zfsctl_common_open			},
	{ VOPNAME_CLOSE,	zfsctl_common_close			},
	{ VOPNAME_IOCTL,	fs_inval				},
	{ VOPNAME_GETATTR,	zfsctl_snapdir_getattr			},
	{ VOPNAME_ACCESS,	zfsctl_common_access			},
	{ VOPNAME_RENAME,	zfsctl_snapdir_rename			},
	{ VOPNAME_RMDIR,	zfsctl_snapdir_remove			},
	{ VOPNAME_READDIR,	gfs_vop_readdir				},
	{ VOPNAME_LOOKUP,	zfsctl_snapdir_lookup			},
	{ VOPNAME_SEEK,		fs_seek					},
	{ VOPNAME_INACTIVE,	(fs_generic_func_p) zfsctl_snapdir_inactive },
	{ VOPNAME_FID,		zfsctl_common_fid			},
	{ NULL }
};

static vnode_t *
zfsctl_snapshot_mknode(vnode_t *pvp, uint64_t objset)
{
	vnode_t *vp;
	zfsctl_node_t *zcp;

	vp = gfs_dir_create(sizeof (zfsctl_node_t), pvp,
	    zfsctl_ops_snapshot, NULL, NULL, MAXNAMELEN, NULL, NULL);
	zcp = vp->v_data;
	zcp->zc_id = objset;

	return (vp);
}

static void
zfsctl_snapshot_inactive(vnode_t *vp, cred_t *cr)
{
	zfsctl_snapdir_t *sdp;
	zfs_snapentry_t *sep, *next;
	vnode_t *dvp;

	VERIFY(gfs_dir_lookup(vp, "..", &dvp) == 0);
	sdp = dvp->v_data;

	mutex_enter(&sdp->sd_lock);

	if (vp->v_count > 1) {
		mutex_exit(&sdp->sd_lock);
		return;
	}
	ASSERT(!vn_ismntpt(vp));

	sep = avl_first(&sdp->sd_snaps);
	while (sep != NULL) {
		next = AVL_NEXT(&sdp->sd_snaps, sep);

		if (sep->se_root == vp) {
			avl_remove(&sdp->sd_snaps, sep);
			kmem_free(sep->se_name, strlen(sep->se_name) + 1);
			kmem_free(sep, sizeof (zfs_snapentry_t));
			break;
		}
		sep = next;
	}
	ASSERT(sep != NULL);

	mutex_exit(&sdp->sd_lock);
	VN_RELE(dvp);

	/*
	 * Dispose of the vnode for the snapshot mount point.
	 * This is safe to do because once this entry has been removed
	 * from the AVL tree, it can't be found again, so cannot become
	 * "active".  If we lookup the same name again we will end up
	 * creating a new vnode.
	 */
	gfs_vop_inactive(vp, cr);
}


/*
 * These VP's should never see the light of day.  They should always
 * be covered.
 */
static const fs_operation_def_t zfsctl_tops_snapshot[] = {
	VOPNAME_INACTIVE, (fs_generic_func_p) zfsctl_snapshot_inactive,
	NULL, NULL
};

int
zfsctl_lookup_objset(vfs_t *vfsp, uint64_t objsetid, zfsvfs_t **zfsvfsp)
{
	zfsvfs_t *zfsvfs = vfsp->vfs_data;
	vnode_t *dvp, *vp;
	zfsctl_snapdir_t *sdp;
	zfsctl_node_t *zcp;
	zfs_snapentry_t *sep;
	int error;

	ASSERT(zfsvfs->z_ctldir != NULL);
	error = zfsctl_root_lookup(zfsvfs->z_ctldir, "snapshot", &dvp,
	    NULL, 0, NULL, kcred);
	if (error != 0)
		return (error);
	sdp = dvp->v_data;

	mutex_enter(&sdp->sd_lock);
	sep = avl_first(&sdp->sd_snaps);
	while (sep != NULL) {
		vp = sep->se_root;
		zcp = vp->v_data;
		if (zcp->zc_id == objsetid)
			break;

		sep = AVL_NEXT(&sdp->sd_snaps, sep);
	}

	if (sep != NULL) {
		VN_HOLD(vp);
		error = traverse(&vp);
		if (error == 0) {
			if (vp == sep->se_root)
				error = EINVAL;
			else
				*zfsvfsp = VTOZ(vp)->z_zfsvfs;
		}
		mutex_exit(&sdp->sd_lock);
		VN_RELE(vp);
	} else {
		error = EINVAL;
		mutex_exit(&sdp->sd_lock);
	}

	VN_RELE(dvp);

	return (error);
}

/*
 * Unmount any snapshots for the given filesystem.  This is called from
 * zfs_umount() - if we have a ctldir, then go through and unmount all the
 * snapshots.
 */
int
zfsctl_umount_snapshots(vfs_t *vfsp, int fflags, cred_t *cr)
{
	zfsvfs_t *zfsvfs = vfsp->vfs_data;
	vnode_t *dvp, *svp;
	zfsctl_snapdir_t *sdp;
	zfs_snapentry_t *sep, *next;
	int error;

	ASSERT(zfsvfs->z_ctldir != NULL);
	error = zfsctl_root_lookup(zfsvfs->z_ctldir, "snapshot", &dvp,
	    NULL, 0, NULL, cr);
	if (error != 0)
		return (error);
	sdp = dvp->v_data;

	mutex_enter(&sdp->sd_lock);

	sep = avl_first(&sdp->sd_snaps);
	while (sep != NULL) {
		svp = sep->se_root;
		next = AVL_NEXT(&sdp->sd_snaps, sep);

		/*
		 * If this snapshot is not mounted, then it must
		 * have just been unmounted by somebody else, and
		 * will be cleaned up by zfsctl_snapdir_inactive().
		 */
		if (vn_ismntpt(svp)) {
			if ((error = vn_vfswlock(svp)) != 0)
				goto out;

			VN_HOLD(svp);
			error = dounmount(vn_mountedvfs(svp), fflags, cr);
			if (error) {
				VN_RELE(svp);
				goto out;
			}

			avl_remove(&sdp->sd_snaps, sep);
			kmem_free(sep->se_name, strlen(sep->se_name) + 1);
			kmem_free(sep, sizeof (zfs_snapentry_t));

			/*
			 * We can't use VN_RELE(), as that will try to
			 * invoke zfsctl_snapdir_inactive(), and that
			 * would lead to an attempt to re-grab the sd_lock.
			 */
			ASSERT3U(svp->v_count, ==, 1);
			gfs_vop_inactive(svp, cr);
		}
		sep = next;
	}
out:
	mutex_exit(&sdp->sd_lock);
	VN_RELE(dvp);

	return (error);
}

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

#pragma ident	"@(#)sdev_ncache.c	1.1	06/08/25 SMI"

/*
 * negative cache handling for the /dev fs
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/t_lock.h>
#include <sys/systm.h>
#include <sys/sysmacros.h>
#include <sys/user.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/file.h>
#include <sys/fcntl.h>
#include <sys/flock.h>
#include <sys/kmem.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/cred.h>
#include <sys/cmn_err.h>
#include <sys/debug.h>
#include <sys/mode.h>
#include <sys/policy.h>
#include <fs/fs_subr.h>
#include <sys/mount.h>
#include <sys/fs/snode.h>
#include <sys/fs/dv_node.h>
#include <sys/fs/sdev_node.h>
#include <sys/sunndi.h>
#include <sys/sunmdi.h>
#include <sys/ddi.h>
#include <sys/modctl.h>
#include <sys/devctl_impl.h>


/*
 * ncache is a negative cache of failed lookups.  An entry
 * is added after an attempt to configure a device by that
 * name failed.  An accumulation of these entries over time
 * gives us a set of device name for which implicit reconfiguration
 * does not need to be attempted.  If a name is created matching
 * an entry in ncache, that entry is removed, with the
 * persistent store updated.
 *
 * Implicit reconfig is initiated for any name during lookup that
 * can't be resolved from the backing store and that isn't
 * present in the negative cache.  This functionality is
 * enabled during system startup once communication with devfsadm
 * can be achieved.  Since readdir is more general, implicit
 * reconfig initiated by reading a directory isn't enabled until
 * the system is more fully booted, at the time of the multi-user
 * milestone, corresponding to init state 2.
 *
 * A maximum is imposed on the number of entries in the cache
 * to limit some script going wild and as a defense against attack.
 * The default limit is 64 and can be adjusted via sdev_nc_max_entries.
 *
 * Each entry also has a expiration count.  When looked up a name in
 * the cache is set to the default.  Subsequent boots will decrement
 * the count if a name isn't referenced.  This permits a once-only
 * entry to eventually be removed over time.
 *
 * sdev_reconfig_delay implements a "debounce" of the timing beyond
 * system available indication, providing what the filesystem considers
 * to be the system-is-fully-booted state.  This is provided to adjust
 * the timing if some application startup is performing a readdir
 * in /dev that initiates a troublesome implicit reconfig on every boot.
 *
 * sdev_nc_disable_reset can be used to disable clearing the negative cache
 * on reconfig boot.  The default is to clear the cache on reconfig boot.
 * sdev_nc_disable can be used to disable the negative cache itself.
 *
 * sdev_reconfig_disable can be used to disable implicit reconfig.
 * The default is that implicit reconfig is enabled.
 */

/* tunables and defaults */
#define	SDEV_NC_EXPIRECNT	4
#define	SDEV_NC_MAX_ENTRIES	64
#define	SEV_RECONFIG_DELAY	6	/* seconds */

int			sdev_nc_expirecnt = SDEV_NC_EXPIRECNT;
int			sdev_nc_max_entries = SDEV_NC_MAX_ENTRIES;
int			sdev_reconfig_delay = SEV_RECONFIG_DELAY;
int			sdev_reconfig_verbose = 0;
int			sdev_reconfig_disable = 0;
int			sdev_nc_disable = 0;
int			sdev_nc_disable_reset = 0;
int			sdev_nc_verbose = 0;

/* globals */
sdev_nc_list_t		*sdev_ncache;
int			sdev_boot_state = SDEV_BOOT_STATE_INITIAL;
int			sdev_reconfig_boot = 0;
static timeout_id_t	sdev_timeout_id = 0;

/* static prototypes */
static void sdev_ncache_write_complete(nvfd_t *);
static void sdev_ncache_write(void);
static void sdev_ncache_process_store(void);
static sdev_nc_list_t *sdev_nc_newlist(void);
static void sdev_nc_free_unlinked_node(sdev_nc_node_t *);
static void sdev_nc_free_all_nodes(sdev_nc_list_t *);
static void sdev_nc_freelist(sdev_nc_list_t *);
static sdev_nc_node_t *sdev_nc_findpath(sdev_nc_list_t *, char *);
static void sdev_nc_insertnode(sdev_nc_list_t *, sdev_nc_node_t *);
static void sdev_nc_free_bootonly(void);


/*
 * called once at filesystem initialization
 */
void
sdev_ncache_init(void)
{
	sdev_ncache = sdev_nc_newlist();
}

/*
 * called at mount of the global instance
 * currently the global instance is never unmounted
 */
void
sdev_ncache_setup(void)
{
	nvfd_t	*nvf = sdevfd;

	nvf_register_write_complete(nvf, sdev_ncache_write_complete);

	i_ddi_read_devname_file();
	sdev_ncache_process_store();
	sdev_devstate_change();
}

static void
sdev_nvp_cache_free(nvfd_t *nvf)
{
	nvp_devname_t	*np;
	nvp_devname_t	*next;

	for (np = NVF_DEVNAME_LIST(nvf); np; np = next) {
		next = NVP_DEVNAME_NEXT(np);
		nfd_nvp_free_and_unlink(nvf, NVPLIST(np));
	}
}

static void
sdev_ncache_process_store(void)
{
	nvfd_t		*nvf = sdevfd;
	sdev_nc_list_t	*ncl = sdev_ncache;
	nvp_devname_t	*np;
	sdev_nc_node_t	*lp;
	char		*path;
	int		i, n;

	if (sdev_nc_disable)
		return;

	for (np = NVF_DEVNAME_LIST(nvf); np; np = NVP_DEVNAME_NEXT(np)) {
		for (i = 0; i < np->nvp_npaths; i++) {
			sdcmn_err5(("    %s %d\n",
			    np->nvp_paths[i], np->nvp_expirecnts[i]));
			if (ncl->ncl_nentries < sdev_nc_max_entries) {
				path = np->nvp_paths[i];
				n = strlen(path) + 1;
				lp = kmem_alloc(sizeof (sdev_nc_node_t),
				    KM_SLEEP);
				lp->ncn_name = kmem_alloc(n, KM_SLEEP);
				bcopy(path, lp->ncn_name, n);
				lp->ncn_flags = NCN_SRC_STORE;
				lp->ncn_expirecnt = np->nvp_expirecnts[i];
				sdev_nc_insertnode(ncl, lp);
			} else if (sdev_nc_verbose) {
				cmn_err(CE_CONT,
				    "?%s: truncating from ncache (max %d)\n",
				    np->nvp_paths[i], sdev_nc_max_entries);
			}
		}
	}
}

static void
sdev_ncache_write_complete(nvfd_t *nvf)
{
	sdev_nc_list_t	*ncl = sdev_ncache;

	mutex_enter(&ncl->ncl_mutex);

	ASSERT(ncl->ncl_flags & NCL_LIST_WRITING);

	if (ncl->ncl_flags & NCL_LIST_DIRTY) {
		sdcmn_err5(("ncache write complete but dirty again\n"));
		ncl->ncl_flags &= ~NCL_LIST_DIRTY;
		mutex_exit(&ncl->ncl_mutex);
		sdev_ncache_write();
	} else {
		sdcmn_err5(("ncache write complete\n"));
		ncl->ncl_flags &= ~NCL_LIST_WRITING;
		mutex_exit(&ncl->ncl_mutex);
		rw_enter(&nvf->nvf_lock, RW_WRITER);
		sdev_nvp_cache_free(nvf);
		rw_exit(&nvf->nvf_lock);
	}
}

static void
sdev_ncache_write(void)
{
	nvfd_t		*nvf = sdevfd;
	sdev_nc_list_t	*ncl = sdev_ncache;
	nvp_devname_t	*np;
	sdev_nc_node_t	*lp;
	int		n, i;

	if (sdev_cache_write_disable) {
		mutex_enter(&ncl->ncl_mutex);
		ncl->ncl_flags &= ~NCL_LIST_WRITING;
		mutex_exit(&ncl->ncl_mutex);
		return;
	}

	/* proper lock ordering here is essential */
	rw_enter(&nvf->nvf_lock, RW_WRITER);
	sdev_nvp_cache_free(nvf);

	rw_enter(&ncl->ncl_lock, RW_READER);
	n = ncl->ncl_nentries;
	ASSERT(n <= sdev_nc_max_entries);

	np = kmem_zalloc(sizeof (nvp_devname_t), KM_SLEEP);
	np->nvp_npaths = n;
	np->nvp_paths = kmem_zalloc(n * sizeof (char *), KM_SLEEP);
	np->nvp_expirecnts = kmem_zalloc(n * sizeof (int), KM_SLEEP);

	i = 0;
	for (lp = list_head(&ncl->ncl_list); lp;
	    lp = list_next(&ncl->ncl_list, lp)) {
		np->nvp_paths[i] = i_ddi_strdup(lp->ncn_name, KM_SLEEP);
		np->nvp_expirecnts[i] = lp->ncn_expirecnt;
		sdcmn_err5(("    %s %d\n",
		    np->nvp_paths[i], np->nvp_expirecnts[i]));
		i++;
	}

	rw_exit(&ncl->ncl_lock);

	NVF_MARK_DIRTY(nvf);
	nfd_nvp_link(nvf, NVPLIST(np));
	rw_exit(&nvf->nvf_lock);

	wake_nvpflush_daemon();
}

static void
sdev_nc_flush_updates(void)
{
	sdev_nc_list_t *ncl = sdev_ncache;

	if (sdev_nc_disable || sdev_cache_write_disable)
		return;

	mutex_enter(&ncl->ncl_mutex);
	if (((ncl->ncl_flags &
	    (NCL_LIST_DIRTY | NCL_LIST_WENABLE | NCL_LIST_WRITING)) ==
	    (NCL_LIST_DIRTY | NCL_LIST_WENABLE))) {
		ncl->ncl_flags &= ~NCL_LIST_DIRTY;
		ncl->ncl_flags |= NCL_LIST_WRITING;
		mutex_exit(&ncl->ncl_mutex);
		sdev_ncache_write();
	} else {
		mutex_exit(&ncl->ncl_mutex);
	}
}

static void
sdev_nc_flush_boot_update(void)
{
	sdev_nc_list_t *ncl = sdev_ncache;

	if (sdev_nc_disable || sdev_cache_write_disable ||
	    (sdev_boot_state == SDEV_BOOT_STATE_INITIAL)) {
		return;
	}
	mutex_enter(&ncl->ncl_mutex);
	if (ncl->ncl_flags & NCL_LIST_WENABLE) {
		mutex_exit(&ncl->ncl_mutex);
		sdev_nc_flush_updates();
	} else {
		mutex_exit(&ncl->ncl_mutex);
	}

}

static void
sdev_state_boot_complete()
{
	sdev_nc_list_t	*ncl = sdev_ncache;
	sdev_nc_node_t	*lp, *next;

	/*
	 * Once boot is complete, decrement the expire count of each entry
	 * in the cache not touched by a reference.  Remove any that
	 * goes to zero.  This effectively removes random entries over
	 * time.
	 */
	rw_enter(&ncl->ncl_lock, RW_WRITER);
	mutex_enter(&ncl->ncl_mutex);

	for (lp = list_head(&ncl->ncl_list); lp; lp = next) {
		next = list_next(&ncl->ncl_list, lp);
		if (sdev_nc_expirecnt > 0 && lp->ncn_expirecnt > 0) {
			if (lp->ncn_flags & NCN_ACTIVE) {
				if (lp->ncn_expirecnt != sdev_nc_expirecnt) {
					lp->ncn_expirecnt = sdev_nc_expirecnt;
					ncl->ncl_flags |= NCL_LIST_DIRTY;
				}
			} else {
				if (--lp->ncn_expirecnt == 0) {
					list_remove(&ncl->ncl_list, lp);
					sdev_nc_free_unlinked_node(lp);
					ncl->ncl_nentries--;
				}
				ncl->ncl_flags |= NCL_LIST_DIRTY;
			}
		}
	}

	mutex_exit(&ncl->ncl_mutex);
	rw_exit(&ncl->ncl_lock);

	sdev_nc_flush_boot_update();
	sdev_boot_state = SDEV_BOOT_STATE_COMPLETE;
}

/*
 * Upon transition to the login state on a reconfigure boot,
 * a debounce timer is set up so that we cache all the nonsense
 * lookups we're hit with by the windowing system startup.
 */

/*ARGSUSED*/
static void
sdev_state_timeout(void *arg)
{
	sdev_timeout_id = 0;
	sdev_state_boot_complete();
}

static void
sdev_state_sysavail()
{
	sdev_nc_list_t *ncl = sdev_ncache;
	clock_t	nticks;
	int nsecs;

	mutex_enter(&ncl->ncl_mutex);
	ncl->ncl_flags |= NCL_LIST_WENABLE;
	mutex_exit(&ncl->ncl_mutex);

	nsecs = sdev_reconfig_delay;
	if (nsecs == 0) {
		sdev_state_boot_complete();
	} else {
		nticks = drv_usectohz(1000000 * nsecs);
		sdcmn_err5(("timeout initiated %ld\n", nticks));
		sdev_timeout_id = timeout(sdev_state_timeout, NULL, nticks);
		sdev_nc_flush_boot_update();
	}
}

/*
 * Called to inform the filesystem of progress during boot,
 * either a notice of reconfiguration boot or an indication of
 * system boot complete.  At system boot complete, set up a
 * timer at the expiration of which no further failed lookups
 * will be added to the negative cache.
 *
 * The dev filesystem infers from reconfig boot that implicit
 * reconfig need not be invoked at all as all available devices
 * will have already been named.
 *
 * The dev filesystem infers from "system available" that devfsadmd
 * can now be run and hence implicit reconfiguration may be initiated.
 * During early stages of system startup, implicit reconfig is
 * not done to avoid impacting boot performance.
 */
void
sdev_devstate_change(void)
{
	int new_state;

	/*
	 * Track system state and manage interesting transitions
	 */
	new_state = SDEV_BOOT_STATE_INITIAL;
	if (i_ddi_reconfig())
		new_state = SDEV_BOOT_STATE_RECONFIG;
	if (i_ddi_sysavail())
		new_state = SDEV_BOOT_STATE_SYSAVAIL;

	if (sdev_boot_state < new_state) {
		switch (new_state) {
		case SDEV_BOOT_STATE_RECONFIG:
			sdcmn_err5(("state change: reconfigure boot\n"));
			sdev_boot_state = new_state;
			sdev_reconfig_boot = 1;
			if (!sdev_nc_disable_reset)
				sdev_nc_free_bootonly();
			break;
		case SDEV_BOOT_STATE_SYSAVAIL:
			sdcmn_err5(("system available\n"));
			sdev_boot_state = new_state;
			sdev_state_sysavail();
			break;
		}
	}
}

/*
 * Lookup: filter out entries in the negative cache
 * Return 1 if the lookup should not cause a reconfig.
 */
int
sdev_lookup_filter(sdev_node_t *dv, char *nm)
{
	int n;
	sdev_nc_list_t *ncl = sdev_ncache;
	sdev_nc_node_t *lp;
	char *path;
	int rval = 0;
	int changed = 0;

	ASSERT(i_ddi_io_initialized());
	ASSERT(SDEVTOV(dv)->v_type == VDIR);

	if (sdev_nc_disable)
		return (0);

	n = strlen(dv->sdev_path) + strlen(nm) + 2;
	path = kmem_alloc(n, KM_SLEEP);
	(void) sprintf(path, "%s/%s", dv->sdev_path, nm);

	rw_enter(&ncl->ncl_lock, RW_READER);
	if ((lp = sdev_nc_findpath(ncl, path)) != NULL) {
		sdcmn_err5(("%s/%s: lookup by %s cached, no reconfig\n",
		    dv->sdev_name, nm, curproc->p_user.u_comm));
		if (sdev_nc_verbose) {
			cmn_err(CE_CONT,
			    "?%s/%s: lookup by %s cached, no reconfig\n",
			    dv->sdev_name, nm, curproc->p_user.u_comm);
		}
		mutex_enter(&ncl->ncl_mutex);
		lp->ncn_flags |= NCN_ACTIVE;
		if (sdev_nc_expirecnt > 0 && lp->ncn_expirecnt > 0 &&
		    lp->ncn_expirecnt < sdev_nc_expirecnt) {
			lp->ncn_expirecnt = sdev_nc_expirecnt;
			ncl->ncl_flags |= NCL_LIST_DIRTY;
			changed = 1;
		}
		mutex_exit(&ncl->ncl_mutex);
		rval = 1;
	}
	rw_exit(&ncl->ncl_lock);
	kmem_free(path, n);
	if (changed)
		sdev_nc_flush_boot_update();
	return (rval);
}

void
sdev_lookup_failed(sdev_node_t *dv, char *nm, int failed_flags)
{
	if (sdev_nc_disable)
		return;

	/*
	 * If we're still in the initial boot stage, always update
	 * the cache - we may not have received notice of the
	 * reconfig boot state yet.  On a reconfigure boot, entries
	 * from the backing store are not re-persisted on update,
	 * but new entries are marked as needing an update.
	 * Never cache dynamic or non-global nodes.
	 */
	if (SDEV_IS_GLOBAL(dv) && !SDEV_IS_DYNAMIC(dv) &&
	    !SDEV_IS_NO_NCACHE(dv) &&
	    ((failed_flags & SLF_NO_NCACHE) == 0) &&
	    ((sdev_reconfig_boot &&
		(sdev_boot_state != SDEV_BOOT_STATE_COMPLETE)) ||
	    (!sdev_reconfig_boot && ((failed_flags & SLF_REBUILT))))) {
			sdev_nc_addname(sdev_ncache,
			    dv, nm, NCN_SRC_CURRENT|NCN_ACTIVE);
	}
}

static sdev_nc_list_t *
sdev_nc_newlist(void)
{
	sdev_nc_list_t	*ncl;

	ncl = kmem_zalloc(sizeof (sdev_nc_list_t), KM_SLEEP);

	rw_init(&ncl->ncl_lock, NULL, RW_DEFAULT, NULL);
	mutex_init(&ncl->ncl_mutex, NULL, MUTEX_DEFAULT, NULL);
	list_create(&ncl->ncl_list, sizeof (sdev_nc_node_t),
	    offsetof(sdev_nc_node_t, ncn_link));

	return (ncl);
}

static void
sdev_nc_free_unlinked_node(sdev_nc_node_t *lp)
{
	kmem_free(lp->ncn_name, strlen(lp->ncn_name) + 1);
	kmem_free(lp, sizeof (sdev_nc_node_t));
}

static void
sdev_nc_free_all_nodes(sdev_nc_list_t *ncl)
{
	sdev_nc_node_t *lp;

	while ((lp = list_head(&ncl->ncl_list)) != NULL) {
		list_remove(&ncl->ncl_list, lp);
		sdev_nc_free_unlinked_node(lp);
		ncl->ncl_nentries--;
	}
	ASSERT(ncl->ncl_nentries == 0);
}

static void
sdev_nc_freelist(sdev_nc_list_t *ncl)
{
	if (!list_is_empty(&ncl->ncl_list))
		sdev_nc_free_all_nodes(ncl);
	ASSERT(list_is_empty(&ncl->ncl_list));
	ASSERT(ncl->ncl_nentries == 0);

	mutex_destroy(&ncl->ncl_mutex);
	rw_destroy(&ncl->ncl_lock);
	list_destroy(&ncl->ncl_list);
	kmem_free(ncl, sizeof (sdev_nc_list_t));
}

static sdev_nc_node_t *
sdev_nc_findpath(sdev_nc_list_t *ncl, char *path)
{
	sdev_nc_node_t *lp;

	ASSERT(RW_LOCK_HELD(&ncl->ncl_lock));

	for (lp = list_head(&ncl->ncl_list); lp;
	    lp = list_next(&ncl->ncl_list, lp)) {
		if (strcmp(path, lp->ncn_name) == 0)
			return (lp);
	}

	return (NULL);
}

static void
sdev_nc_insertnode(sdev_nc_list_t *ncl, sdev_nc_node_t *new)
{
	sdev_nc_node_t *lp;

	rw_enter(&ncl->ncl_lock, RW_WRITER);

	lp = sdev_nc_findpath(ncl, new->ncn_name);
	if (lp == NULL) {
		if (ncl->ncl_nentries == sdev_nc_max_entries) {
			sdcmn_err5((
			    "%s by %s: not adding to ncache (max %d)\n",
			    new->ncn_name, curproc->p_user.u_comm,
			    ncl->ncl_nentries));
			if (sdev_nc_verbose) {
				cmn_err(CE_CONT, "?%s by %s: "
				    "not adding to ncache (max %d)\n",
				    new->ncn_name, curproc->p_user.u_comm,
				    ncl->ncl_nentries);
			}
			rw_exit(&ncl->ncl_lock);
			sdev_nc_free_unlinked_node(new);
		} else {

			list_insert_tail(&ncl->ncl_list, new);
			ncl->ncl_nentries++;

			/* don't mark list dirty for nodes from store */
			mutex_enter(&ncl->ncl_mutex);
			if ((new->ncn_flags & NCN_SRC_STORE) == 0) {
				sdcmn_err5(("%s by %s: add to ncache\n",
				    new->ncn_name, curproc->p_user.u_comm));
				if (sdev_nc_verbose) {
					cmn_err(CE_CONT,
					    "?%s by %s: add to ncache\n",
					    new->ncn_name,
					    curproc->p_user.u_comm);
				}
				ncl->ncl_flags |= NCL_LIST_DIRTY;
			}
			mutex_exit(&ncl->ncl_mutex);
			rw_exit(&ncl->ncl_lock);
			lp = new;
			sdev_nc_flush_boot_update();
		}
	} else {
		mutex_enter(&ncl->ncl_mutex);
		lp->ncn_flags |= new->ncn_flags;
		mutex_exit(&ncl->ncl_mutex);
		rw_exit(&ncl->ncl_lock);
		sdev_nc_free_unlinked_node(new);
	}
}

void
sdev_nc_addname(sdev_nc_list_t *ncl, sdev_node_t *dv, char *nm, int flags)
{
	int n;
	sdev_nc_node_t *lp;

	ASSERT(SDEVTOV(dv)->v_type == VDIR);

	lp = kmem_zalloc(sizeof (sdev_nc_node_t), KM_SLEEP);

	n = strlen(dv->sdev_path) + strlen(nm) + 2;
	lp->ncn_name = kmem_alloc(n, KM_SLEEP);
	(void) sprintf(lp->ncn_name, "%s/%s",
		dv->sdev_path, nm);
	lp->ncn_flags = flags;
	lp->ncn_expirecnt = sdev_nc_expirecnt;
	sdev_nc_insertnode(ncl, lp);
}

void
sdev_nc_node_exists(sdev_node_t *dv)
{
	/* dynamic and non-global nodes are never cached */
	if (SDEV_IS_GLOBAL(dv) && !SDEV_IS_DYNAMIC(dv) &&
	    !SDEV_IS_NO_NCACHE(dv)) {
		sdev_nc_path_exists(sdev_ncache, dv->sdev_path);
	}
}

void
sdev_nc_path_exists(sdev_nc_list_t *ncl, char *path)
{
	sdev_nc_node_t *lp;

	if (sdev_nc_disable)
		return;

	rw_enter(&ncl->ncl_lock, RW_READER);
	if ((lp = sdev_nc_findpath(ncl, path)) == NULL) {
		rw_exit(&ncl->ncl_lock);
		return;
	}
	if (rw_tryupgrade(&ncl->ncl_lock) == 0) {
		rw_exit(&ncl->ncl_lock);
		rw_enter(&ncl->ncl_lock, RW_WRITER);
		lp = sdev_nc_findpath(ncl, path);
	}
	if (lp) {
		list_remove(&ncl->ncl_list, lp);
		ncl->ncl_nentries--;
		mutex_enter(&ncl->ncl_mutex);
		ncl->ncl_flags |= NCL_LIST_DIRTY;
		if (ncl->ncl_flags & NCL_LIST_WENABLE) {
			mutex_exit(&ncl->ncl_mutex);
			rw_exit(&ncl->ncl_lock);
			sdev_nc_flush_updates();
		} else {
			mutex_exit(&ncl->ncl_mutex);
			rw_exit(&ncl->ncl_lock);
		}
		sdev_nc_free_unlinked_node(lp);
		sdcmn_err5(("%s by %s: removed from ncache\n",
		    path, curproc->p_user.u_comm));
		if (sdev_nc_verbose) {
			cmn_err(CE_CONT, "?%s by %s: removed from ncache\n",
			    path, curproc->p_user.u_comm);
		}
	} else
		rw_exit(&ncl->ncl_lock);
}

static void
sdev_nc_free_bootonly(void)
{
	sdev_nc_list_t	*ncl = sdev_ncache;
	sdev_nc_node_t *lp;
	sdev_nc_node_t *next;

	ASSERT(sdev_reconfig_boot);

	rw_enter(&ncl->ncl_lock, RW_WRITER);

	for (lp = list_head(&ncl->ncl_list); lp; lp = next) {
		next = list_next(&ncl->ncl_list, lp);
		if ((lp->ncn_flags & NCN_SRC_CURRENT) == 0) {
			sdcmn_err5(("freeing %s\n", lp->ncn_name));
			mutex_enter(&ncl->ncl_mutex);
			ncl->ncl_flags |= NCL_LIST_DIRTY;
			mutex_exit(&ncl->ncl_mutex);
			list_remove(&ncl->ncl_list, lp);
			sdev_nc_free_unlinked_node(lp);
			ncl->ncl_nentries--;
		}
	}

	rw_exit(&ncl->ncl_lock);
}

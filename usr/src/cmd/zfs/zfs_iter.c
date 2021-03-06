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

#pragma ident	"@(#)zfs_iter.c	1.4	06/07/12 SMI"

#include <libintl.h>
#include <libuutil.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include <libzfs.h>

#include "zfs_util.h"
#include "zfs_iter.h"

/*
 * This is a private interface used to gather up all the datasets specified on
 * the command line so that we can iterate over them in order.
 *
 * First, we iterate over all filesystems, gathering them together into an
 * AVL tree.  We report errors for any explicitly specified datasets
 * that we couldn't open.
 *
 * When finished, we have an AVL tree of ZFS handles.  We go through and execute
 * the provided callback for each one, passing whatever data the user supplied.
 */

typedef struct zfs_node {
	zfs_handle_t	*zn_handle;
	uu_avl_node_t	zn_avlnode;
} zfs_node_t;

typedef struct callback_data {
	uu_avl_t	*cb_avl;
	int		cb_recurse;
	zfs_type_t	cb_types;
	zfs_sort_column_t *cb_sortcol;
} callback_data_t;

uu_avl_pool_t *avl_pool;

/*
 * Called for each dataset.  If the object the object is of an appropriate type,
 * add it to the avl tree and recurse over any children as necessary.
 */
int
zfs_callback(zfs_handle_t *zhp, void *data)
{
	callback_data_t *cb = data;
	int dontclose = 0;

	/*
	 * If this object is of the appropriate type, add it to the AVL tree.
	 */
	if (zfs_get_type(zhp) & cb->cb_types) {
		uu_avl_index_t idx;
		zfs_node_t *node = safe_malloc(sizeof (zfs_node_t));

		node->zn_handle = zhp;
		uu_avl_node_init(node, &node->zn_avlnode, avl_pool);
		if (uu_avl_find(cb->cb_avl, node, cb->cb_sortcol,
		    &idx) == NULL) {
			uu_avl_insert(cb->cb_avl, node, idx);
			dontclose = 1;
		} else {
			free(node);
		}
	}

	/*
	 * Recurse if necessary.
	 */
	if (cb->cb_recurse && (zfs_get_type(zhp) == ZFS_TYPE_FILESYSTEM ||
	    (zfs_get_type(zhp) == ZFS_TYPE_VOLUME && (cb->cb_types &
	    ZFS_TYPE_SNAPSHOT))))
		(void) zfs_iter_children(zhp, zfs_callback, data);

	if (!dontclose)
		zfs_close(zhp);

	return (0);
}

void
zfs_add_sort_column(zfs_sort_column_t **sc, zfs_prop_t prop,
    boolean_t reverse)
{
	zfs_sort_column_t *col;

	col = safe_malloc(sizeof (zfs_sort_column_t));

	col->sc_prop = prop;
	col->sc_reverse = reverse;
	col->sc_next = NULL;

	if (*sc == NULL) {
		col->sc_last = col;
		*sc = col;
	} else {
		(*sc)->sc_last->sc_next = col;
		(*sc)->sc_last = col;
	}
}

void
zfs_free_sort_columns(zfs_sort_column_t *sc)
{
	zfs_sort_column_t *col;

	while (sc != NULL) {
		col = sc->sc_next;
		free(sc);
		sc = col;
	}
}

/* ARGSUSED */
static int
zfs_compare(const void *larg, const void *rarg, void *unused)
{
	zfs_handle_t *l = ((zfs_node_t *)larg)->zn_handle;
	zfs_handle_t *r = ((zfs_node_t *)rarg)->zn_handle;
	const char *lname = zfs_get_name(l);
	const char *rname = zfs_get_name(r);
	char *lat, *rat;
	uint64_t lcreate, rcreate;
	int ret;

	lat = (char *)strchr(lname, '@');
	rat = (char *)strchr(rname, '@');

	if (lat != NULL)
		*lat = '\0';
	if (rat != NULL)
		*rat = '\0';

	ret = strcmp(lname, rname);
	if (ret == 0) {
		/*
		 * If we're comparing a dataset to one of its snapshots, we
		 * always make the full dataset first.
		 */
		if (lat == NULL) {
			ret = -1;
		} else if (rat == NULL) {
			ret = 1;
		} else {
			/*
			 * If we have two snapshots from the same dataset, then
			 * we want to sort them according to creation time.  We
			 * use the hidden CREATETXG property to get an absolute
			 * ordering of snapshots.
			 */
			lcreate = zfs_prop_get_int(l, ZFS_PROP_CREATETXG);
			rcreate = zfs_prop_get_int(r, ZFS_PROP_CREATETXG);

			if (lcreate < rcreate)
				ret = -1;
			else if (lcreate > rcreate)
				ret = 1;
		}
	}

	if (lat != NULL)
		*lat = '@';
	if (rat != NULL)
		*rat = '@';

	return (ret);
}

/*
 * Sort datasets by specified columns.
 *
 * o  Numeric types sort in ascending order.
 * o  String types sort in alphabetical order.
 * o  Types inappropriate for a row sort that row to the literal
 *    bottom, regardless of the specified ordering.
 *
 * If no sort columns are specified, or two datasets compare equally
 * across all specified columns, they are sorted alphabetically by name
 * with snapshots grouped under their parents.
 */
static int
zfs_sort(const void *larg, const void *rarg, void *data)
{
	zfs_handle_t *l = ((zfs_node_t *)larg)->zn_handle;
	zfs_handle_t *r = ((zfs_node_t *)rarg)->zn_handle;
	zfs_sort_column_t *sc = (zfs_sort_column_t *)data;
	zfs_sort_column_t *psc;

	for (psc = sc; psc != NULL; psc = psc->sc_next) {
		char lstr[ZFS_MAXPROPLEN], rstr[ZFS_MAXPROPLEN];
		uint64_t lnum, rnum;
		int lvalid, rvalid;
		int ret = 0;

		if (zfs_prop_is_string(psc->sc_prop)) {
			lvalid = zfs_prop_get(l, psc->sc_prop, lstr,
			    sizeof (lstr), NULL, NULL, 0, B_TRUE);
			rvalid = zfs_prop_get(r, psc->sc_prop, rstr,
			    sizeof (rstr), NULL, NULL, 0, B_TRUE);

			if ((lvalid == -1) && (rvalid == -1))
				continue;
			if (lvalid == -1)
				return (1);
			else if (rvalid == -1)
				return (-1);

			ret = strcmp(lstr, rstr);
		} else {
			lvalid = zfs_prop_valid_for_type(psc->sc_prop,
			    zfs_get_type(l));
			rvalid = zfs_prop_valid_for_type(psc->sc_prop,
			    zfs_get_type(r));

			if (!lvalid && !rvalid)
				continue;
			else if (!lvalid)
				return (1);
			else if (!rvalid)
				return (-1);

			(void) zfs_prop_get_numeric(l, psc->sc_prop, &lnum,
			    NULL, NULL, 0);
			(void) zfs_prop_get_numeric(r, psc->sc_prop, &rnum,
			    NULL, NULL, 0);

			if (lnum < rnum)
				ret = -1;
			else if (lnum > rnum)
				ret = 1;
		}

		if (ret != 0) {
			if (psc->sc_reverse == B_TRUE)
				ret = (ret < 0) ? 1 : -1;
			return (ret);
		}
	}

	return (zfs_compare(larg, rarg, NULL));
}

int
zfs_for_each(int argc, char **argv, boolean_t recurse, zfs_type_t types,
    zfs_sort_column_t *sortcol, zfs_iter_f callback, void *data)
{
	callback_data_t cb;
	int ret = 0;
	zfs_node_t *node;
	uu_avl_walk_t *walk;

	avl_pool = uu_avl_pool_create("zfs_pool", sizeof (zfs_node_t),
	    offsetof(zfs_node_t, zn_avlnode), zfs_sort, UU_DEFAULT);

	if (avl_pool == NULL) {
		(void) fprintf(stderr,
		    gettext("internal error: out of memory\n"));
		exit(1);
	}

	cb.cb_sortcol = sortcol;
	cb.cb_recurse = recurse;
	cb.cb_types = types;
	if ((cb.cb_avl = uu_avl_create(avl_pool, NULL, UU_DEFAULT)) == NULL) {
		(void) fprintf(stderr,
		    gettext("internal error: out of memory\n"));
		exit(1);
	}

	if (argc == 0) {
		/*
		 * If given no arguments, iterate over all datasets.
		 */
		cb.cb_recurse = 1;
		ret = zfs_iter_root(g_zfs, zfs_callback, &cb);
	} else {
		int i;
		zfs_handle_t *zhp;
		zfs_type_t argtype;

		/*
		 * If we're recursive, then we always allow filesystems as
		 * arguments.  If we also are interested in snapshots, then we
		 * can take volumes as well.
		 */
		argtype = types;
		if (recurse) {
			argtype |= ZFS_TYPE_FILESYSTEM;
			if (types & ZFS_TYPE_SNAPSHOT)
				argtype |= ZFS_TYPE_VOLUME;
		}

		for (i = 0; i < argc; i++) {
			if ((zhp = zfs_open(g_zfs, argv[i], argtype)) != NULL)
				ret |= zfs_callback(zhp, &cb);
			else
				ret = 1;
		}
	}

	/*
	 * At this point we've got our AVL tree full of zfs handles, so iterate
	 * over each one and execute the real user callback.
	 */
	for (node = uu_avl_first(cb.cb_avl); node != NULL;
	    node = uu_avl_next(cb.cb_avl, node))
		ret |= callback(node->zn_handle, data);

	/*
	 * Finally, clean up the AVL tree.
	 */
	if ((walk = uu_avl_walk_start(cb.cb_avl, UU_WALK_ROBUST)) == NULL) {
		(void) fprintf(stderr,
		    gettext("internal error: out of memory"));
		exit(1);
	}

	while ((node = uu_avl_walk_next(walk)) != NULL) {
		uu_avl_remove(cb.cb_avl, node);
		zfs_close(node->zn_handle);
		free(node);
	}

	uu_avl_walk_end(walk);
	uu_avl_destroy(cb.cb_avl);
	uu_avl_pool_destroy(avl_pool);

	return (ret);
}

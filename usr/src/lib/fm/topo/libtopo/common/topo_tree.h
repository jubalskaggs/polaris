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

#ifndef _TOPO_TREE_H
#define	_TOPO_TREE_H

#pragma ident	"@(#)topo_tree.h	1.1	06/02/11 SMI"

#include <fm/topo_mod.h>

#include <topo_list.h>
#include <topo_prop.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct topo_modhash topo_modhash_t;

typedef struct topo_range {
	topo_instance_t	tr_min;
	topo_instance_t tr_max;
} topo_range_t;

typedef struct topo_nodehash {
	topo_list_t th_list;		/* next/prev pointers */
	tnode_t **th_nodearr;		/* node array */
	uint_t th_arrlen;		/* size of node array */
	char *th_name;			/* name for all nodes in this hash */
	topo_mod_t *th_enum;		/* enumerator module */
	topo_range_t th_range;		/* instance ranges for nodes */
} topo_nodehash_t;

typedef struct topo_imethod {
	topo_list_t tim_list;		/* next/prev pointers */
	pthread_mutex_t tim_lock;	/* method entry lock */
	pthread_cond_t	tim_cv;		/* method entry cv */
	uint_t tim_busy;		/* method entry busy indicator */
	char *tim_name;			/* Method name */
	topo_version_t tim_version;	/* Method version */
	topo_stability_t tim_stability;	/* SMI stability of method */
	char *tim_desc;			/* Method description */
	topo_method_f *tim_func;	/* Method function */
	struct topo_mod *tim_mod;	/* Ptr to controlling module */
} topo_imethod_t;

struct topo_node {
	pthread_mutex_t	tn_lock;	/* lock protecting members */
	char *tn_name;			/* Node name */
	topo_instance_t tn_instance;	/* Node instance */
	int tn_state;			/* node state (see below) */
	int tn_fflags;			/* fmri flags (see libtopo.h) */
	struct topo_node *tn_parent;	/* Node parent */
	topo_nodehash_t *tn_phash;	/* parent hash bucket for this node */
	topo_hdl_t *tn_hdl;		/* topo handle pointer */
	topo_mod_t *tn_enum;		/* Enumerator module */
	topo_list_t tn_children;	/* hash table of child nodes */
	topo_list_t tn_pgroups;		/* Property group list */
	topo_list_t tn_methods;		/* Registered method list */
	void *tn_priv;			/* Private enumerator data */
	int tn_refs;			/* node reference count */
};

#define	TOPO_NODE_INIT		0x0001
#define	TOPO_NODE_ROOT		0x0002
#define	TOPO_NODE_BOUND		0x0004
#define	TOPO_NODE_LINKED	0x0008

typedef struct topo_tree {
	topo_list_t tt_list;		/* next/prev pointers */
	char *tt_scheme;		/* Scheme name */
	void *tt_file;			/* Topology file info */
	struct topo_node *tt_root;	/* Root node */
	topo_walk_t *tt_walk;		/* private walker */
} ttree_t;

struct topo_walk {
	struct topo_hdl *tw_thp;	/* Topo handle pointer */
	struct topo_node *tw_root;	/* Root node of current walk */
	struct topo_node *tw_node;	/* Current walker node */
	topo_walk_cb_t tw_cb;		/* Walker callback function */
	void *tw_pdata;			/* Private callback data */
};

typedef struct topo_alloc {
	int ta_flags;
	nv_alloc_t ta_nva;
	nv_alloc_ops_t ta_nvops;
	void *(*ta_alloc)(size_t, int);
	void *(*ta_zalloc)(size_t, int);
	void (*ta_free)(void *, size_t);
} topo_alloc_t;

struct topo_hdl {
	pthread_mutex_t	th_lock;	/* lock protecting hdl */
	char *th_uuid;			/* uuid of snapshot */
	char *th_rootdir;		/* Root directory of plugin paths */
	topo_modhash_t *th_modhash;	/* Module hash */
	topo_list_t th_trees;		/* Scheme-specific topo tree list */
	topo_alloc_t *th_alloc;		/* allocators */
	int th_errno;			/* errno */
	int th_debug;			/* Debug mask */
	int th_dbout;			/* Debug channel */
};

#define	TOPO_UUID_SIZE	37	/* libuuid limit + 1 */

extern ttree_t *topo_tree_create(topo_hdl_t *, topo_mod_t *, const char *);
extern void topo_tree_destroy(topo_hdl_t *, ttree_t *);
extern int topo_tree_enum_all(topo_hdl_t *);

extern int topo_file_load(topo_hdl_t *, topo_mod_t *, ttree_t *);
extern void topo_file_unload(topo_hdl_t *, ttree_t *);

extern void topo_node_lock(tnode_t *);
extern void topo_node_unlock(tnode_t *);
extern void topo_node_hold(tnode_t *);
extern void topo_node_rele(tnode_t *);
extern tnode_t *topo_node_lookup(tnode_t *, const char *, topo_instance_t);
extern int topo_node_hash(topo_nodehash_t *, topo_instance_t);

extern int topo_walk_bottomup(topo_walk_t *, int);

#ifdef __cplusplus
}
#endif

#endif	/* _TOPO_TREE_H */
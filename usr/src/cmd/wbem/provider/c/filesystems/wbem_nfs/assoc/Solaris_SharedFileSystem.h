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
 * Copyright 2003 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SOLARIS_SHAREDFILESYSTEM_H
#define	_SOLARIS_SHAREDFILESYSTEM_H

#pragma ident	"@(#)Solaris_SharedFileSystem.h	1.2	05/06/08 SMI"

#ifdef __cplusplus
extern "C" {
#endif

#include <cimapi.h>
#include <cimomhandle.h>
#include "nfsprov_include.h"

#define	PROPCOUNT 2

static nfs_prov_prop_t sharedFSProps[] = {
#define	SAME 0
	{"SameElement", cim_true, reference},
#define	SYS (SAME + 1)
	{"SystemElement", cim_true, reference}
};

#ifdef __cplusplus
}
#endif

#endif /* _SOLARIS_SHAREDFILESYSTEM_H */

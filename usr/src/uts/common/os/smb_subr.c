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

#pragma ident	"@(#)smb_subr.c	1.1	05/08/26 SMI"

#include <sys/smbios_impl.h>
#include <sys/cmn_err.h>
#include <sys/varargs.h>
#include <sys/systm.h>
#include <sys/kmem.h>

/*ARGSUSED*/
const char *
smb_strerror(int err)
{
	return (NULL);
}

void *
smb_alloc(size_t len)
{
	return (kmem_alloc(len, KM_SLEEP));
}

void *
smb_zalloc(size_t len)
{
	return (kmem_zalloc(len, KM_SLEEP));
}

void
smb_free(void *buf, size_t len)
{
	kmem_free(buf, len);
}

/*PRINTFLIKE2*/
void
smb_dprintf(smbios_hdl_t *shp, const char *format, ...)
{
	va_list ap;

	if (!(shp->sh_flags & SMB_FL_DEBUG))
		return;

	va_start(ap, format);
	vcmn_err(CE_CONT, format, ap);
	va_end(ap);
}

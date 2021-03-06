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

#pragma ident	"@(#)vlfmt.c	1.8	06/08/15 SMI"

/*	Copyright (c) 1988 AT&T	*/
/*	  All Rights Reserved  	*/


/* vlfmt() - format, print and log (variable arguments) */

#pragma	weak vlfmt = _vlfmt

#include "synonyms.h"
#include "mtlib.h"
#include <sys/types.h>
#include <pfmt.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <thread.h>
#include "pfmt_data.h"

int
vlfmt(FILE *stream, long flag, const char *format, va_list args)
{
	int ret;

	const char *text, *sev;
	if ((ret = __pfmt_print(stream, flag, format, &text, &sev, args)) < 0)
		return (ret);

	return (__lfmt_log(text, sev, args, flag, ret));
}

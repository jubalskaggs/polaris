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
#pragma ident	"@(#)unused.c	1.7	06/04/14 SMI"

#include	"msg.h"
#include	"_debug.h"
#include	"libld.h"

void
Dbg_unused_unref(Rt_map *lmp, const char *depend)
{
	if (DBG_NOTCLASS(DBG_C_UNUSED))
		return;
	if (DBG_NOTDETAIL())
		return;

	dbg_print(LIST(lmp), MSG_INTL(MSG_USD_UNREF), NAME(lmp), depend);
}

void
Dbg_unused_sec(Lm_list *lml, Is_desc *isp)
{
	const char	*str;

	if (DBG_NOTCLASS(DBG_C_UNUSED))
		return;
	if (DBG_NOTDETAIL())
		return;

	/*
	 * If the file from which this section originates hasn't been referenced
	 * at all, skip this diagnostic, as it would have been covered under
	 * Dbg_unused_file() called from ignore_section_processing().
	 */
	if (isp->is_file &&
	    ((isp->is_file->ifl_flags & FLG_IF_FILEREF) == 0))
		return;

	if (isp->is_flags & FLG_IS_DISCARD)
		str = MSG_INTL(MSG_USD_SECDISCARD);
	else
		str = MSG_ORIG(MSG_STR_EMPTY);

	dbg_print(lml, MSG_INTL(MSG_USD_SEC), isp->is_basename,
	    EC_XWORD(isp->is_shdr->sh_size), isp->is_file->ifl_name, str);
}

void
Dbg_unused_file(Lm_list *lml, const char *name, int needstr, uint_t cycle)
{
	if (DBG_NOTCLASS(DBG_C_UNUSED))
		return;

	if (needstr)
		dbg_print(lml, MSG_INTL(MSG_USD_NEEDSTR), name);
	else if (cycle)
		dbg_print(lml, MSG_INTL(MSG_USD_FILECYCLIC), name, cycle);
	else
		dbg_print(lml, MSG_INTL(MSG_USD_FILE), name);
}
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
#pragma ident	"@(#)sections.c	1.29	06/08/29 SMI"

#include	"msg.h"
#include	"_debug.h"
#include	"libld.h"

/*
 * Error message string table.
 */
static const Msg order_errors[] = {
	MSG_ORD_ERR_INFORANGE,		/* MSG_INTL(MSG_ORD_ERR_INFORANGE) */
	MSG_ORD_ERR_ORDER,		/* MSG_INTL(MSG_ORD_ERR_ORDER) */
	MSG_ORD_ERR_LINKRANGE,		/* MSG_INTL(MSG_ORD_ERR_LINKRANGE) */
	MSG_ORD_ERR_FLAGS,		/* MSG_INTL(MSG_ORD_ERR_FLAGS) */
	MSG_ORD_ERR_CYCLIC,		/* MSG_INTL(MSG_ORD_ERR_CYCLIC) */
	MSG_ORD_ERR_LINKINV		/* MSG_INTL(MSG_ORD_ERR_LINKINV) */
};

void
Dbg_sec_strtab(Lm_list *lml, Os_desc *osp, Str_tbl *stp)
{
	uint_t	i;

	if (DBG_NOTCLASS(DBG_C_STRTAB))
		return;

	if (!osp)
		return;

	Dbg_util_nl(lml, DBG_NL_STD);
	if (stp->st_flags & FLG_STTAB_COMPRESS)
		dbg_print(lml, MSG_INTL(MSG_SEC_STRTAB_COMP), osp->os_name,
		    stp->st_fullstringsize, stp->st_stringsize);
	else
		dbg_print(lml, MSG_INTL(MSG_SEC_STRTAB_STND), osp->os_name,
		    stp->st_fullstringsize);

	if ((DBG_NOTDETAIL()) ||
	    ((stp->st_flags & FLG_STTAB_COMPRESS) == 0))
		return;

	dbg_print(lml, MSG_ORIG(MSG_STR_EMPTY));
	dbg_print(lml, MSG_INTL(MSG_SEC_STRTAB_HD), osp->os_name,
	    stp->st_hbckcnt);

	for (i = 0; i < stp->st_hbckcnt; i++) {
		Str_hash	*sthash;

		dbg_print(lml, MSG_INTL(MSG_SEC_STRTAB_BCKT), i);

		for (sthash = stp->st_hashbcks[i]; sthash;
		    sthash = sthash->hi_next) {
			uint_t	stroff = sthash->hi_mstr->sm_stlen -
			    sthash->hi_stlen;

			if (stroff == 0) {
				dbg_print(lml, MSG_INTL(MSG_SEC_STRTAB_MSTR),
				    sthash->hi_refcnt, sthash->hi_mstr->sm_str);
			} else {
				dbg_print(lml, MSG_INTL(MSG_SEC_STRTAB_SUFSTR),
				    sthash->hi_refcnt,
				    &sthash->hi_mstr->sm_str[stroff],
				    sthash->hi_mstr->sm_str);
			}
		}

	}
}

void
Dbg_sec_in(Lm_list *lml, Is_desc *isp)
{
	const char	*str;

	if (DBG_NOTCLASS(DBG_C_SECTIONS))
		return;

	if (isp->is_file != NULL)
		str = isp->is_file->ifl_name;
	else
		str = MSG_INTL(MSG_STR_NULL);

	dbg_print(lml, MSG_INTL(MSG_SEC_INPUT), isp->is_name, str);
}

void
Dbg_sec_added(Lm_list *lml, Os_desc *osp, Sg_desc *sgp)
{
	const char	*str;

	if (DBG_NOTCLASS(DBG_C_SECTIONS))
		return;

	if (sgp->sg_name && *sgp->sg_name)
		str = sgp->sg_name;
	else
		str = MSG_INTL(MSG_STR_NULL);

	dbg_print(lml, MSG_INTL(MSG_SEC_ADDED), osp->os_name, str);
}

void
Dbg_sec_created(Lm_list *lml, Os_desc *osp, Sg_desc *sgp)
{
	const char	*str;

	if (DBG_NOTCLASS(DBG_C_SECTIONS))
		return;

	if (sgp->sg_name && *sgp->sg_name)
		str = sgp->sg_name;
	else
		str = MSG_INTL(MSG_STR_NULL);

	dbg_print(lml, MSG_INTL(MSG_SEC_CREATED), osp->os_name, str);
}

void
Dbg_sec_discarded(Lm_list *lml, Is_desc *isp, Is_desc *disp)
{
	if (DBG_NOTCLASS(DBG_C_SECTIONS))
		return;

	dbg_print(lml, MSG_INTL(MSG_SEC_DISCARDED), isp->is_basename,
	    isp->is_file->ifl_name, disp->is_basename,
	    disp->is_file->ifl_name);
}

void
Dbg_sec_group(Lm_list *lml, Is_desc *isp, Group_desc *gdp)
{
	const char	*fmt;

	if (DBG_NOTCLASS(DBG_C_SECTIONS))
		return;

	if (gdp->gd_flags & GRP_FLG_DISCARD)
		fmt = MSG_INTL(MSG_SEC_GRP_DISCARDED);
	else
		fmt = MSG_INTL(MSG_SEC_GRP_INPUT);

	dbg_print(lml, fmt, isp->is_name, isp->is_file->ifl_name,
	    gdp->gd_gsectname, gdp->gd_symname);
}

void
Dbg_sec_order_list(Ofl_desc *ofl, int flag)
{
	Os_desc		*osp;
	Is_desc		*isp1;
	Listnode	*lnp1, *lnp2;
	Lm_list		*lml = ofl->ofl_lml;
	const char	*str;

	if (DBG_NOTCLASS(DBG_C_SECTIONS))
		return;
	if (DBG_NOTDETAIL())
		return;

	Dbg_util_nl(lml, DBG_NL_STD);

	/*
	 * If the flag == 0, then the routine is called before sorting.
	 */
	if (flag == 0)
		str = MSG_INTL(MSG_ORD_SORT_BEFORE);
	else
		str = MSG_INTL(MSG_ORD_SORT_AFTER);

	for (LIST_TRAVERSE(&ofl->ofl_ordered, lnp1, osp)) {
		Sort_desc	*sort = osp->os_sort;

		dbg_print(lml, str, osp->os_name);
		dbg_print(lml, MSG_INTL(MSG_ORD_HDR_1),
		    EC_WORD(sort->st_beforecnt), EC_WORD(sort->st_aftercnt),
		    EC_WORD(sort->st_ordercnt));

		for (LIST_TRAVERSE(&osp->os_isdescs, lnp2, isp1)) {
			Word		link;
			Ifl_desc	*ifl = isp1->is_file;
			Is_desc		*isp2;
			const char	*msg;

			if ((isp1->is_flags & FLG_IS_ORDERED) == 0) {
				dbg_print(lml, MSG_INTL(MSG_ORD_TITLE_0),
				    isp1->is_name, isp1->is_file->ifl_name);
				continue;
			}

			if (isp1->is_shdr->sh_flags & SHF_ORDERED) {
				link = isp1->is_shdr->sh_info;
				msg = MSG_ORIG(MSG_SH_INFO);
			} else {
				/* SHF_LINK_ORDER */
				link = isp1->is_shdr->sh_link;
				msg = MSG_ORIG(MSG_SH_LINK);
			}

			if (link == SHN_BEFORE) {
				dbg_print(lml, MSG_INTL(MSG_ORD_TITLE_1),
				    isp1->is_name, isp1->is_file->ifl_name,
				    msg);
				continue;
			}

			if (link == SHN_AFTER) {
				dbg_print(lml, MSG_INTL(MSG_ORD_TITLE_2),
				    isp1->is_name, isp1->is_file->ifl_name,
				    msg);
				continue;
			}

			isp2 = ifl->ifl_isdesc[link];
			dbg_print(lml, MSG_INTL(MSG_ORD_TITLE_3),
			    isp1->is_name, ifl->ifl_name, msg, isp2->is_name,
			    isp2->is_key);
		}
	}
	Dbg_util_nl(lml, DBG_NL_STD);
}

void
Dbg_sec_order_error(Lm_list *lml, Ifl_desc *ifl, Word ndx, int error)
{
	if (DBG_NOTCLASS(DBG_C_SECTIONS))
		return;
	if (DBG_NOTDETAIL())
		return;

	if (error == 0)
		return;

	dbg_print(lml, MSG_INTL(MSG_ORD_ERR_TITLE),
	    ifl->ifl_isdesc[ndx]->is_name, ifl->ifl_name);

	if (error)
		dbg_print(lml, MSG_INTL(order_errors[error - 1]));
}

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

#pragma ident	"@(#)cmd_dimm.c	1.6	05/12/20 SMI"

/*
 * Support routines for DIMMs.
 */

#include <cmd_mem.h>
#include <cmd_dimm.h>
#include <cmd_bank.h>
#include <cmd.h>

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <fm/fmd_api.h>
#include <sys/fm/protocol.h>
#include <sys/mem.h>
#include <sys/nvpair.h>

/*
 * Some errors (RxE/FRx pairs) don't have accurate DIMM (resource) FMRIs,
 * because sufficient information was unavailable prior to correlation.
 * When the DE completes the pair, it uses this routine to retrieve the
 * correct FMRI.
 */
nvlist_t *
cmd_dimm_fmri_derive(fmd_hdl_t *hdl, uint64_t afar, uint16_t synd,
    uint64_t afsr)
{
	nvlist_t *fmri;

	if ((fmri = cmd_mem_fmri_derive(hdl, afar, afsr, synd)) == NULL)
		return (NULL);

	if (fmd_nvl_fmri_expand(hdl, fmri) < 0) {
		nvlist_free(fmri);
		return (NULL);
	}

	return (fmri);
}

nvlist_t *
cmd_dimm_fru(cmd_dimm_t *dimm)
{
	return (dimm->dimm_asru_nvl);
}

nvlist_t *
cmd_dimm_create_fault(fmd_hdl_t *hdl, cmd_dimm_t *dimm, const char *fltnm,
    uint_t cert)
{
	return (fmd_nvl_create_fault(hdl, fltnm, cert, dimm->dimm_asru_nvl,
	    dimm->dimm_asru_nvl, NULL));
}

static void
cmd_dimm_free(fmd_hdl_t *hdl, cmd_dimm_t *dimm, int destroy)
{
	cmd_case_t *cc = &dimm->dimm_case;

	if (cc->cc_cp != NULL) {
		cmd_case_fini(hdl, cc->cc_cp, destroy);
		if (cc->cc_serdnm != NULL) {
			if (fmd_serd_exists(hdl, cc->cc_serdnm) &&
			    destroy)
				fmd_serd_destroy(hdl, cc->cc_serdnm);
			fmd_hdl_strfree(hdl, cc->cc_serdnm);
		}
	}

	if (dimm->dimm_bank != NULL)
		cmd_bank_remove_dimm(hdl, dimm->dimm_bank, dimm);

	cmd_fmri_fini(hdl, &dimm->dimm_asru, destroy);

	if (destroy)
		fmd_buf_destroy(hdl, NULL, dimm->dimm_bufname);
	cmd_list_delete(&cmd.cmd_dimms, dimm);
	fmd_hdl_free(hdl, dimm, sizeof (cmd_dimm_t));
}

void
cmd_dimm_destroy(fmd_hdl_t *hdl, cmd_dimm_t *dimm)
{
	fmd_stat_destroy(hdl, 1, &(dimm->dimm_retstat));
	cmd_dimm_free(hdl, dimm, FMD_B_TRUE);
}

static cmd_dimm_t *
dimm_lookup_by_unum(const char *unum)
{
	cmd_dimm_t *dimm;

	for (dimm = cmd_list_next(&cmd.cmd_dimms); dimm != NULL;
	    dimm = cmd_list_next(dimm)) {
		if (strcmp(dimm->dimm_unum, unum) == 0)
			return (dimm);
	}

	return (NULL);
}

static void
dimm_attach_to_bank(fmd_hdl_t *hdl, cmd_dimm_t *dimm)
{
	cmd_bank_t *bank;

	for (bank = cmd_list_next(&cmd.cmd_banks); bank != NULL;
	    bank = cmd_list_next(bank)) {
		if (fmd_nvl_fmri_contains(hdl, bank->bank_asru_nvl,
		    dimm->dimm_asru_nvl)) {
			cmd_bank_add_dimm(hdl, bank, dimm);
			return;
		}
	}
}

cmd_dimm_t *
cmd_dimm_create(fmd_hdl_t *hdl, nvlist_t *asru)
{
	cmd_dimm_t *dimm;
	const char *unum;
	nvlist_t *fmri;

	if (!fmd_nvl_fmri_present(hdl, asru)) {
		fmd_hdl_debug(hdl, "dimm_lookup: discarding old ereport\n");
		return (NULL);
	}

	if ((unum = cmd_fmri_get_unum(asru)) == NULL) {
		CMD_STAT_BUMP(bad_mem_asru);
		return (NULL);
	}

	fmri = cmd_mem_fmri_create(unum);
	if (fmd_nvl_fmri_expand(hdl, fmri) < 0) {
		CMD_STAT_BUMP(bad_mem_asru);
		nvlist_free(fmri);
		return (NULL);
	}

	fmd_hdl_debug(hdl, "dimm_create: creating new DIMM %s\n", unum);
	CMD_STAT_BUMP(dimm_creat);

	dimm = fmd_hdl_zalloc(hdl, sizeof (cmd_dimm_t), FMD_SLEEP);
	dimm->dimm_nodetype = CMD_NT_DIMM;
	dimm->dimm_version = CMD_DIMM_VERSION;

	cmd_bufname(dimm->dimm_bufname, sizeof (dimm->dimm_bufname), "dimm_%s",
	    unum);
	cmd_fmri_init(hdl, &dimm->dimm_asru, fmri, "dimm_asru_%s", unum);

	nvlist_free(fmri);

	(void) nvlist_lookup_string(dimm->dimm_asru_nvl, FM_FMRI_MEM_UNUM,
	    (char **)&dimm->dimm_unum);

	dimm_attach_to_bank(hdl, dimm);

	cmd_mem_retirestat_create(hdl, &dimm->dimm_retstat, dimm->dimm_unum, 0);

	cmd_list_append(&cmd.cmd_dimms, dimm);
	cmd_dimm_dirty(hdl, dimm);

	return (dimm);
}

cmd_dimm_t *
cmd_dimm_lookup(fmd_hdl_t *hdl, nvlist_t *asru)
{
	cmd_dimm_t *dimm;
	const char *unum;

	if ((unum = cmd_fmri_get_unum(asru)) == NULL) {
		CMD_STAT_BUMP(bad_mem_asru);
		return (NULL);
	}

	dimm = dimm_lookup_by_unum(unum);

	if (dimm != NULL && !fmd_nvl_fmri_present(hdl, dimm->dimm_asru_nvl)) {
		/*
		 * The DIMM doesn't exist anymore, so we need to delete the
		 * state structure, which is now out of date.  The containing
		 * bank (if any) is also out of date, so blow it away too.
		 */
		fmd_hdl_debug(hdl, "dimm_lookup: discarding old dimm\n");

		if (dimm->dimm_bank != NULL)
			cmd_bank_destroy(hdl, dimm->dimm_bank);
		cmd_dimm_destroy(hdl, dimm);

		return (NULL);
	}

	return (dimm);
}

static cmd_dimm_t *
dimm_v0tov1(fmd_hdl_t *hdl, cmd_dimm_0_t *old, size_t oldsz)
{
	cmd_dimm_t *new;

	if (oldsz != sizeof (cmd_dimm_0_t)) {
		fmd_hdl_abort(hdl, "size of state doesn't match size of "
		    "version 0 state (%u bytes).\n", sizeof (cmd_dimm_0_t));
	}

	new = fmd_hdl_zalloc(hdl, sizeof (cmd_dimm_t), FMD_SLEEP);
	new->dimm_header = old->dimm0_header;
	new->dimm_version = CMD_DIMM_VERSION;
	new->dimm_asru = old->dimm0_asru;
	new->dimm_nretired = old->dimm0_nretired;

	fmd_hdl_free(hdl, old, oldsz);
	return (new);
}

static cmd_dimm_t *
dimm_wrapv1(fmd_hdl_t *hdl, cmd_dimm_pers_t *pers, size_t psz)
{
	cmd_dimm_t *dimm;

	if (psz != sizeof (cmd_dimm_pers_t)) {
		fmd_hdl_abort(hdl, "size of state doesn't match size of "
		    "version 1 state (%u bytes).\n", sizeof (cmd_dimm_pers_t));
	}

	dimm = fmd_hdl_zalloc(hdl, sizeof (cmd_dimm_t), FMD_SLEEP);
	bcopy(pers, dimm, sizeof (cmd_dimm_pers_t));
	fmd_hdl_free(hdl, pers, psz);
	return (dimm);
}

void *
cmd_dimm_restore(fmd_hdl_t *hdl, fmd_case_t *cp, cmd_case_ptr_t *ptr)
{
	cmd_dimm_t *dimm;

	for (dimm = cmd_list_next(&cmd.cmd_dimms); dimm != NULL;
	    dimm = cmd_list_next(dimm)) {
		if (strcmp(dimm->dimm_bufname, ptr->ptr_name) == 0)
			break;
	}

	if (dimm == NULL) {
		int migrated = 0;
		size_t dimmsz;

		fmd_hdl_debug(hdl, "restoring dimm from %s\n", ptr->ptr_name);

		if ((dimmsz = fmd_buf_size(hdl, NULL, ptr->ptr_name)) == 0) {
			fmd_hdl_abort(hdl, "dimm referenced by case %s does "
			    "not exist in saved state\n",
			    fmd_case_uuid(hdl, cp));
		} else if (dimmsz > CMD_DIMM_MAXSIZE ||
		    dimmsz < CMD_DIMM_MINSIZE) {
			fmd_hdl_abort(hdl, "dimm buffer referenced by case %s "
			    "is out of bounds (is %u bytes, max %u, min %u)\n",
			    fmd_case_uuid(hdl, cp), dimmsz,
			    CMD_DIMM_MAXSIZE, CMD_DIMM_MINSIZE);
		}

		if ((dimm = cmd_buf_read(hdl, NULL, ptr->ptr_name,
		    dimmsz)) == NULL) {
			fmd_hdl_abort(hdl, "failed to read dimm buf %s",
			    ptr->ptr_name);
		}

		fmd_hdl_debug(hdl, "found %d in version field\n",
		    dimm->dimm_version);

		if (CMD_DIMM_VERSIONED(dimm)) {
			switch (dimm->dimm_version) {
			case CMD_DIMM_VERSION_1:
				dimm = dimm_wrapv1(hdl, (cmd_dimm_pers_t *)dimm,
				    dimmsz);
				break;
			default:
				fmd_hdl_abort(hdl, "unknown version (found %d) "
				    "for dimm state referenced by case %s.\n",
				    dimm->dimm_version, fmd_case_uuid(hdl, cp));
				break;
			}
		} else {
			dimm = dimm_v0tov1(hdl, (cmd_dimm_0_t *)dimm, dimmsz);
			migrated = 1;
		}

		if (migrated) {
			CMD_STAT_BUMP(dimm_migrat);
			cmd_dimm_dirty(hdl, dimm);
		}

		cmd_fmri_restore(hdl, &dimm->dimm_asru);

		if ((errno = nvlist_lookup_string(dimm->dimm_asru_nvl,
		    FM_FMRI_MEM_UNUM, (char **)&dimm->dimm_unum)) != 0)
			fmd_hdl_abort(hdl, "failed to retrieve unum from asru");

		dimm_attach_to_bank(hdl, dimm);

		cmd_mem_retirestat_create(hdl, &dimm->dimm_retstat,
		    dimm->dimm_unum, dimm->dimm_nretired);

		cmd_list_append(&cmd.cmd_dimms, dimm);
	}

	switch (ptr->ptr_subtype) {
	case BUG_PTR_DIMM_CASE:
		fmd_hdl_debug(hdl, "recovering from out of order dimm ptr\n");
		cmd_case_redirect(hdl, cp, CMD_PTR_DIMM_CASE);
		/*FALLTHROUGH*/
	case CMD_PTR_DIMM_CASE:
		cmd_mem_case_restore(hdl, &dimm->dimm_case, cp, "dimm",
		    dimm->dimm_unum);
		break;
	default:
		fmd_hdl_abort(hdl, "invalid %s subtype %d\n",
		    ptr->ptr_name, ptr->ptr_subtype);
	}

	return (dimm);
}

void
cmd_dimm_validate(fmd_hdl_t *hdl)
{
	cmd_dimm_t *dimm, *next;

	for (dimm = cmd_list_next(&cmd.cmd_dimms); dimm != NULL; dimm = next) {
		next = cmd_list_next(dimm);

		if (!fmd_nvl_fmri_present(hdl, dimm->dimm_asru_nvl))
			cmd_dimm_destroy(hdl, dimm);
	}
}

void
cmd_dimm_dirty(fmd_hdl_t *hdl, cmd_dimm_t *dimm)
{
	if (fmd_buf_size(hdl, NULL, dimm->dimm_bufname) !=
	    sizeof (cmd_dimm_pers_t))
		fmd_buf_destroy(hdl, NULL, dimm->dimm_bufname);

	/* No need to rewrite the FMRIs in the dimm - they don't change */
	fmd_buf_write(hdl, NULL, dimm->dimm_bufname, &dimm->dimm_pers,
	    sizeof (cmd_dimm_pers_t));
}

void
cmd_dimm_gc(fmd_hdl_t *hdl)
{
	cmd_dimm_validate(hdl);
}

void
cmd_dimm_fini(fmd_hdl_t *hdl)
{
	cmd_dimm_t *dimm;

	while ((dimm = cmd_list_next(&cmd.cmd_dimms)) != NULL)
		cmd_dimm_free(hdl, dimm, FMD_B_FALSE);
}

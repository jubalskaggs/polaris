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

#pragma ident	"@(#)handlers.c	1.10	05/10/06 SMI"

#include "pmconfig.h"
#include <sys/mkdev.h>
#include <sys/syslog.h>
#include <sys/openpromio.h>
#include <sys/mnttab.h>
#include <syslog.h>
#include <stdlib.h>


#define	STRCPYLIM(dst, src, str) strcpy_limit(dst, src, sizeof (dst), str)
#define	LASTBYTE(str) (str + strlen(str) - 1)

static char nerr_fmt[] = "number is out of range (%s)\n";
static char alloc_fmt[] = "cannot allocate space for \"%s\", %s\n";
static char set_thresh_fmt[] = "error setting threshold(s) for \"%s\", %s\n";
static char bad_thresh_fmt[] = "bad threshold(s)\n";
static char stat_fmt[] = "cannot stat \"%s\", %s\n";
static char always_on[] = "always-on";

/*
 * When lines in a config file (usually "/etc/power.conf") start with
 * a recognized keyword, a "handler" routine is called for specific
 * CPR or PM -related action(s).  Each routine returns a status code
 * indicating whether all tasks were successful; if any errors occured,
 * future CPR or PM updates are skipped.  Following are the handler
 * routines for all keywords:
 */


/*
 * Check for valid autopm behavior and save after ioctl success.
 */
int
autopm(void)
{
	struct btoc {
		char *behavior;
		int cmd, Errno, isdef;
	};
	static struct btoc blist[] = {
		"default",	PM_START_PM,	EBUSY,	1,
		"disable",	PM_STOP_PM,	EINVAL,	0,
		"enable",	PM_START_PM,	EBUSY,	0,
		NULL,		0,		0,	0,
	};
	struct btoc *bp;
	char *behavior;

	for (behavior = LINEARG(1), bp = blist; bp->cmd; bp++) {
		if (strcmp(behavior, bp->behavior) == 0)
			break;
	}
	if (bp->cmd == 0) {
		mesg(MERR, "invalid autopm behavior \"%s\"\n", behavior);
		return (NOUP);
	}

	/*
	 * for "default" behavior, do not enable autopm if not ESTAR_V3
	 */
	if (!bp->isdef || (estar_vers == ESTAR_V3)) {
		if (ioctl(pm_fd, bp->cmd, NULL) == -1 && errno != bp->Errno) {
			mesg(MERR, "autopm %s failed, %s\n",
			    behavior, strerror(errno));
			return (NOUP);
		}
	}
	(void) strcpy(new_cc.apm_behavior, behavior);
	return (OKUP);
}


static int
gethm(char *src, int *hour, int *min)
{
	if (sscanf(src, "%d:%d", hour, min) != 2) {
		mesg(MERR, "bad time format (%s)\n", src);
		return (-1);
	}
	return (0);
}


static void
strcpy_limit(char *dst, char *src, size_t limit, char *info)
{
	if (strlcpy(dst, src, limit) >= limit)
		mesg(MEXIT, "%s is too long (%s)\n", info, src);
}


/*
 * Convert autoshutdown idle and start/finish times;
 * check and record autoshutdown behavior.
 */
int
autosd(void)
{
	char **bp, *behavior;
	char *unrec = gettext("unrecognized autoshutdown behavior");
	static char *blist[] = {
		"autowakeup", "default", "noshutdown",
		"shutdown", "unconfigured", NULL
	};

	new_cc.as_idle = atoi(LINEARG(1));
	if (gethm(LINEARG(2), &new_cc.as_sh, &new_cc.as_sm) ||
	    gethm(LINEARG(3), &new_cc.as_fh, &new_cc.as_fm))
		return (NOUP);
	mesg(MDEBUG, "idle %d, start %d:%02d, finish %d:%02d\n",
	    new_cc.as_idle, new_cc.as_sh, new_cc.as_sm,
	    new_cc.as_fh, new_cc.as_fm);

	for (behavior = LINEARG(4), bp = blist; *bp; bp++) {
		if (strcmp(behavior, *bp) == 0)
			break;
	}
	if (*bp == NULL) {
		mesg(MERR, "%s: \"%s\"\n", unrec, behavior);
		return (NOUP);
	}
	STRCPYLIM(new_cc.as_behavior, *bp, unrec);
	return (OKUP);
}


/*
 * Check for a real device and try to resolve to a full path.
 * The orig/resolved path may be modified into a prom pathname,
 * and an allocated copy of the result is stored at *destp;
 * the caller will need to free that space.  Returns 1 for any
 * error, otherwise 0; also sets *errp after an alloc error.
 */
static int
devpath(char **destp, char *src, int *errp)
{
	struct stat stbuf;
	char buf[PATH_MAX];
	char *cp, *dstr;
	int devok, dcs = 0;
	size_t len;

	/*
	 * When there's a real device, try to resolve the path
	 * and trim the leading "/devices" component.
	 */
	if ((devok = (stat(src, &stbuf) == 0 && stbuf.st_rdev)) != 0) {
		if (realpath(src, buf) == NULL) {
			mesg(MERR, "realpath cannot resolve \"%s\"\n",
			    src, strerror(errno));
			return (1);
		}
		src = buf;
		dstr = "/devices";
		len = strlen(dstr);
		dcs = (strncmp(src, dstr, len) == 0);
		if (dcs)
			src += len;
	} else
		mesg(MDEBUG, stat_fmt, src, strerror(errno));

	/*
	 * When the path has ":anything", display an error for
	 * a non-device or truncate a resolved+modifed path.
	 */
	if (cp = strchr(src, ':')) {
		if (devok == 0) {
			mesg(MERR, "physical path may not contain "
			    "a minor string (%s)\n", src);
			return (1);
		} else if (dcs)
			*cp = '\0';
	}

	if ((*destp = strdup(src)) == NULL) {
		*errp = NOUP;
		mesg(MERR, alloc_fmt, src, strerror(errno));
	}
	return (*destp == NULL);
}


/*
 * Call pm ioctl request(s) to set property/device dependencies.
 */
static int
dev_dep_common(int isprop)
{
	int cmd, argn, upval = OKUP;
	char *src, *first, **destp;
	pm_req_t pmreq;

	bzero(&pmreq, sizeof (pmreq));
	src = LINEARG(1);
	if (isprop) {
		cmd = PM_ADD_DEPENDENT_PROPERTY;
		first = NULL;
		pmreq.pmreq_kept = src;
	} else {
		cmd = PM_ADD_DEPENDENT;
		if (devpath(&first, src, &upval))
			return (upval);
		pmreq.pmreq_kept = first;
	}
	destp = &pmreq.pmreq_keeper;

	/*
	 * Now loop through any dependents.
	 */
	for (argn = 2; (src = LINEARG(argn)) != NULL; argn++) {
		if (devpath(destp, src, &upval)) {
			if (upval != OKUP)
				return (upval);
			break;
		}
		if ((upval = ioctl(pm_fd, cmd, &pmreq)) == -1) {
			mesg(MDEBUG, "pm ioctl, cmd %d, errno %d\n"
			    "kept \"%s\", keeper \"%s\"\n",
			    cmd, errno, pmreq.pmreq_kept, pmreq.pmreq_keeper);
			mesg(MERR, "cannot set \"%s\" dependency "
			    "for \"%s\", %s\n", pmreq.pmreq_keeper,
			    pmreq.pmreq_kept, strerror(errno));
		}
		free(*destp);
		*destp = NULL;
		if (upval != OKUP)
			break;
	}

	free(first);
	return (upval);
}


int
ddprop(void)
{
	return (dev_dep_common(1));
}


int
devdep(void)
{
	return (dev_dep_common(0));
}


/*
 * Convert a numeric string (with a possible trailing scaling byte)
 * into an integer.  Returns a converted value and *nerrp unchanged,
 * or 0 with *nerrp set to 1 for a conversion error.
 */
static int
get_scaled_value(char *str, int *nerrp)
{
	longlong_t svalue = 0, factor = 1;
	char *sp;

	errno = 0;
	svalue = strtol(str, &sp, 0);
	if (errno || (*str != '-' && (*str < '0' || *str > '9')))
		*nerrp = 1;
	else if (sp && *sp != '\0') {
		if (*sp == 'h')
			factor = 3600;
		else if (*sp == 'm')
			factor = 60;
		else if (*sp != 's')
			*nerrp = 1;
	}
	/* any bytes following sp are ignored */

	if (*nerrp == 0) {
		svalue *= factor;
		if (svalue < INT_MIN || svalue > INT_MAX)
			*nerrp = 1;
	}
	if (*nerrp)
		mesg(MERR, nerr_fmt, str);
	mesg(MDEBUG, "got scaled value %d\n", (int)svalue);
	return ((int)svalue);
}


/*
 * Increment the count of threshold values,
 * reallocate *vlistp and append another element.
 * Returns 1 on error, otherwise 0.
 */
static int
vlist_append(int **vlistp, int *vcntp, int value)
{
	(*vcntp)++;
	if (*vlistp = realloc(*vlistp, *vcntp * sizeof (**vlistp)))
		*(*vlistp + *vcntp - 1) = value;
	else
		mesg(MERR, alloc_fmt, "threshold list", strerror(errno));
	return (*vlistp == NULL);
}


/*
 * Convert a single threshold string or paren groups of thresh's as
 * described below.  All thresh's are saved to an allocated list at
 * *vlistp; the caller will need to free that space.  On return:
 * *vcntp is the count of the vlist array, and vlist is either
 * a single thresh or N groups of thresh's with a trailing zero:
 * (cnt_1 thr_1a thr_1b [...]) ... (cnt_N thr_Na thr_Nb [...]) 0.
 * Returns 0 when all conversions were OK, and 1 for any syntax,
 * conversion, or alloc error.
 */
static int
get_thresh(int **vlistp, int *vcntp)
{
	int argn, value, gci, grp_cnt = 0, paren = 0, nerr = 0;
	char *rp, *src;

	for (argn = 2; (src = LINEARG(argn)) != NULL; argn++) {
		if (*src == LPAREN) {
			gci = *vcntp;
			if (nerr = vlist_append(vlistp, vcntp, 0))
				break;
			paren = 1;
			src++;
		}
		if (*(rp = LASTBYTE(src)) == RPAREN) {
			if (paren) {
				grp_cnt = *vcntp - gci;
				*(*vlistp + gci) = grp_cnt;
				paren = 0;
				*rp = '\0';
			} else {
				nerr = 1;
				break;
			}
		}

		value = get_scaled_value(src, &nerr);
		if (nerr || (nerr = vlist_append(vlistp, vcntp, value)))
			break;
	}

	if (nerr == 0 && grp_cnt)
		nerr = vlist_append(vlistp, vcntp, 0);
	return (nerr);
}


/*
 * Set device thresholds from (3) formats:
 * 	path	"always-on"
 * 	path	time-spec: [0-9]+[{h,m,s}]
 *	path	(ts1 ts2 ...)+
 */
int
devthr(void)
{
	int cmd, upval = OKUP, nthresh = 0, *vlist = NULL;
	pm_req_t pmreq;

	bzero(&pmreq, sizeof (pmreq));
	if (devpath(&pmreq.physpath, LINEARG(1), &upval))
		return (upval);

	if (strcmp(LINEARG(2), always_on) == 0) {
		cmd = PM_SET_DEVICE_THRESHOLD;
		pmreq.value = INT_MAX;
	} else if (get_thresh(&vlist, &nthresh)) {
		mesg(MERR, bad_thresh_fmt);
		upval = NOUP;
	} else if (nthresh == 1) {
		pmreq.value = *vlist;
		cmd = PM_SET_DEVICE_THRESHOLD;
	} else {
		pmreq.data = vlist;
		pmreq.datasize = (nthresh * sizeof (*vlist));
		cmd = PM_SET_COMPONENT_THRESHOLDS;
	}

	if (upval != NOUP && (upval = ioctl(pm_fd, cmd, &pmreq)) == -1)
		mesg(MERR, set_thresh_fmt, pmreq.physpath, strerror(errno));

	free(vlist);
	free(pmreq.physpath);
	return (upval);
}


static int
scan_int(char *src, int *dst)
{
	long lval;

	errno = 0;

	lval = strtol(LINEARG(1), NULL, 0);
	if (errno || lval > INT_MAX || lval < 0) {
		mesg(MERR, nerr_fmt, src);
		return (NOUP);
	}

	*dst = (int)lval;
	return (OKUP);
}

static int
scan_float(char *src, float *dst)
{
	float fval;

	errno = 0;

	fval = strtof(src, NULL);
	if (errno || fval < 0.0) {
		mesg(MERR, nerr_fmt, src);
		return (NOUP);
	}

	*dst = fval;
	return (OKUP);
}


int
dreads(void)
{
	return (scan_int(LINEARG(1), &new_cc.diskreads_thold));
}


/*
 * Set pathname for idlecheck;
 * an overflowed pathname is treated as a fatal error.
 */
int
idlechk(void)
{
	STRCPYLIM(new_cc.idlecheck_path, LINEARG(1), "idle path");
	return (OKUP);
}


int
loadavg(void)
{
	return (scan_float(LINEARG(1), &new_cc.loadaverage_thold));
}


int
nfsreq(void)
{
	return (scan_int(LINEARG(1), &new_cc.nfsreqs_thold));
}


#ifdef sparc
static char open_fmt[] = "cannot open \"%s\", %s\n";

/*
 * Verify the filesystem type for a regular statefile is "ufs"
 * or verify a block device is not in use as a mounted filesytem.
 * Returns 1 if any error, otherwise 0.
 */
static int
check_mount(char *sfile, dev_t sfdev, int ufs)
{
	char *src, *err_fmt = NULL, *mnttab = MNTTAB;
	int rgent, match = 0;
	struct extmnttab ent;
	FILE *fp;

	if ((fp = fopen(mnttab, "r")) == NULL) {
		mesg(MERR, open_fmt, mnttab, strerror(errno));
		return (1);
	}

	/*
	 * Search for a matching dev_t;
	 * ignore non-ufs filesystems for a regular statefile.
	 */
	while ((rgent = getextmntent(fp, &ent, sizeof (ent))) != -1) {
		if (rgent > 0) {
			mesg(MERR, "error reading \"%s\"\n", mnttab);
			(void) fclose(fp);
			return (1);
		} else if (ufs && strcmp(ent.mnt_fstype, "ufs"))
			continue;
		else if (makedev(ent.mnt_major, ent.mnt_minor) == sfdev) {
			match = 1;
			break;
		}
	}
	(void) fclose(fp);

	/*
	 * No match is needed for a block device statefile,
	 * a match is needed for a regular statefile.
	 */
	if (match == 0) {
		if (new_cc.cf_type == CFT_SPEC)
			STRCPYLIM(new_cc.cf_devfs, sfile, "block statefile");
		else
			err_fmt = "cannot find ufs mount point for \"%s\"\n";
	} else if (new_cc.cf_type == CFT_UFS) {
		STRCPYLIM(new_cc.cf_fs, ent.mnt_mountp, "mnt entry");
		STRCPYLIM(new_cc.cf_devfs, ent.mnt_special, "mnt special");
		while (*(sfile + 1) == '/') sfile++;
		src = sfile + strlen(ent.mnt_mountp);
		while (*src == '/') src++;
		STRCPYLIM(new_cc.cf_path, src, "statefile path");
	} else
		err_fmt = "statefile device \"%s\" is a mounted filesystem\n";
	if (err_fmt)
		mesg(MERR, err_fmt, sfile);
	return (err_fmt != NULL);
}


/*
 * Convert a Unix device to a prom device and save on success,
 * log any ioctl/conversion error.
 */
static int
utop(void)
{
	union obpbuf {
		char	buf[OBP_MAXPATHLEN + sizeof (uint_t)];
		struct	openpromio oppio;
	};
	union obpbuf oppbuf;
	struct openpromio *opp;
	char *promdev = "/dev/openprom";
	int fd, upval;

	if ((fd = open(promdev, O_RDONLY)) == -1) {
		mesg(MERR, open_fmt, promdev, strerror(errno));
		return (NOUP);
	}

	opp = &oppbuf.oppio;
	opp->oprom_size = OBP_MAXPATHLEN;
	strcpy_limit(opp->oprom_array, new_cc.cf_devfs,
	    OBP_MAXPATHLEN, "statefile device");
	upval = ioctl(fd, OPROMDEV2PROMNAME, opp);
	(void) close(fd);
	if (upval == OKUP)
		STRCPYLIM(new_cc.cf_dev_prom, opp->oprom_array, "prom device");
	else {
		openlog("pmconfig", 0, LOG_DAEMON);
		syslog(LOG_NOTICE,
		    gettext("cannot convert \"%s\" to prom device"),
		    new_cc.cf_devfs);
		closelog();
	}

	return (upval);
}


/*
 * Check for a valid statefile pathname, inode and mount status.
 */
int
sfpath(void)
{
	static int statefile;
	char *err_fmt = NULL;
	char *sfile, *sp, ch;
	struct stat stbuf;
	int dir = 0;
	dev_t dev;

	if (statefile) {
		mesg(MERR, "ignored redundant statefile entry\n");
		return (OKUP);
	} else if (ua_err) {
		if (ua_err != ENOTSUP)
			mesg(MERR, "uadmin(A_FREEZE, A_CHECK, 0): %s\n",
			    strerror(ua_err));
		return (NOUP);
	}

	/*
	 * Check for an absolute path and trim any trailing '/'.
	 */
	sfile = LINEARG(1);
	if (*sfile != '/') {
		mesg(MERR, "statefile requires an absolute path\n");
		return (NOUP);
	}
	for (sp = sfile + strlen(sfile) - 1; sp > sfile && *sp == '/'; sp--)
		*sp = '\0';

	/*
	 * If the statefile doesn't exist, the leading path must be a dir.
	 */
	if (stat(sfile, &stbuf) == -1) {
		if (errno == ENOENT) {
			dir = 1;
			if ((sp = strrchr(sfile, '/')) == sfile)
				sp++;
			ch = *sp;
			*sp = '\0';
			if (stat(sfile, &stbuf) == -1)
				err_fmt = stat_fmt;
			*sp = ch;
		} else
			err_fmt = stat_fmt;
		if (err_fmt) {
			mesg(MERR, err_fmt, sfile, strerror(errno));
			return (NOUP);
		}
	}

	/*
	 * Check for regular/dir/block types, set cf_type and dev.
	 */
	if (S_ISREG(stbuf.st_mode) || (dir && S_ISDIR(stbuf.st_mode))) {
		new_cc.cf_type = CFT_UFS;
		dev = stbuf.st_dev;
	} else if (S_ISBLK(stbuf.st_mode)) {
		if (minor(stbuf.st_rdev) != 2) {
			new_cc.cf_type = CFT_SPEC;
			dev = stbuf.st_rdev;
		} else
			err_fmt = "statefile device cannot be slice 2 (%s)\n"
			    "would clobber the disk label and boot-block\n";
	} else
		err_fmt = "bad file type for \"%s\"\n"
		    "statefile must be a regular file or block device\n";
	if (err_fmt) {
		mesg(MERR, err_fmt, sfile);
		return (NOUP);
	}

	if (check_mount(sfile, dev, (new_cc.cf_type == CFT_UFS)) || utop())
		return (NOUP);
	new_cc.cf_magic = CPR_CONFIG_MAGIC;
	statefile = 1;
	return (OKUP);
}
#endif /* sparc */


/*
 * Try setting system threshold.
 */
int
systhr(void)
{
	int value, nerr = 0, upval = OKUP;
	char *thresh = LINEARG(1);

	if (strcmp(thresh, always_on) == 0)
		value = INT_MAX;
	else if ((value = get_scaled_value(thresh, &nerr)) < 0 || nerr) {
		mesg(MERR, "%s must be a positive value\n", LINEARG(0));
		upval = NOUP;
	}
	if (upval == OKUP)
		(void) ioctl(pm_fd, PM_SET_SYSTEM_THRESHOLD, value);
	return (upval);
}


int
tchars(void)
{
	return (scan_int(LINEARG(1), &new_cc.ttychars_thold));
}

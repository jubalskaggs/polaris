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
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)rem_drv.c	1.23	05/06/08 SMI"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <libintl.h>
#include <string.h>
#include <fcntl.h>
#include <sys/buf.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <malloc.h>
#include <locale.h>
#include <ftw.h>
#include <sys/types.h>
#include <sys/mkdev.h>
#include "addrem.h"
#include "errmsg.h"

#define	FT_DEPTH	15	/* device tree depth for nftw() */

static void usage(void);

int
main(int argc, char *argv[])
{
	int opt;
	char *basedir = NULL, *driver_name = NULL;
	int server = 0, mod_unloaded = 0;
	int modid, found;
	char maj_num[MAX_STR_MAJOR + 1];
	int err;

	(void) setlocale(LC_ALL, "");
#if	!defined(TEXT_DOMAIN)	/* Should be defined by cc -D */
#define	TEXT_DOMAIN "SYS_TEST"	/* Use this only if it weren't */
#endif
	(void) textdomain(TEXT_DOMAIN);

	/*  must be run by root */

	if (getuid() != 0) {
		(void) fprintf(stderr, gettext(ERR_NOT_ROOT));
		exit(1);
	}

	while ((opt = getopt(argc, argv, "b:")) != -1) {
		switch (opt) {
		case 'b' :
			server = 1;
			basedir = calloc(strlen(optarg) + 1, 1);
			if (basedir == NULL) {
				(void) fprintf(stderr, gettext(ERR_NO_MEM));
				exit(1);
			}
			(void) strcat(basedir, optarg);
			break;
		case '?' :
			usage();
			exit(1);
		}
	}

	if (argv[optind] != NULL) {
		driver_name = calloc(strlen(argv[optind]) + 1, 1);
		if (driver_name == NULL) {
			(void) fprintf(stderr, gettext(ERR_NO_MEM));
			exit(1);

		}
		(void) strcat(driver_name, argv[optind]);
		/*
		 * check for extra args
		 */
		if ((optind + 1) != argc) {
			usage();
			exit(1);
		}

	} else {
		usage();
		exit(1);
	}

	/* set up add_drv filenames */
	if ((build_filenames(basedir)) == ERROR) {
		exit(1);
	}

	/* must be only running version of add_drv/mod_drv/rem_drv */
	enter_lock();

	if ((check_perms_aliases(1, 1)) == ERROR)
		err_exit();

	if ((check_name_to_major(R_OK | W_OK)) == ERROR)
		err_exit();

	/* look up the major number of the driver being removed. */
	if ((found = get_major_no(driver_name, name_to_major)) == ERROR) {
		(void) fprintf(stderr, gettext(ERR_MAX_MAJOR), name_to_major);
		err_exit();
	}
	if (found == UNIQUE) {
		(void) fprintf(stderr, gettext(ERR_NOT_INSTALLED),
		    driver_name);
		err_exit();
	}

	if (!server) {
		mod_unloaded = 1;

		/* get the module id for this driver */
		get_modid(driver_name, &modid);

		/* module is installed */
		if (modid != -1) {
			if (modctl(MODUNLOAD, modid) < 0) {
				perror(NULL);
				(void) fprintf(stderr, gettext(ERR_MODUN),
				    driver_name);
				mod_unloaded = 0;
			}
		}
		/* unload driver.conf file */
		if (modctl(MODUNLOADDRVCONF, (major_t)found) < 0) {
			perror(NULL);
			(void) fprintf(stderr,
			    gettext("cannot unload %s.conf\n"), driver_name);
		}
	}

	if (mod_unloaded && (modctl(MODREMMAJBIND, (major_t)found) < 0)) {
		perror(NULL);
		(void) fprintf(stderr, gettext(ERR_MODREMMAJ), found);
	}
	/*
	 * add driver to rem_name_to_major; if this fails, don`t
	 * delete from name_to_major
	 */
	(void) sprintf(maj_num, "%d", found);

	if (append_to_file(driver_name, maj_num,
	    rem_name_to_major, ' ', " ") == ERROR) {
		(void) fprintf(stderr, gettext(ERR_NO_UPDATE),
		    rem_name_to_major);
		err_exit();
	}

	/*
	 * If removing the driver from the running system, notify
	 * kernel dynamically to remove minor perm entries.
	 */
	if (basedir == NULL || (strcmp(basedir, "/") == 0)) {
		err = devfs_rm_minor_perm(driver_name, log_minorperm_error);
		if (err != 0) {
			(void) fprintf(stderr, gettext(ERR_UPDATE_PERM),
				driver_name, err);
		}
	}

	/*
	 * delete references to driver in add_drv/rem_drv database
	 */
	remove_entry(CLEAN_ALL, driver_name);

	/*
	 * Clean up any dangling devfs shadow nodes for this
	 * driver so that, in the event the driver is re-added
	 * to the system, newly created nodes won't incorrectly
	 * pick up these stale shadow node permissions.
	 */
	if (basedir == NULL || (strcmp(basedir, "/") == 0)) {
		err = modctl(MODREMDRVCLEANUP, driver_name, 0, NULL);
		if (err != 0) {
			(void) fprintf(stderr, gettext(ERR_REMDRV_CLEANUP),
				driver_name, err);
		}
	}

	exit_unlock();

	return (NOERR);
}

static void
usage()
{
	(void) fprintf(stderr, gettext(REM_USAGE1));
}

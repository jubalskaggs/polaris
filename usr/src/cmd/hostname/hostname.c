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
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */

/* Portions Copyright 2006 Jeremy Teo */

/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
#pragma ident	"@(#)hostname.c	1.1	06/06/14 SMI"

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <libgen.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/utsname.h>
#include <sys/systeminfo.h>

#ifndef	TEXT_DOMAIN		/* should be defined by cc -D */
#define	TEXT_DOMAIN "SYS_TEST"	/* use this only if it wasn't */
#endif

static char *progname;

static void
usage(void)
{
	(void) fprintf(stderr, gettext("usage: %s [system_name]\n"),
		basename(progname));
	exit(1);
}

int
main(int argc, char *argv[])
{
	char	*nodename = NULL;
	char	c_hostname[SYS_NMLN];
	int	optlet;
	char	*optstring = "?";

	(void) setlocale(LC_ALL, "");
	(void) textdomain(TEXT_DOMAIN);

	progname = argv[0];

	opterr = 0;
	while ((optlet = getopt(argc, argv, optstring)) != -1) {
		switch (optlet) {
		case '?':
			usage();
		}
	}

	/*
	 * if called with no options, just print out the hostname/nodename
	 */
	if (argc <= optind) {
		if (sysinfo(SI_HOSTNAME, c_hostname, SYS_NMLN)) {
			(void) fprintf(stdout, "%s\n", c_hostname);
		} else {
			(void) fprintf(stderr,
			    gettext("%s: unable to obtain hostname\n"),
			    basename(progname));
			exit(1);
		}
	} else {
		/*
		 * if called with an argument,
		 * we have to try to set the new hostname/nodename
		 */
		if (argc > optind + 1)
			usage();	/* too many arguments */

		nodename = argv[optind];
		if (sysinfo(SI_SET_HOSTNAME, nodename, strlen(nodename)) < 0) {
			int err = errno;
			(void) fprintf(stderr,
			    gettext("%s: error in setting name: %s\n"),
			    basename(progname), strerror(err));
			exit(1);
		}
	}
	return (0);
}

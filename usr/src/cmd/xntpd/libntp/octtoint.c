/*
 * Copyright (c) 1996 by Sun Microsystems, Inc.
 * All Rights Reserved.
 */

#pragma ident	"@(#)octtoint.c	1.1	96/11/01 SMI"

/*
 * octtoint - convert an ascii string in octal to an unsigned
 *	      long, with error checking
 */
#include <stdio.h>
#include <ctype.h>

#include "ntp_stdlib.h"

int
octtoint(str, ival)
	const char *str;
	u_long *ival;
{
	register u_long u;
	register const char *cp;

	cp = str;

	if (*cp == '\0')
		return 0;

	u = 0;
	while (*cp != '\0') {
		if (!isdigit(*cp) || *cp == '8' || *cp == '9')
			return 0;
		if (u >= 0x20000000)
			return 0;	/* overflow */
		u <<= 3;
		u += *cp++ - '0';	/* ascii dependent */
	}
	*ival = u;
	return 1;
}

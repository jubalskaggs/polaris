
/*
 *
 * Portions Copyright 07/17/00 Sun Microsystems, Inc. 
 * All Rights Reserved
 *
 */
#pragma ident	"@(#)addentry.c	1.3	00/07/17 SMI"
/*
 *  Copyright (c) 1990 Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  addentry.c
 */

#ifndef lint 
static char copyright[] = "@(#) Copyright (c) 1990 Regents of the University of Michigan.\nAll rights reserved.\n";
#endif

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#ifdef MACOS
#include <stdlib.h>
#include "macos.h"
#else /* MACOS */
#if defined( DOS ) || defined( _WIN32 )
#include <malloc.h>
#include "msdos.h"
#else /* DOS */
#include <sys/types.h>
#include <sys/socket.h>
#endif /* DOS */
#endif /* MACOS */

#include "lber.h"
#include "ldap.h"
#include "ldap-private.h"

LDAPMessage *
ldap_delete_result_entry( LDAPMessage **list, LDAPMessage *e )
{
	LDAPMessage	*tmp, *prev = NULL;

	for ( tmp = *list; tmp != NULL && tmp != e; tmp = tmp->lm_chain )
		prev = tmp;

	if ( tmp == NULL )
		return( NULL );

	if ( prev == NULL )
		*list = tmp->lm_chain;
	else
		prev->lm_chain = tmp->lm_chain;
	tmp->lm_chain = NULL;

	return( tmp );
}

void
ldap_add_result_entry( LDAPMessage **list, LDAPMessage *e )
{
	e->lm_chain = *list;
	*list = e;
}

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

#ifndef _UTILS_H
#define	_UTILS_H

#pragma ident	"@(#)utils.h	1.13	05/06/08 SMI"

#ifdef	__cplusplus
extern "C" {
#endif

#include <security/pam_appl.h>
#include <security/pam_impl.h>
#include <krb5.h>
#include <netdb.h>

#define	KRB5_DATA	"SUNW-KRB5-AUTH-DATA"
#define	ROOT_UNAME	"root"

typedef struct {
	int		debug;
	int		warn;
	int		err_on_exp;
	int		auth_status;
	char		*env;		/* don't free! sent to putenv... */
	krb5_ccache	ccache;		/* file credential cache */
	krb5_context	kcontext;
	krb5_creds	initcreds;	/* initial creds from */
					/* pam_authenticate() */
	char		*password;
	int		age_status;
} krb5_module_data_t;

int get_pw_uid(char *, uid_t *);
int get_pw_gid(char *, gid_t *);
int get_kmd_kuser(krb5_context, const char *, char *, int);
int key_in_keytab(const char *, int);
int set_active_user(char *, uid_t *, gid_t *);

#ifdef	__cplusplus
}
#endif

#endif /* _UTILS_H */

/*	$OpenBSD: tildexpand.h,v 1.4 2001/06/26 17:27:25 markus Exp $	*/

#ifndef	_TILDEXPAND_H
#define	_TILDEXPAND_H

#pragma ident	"@(#)tildexpand.h	1.4	03/11/19 SMI"

#ifdef __cplusplus
extern "C" {
#endif


/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 */

char	*tilde_expand_filename(const char *, uid_t);

#ifdef __cplusplus
}
#endif

#endif /* _TILDEXPAND_H */

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

#pragma ident	"@(#)_lwp_mutex_unlock.s	1.2	05/06/08 SMI"

	.file	"_lwp_mutex_unlock.s"

#include <sys/asm_linkage.h>

	ANSI_PRAGMA_WEAK(_lwp_mutex_unlock,function)

#include "SYS.h"
#include <../assym.h>

	ANSI_PRAGMA_WEAK2(_private_lwp_mutex_unlock,_lwp_mutex_unlock,function)

	ENTRY(_lwp_mutex_unlock)
	movq	%rdi, %rax
	addq	$MUTEX_LOCK_WORD, %rax
	xorl	%ecx, %ecx
	xchgl	(%rax), %ecx	/* clear lock and get old lock into %ecx */
	andl	$WAITER_MASK, %ecx	/* was anyone waiting on it? */
	je	1f
	SYSTRAP_RVAL1(lwp_mutex_wakeup)
	SYSLWPERR
	RET
1:
	RETC
	SET_SIZE(_lwp_mutex_unlock)

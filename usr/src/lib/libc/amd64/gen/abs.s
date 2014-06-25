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
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

	.ident	"@(#)abs.s	1.3	06/01/02 SMI"

	.file	"abs.s"

/*
 *	Assembler program to implement the following C program
 *
 *	int
 *	abs(int arg)
 *	{
 *		return((arg < 0)? -arg: arg);
 *	}
 */

#include <sys/asm_linkage.h>

	ANSI_PRAGMA_WEAK(llabs,function)

#include "SYS.h"

	ENTRY(abs)
	movl	%edi, %eax
	testl	%eax, %eax	/* arg < 0? */
	jns	1f
	negl	%eax		/* yes, return -arg */
1:
	ret
	SET_SIZE(abs)

 	ENTRY(labs)
 	movq	%rdi, %rax
 	testq	%rax, %rax	/* arg < 0? */
 	jns	1f
 	negq	%rax		/* yes, return -arg */
1:
 	ret
 	SET_SIZE(labs)

 	ENTRY(llabs)
 	movq	%rdi, %rax
 	testq	%rax, %rax	/* arg < 0? */
 	jns	1f
 	negq	%rax		/* yes, return -arg */
1:
 	ret
 	SET_SIZE(llabs)
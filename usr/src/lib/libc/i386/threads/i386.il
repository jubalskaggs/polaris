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

#pragma ident	"@(#)i386.il	1.8	05/06/08 SMI"

	.inline	_curthread, 0
	movl	%gs:0, %eax
	.end

	.inline	__curthread, 0
	xorl	%eax, %eax
	movw	%gs, %ax
	andl	%eax, %eax
	je	1f
	movl	%gs:0, %eax
1:
	.end

	.inline	stkptr, 0
	movl	%esp, %eax
	.end

	.inline	gethrtime, 0
	movl	$3, %eax
	int	$0xd2
	.end

	.inline	set_lock_byte, 0
	movl	$1, %eax
	movl	(%esp), %ecx
	xchgb	%al, (%ecx)
	.end

	.inline	cas32, 0
	movl	0(%esp), %edx
	movl	4(%esp), %eax
	movl	8(%esp), %ecx
	lock
	cmpxchgl %ecx, (%edx)
	.end

	.inline	swap32, 0
	movl	0(%esp), %ecx
	movl	4(%esp), %eax
	xchgl	(%ecx), %eax
	.end

	.inline	incr32, 0
	movl	0(%esp), %eax
	lock
	incl	(%eax)
	.end

	.inline	decr32, 0
	movl	0(%esp), %eax
	lock
	decl	(%eax)
	.end

	.inline ht_pause, 0
	rep			/ "rep nop" is equivalent to "pause"
	nop
	.end

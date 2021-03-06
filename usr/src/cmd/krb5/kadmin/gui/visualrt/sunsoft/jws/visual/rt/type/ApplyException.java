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
 * ident	"@(#)ApplyException.java	1.2	05/06/08 SMI"
 *
 * Copyright (c) 2000 by Sun Microsystems, Inc.
 * All rights reserved.
 */

/*
 *        Copyright (C) 1996  Active Software, Inc.
 *                  All rights reserved.
 *
 * @(#) ApplyException.java 1.2 - last change made 07/18/96
 */

package sunsoft.jws.visual.rt.type;

/**
 * An exception thrown by type editors when there is a problem with
 * the value during an apply.  The Designer will place the error
 * message into a popup window.
 *
 * @see TypeEditor
 * @version 1.2, 07/18/96
 */
public class ApplyException extends Exception {
    public ApplyException() {
        super();
    }
    
    public ApplyException(String s) {
        super(s);
    }
}

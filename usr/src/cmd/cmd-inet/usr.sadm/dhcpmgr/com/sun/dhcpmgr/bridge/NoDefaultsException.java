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
 * ident	"@(#)NoDefaultsException.java	1.2	05/06/08 SMI"
 *
 * Copyright (c) 2000-2001 by Sun Microsystems, Inc.
 * All rights reserved.
 */
package com.sun.dhcpmgr.bridge;

/**
 * The exception that occurs if no DHCP defaults file exists.
 */
public class NoDefaultsException extends BridgeException {

    /**
     * The basic constructor.
     */
    public NoDefaultsException() {
	super("no_defaults_exception");
    } // constructor

    /**
     * Constructor used by JNI code.
     * @param ignored this argument will be ignored
     * @param args this argument will be ignored as well.
     */
    public NoDefaultsException(String ignored, Object [] args) {
	this();
    } // constructor

} // NoDefaultsException

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
 * Copyright 2001-2003 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * ident	"@(#)AuditEvent_rlogin.java	1.2	05/06/08 SMI"
 */
package com.sun.audit;

// audit event:  AUE_rlogin = 6155

public class AuditEvent_rlogin extends AuditEvent {

	private native void putEvent(byte[]session, 
	    int status, int ret_val,
	    int		message);

	public AuditEvent_rlogin(AuditSession session)
		throws Exception
	{
		super(session);
	}


	// See adt_login_text in AuditEvent.java for valid values
	private int message_val = 0;	// optional
	public void message(int setTo)
	{
		message_val = setTo;
	}

	public void putEvent(int status, int ret_val)
	{
		byte[]	session = super.sh.getSession();

		if ((super.sh.AuditIsOn) && (super.sh.ValidSession))
			putEvent(session, status, ret_val,
			    message_val);
	}
}

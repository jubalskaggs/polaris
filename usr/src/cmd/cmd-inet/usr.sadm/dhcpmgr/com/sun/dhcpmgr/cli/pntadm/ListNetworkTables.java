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
 * ident	"@(#)ListNetworkTables.java	1.3	05/06/08 SMI"
 *
 * Copyright (c) 2001 by Sun Microsystems, Inc.
 * All rights reserved.
 */
package com.sun.dhcpmgr.cli.pntadm;

import com.sun.dhcpmgr.data.Network;
import com.sun.dhcpmgr.bridge.NoEntryException;
import com.sun.dhcpmgr.bridge.NoDefaultsException;

import java.lang.IllegalArgumentException;

/**
 * The main class for the "list network tables" functionality
 * of pntadm.
 */
public class ListNetworkTables extends PntAdmFunction {

    /**
     * The valid options associated with listing network tables.
     */
    static final int supportedOptions[] = {
	PntAdm.RESOURCE,
	PntAdm.RESOURCE_CONFIG,
	PntAdm.PATH
    };

    /**
     * Constructs a ListNetworkTables object.
     */
    public ListNetworkTables() {

	validOptions = supportedOptions;

    } // constructor

    /**
     * Returns the option flag for this function.
     * @returns the option flag for this function.
     */
    public int getFunctionFlag() {
	return (PntAdm.LIST_NETWORK_TABLES);
    }

    /**
     * Executes the "list network tables" functionality.
     * @return PntAdm.SUCCESS, PntAdm.ENOENT, PntAdm.WARNING, or
     * PntAdm.CRITICAL
     */
    public int execute()
	throws IllegalArgumentException {

	int returnCode = PntAdm.SUCCESS;

	// Get the list of networks.
	//
	Network [] networks = null;
	try {
		networks = getNetMgr().getNetworks(getDhcpDatastore());
	} catch (NoEntryException e) {
	    // No network tables
	} catch (Throwable e) {
	    printErrMessage(getMessage(e));
	    return (PntAdm.WARNING);
	}

	if (networks != null) {
	    for (int i = 0; i < networks.length; i++) {
		System.out.println(networks[i].toString());
	    }
	}

	return (returnCode);

    } // execute

} // ListNetworkTables
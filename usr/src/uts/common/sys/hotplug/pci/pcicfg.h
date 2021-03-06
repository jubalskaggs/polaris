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

#ifndef	_SYS_HOTPLUG_PCI_PCICFG_H
#define	_SYS_HOTPLUG_PCI_PCICFG_H

#pragma ident	"@(#)pcicfg.h	1.5	05/11/08 SMI"

#ifdef	__cplusplus
extern "C" {
#endif

#include <sys/hotplug/pci/pcihp_impl.h>

/*
 * Interfaces exported by PCI configurator module, kernel/misc/pcicfg.
 */
int pcicfg_configure(dev_info_t *, uint_t);
int pcicfg_unconfigure(dev_info_t *, uint_t);

#define	PCICFG_SUCCESS DDI_SUCCESS
#define	PCICFG_FAILURE DDI_FAILURE

/*
 * The following subclass definition for Non Transparent bridge should
 * be moved to pci.h.
 */
#define	PCI_BRIDGE_STBRIDGE	0x9

#define	PCICFG_CONF_INDIRECT_MAP	1
#define	PCICFG_CONF_DIRECT_MAP		0

#define	PCICFG_DEV_CONF_MAP_PROP	PCI_DEV_CONF_MAP_PROP
#define	PCICFG_BUS_CONF_MAP_PROP	PCI_BUS_CONF_MAP_PROP

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_HOTPLUG_PCI_PCICFG_H */

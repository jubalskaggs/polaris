#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
#ident	"@(#)Makefile	1.4	06/06/28 SMI"

MODULE = wrsmd.so
MDBTGT = kvm

MODSRCS = wrsmd.c

include $(SRC)/cmd/Makefile.cmd
include $(SRC)/cmd/Makefile.cmd.64
include $(SRC)/cmd/mdb/sparc/Makefile.sparcv9
include $(SRC)/cmd/mdb/sun4u/Makefile.sun4u
include $(SRC)/cmd/mdb/Makefile.module

CPPFLAGS += -DMP -D_MACHDEP
CPPFLAGS += -D_KERNEL
CPPFLAGS += -I../../../../../../../src/cmd/mdb/common
CPPFLAGS += -I$(SRC)/uts/sun4u
CPPFLAGS += -I$(SRC)/uts/sfmmu
CPPFLAGS += -I$(SRC)/uts/sparc/v9

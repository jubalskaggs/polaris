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
# ident	"@(#)Makefile.com	1.9	06/08/01 SMI"
#

LIBRARY =	libdiagcode.a
VERS =		.1
OBJECTS =	diagcode.o
TSTOBJECTS =    diagcode_test.o

include ../../../Makefile.lib
include ../../Makefile.lib

LIBS =		$(DYNLIB) $(LINTLIB)
LDLIBS +=	-lc

CLOBBERFILES += test $(TSTOBJECTS)

SRCDIR =	../common

CFLAGS +=	$(CCVERBOSE)

$(LINTLIB) := SRCS = $(SRCDIR)/$(LINTSRC)
$(LINTLIB) := LINTFLAGS = -nsvx
$(LINTLIB) := LINTFLAGS64 = -nsvx -Xarch=$(MACH64:sparcv9=v9)

.KEEP_STATE:

all:

install:

lint: lintcheck

test: $(TSTOBJECTS) $(PICS)
	$(LINK.c) -o $@ $(TSTOBJECTS) $(PICS) $(LDLIBS)
	./test $(SRCDIR)/tests

%.o: $(SRCDIR)/%.c
	$(COMPILE.c) $<

include ../../../Makefile.targ
include ../../Makefile.targ

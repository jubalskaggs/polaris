#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License, Version 1.0 only
# (the "License").  You may not use this file except in compliance
# with the License.
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
# ident	"@(#)Makefile	1.14	05/06/08 SMI"
#
# Copyright (c) 1990 by Sun Microsystems, Inc.
#


include ../../Makefile.cmd

PROG = fmli vsig
SRCS = vsig.c main.c
FILEMODE =	0755

# fmli local libraries 
LOCAL = wish oh wish oeu form qued menu proc sys vt
LDLIBS.local = $(LOCAL:%=../%/lib%.a)
fmli := LDLIBS += $(LDLIBS.local) -lcurses

CPPFLAGS = -I../inc $(CPPFLAGS.master)
$(PROG) := LDFLAGS +=

.KEEP_STATE:

all: $(PROG) 

fmli: main.c 
	$(LINK.c) -o $@ main.c $(LDLIBS)
	$(POST_PROCESS)

# check if local libraries are up to date.
$(LDLIBS.local):
	@cd $(@D); pwd; $(MAKE) $(MFLAGS) all

install: all $(ROOTPROG)

clean:

lint: $(SRCS)
	$(LINT.c) main.c
	$(LINT.c) vsig.c

include ../../Makefile.targ

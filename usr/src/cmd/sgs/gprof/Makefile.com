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
# ident	"@(#)Makefile.com	1.10	05/07/21 SMI"
#
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# cmd/sgs/gprof/Makefile.com
#

include 	../../../Makefile.cmd

COMOBJS=	gprof.o arcs.o dfn.o lookup.o calls.o \
		printgprof.o printlist.o readelf.o
WHATOBJS=	whatdir.o

OBJS=		$(COMOBJS) $(WHATOBJS)
BLURBS=		gprof.callg.blurb gprof.flat.blurb
SRCS=		$(COMOBJS:%.o=../common/%.c) \
		$(WHATOBJS:%.o=../../whatdir/common/%.c)

INCLIST=	-I../common -I../../include -I../../include/$(MACH)
DEFLIST=	-DELF_OBJ -DELF
CPPFLAGS=	$(INCLIST) $(DEFLIST) $(CPPFLAGS.master)
CFLAGS +=	$(CCVERBOSE)
C99MODE=	$(C99_ENABLE)
LDLIBS +=	../../sgsdemangler/`mach`/libdemangle.a
LINTFLAGS +=	$(LDLIBS)
CLEANFILES +=	$(LINTOUT)

ROOTCCSBLURB=	$(BLURBS:%=$(ROOTCCSBIN)/%)

$(ROOTCCSBLURB) :=	FILEMODE=	444

$(ROOTCCSBIN)/%: ../common/%
		$(INS.file)

%.o:		../common/%.c
		$(COMPILE.c) $<

%.o:		../../whatdir/common/%.c
		$(COMPILE.c) $<
.PARALLEL: $(OBJS)

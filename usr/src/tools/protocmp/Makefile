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
#ident	"@(#)Makefile	1.5	05/06/08 SMI"
#
# Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.

PROG=		protocmp cpiotranslate
STD_OBJS=	proto_list.o protodir.o list.o stdusers.o \
		exception_list.o arch.o depend.o
CMP_OBJS=	protocmp.o $(STD_OBJS)
CPIO_OBJS=	cpiotranslate.o $(STD_OBJS)
CMP_SRCS=	$(CMP_OBJS:%.o=%.c)
CPIO_SRCS=	$(CPIO_OBJS:%.o=%.c)
CLEANFILES +=	$(CMP_OBJS) $(CPIO_OBJS)

include ../Makefile.tools

CFLAGS += $(CCVERBOSE)

.KEEP_STATE:

all:	$(PROG)

protocmp: $(CMP_OBJS)
	$(LINK.c) -o $@ $(CMP_OBJS) $(LDLIBS)
	$(POST_PROCESS)

cpiotranslate: $(CPIO_OBJS)
	$(LINK.c) -o $@ $(CPIO_OBJS) $(LDLIBS)
	$(POST_PROCESS)

install: all .WAIT $(ROOTONBLDMACHPROG)

lint:	FRC
	$(LINT.c) $(CMP_SRCS)
	$(LINT.c) $(CPIO_SRCS)

FRC:

clean:
	$(RM) $(CLEANFILES)

include ../Makefile.targ

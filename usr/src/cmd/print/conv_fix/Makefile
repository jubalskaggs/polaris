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
# Copyright (c) 1994,1998,2001 by Sun Microsystems, Inc.
# All rights reserved.
#
#ident	"@(#)Makefile	1.11	05/06/08 SMI"
#
# cmd/lp/client/conv_fix/Makefile
#

include 	../Makefile.sp

PROG=		conv_fix

SRCS= 		$(PROG).c

OBJS=		$(SRCS:.c=.o)

ROOTLIBPRINTPROG=	$(PROG:%=$(ROOTLIBPRINT)/%)

FILEMODE=	0555
OWNER=		root

CPPFLAGS +=	-I$(NPRTINC)

.KEEP_STATE:

all:		$(PROG)

install:	all $(ROOTLIBPRINTPROG) 

$(ROOTLIBPRINT)/%: %
	$(INS.file)

strip:
	$(STRIP) $(PROG)

lint:
	$(LINT.c) $(PROG).c $(LDLIBS)

cstyle:
	cstyle $(SRCS)

_msg:
	@echo "Messages are made in usr/src/cmd/print"

clean:
	$(RM) $(OBJS)

clobber: clean
	-$(RM) $(PROG) $(CLOBBERFILES)

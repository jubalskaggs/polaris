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
# Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"@(#)Makefile	1.5	05/06/08 SMI"
#
# cmd/cmd-inet/usr.lib/inetd/Makefile
#

PROG	=	inetd
MANIFEST=	inetd.xml inetd-upgrade.xml
SVCMETHOD=	inetd-upgrade

OBJS	= inetd.o tlx.o config.o util.o contracts.o repval.o wait.o env.o
SRCS	= $(OBJS:%.o=%.c)

include ../../../Makefile.cmd
include ../../Makefile.cmd-inet
include ../../../svc/Makefile.ctf

ROOTMANIFESTDIR=	$(ROOTSVCNETWORK)
$(ROOTMANIFEST)		:= FILEMODE= 444

CPPFLAGS  += -D_FILE_OFFSET_BITS=64 -I$(CMDINETCOMMONDIR) -I$(ROOTSFWINCLUDE) \
		-D_REENTRANT
$(RELEASE_BUILD)CPPFLAGS +=     -DNDEBUG

LDFLAGS   += -R$(SFW_ROOT)/lib
LDLIBS    += -lsocket -lnsl -lrestart -lscf -lcontract -linetutil \
		-L$(ROOTSFWLIB) -lwrap -linetsvc -luutil -lumem -lbsm

CLOBBERFILES += $(SVCMETHOD)

# lint doesn't like the unused _umem_*_init()
lint_SRCS := LINTFLAGS += -xerroff=E_NAME_DEF_NOT_USED2

.PARALLEL:	$(OBJS)
.WAIT:		$(PROG)
.KEEP_STATE:

all:		$(PROG) $(SVCMETHOD)

$(PROG): 	$(OBJS)
		$(LINK.c) $(OBJS) -o $@ $(LDLIBS) $(CTFMERGE_HOOK)
		$(POST_PROCESS)

include ../Makefile.lib

install:	all $(ROOTLIBINETPROG) $(ROOTMANIFEST) $(ROOTSVCMETHOD)
		-$(RM) $(ROOTUSRSBINPROG)
		-$(SYMLINK) ../lib/inet/${PROG} $(ROOTUSRSBINPROG)

$(ROOTMANIFEST): $(ROOTMANIFESTDIR)

$(ROOTMANIFESTDIR):
		$(INS.dir)

$(ROOTMANIFESTDIR)/%: %
		$(INS.file)

check:		$(CHKMANIFEST)

clean:
		$(RM) $(OBJS)

lint:		lint_SRCS

include ../../../Makefile.targ

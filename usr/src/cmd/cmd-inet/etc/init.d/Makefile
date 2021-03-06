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
#ident	"@(#)Makefile	1.23	05/06/08 SMI"
#
# cmd/cmd-inet/etc/init.d/Makefile

PROG4=	ncalogd
PROG5=	ncakmod
MIPD=	mipagent
PPPD=	pppd
PROG=	$(PROG4) $(PROG5) \
	$(MIPD) $(PPPD)

include ../../../Makefile.cmd

STARTINET3= $(ROOTETC)/rc2.d/S94ncalogd
STARTINET4= $(ROOTETC)/rc2.d/S42ncakmod

MIPDK= K06mipagent
MIPDS= S80mipagent

PPPDK=	K50pppd
PPPDS=	S47pppd

INITD= $(ROOTETC)/init.d
DIRS= $(INITD) $(ROOTETC)/rc0.d $(ROOTETC)/rcS.d $(ROOTETC)/rc1.d \
	$(ROOTETC)/rc2.d $(ROOTETC)/rc3.d
FILEMODE= 0744
OWNER= root
GROUP= sys

ETCINITPROG=	$(PROG:%=$(INITD)/%)

.KEEP_STATE:

all: $(PROG)

install: all $(DIRS) $(ETCINITPROG) $(ETCDFLTPROG) ln_PROG

$(INITD)/% : % $(INITD)
	$(INS.file)

ln_PROG : $(ETCINITPROG)
	@for dir in rc0.d rcS.d rc1.d; do \
		echo $(LN) $(INITD)/$(MIPD) $(ROOTETC)/$$dir/$(MIPDK); \
		$(RM) $(ROOTETC)/$$dir/$(MIPDK); \
		$(LN) $(INITD)/$(MIPD) $(ROOTETC)/$$dir/$(MIPDK); \
		echo $(LN) $(INITD)/$(PROG4) $(ROOTETC)/$$dir/K34ncalogd; \
		$(RM) $(ROOTETC)/$$dir/K34ncalogd; \
		$(LN) $(INITD)/$(PROG4) $(ROOTETC)/$$dir/K34ncalogd; \
		echo $(LN) $(INITD)/$(PPPD) $(ROOTETC)/$$dir/$(PPPDK); \
		$(RM) $(ROOTETC)/$$dir/$(PPPDK); \
		$(LN) $(INITD)/$(PPPD) $(ROOTETC)/$$dir/$(PPPDK); \
	done
	-$(RM) $(ROOTETC)/rc2.d/$(MIPDK)
	$(LN) $(INITD)/$(MIPD) $(ROOTETC)/rc2.d/$(MIPDK)
	-$(RM) $(ROOTETC)/rc3.d/$(MIPDS)
	$(LN) $(INITD)/$(MIPD) $(ROOTETC)/rc3.d/$(MIPDS)
	-$(RM) $(STARTINET3)
	$(LN) $(INITD)/$(PROG4) $(STARTINET3)
	-$(RM) $(STARTINET4)
	$(LN) $(INITD)/$(PROG5) $(STARTINET4)
	-$(RM) $(ROOTETC)/rc2.d/$(PPPDS)
	$(LN) $(INITD)/$(PPPD) $(ROOTETC)/rc2.d/$(PPPDS)

$(DIRS):
	$(INS.dir)

clean:

lint:

include ../../../Makefile.targ

.PARALLEL:

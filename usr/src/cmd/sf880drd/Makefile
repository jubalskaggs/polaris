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
#ident	"@(#)Makefile	1.7	05/06/08 SMI"
#
# cmd/sf880drd/Makefile
#

PROG =		sf880drd
PLATFORM =	SUNW,Sun-Fire-880
LINKED_PLATFORM = SUNW,Sun-Fire-V890
MANIFEST =	sf880drd.xml
SVCMETHOD =	sf880dr

include ../Makefile.cmd
include ../../Makefile.psm

ROOTMANIFESTDIR = $(ROOTSVCPLATFORMSUN4U)

CPPFLAGS += -I$(ROOT)/usr/platform/sun4u/include
LDLIBS += -lcfgadm

FILEMODE = 0755
OWNER = root
GROUP = sys

$(ROOTMANIFEST) :=	FILEMODE = 0444
$(ROOTSVCMETHOD) :=	FILEMODE = 0555

PSMPROG = $(USR_PSM_LIB_DIR)/$(PROG)

.KEEP_STATE:

all: $(PROG)

install: all $(PSMPROG) $(ROOTMANIFEST) $(ROOTSVCMETHOD)

check: $(CHKMANIFEST)

clean:

lint:	lint_PROG

$(USR_PLAT_DIR):
	-$(INS.dir.root.sys)

$(USR_PSM_DIR):		$(USR_PLAT_DIR)
	-$(INS.dir.root.sys)

$(USR_PSM_LIB_DIR):	$(USR_PSM_DIR)
	-$(INS.dir.root.sys)

$(USR_PSM_LIB_DIR)/%:		% $(USR_PSM_LIB_DIR)
	$(INS.file)
	cd $(USR_PLAT_DIR)/$(LINKED_PLATFORM)/lib; $(RM) $(PROG); \
	$(SYMLINK) ../../$(PLATFORM)/lib/$(PROG) $(PROG) $(CHOWNLINK) \
	$(CHGRPLINK)

include ../Makefile.targ

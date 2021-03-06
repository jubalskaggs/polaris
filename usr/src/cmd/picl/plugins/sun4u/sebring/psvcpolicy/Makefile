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
# Copyright 2003 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"@(#)Makefile	1.3	05/06/08 SMI"
#

include $(SRC)/Makefile.psm

# include library definitions
include $(SRC)/lib/Makefile.lib

ROOT_PLATFORM = $(USR_PLAT_DIR)/SUNW,Sun-Fire-V490
CHERRY_LINKTO_PLATFORM = SUNW,Sun-Fire-480R

include $(SRC)/cmd/picl/plugins/Makefile.com

ROOTLIBDIR =	$(ROOT_PLAT_PLUGINDIR)
ROOTLIBDIR	:= OWNER = root
ROOTLIBDIR	:= GROUP = sys

.KEEP_STATE:

SUBDIRS=

all :=		TARGET= all
install :=	TARGET= install
clean :=	TARGET= clean
clobber :=	TARGET= clobber
lint :=		TARGET= lint
_msg :=		TARGET= _msg

all:

install: $(ROOTLIBDIR) all; $(MAKE) $(JAG_LINK)

_msg:	$(MSGDOMAIN)

$(MSGDOMAIN):
	$(INS.dir)

$(LIBLINKS):	FRC
	$(RM) $@; $(SYMLINK) $(DYNLIB) $@

# include library targets
include $(SRC)/lib/Makefile.targ
include $(SRC)/cmd/picl/plugins/Makefile.targ

$(ROOTLINTDIR)/%: ../%
	$(INS.file)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:

JAG_LINK=`cd $(ROOT_PLATFORM)/lib; \
echo $(ROOT_PLATFORM); \
rm -f libpsvcpolicy_psr.so.1; \
rm -f libpsvcpolicy_psr.so; \
ln -s ../../$(CHERRY_LINKTO_PLATFORM)/lib/libpsvcpolicy_psr.so.1 libpsvcpolicy_psr.so.1; \
ln -s ../../$(CHERRY_LINKTO_PLATFORM)/lib/libpsvcpolicy_psr.so libpsvcpolicy_psr.so;`

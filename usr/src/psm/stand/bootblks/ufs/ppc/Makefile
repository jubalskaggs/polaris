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
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# psm/stand/bootblks/ufs/sparc/Makefile
#

BASEDIR = ../..

include $(BASEDIR)/ufs/Makefile.ufs

SUBDIRS	= unix chrp

all	:=	TARGET= all
install	:=	TARGET= install
clean	:=	TARGET= clean
clobber	:=	TARGET= clobber
lint	:=	TARGET= lint

.KEEP_STATE:

all: $(INSTALLBOOT) $(SUBDIRS)

install: $(USR_SBIN_INSTALLBOOT) $(SUBDIRS)

lint clean: $(SUBDIRS)

clobber: $(SUBDIRS)
	-$(RM) $(INSTALLBOOT)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:

#
# Pattern matching rules for source in this directory
# XXX do we need the installboot.sh?
%: %.sh
	$(RM) $@
	cat $< > $@
	chmod +x $@

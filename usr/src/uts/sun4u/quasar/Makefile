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
# uts/sun4u/quasar/Makefile
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
#ident	"@(#)Makefile	1.5	05/12/17 SMI"
#
#	This makefile drives the production of the sun4u quasar platform
#	module.
#
#	sun4u implementation architecture dependent
#

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

#
#	Include common rules.
#
include $(UTSBASE)/sun4u/quasar/Makefile.quasar

def		:=	TARGET= def
all		:=	TARGET= all
install		:=	TARGET= install
install_h	:=	TARGET= install_h
clean		:=	TARGET= clean
clobber		:=	TARGET= clobber
lint		:=	TARGET= lint
lintlib		:=	TARGET= lintlib
modlintlib	:=	TARGET= modlintlib
modlist		:=	TARGET= modlist
modlist		:=	NO_STATE= -K $$MODSTATE$$$$
clean.lint	:=	TARGET= clean.lint
check		:=	TARGET= check

#
#	Default build targets.
#
# Note that quasar doesn't currently have any modules of its own to
# build.
#
.KEEP_STATE:

def all install_h clean lint lintlib modlintlib clean.lint clobber check:

modlist:

install:	install_darwin $(INSTALL_DEPS) $(USR_QUASAR_DIR)

INSTALL_TARGET	=	platmod

platmod: 	$(ROOT_QUASAR_MISC_DIR)
	-@$(RM) -r $(OBJS_DIR)
	-@$(RM) $(ROOT_QUASAR_MISC_DIR)/$@
	$(SYMLINK) $(ROOT_QUASAR_MISC_LINK)/$@ $(ROOT_QUASAR_MISC_DIR)/$@

install_darwin:
	@cd ../darwin; $(MAKE) $(TARGET)

#
#	Include common targets.
#
include $(UTSBASE)/sun4u/quasar/Makefile.targ

#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
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
# uts/sun4v/montoya/Makefile
# Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
#ident	"@(#)Makefile	1.1	06/05/05 SMI"
#
#	This makefile drives the production of the sun4v montoya platform
#	modules.
#
#	sun4v montoya implementation architecture dependent
#

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

#
#	Include common rules.
#
include $(UTSBASE)/sun4v/montoya/Makefile.montoya

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
.KEEP_STATE:

def all clean clobber clean.lint modlist:	$(MONTOYA_KMODS)

lintlib:      unix

modlintlib:   $(MONTOYA_KMODS)

IMPLEMENTED_PLATFORM	= SUNW,Netra-CP3060

install:	$(ROOT_MONTOYA_DIR) $(USR_MONTOYA_DIR) \
		$(USR_MONTOYA_LIB_DIR) \
		$(ROOT_MONTOYA_LIB_DIR) \
		.WAIT $(MONTOYA_KMODS)

$(MONTOYA_KMODS):	FRC
	@cd $@; pwd; $(MAKE) $(NO_STATE) $(TARGET)

install_h check:	FRC

lint:	modlintlib .WAIT $(LINT_DEPS)

LINT_LIBS	= $(LINT_LIB) \
			-L$(MONTOYA_LINT_LIB_DIR) \
			-L$(LINT_LIB_DIR) $(LINT_KMODS:%=-l%) \
			    $(CLOSED_LINT_KMODS:%=-l%) \
			-L$(SPARC_LIB_DIR) $(SPARC_LINTS:%=-l%)

lint.platmod:   modlintlib
	@-$(ECHO) "\n$(IMPLEMENTED_PLATFORM) platform-dependent module: global crosschecks:"
	@-$(LINT) $(LINTFLAGS) $(LINT_LIBS) 2>&1 | $(LGREP.2)

EXPORT_SRC:
	$(RM) Makefile+	
	sed -e "/^# EXPORT DELETE START/,/^# EXPORT DELETE END/d" \
		< Makefile > Makefile+
	$(MV) Makefile+ Makefile
	$(CHMOD) 444 Makefile
# EXPORT DELETE END

#
#	Include common targets.
#
include $(UTSBASE)/$(PLATFORM)/montoya/Makefile.targ

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
# uts/sparc/usb_ah/Makefile
#
#	This makefile drives the production of the usb_ah streams module.
#	sparc architecture dependent
#

#pragma ident	"@(#)Makefile	1.4	05/06/08 SMI"

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

#
#	Define the module and object file sets.
#
MODULE		= usb_ah
OBJECTS		= $(USB_AH_OBJS:%=$(OBJS_DIR)/%)
LINTS		= $(USB_AH_OBJS:%.o=$(LINTS_DIR)/%.ln)
ROOTMODULE	= $(ROOT_STRMOD_DIR)/$(MODULE)
WARLOCK_OUT	= $(USB_AH_OBJS:%.o=%.ll)
WARLOCK_OK	= $(MODULE).ok

#
#	Include common rules.
#
include $(UTSBASE)/sparc/Makefile.sparc

# 
# lint pass one enforcement 
# 
CFLAGS += $(CCVERBOSE)

#
# depends on usba, hidparser and mixer related misc modules
#
LDFLAGS	+= -dy -Nmisc/usba -Nmisc/hidparser -Nmisc/audiosup -Nmisc/mixer

#
#	Define targets
#
ALL_TARGET	= $(BINARY)
LINT_TARGET	= $(MODULE).lint
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE)

.KEEP_STATE:

all:		$(ALL_DEPS)

def:		$(DEF_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

lint:		$(LINT_DEPS)

modlintlib:	$(MODLINTLIB_DEPS)

clean.lint:	$(CLEAN_LINT_DEPS)

install:	$(INSTALL_DEPS)


#
#	Include common targets.
#
include $(UTSBASE)/sparc/Makefile.targ

#
#	Defines for local commands.
#
WARLOCK		= warlock
WLCC		= wlcc
TOUCH		= touch
SCCS		= sccs
TEST		= test

#
#	lock_lint rules
#
USB_AH_FILES	= $(MODULE).ll

warlock: $(MODULE).ok

%.wlcmd:
	$(TEST) -f $@ || $(SCCS) get $@

usb_ah.ok: usb_ah.ll usb_ah.wlcmd warlock_ddi.files
	$(WARLOCK) -c usb_ah.wlcmd $(USB_AH_FILES)  \
		-l ../warlock/ddi_dki_impl.ll
	$(TOUCH) $@

%.ll: $(UTSBASE)/common/io/usb/clients/audio/usb_ah/%.c \
		$(UTSBASE)/common/sys/usb/clients/audio/usb_ah/usb_ah.h
	$(WLCC) $(CPPFLAGS) -DDEBUG -o $@ $<

warlock_ddi.files:
	cd ../warlock; pwd; $(MAKE) warlock


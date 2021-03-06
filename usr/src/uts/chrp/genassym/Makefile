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
# Copyright 2006 Cyril Plisko.  All rights reserved.
# Use is subject to license terms.
#
# Copyright 2006 Sun Microsystems Laboratories.  All rights reserved
# Use is subject to license terms.
#
# ident	"%Z%%M%	%I%	%E% CP"
#
#	This makefile drives the production of assym.h through
#	compile time intialized data.
#
#	chrp implementation architecture dependent
#

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

ASSYM_H		= $(DSF_DIR)/$(OBJS_DIR)/assym.h
GENASSYM	= $(DSF_DIR)/$(OBJS_DIR)/genassym

# XXX - added include for /intel/sys in order to locate specific header files
# since were building on x86, not PPC yet

XCOMPILE_INC	= -I$(UTSBASE)/intel

#
#	Include common rules.
#
include $(UTSBASE)/chrp/Makefile.chrp

#
#	Define targets
#
ALL_TARGET	= $(ASSYM_H)

#
# This is DSF_DIR.  Use a short path.
#
DSF_DIR		= .


#
#	Overrides
#
CLEANFILES	= $(GENASSYM) Nothing_to_remove
CLOBBERFILES	= $(ASSYM_H) $(CLEANFILES) Nothing_to_remove

#
#	Default build targets.
#
.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

clean.lint:

install:	def

#
# Create assym.h
#
$(GENASSYM): $(GENASSYM_SRC)
	$(NATIVECC) $(ALWAYS_DEFS) $(GENASSYM_DEFS) $(NATIVE_INC_PATH) $(XCOMPILE_INC) \
	   -o $@ $(GENASSYM_SRC)

$(ASSYM_H): $(OFFSETS_SRC) $(PLATFORM_OFFSETS_SRC) $(GENASSYM)
	$(OFFSETS_CREATE) <$(OFFSETS_SRC) >$@
	$(OFFSETS_CREATE) <$(PLATFORM_OFFSETS_SRC) >>$@
	$(GENASSYM) >>$@

#
#	Include common targets.
#
include $(UTSBASE)/chrp/Makefile.targ

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
# Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
#ident	"@(#)Makefile	1.7	06/06/13 SMI"
#
#	This makefile drives the production of the zulunvm
#	mics module
#
#	sun4u implementation architecture dependent
#

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

#
#	Define the module and object file sets.
#
MODULE		= zuluvm
OBJECTS		= $(ZULUVM_OBJS:%=$(OBJS_DIR)/%)
LINTS		= $(ZULUVM_OBJS:%.o=$(LINTS_DIR)/%.ln)
ROOTMODULE	= $(ROOT_PSM_MISC_DIR)/$(MODULE)

#
#	Include common rules.
#
include $(UTSBASE)/sun4u/Makefile.sun4u

#
#	Override defaults to build a unique, local modstubs.o.
#
MODSTUBS_DIR	 = .
$(MODSTUBS_O)	:= AS_CPPFLAGS += -DZULU_MISC_MODULE

CLEANFILES	+= $(MODSTUBS_O)

#
#	Define targets
#
ALL_TARGET	= $(BINARY)
LINT_TARGET	= $(MODULE).lint
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE)

ZULUVM_OFFSETS		= $(UTSBASE)/sun4u/zuluvm/zuluvm_offsets.in
ZULUVM_OFFSETS_H	= $(OBJS_DIR)/zuluvm_offsets.h

ZULUVM_STATS = -DZULUVM_STATS

#
# We turn off tnf probes for opt builds.
# 
PROBE_FLAGS_OBJ64 = -DNPROBE

#
# lint pass one enforcement
#
CFLAGS += $(CCVERBOSE) $(ZULUVM_STATS) $(PROBE_FLAGS_$(BUILD_TYPE))
ASFLAGS += $(ZULUVM_STATS)
LINTFLAGS += -I$(OBJS_DIR)

#
#	Default build targets.
#
.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

lint:		$(LINT_DEPS)

modlintlib:	$(MODLINTLIB_DEPS) lint64

clean.lint:	$(CLEAN_LINT_DEPS)

install:	$(INSTALL_DEPS)

#
# Special rules for generating assym.h for inclusion in assembly files
#
#$(DSF_DIR)/$(OBJS_DIR)/assym.h: FRC
#	@cd $(DSF_DIR); $(MAKE) all.targ
#
AS_INC_PATH	+= -I$(OBJS_DIR)

ZULUVM_DEPS	+= zulu_hat_asm.o zulu_asm.o zulu_asm.ln zulu_hat_asm.ln

CLEANFILES	+= $(ZULUVM_OFFSETS_H) $(ZULUVM_OFFSETS_OUT)

$(ZULUVM_DEPS:%=$(OBJS_DIR)/%):	$(ZULUVM_OFFSETS_H)

$(ZULUVM_OFFSETS_H): $(ZULUVM_OFFSETS)
	$(OFFSETS_CREATE) <$(ZULUVM_OFFSETS) >$@

#
#	Include common targets.
#
include $(UTSBASE)/sun4u/Makefile.targ

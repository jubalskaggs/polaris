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
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"@(#)Makefile	1.1	06/01/13 SMI"
#

#
# uts/sparc/si3124/Makefile
#
#	This makefile drives the production of the 
#	"platform/i86pc/kernel/drv/si3124" kernel module.
#
#	sparc architecture dependent
#

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

#
#	Define the module and object file sets.
#
MODULE		= si3124
OBJECTS		= $(SI3124_OBJS:%=$(OBJS_DIR)/%)
LINTS		= $(SI3124_OBJS:%.o=$(LINTS_DIR)/%.ln)
WARLOCK_OUT     = $(SI3124_OBJS:%.o=%.ll)
WARLOCK_OK      = $(MODULE).ok
ROOTMODULE	= $(ROOT_DRV_DIR)/$(MODULE)
CONF_SRCDIR     = $(UTSBASE)/common/io/sata/adapters/si3124

#
#	Include common rules.
#
include $(UTSBASE)/sparc/Makefile.sparc

#
#	Define targets
#
ALL_TARGET	= $(BINARY)
LINT_TARGET	= $(MODULE).lint
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE)

#
#	Overrides.
#
DEBUG_FLGS	=
DEBUG_DEFS	+= $(DEBUG_FLGS)

#
# lint pass one enforcement
#  
CFLAGS += $(CCVERBOSE)

#
#	Default build targets.
#
.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS); \
		$(RM) $(WARLOCK_OUT) $(WARLOCK_OK)

clobber:	$(CLOBBER_DEPS); \
		$(RM) $(WARLOCK_OUT) $(WARLOCK_OK)

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


SI3124_FILES   = $(MODULE).ll
SD_FILES = $(SD_OBJS:%.o=../sd/%.ll)
SATA_FILES = $(SATA_OBJS:%.o=-l ../sata/%.ll)
SCSI_FILES = $(SCSI_OBJS:%.o=-l ../scsi/%.ll)
 
warlock: $(MODULE).ok

%.wlcmd:
	$(TEST) -f $@  || $(SCCS) get $@


si3124.ok: si3124.wlcmd $(SI3124_FILES)  warlock_ddi.files \
	sata.files scsi.files sd.files
	$(WARLOCK) -c ./si3124.wlcmd $(SI3124_FILES) \
	$(SD_FILES) \
	$(SCSI_FILES) \
        $(SATA_FILES) \
	-l ../warlock/ddi_dki_impl.ll
	$(TOUCH) $@

%.ll: $(UTSBASE)/common/io/sata/adapters/si3124/%.c
	$(WLCC) $(CPPFLAGS) -D DEBUG -D __sparcv9 -o $@ $<

sata.files:
	@cd ../sata; pwd; $(MAKE) warlock
 
scsi.files:
	@cd ../scsi; pwd; $(MAKE) warlock

sd.files: 
	@cd ../sd; pwd; $(MAKE) warlock_alone

warlock_ddi.files:
	@cd ../warlock; pwd; $(MAKE) warlock

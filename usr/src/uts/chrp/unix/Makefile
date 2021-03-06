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
# Copyright 2005 Cyril Plisko.  All rights reserved.
# Use is subject to license terms.
#
#
# Copyright 2005 Sun Microsystems Laboratories, Inc.  All rights reserved
# Use is subject to license terms.
#
# ident	"%Z%%M%	%I%	%E% CP"
#
#
#	This makefile drives the production of /unix (and unix.o).
#
#	chrp implementation architecture dependent
#

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

#
#	Define the module and object file sets.
#
UNIX		= unix

OBJECTS		= $(SPECIAL_OBJS:%=$(OBJS_DIR)/%) \
		  $(CORE_OBJS:%=$(OBJS_DIR)/%) \
		  $(MACH_NOT_YET_KMODS:%=$(OBJS_DIR)/%)

LINTS		= $(SPECIAL_OBJS:%.o=$(LINTS_DIR)/%.ln) \
		  $(CORE_OBJS:%.o=$(LINTS_DIR)/%.ln) \
		  $(MACH_NOT_YET_KMODS:%.o=$(LINTS_DIR)/%.ln) \
		  $(LINTS_DIR)/vers.ln \
		  $(LINTS_DIR)/modstubs.ln

ROOTMODULE	= $(ROOT_PSM_KERN_DIR)/$(UNIX)

UNIX_BIN	= $(OBJS_DIR)/$(UNIX)

KRTLD_32	= misc/krtld
KRTLD_64	= misc/$(SUBDIR64)/krtld
KRTLD		= $(KRTLD_$(CLASS))

# XXX - /sun4u/unix has cpulib and platlib
# LIBS		= $(GENLIB) $(PLATLIB) $(CPULIB)

LIBS		= $(GENLIB)

GENUNIX		= genunix
GENUNIX_DIR	= ../../ppc/$(GENUNIX)
# XXX - /sun4u/unix has genopts
#GENOPTS		= -L $(GENUNIX_DIR)/$(OBJS_DIR) -l $(GENUNIX)

# XXX - /sun4u/unix has cpuopts
# CPU_DIR		= .
# CPUOPTS		= -L $(CPU_DIR)/$(OBJS_DIR) -l $(CPUNAME)

# XXX - added plat stuff from /sun4u/unix
PLAT_DIR	= ../platmod
PLATOPTS	= -L $(PLAT_DIR)/$(OBJS_DIR) -l$(PLATMOD)

LIBOPTS		= -L $(GENUNIX_DIR)/$(OBJS_DIR) -l$(GENUNIX)

CTFEXTRAOBJS	= $(OBJS_DIR)/vers.o

#
#	Include common rules.
#
include $(UTSBASE)/chrp/Makefile.chrp

#
#	Define targets
#
ALL_TARGET	= $(UNIX_BIN)
LINT_TARGET	= $(LINT_LIB)
INSTALL_TARGET	= $(UNIX_BIN) $(ROOTMODULE)

#
#	This is UNIX_DIR. Use a short path.
#
UNIX_DIR	= .

#
#	Overrides
#
CLEANFILES	+= $(UNIX_O) $(MODSTUBS_O) $(OBJS_DIR)/vers.c \
		   $(OBJS_DIR)/vers.o \
		   $(DTRACESTUBS_O) $(DTRACESTUBS)

CLOBBERFILES	= $(CLEANFILES) $(UNIX_BIN)
CLEANLINTFILES	+= $(LINT_LIB)

#
#	Default build targets.
#
.KEEP_STATE:

def: $(DEF_DEPS)

all: $(ALL_DEPS)

clean: $(CLEAN_DEPS)

clobber: $(CLOBBER_DEPS)

lint: $(LINT_DEPS)

clean.lint: $(CLEAN_LINT_DEPS)

install: $(INSTALL_DEPS)

MAPFILE_32 = $(MAPFILE)
MAPFILE_64 = $(MAPFILE).ppc64

MAPFILE_NAME = $(MAPFILE_$(CLASS))

$(UNIX_BIN):	$(UNIX_O) $(MODSTUBS_O) $(MAPFILE_NAME)  $(LIBS) \
		$(GENLIB) $(DTRACESTUBS)
	$(LD) -dy -o $@ -e _start -I $(KRTLD) -T $(MAPFILE_NAME) -Map mapfile.link \
	    $(UNIX_O) $(MODSTUBS_O) $(LIBOPTS) $(DTRACESTUBS)
	$(CTFMERGE_UNIQUIFY_AGAINST_GENUNIX)
	$(POST_PROCESS)

$(UNIX_O):	$(OBJECTS) $(OBJS_DIR)/vers.o
	$(LD) -r -o $@ $(OBJECTS) $(OBJS_DIR)/vers.o

#
#	Special rules for generating assym.h for inclusion in assembly files.
#
$(DSF_DIR)/$(OBJS_DIR)/assym.h:	FRC
	@cd $(DSF_DIR); $(MAKE) all.targ

$(GENLIB): FRC
	@(cd $(GENLIB_DIR); pwd; $(MAKE) all.targ)
	@pwd

# XXX added for the moment to match sparc 

$(PLATLIB):
#	?@(cd $(PLAT_DIR); pwd; $(MAKE) all.targ)
#	?@pwd

#	The global lint target builds the kernel lint library (llib-lunix.ln)
#	which is equivalent to a lint of /unix.o. Then all kernel modules for
#	this architecture are linted against the kernel lint library.
#
#	Note:	lint errors in the kernel lint library will be repeated for
#		each module. It is important that the kernel lint library
#		be clean to keep the textual output to a reasonable level.
#

$(LINT_LIB):	$(LINT_LIB_DIR) $(LINTS)
	@-$(ECHO) "\n$(UNIX): (library construction):"
	@$(LINT) -o$(UNIX) $(LINTFLAGS) $(LINTS)
	@$(MV) $(@F) $@

lintlib:	$(LINT_DEPS)

#
#	Include common targets.
#
include $(UTSBASE)/chrp/Makefile.targ

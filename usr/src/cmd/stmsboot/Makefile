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
#ident	"@(#)Makefile	1.3	05/06/08 SMI"
#
# Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

include ../Makefile.cmd

SHFILES = stmsboot
SRCS = stmsboot_util.c
STMSBOOT_UTIL = stmsboot_util
MANIFEST = mpxio-upgrade.xml

POFILES = $(SRCS:.c=.po) $(SHFILES:%=%.po)
POFILE = pstmsboot.po
CLOBBERFILES = $(SHFILES) $(STMSBOOT_UTIL) $(POFILE)
ROOTFSLIB_MPXIO = $(ROOT)/lib/mpxio

ROOTMANIFESTDIR=	$(ROOTSVCPLATFORMSUN4U)
ROOTMETHOD=		$(ROOTLIBSVCMETHOD)/mpxio-upgrade

$(ROOTMANIFEST) :=	FILEMODE = 0444

LDLIBS += -ldevinfo -lgen -ldevid

.KEEP_STATE:

all: $(STMSBOOT_UTIL) $(SHFILES)

install: all					\
	$(ROOTFSLIB_MPXIO)			\
	$(ROOTFSLIB_MPXIO)/$(STMSBOOT_UTIL)	\
	$(ROOTMANIFEST)				\
	$(ROOTMETHOD)				\
	$(ROOTUSRSBIN)/stmsboot

check:
	$(CSTYLE) -pP $(SRCS)

clean:
	$(RM) $(POFILES)

lint:	lint_SRCS

include ../Makefile.targ

$(ROOTFSLIB_MPXIO):
	$(INS.dir)

$(ROOTFSLIB_MPXIO)/%: %
	$(INS.file)

$(POFILE): $(POFILES)
	$(RM) $@; cat $(POFILES) > $@

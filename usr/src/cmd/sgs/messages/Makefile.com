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
# ident	"@(#)Makefile.com	1.8	05/06/08 SMI"
#
# Copyright 2000,2003 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# cmd/sgs/messages/Makefile.com

include		$(SRC)/Makefile.master
include		$(SRC)/cmd/sgs/Makefile.com


# Establish our own domain.

TEXT_DOMAIN=	SUNW_OST_SGS

POFILE=		sgs.po

MSGFMT=		msgfmt

# The following message files are generated as part of each utilites build via
# sgsmsg(1l).  By default each file is formatted as a portable object file
# (.po) - see msgfmt(1).  If the sgsmsg -C option has been employed, each file
# is formatted as a message text source file (.msg) - see gencat(1).

POFILES=	ld		ldd		libld		liblddbg \
		libldstab	librtld		rtld		libelf \
		ldprof		libcrle		crle		pvs \
		elfdump		lari


# Define a local version of the message catalog.  Test using: LANG=piglatin

MSGDIR=		$(ROOT)/usr/lib/locale/piglatin/LC_MESSAGES
TEST_MSGID=	test-msgid.po
TEST_MSGSTR=	test-msgstr.po
TEST_POFILE=	test-msg.po
TEST_MOFILE=	$(TEXT_DOMAIN).mo


CLEANFILES=	$(POFILE) $(TEST_MSGID) $(TEST_MSGSTR) $(TEST_POFILE) \
		$(TEST_MOFILE)
CLOBBERFILES=	$(POFILES)

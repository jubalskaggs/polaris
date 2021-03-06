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
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
#ident	"@(#)Makefile	1.4	05/12/17 SMI"
#
# lib/pkcs11/Makefile

include ../Makefile.lib

HDRS =     	cryptoki.h pkcs11.h pkcs11f.h pkcs11t.h
HDRDIR =	include
ROOTHDRDIR =	$(ROOT)/usr/include/security

# RSA PKCS#11 sourced header files won't pass hrdchk
CHECKHDRS =	$(HDRDIR)/cryptoki.h

SUBDIRS = \
	libpkcs11

#
# Don't build these for OpenSolaris, since they will be replaced by
# binaries that are signed by Sun RE.
#
$(CLOSED_BUILD)SUBDIRS += \
			pkcs11_kernel	\
			pkcs11_softtoken

# EXPORT DELETE START
$(CLOSED_BUILD)SUBDIRS	+= pkcs11_softtoken_extra
# EXPORT DELETE END

all 	:=	TARGET= all
clean 	:=	TARGET= clean
clobber	:=	TARGET= clobber
install :=	TARGET= install
lint :=		TARGET= lint

.KEEP_STATE:

all clean clobber install lint: $(SUBDIRS)

install_h: $(ROOTHDRS)

check:	$(CHECKHDRS)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:

# EXPORT DELETE START
EXPORT_SRC:
	$(RM) Makefile+
	$(SED) -e "/^# EXPORT DELETE START/,/^# EXPORT DELETE END/d" \
		< Makefile > Makefile+
	$(MV) -f Makefile+ Makefile
	$(CHMOD) 444 Makefile
# EXPORT DELETE END

include ../Makefile.targ

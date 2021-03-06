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
# ident	"@(#)Makefile	1.5	05/06/08 SMI"
#

include ../Makefile.cmd

ATTR =			audit_record_attr
ROOTETCSECURITY =	$(ROOT)/etc/security

ETCSECURITYFILES = 	$(ATTR:%=$(ROOTETCSECURITY)/%)
$(ETCSECURITYFILES) :=	FILEMODE = 0644
$(ETCSECURITYFILES) :=	GROUP = sys

PROG		= bsmrecord

AGETTEXT	= mkmsg
STRIPTEXT	= filter_txt

.KEEP_STATE:

all: $(PROG) $(ATTR) 

install: all $(ROOTUSRSBINPROG) install_data

install_data: $(ETCSECURITYFILES) $(ATTR)

$(ETCSECURITYFILES): $(ETCSECURITY) $(ROOTETCSECURITY)
	$(INS.file) $(ATTR)

_msg: $(PROG).po

clean:
	$(RM) $(PROG) $(TEXT_DOMAIN).po $(ATTR) $(STRIPTEXT) $(AGETTEXT)

%: %.pl
	$(SED) -e "s@TEXT_DOMAIN@\"$(TEXT_DOMAIN)\"@" $< > $@
	$(CHMOD) 755 $@

$(ATTR): $(STRIPTEXT) $(ATTR).txt
	./$(STRIPTEXT) < $(ATTR).txt > $(ATTR)

$(ROOTUSRSBINPROG): $(PROG)
	$(INS.file) $(PROG)

$(PROG).po: $(PROG) $(ATTR) $(AGETTEXT)
	export PERL5LIB; PERL5LIB=../perl/contrib; \
	./$(AGETTEXT) $(TEXT_DOMAIN) $(PROG).po; \
	$(XGETTEXT) -d $(PROG) -j $(PROG)

lint:

include ../Makefile.targ

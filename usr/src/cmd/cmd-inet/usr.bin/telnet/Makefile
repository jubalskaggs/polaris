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
# ident	"@(#)Makefile	1.12	05/06/08 SMI"
#

PROG=		telnet
OBJS=		telnet.o commands.o main.o network.o ring.o \
		sys_bsd.o terminal.o utilities.o genget.o

include		$(SRC)/lib/gss_mechs/mech_krb5/Makefile.mech_krb5
AUTH_OBJS=	auth.o authenc.o kerberos5.o
AUTH_INCLUDES=	-I$(SRC)/uts/common/gssapi/mechs/krb5/include \
		-I$(SRC)/lib/gss_mechs/mech_krb5/include \
		-I$(SRC)/lib/gss_mechs/mech_krb5

ENC_OBJS=	enc_des.o encrypt.o

OBJS +=		$(AUTH_OBJS) $(ENC_OBJS)

SRCS=		$(OBJS:%.o=%.c)

include		../../../Makefile.cmd

CPPFLAGS +=	-DKLUDGELINEMODE -DUSE_TERMIO -DENV_HACK -DOLD_ENVIRON
CPPFLAGS +=	$(AUTH_INCLUDES)
LDLIBS +=	-lsocket -lnsl -ltermlib
LDLIBS +=	-lmech_krb5 
LDFLAGS +=	$(KRUNPATH) -L$(ROOT)$(KLIBDIR_DO) -L$(ROOT)$(KLIBDIR_GL)
LDFLAGS +=	$(ZLAZYLOAD)

.KEEP_STATE:

all: $(PROG)

$(PROG): $(OBJS)
	$(LINK.c) $(OBJS) -o $@ $(LDLIBS)
	$(POST_PROCESS)

install: all $(ROOTPROG)

clean:
	$(RM) $(OBJS)

lint:   lint_SRCS

include		../../../Makefile.targ

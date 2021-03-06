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
# ident	"@(#)Makefile	1.60	05/06/08 SMI"
#

TESTPROG = gssdtest

OUTPUT_OPTION = -I.

PROG= gssd

MANIFEST=	gss.xml

GSSD_BASEOBJS = gssd.o gssd_proc.o gssd_generic.o gssd_getuid.o
GSSC_BASEOBJS = gssdtest.o gssd_release_name_and_type.o gssd_clnt_stubs.o \
		gssd_handle.o

GD_OBJS	= gssd_svc.o
GC_OBJS	= gssd_clnt.o
G_OBJS	= gssd_xdr.o 
GSSDOBJS = $(GSSD_BASEOBJS) $(GD_OBJS) $(G_OBJS)
GSSCOBJS = $(GSSC_BASEOBJS) $(GC_OBJS) $(G_OBJS)

GSSD_LINTS = $(GSSD_BASEOBJS:.o=.c)
GSSC_LINTS = $(GSSC_BASEOBJS:.o=.c)

ROBJS	= $(GD_OBJS) $(GC_OBJS) $(G_OBJS)
OBJS	= $(GSSD_BASEOBJS) $(GD_OBJS) $(GSSC_BASEOBJS) $(GC_OBJS) $(G_OBJS)
SRCS	= $(OBJS:.o=.c)
RSRC	= $(ROBJS:.o=.c)
RSRC 	+= gssd.h

CLOBBERFILES += $(TESTPROG)

include ../../Makefile.cmd

ROOTMANIFESTDIR=	$(ROOTSVCNETWORKRPC)
$(ROOTMANIFEST)		:= FILEMODE= 444

TEXT_DOMAIN = SUNW_OST_NETRPC
POFILE = $(PROG).po
POFILES = generic.po

#
# Override $ROOTLIB
#
ROOTLIB=	$(ROOT)/usr/lib/gss

DIRS=	$(ROOTLIB)

$(ROOTLIB)/gssd :=	OWNER=	root
$(ROOTLIB)/gssd :=	GROUP=	bin

CPPFLAGS += -I$(SRC)/uts/common/gssapi/include
COPTFLAG += $(XESS) #-I$(KINCDIR) 

LDLIBS += -lgss -lnsl

$(GPROGS) := CPPFLAGS += -DSYSV -DSunOS=50 

.KEEP_STATE:

all: $(PROG) $(TESTPROG)

$(ROOTLIB):
	$(INS.dir)

$(ROOTLIB)/%:	%
	$(INS.file)

gssd:	$(GSSDOBJS) 
	$(LINK.c) $(GSSDOBJS) -o $@ $(LDLIBS)
	$(POST_PROCESS)

gssdtest:       $(GSSCOBJS) 
	$(LINK.c) $(GSSCOBJS) -o $@ $(LDLIBS)
	$(POST_PROCESS)

GSSDX=	$(SRC)/uts/common/gssapi/gssd.x
gssd.x:	$(GSSDX)
	rm -f $@
	cp $(GSSDX) $@

#  Rules to generate derived rpcgen files from gssd.x spec file.

gssd.h:        gssd.x
	$(RM) $@
	$(RPCGEN) -M -h gssd.x > $@

gssd_clnt.c:   gssd.x
	$(RM) $@
	$(RPCGEN) -M -l gssd.x > $@

gssd_svc.c:    gssd.x
	$(RM) $@
	$(RPCGEN) -M -m gssd.x > $@

gssd_xdr.c:    gssd.x
	$(RM) $@
	$(RPCGEN) -M -c gssd.x > $@

$(OBJS): gssd.h

install: all $(DIRS) $(ROOTLIBPROG) $(ROOTMANIFEST)

install_h:

clean:
	$(RM) $(OBJS) $(RSRC) gssd.x

lint_gssd:
	$(LINT.c) $(GSSD_LINTS)

lint_gssc:
	$(LINT.c) $(GSSC_LINTS)

lint:	lint_gssd lint_gssc

check:	$(CHKMANIFEST)

include ../../Makefile.targ

# EXPORT DELETE START
# Special targets to clean up the source tree for export distribution
# The WS target modifies the SCCS files as well, so a working workspace
# can be shipped.
# Warning: These targets change the source tree, the first only at the
#		plain source level, but the second changes the guts!
EXPORT_SRC:
	$(RM) Makefile+ gssd_clnt_stubs.c+ gssd_proc.c+ gssdtest.c+
	sed -e "/^# EXPORT DELETE START/,/^# EXPORT DELETE END/d" \
		< Makefile > Makefile+
	$(MV) Makefile+ Makefile
	sed -e "/EXPORT DELETE START/,/EXPORT DELETE END/d" \
		< gssd_clnt_stubs.c > gssd_clnt_stubs.c+
	$(MV) gssd_clnt_stubs.c+ gssd_clnt_stubs.c
	sed -e "/EXPORT DELETE START/,/EXPORT DELETE END/d" \
		< gssd_proc.c > gssd_proc.c+
	$(MV) gssd_proc.c+ gssd_proc.c
	sed -e "/EXPORT DELETE START/,/EXPORT DELETE END/d" \
		< gssdtest.c > gssdtest.c+
	$(MV) gssdtest.c+ gssdtest.c
	$(CHMOD) 444  Makefile gssd_clnt_stubs.c gssd_proc.c gssdtest.c

# EXPORT DELETE END

$(POFILE): $(DERIVED_FILES) .WAIT $(POFILES)
	$(RM) $@
	$(CAT) $(POFILES) > $@

generic.po: FRC
	$(RM) messages.po
	$(XGETTEXT) $(XGETFLAGS) `$(GREP) -l gettext *.[ch]`
	$(SED) "/^domain/d" messages.po > $@
	$(RM) messages.po

FRC:


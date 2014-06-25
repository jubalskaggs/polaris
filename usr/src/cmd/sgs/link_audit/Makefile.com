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
# ident	"@(#)Makefile.com	1.27	06/08/03 SMI"
#

include		../../../../lib/Makefile.lib
include		../../Makefile.com

NO_ASM_WARN=	-erroff=E_ASM_DISABLES_OPTIMIZATION

ZIGNORE=

SGSPROTO=	../../proto/$(MACH)

TRUSSLIB=	truss.so.1
TRUSSSRC=	truss.c

SYMBINDREP=	symbindrep.so.1
SYMBINDREPSRC=	symbindrep.c

PERFLIB=	perfcnt.so.1
PERFSRC=	perfcnt.c hash.c

WHOLIB=		who.so.1
WHOSRC=		who.c

BINDLIB=	bindings.so.1
BINDSRC=	bindings.c

ONSCRIPTS=	perfcnt symbindrep
ONPROGS=	dumpbind
ONLIBS=		$(SYMBINDREP) $(PERFLIB) $(BINDLIB)

USRSCRIPTS=	sotruss whocalls
CCSLIBS=	$(TRUSSLIB) $(WHOLIB)
 
PICDIR=		pics
OBJDIR=		objs

TRUSSPICS=	$(TRUSSSRC:%.c=$(PICDIR)/%.o) $(PICDIR)/env.o
PERFPICS=	$(PERFSRC:%.c=$(PICDIR)/%.o) $(PICDIR)/env.o
WHOPICS=	$(WHOSRC:%.c=$(PICDIR)/%.o) $(PICDIR)/env.o
SYMBINDREPPICS=	$(SYMBINDREPSRC:%.c=$(PICDIR)/%.o) $(PICDIR)/env.o
BINDPICS=	$(BINDSRC:%.c=$(PICDIR)/%.o) $(PICDIR)/env.o

$(WHOPICS):=	SEMANTICCHK=

LDLIBS +=	$(CONVLIBDIR) $(CONV_LIB)

# Building SUNWonld results in a call to the `package' target.  Requirements
# needed to run this application on older releases are established:
#   dlopen/dlclose requires libdl.so.1 prior to 5.10
# 
DLLIB = $(VAR_DL_LIB)
package	:=  DLLIB = $(VAR_PKG_DL_LIB)

$(TRUSSLIB):=	PICS = $(TRUSSPICS)
$(PERFLIB):=	PICS = $(PERFPICS)
$(WHOLIB):=	PICS = $(WHOPICS)
$(SYMBINDREP):=	PICS = $(SYMBINDREPPICS)
$(BINDLIB):=	PICS = $(BINDPICS)

$(TRUSSLIB):=	LDLIBS += -lmapmalloc -lc
$(PERFLIB):=	LDLIBS += -lmapmalloc -lc
$(WHOLIB):=	LDLIBS += $(ELFLIBDIR) -lelf -lmapmalloc $(DLLIB) -lc
$(SYMBINDREP):=	LDLIBS += -lmapmalloc -lc
$(BINDLIB):=	LDLIBS += -lmapmalloc -lc

$(TRUSSLIB):=	SONAME = $(TRUSSLIB)
$(PERFLIB):=	SONAME = $(PERFLIB)
$(WHOLIB):=	SONAME = $(WHOLIB)
$(SYMBINDREP):=	SONAME = $(SYMBINDREP)
$(BINDLIB):=	SONAME = $(BINDLIB)

$(TRUSSLIB):=	MAPFILES = mapfile-vers-truss
$(PERFLIB):=	MAPFILES = mapfile-vers-perfcnt
$(WHOLIB):=	MAPFILES = mapfile-vers-who
$(SYMBINDREP):=	MAPFILES = mapfile-vers-symbindrep
$(BINDLIB):=	MAPFILES = mapfile-vers-bindings

$(ROOTCCSLIB) :=	OWNER =		root
$(ROOTCCSLIB) :=	GROUP =		bin
$(ROOTCCSLIB) :=	DIRMODE =	755

CPPFLAGS +=	-D_REENTRANT
LDFLAGS +=	$(USE_PROTO)
DYNFLAGS +=	$(VERSREF)

LINTFLAGS +=	-uaxs $(LDLIBS)
LINTFLAGS64 +=	-uaxs $(LDLIBS)

CLEANFILES +=	$(LINTOUT) $(OBJDIR)/* $(PICDIR)/*
CLOBBERFILES +=	$(ONSCRIPTS) $(ONPROGS) $(ONLIBS) $(CCSLIBS) $(USRSCRIPTS)

ROOTONLDLIB=		$(ROOT)/opt/SUNWonld/lib
ROOTONLDLIBS=		$(ONLIBS:%=$(ROOTONLDLIB)/%)
ROOTONLDLIB64=		$(ROOTONLDLIB)/$(MACH64)
ROOTONLDLIBS64=		$(ONLIBS:%=$(ROOTONLDLIB64)/%)

ROOTONLDBIN=		$(ROOT)/opt/SUNWonld/bin
ROOTONLDBINPROG=	$(ONSCRIPTS:%=$(ROOTONLDBIN)/%) \
			$(ONPROGS:%=$(ROOTONLDBIN)/%)

ROOTCCSLIB=		$(ROOT)/usr/lib/link_audit
ROOTCCSLIB64=		$(ROOT)/usr/lib/link_audit/$(MACH64)
ROOTCCSLIBS=		$(CCSLIBS:%=$(ROOTCCSLIB)/%)
ROOTCCSLIBS64=		$(CCSLIBS:%=$(ROOTCCSLIB64)/%)

ROOTUSRBIN=		$(ROOT)/usr/bin
ROOTUSRBINS=		$(USRSCRIPTS:%=$(ROOTUSRBIN)/%)

FILEMODE=	0755

.PARALLEL:	$(LIBS) $(PROGS) $(SCRIPTS) $(TRUSSPICS) $(PERFPICS) \
		$(WHOPICS) $(SYMBINDREPPICS) $(BINDPICS)
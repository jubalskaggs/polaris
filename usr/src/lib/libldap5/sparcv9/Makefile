#
# Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"@(#)Makefile	1.4	04/09/28 SMI"
#
# lib/libldap5/sparcv9/Makefile
#

PR_MDUSOBJS = os_SunOS.o

include ../Makefile.com
include ../../Makefile.lib.64

LDLIBS +=	-L$(NSS_LDPATH64) $(NSS_LIBS)
DYNFLAGS +=	-R$(NSS_LDPATH64)

all: $(LIBS)

install: all $(ROOTLIBS64) $(ROOTLINKS64)

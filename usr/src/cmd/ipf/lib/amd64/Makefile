#
# Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"@(#)Makefile	1.1	04/09/28 SMI"
#

include ../Makefile.com
include $(SRC)/lib/Makefile.lib.64


#
# XX64
#   work around the problem that gcc currently produces
#   buggy gcc -mcmodel=small code
#
CFLAGS64 += $(C_PICFLAGS64)

install: all

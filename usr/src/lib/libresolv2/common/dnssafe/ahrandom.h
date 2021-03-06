/*
 * Copyright (c) 1999 by Sun Microsystems, Inc.
 * All rights reserved.
 */

#pragma ident	"@(#)ahrandom.h	1.1	00/06/27 SMI"

/* Copyright (C) RSA Data Security, Inc. created 1990, 1996.  This is an
   unpublished work protected as such under copyright law.  This work
   contains proprietary, confidential, and trade secret information of
   RSA Data Security, Inc.  Use, disclosure or reproduction without the
   express written authorization of RSA Data Security, Inc. is
   prohibited.
 */

#ifndef _AHRANDOM_H_
#define _AHRANDOM_H_ 1

#include "btypechk.h"

/* Use the THIS_RANDOM macro to define the type of object in the
     virtual function prototype.  It defaults to the most base class, but
     derived modules may define the macro to a more derived class before
     including this header file.
 */
#ifndef THIS_RANDOM
#define THIS_RANDOM struct AHRandom
#endif

struct AHRandom;

typedef struct {
  void (*Destructor) PROTO_LIST ((THIS_RANDOM *));
  int (*RandomInit) PROTO_LIST
    ((THIS_RANDOM *, B_ALGORITHM_CHOOSER, A_SURRENDER_CTX *));
  int (*RandomUpdate) PROTO_LIST
    ((THIS_RANDOM *, unsigned char *, unsigned int, A_SURRENDER_CTX *));
  int (*GenerateBytes) PROTO_LIST
    ((THIS_RANDOM *, unsigned char *, unsigned int, A_SURRENDER_CTX *));
} AHRandomVTable;

typedef struct AHRandom {
  B_TypeCheck typeCheck;                                        /* inherited */
  AHRandomVTable *vTable;                                    /* pure virtual */
} AHRandom;

/* The constructor does not set the vTable since this is a pure base class.
 */
void AHRandomConstructor PROTO_LIST ((AHRandom *));
/* No destructor because it is pure virtual. Also, do not call destructor
     for B_TypeCheck, since this will just re-invoke this virtual
     destructor. */

#endif

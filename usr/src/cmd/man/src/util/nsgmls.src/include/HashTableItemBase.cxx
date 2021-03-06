// Copyright (c) 1994 James Clark
// See the file COPYING for copying permission.
#pragma ident	"@(#)HashTableItemBase.cxx	1.4	00/07/17 SMI"

#ifndef HashTableItemBase_DEF_INCLUDED
#define HashTableItemBase_DEF_INCLUDED 1

#ifdef SP_NAMESPACE
namespace SP_NAMESPACE {
#endif

template<class K>
HashTableItemBase<K>::~HashTableItemBase()
{
}

template<class K>
HashTableItemBase<K>::HashTableItemBase(const K &k) : key(k)
{
}


#ifdef SP_NAMESPACE
}
#endif

#endif /* not HashTableItemBase_DEF_INCLUDED */

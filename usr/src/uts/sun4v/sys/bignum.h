/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef	_BIGNUM_H
#define	_BIGNUM_H

#pragma ident	"@(#)bignum.h	1.2	05/06/08 SMI"


#ifdef	__cplusplus
extern "C" {
#endif

#include <sys/types.h>

typedef int BIG_ERR_CODE;

#define	BITSINBYTE	8

#ifdef BIGNUM_CHUNK_32
#define	BIG_CHUNK_SIZE		32
#define	BIG_CHUNK_TYPE		uint32_t
#define	BIG_CHUNK_HIGHBIT	0x80000000
#define	BIG_CHUNK_ALLBITS	0xffffffff
#define	BIG_CHUNK_LOWHALFBITS	0xffff
#define	BIG_CHUNK_HALF_HIGHBIT	0x8000
#else
#define	BIG_CHUNK_SIZE		64
#define	BIG_CHUNK_TYPE		uint64_t
#define	BIG_CHUNK_HIGHBIT	0x8000000000000000ULL
#define	BIG_CHUNK_ALLBITS	0xffffffffffffffffULL
#define	BIG_CHUNK_LOWHALFBITS	0xffffffffULL
#define	BIG_CHUNK_HALF_HIGHBIT	0x80000000ULL
#endif

#define	CHARLEN2BIGNUMLEN(x)	((x + sizeof (BIG_CHUNK_TYPE) - 1) / \
				sizeof (BIG_CHUNK_TYPE))

#define	BIGNUM_WORDSIZE	(BIG_CHUNK_SIZE / BITSINBYTE)  /* word size in bytes */
#define	BIG_CHUNKS_FOR_160BITS	((160 + BIG_CHUNK_SIZE - 1) / BIG_CHUNK_SIZE)

/*
 * leading 0's are permitted
 * 0 should be represented by size>=1, size>=len>=1, sign=1,
 * value[i]=0 for 0<i<len
 */
typedef struct {
	int size; /* the size of memory allocated for value (in words) */
	int len;  /* the number of words that hold valid data in value */
	int sign; /* 1 for nonnegative, -1 for negative   */
	int malloced; /* 1 if value was malloced 0 if not */
	BIG_CHUNK_TYPE *value;
} BIGNUM;

typedef struct {
	int64_t	size;		/* key size in bits */
	BIGNUM	p;		/* p */
	BIGNUM	q;		/* q */
	BIGNUM	n;		/* n = p * q (the modulus) */
	BIGNUM	d;		/* private exponent */
	BIGNUM	e;		/* public exponent */
	BIGNUM	dmodpminus1;	/* d mod (p - 1) */
	BIGNUM	dmodqminus1;	/* d mod (q - 1) */
	BIGNUM	pinvmodq;	/* p^(-1) mod q */
	BIGNUM	p_rr;		/* 2^(2*(32*p->len)) mod p */
	BIGNUM	q_rr;		/* 2^(2*(32*q->len)) mod q */
	BIGNUM	n_rr;		/* 2^(2*(32*n->len)) mod n */
} RSAkey;

typedef struct {
	int64_t	size;		/* key size in bits */
	BIGNUM	q;		/* q (160-bit prime) */
	BIGNUM	p;		/* p (<size-bit> prime) */
	BIGNUM	g;		/* g (the base) */
	BIGNUM	x;		/* private key (< q) */
	BIGNUM	y;		/* = g^x mod p */
	BIGNUM	k;		/* k (random number < q) */
	BIGNUM	r;		/* r (signiture 1st part) */
	BIGNUM	s;		/* s (signiture 1st part) */
	BIGNUM	v;		/* v (verification value - should be = r ) */
	BIGNUM	p_rr;		/* 2^(2*(32*p->len)) mod p */
	BIGNUM	q_rr;		/* 2^(2*(32*q->len)) mod q */
} DSAkey;

#define	BIGTMPSIZE 65

#define	BIG_TRUE 1
#define	BIG_FALSE 0

/* error codes */
#define	BIG_OK 0
#define	BIG_NO_MEM -1
#define	BIG_INVALID_ARGS -2
#define	BIG_DIV_BY_0 -3
#define	BIG_NO_RANDOM -4
#define	BIG_TEST_FAILED -5

#define	arraysize(x) (sizeof (x) / sizeof (x[0]))

#ifdef	MODEXP_FLOAT
void conv_d16_to_i32(uint32_t *i32, double *d16, int64_t *tmp, int ilen);
void conv_i32_to_d32(double *d32, uint32_t *i32, int len);
void conv_i32_to_d16(double *d16, uint32_t *i32, int len);
void conv_i32_to_d32_and_d16(double *d32, double *d16,
    uint32_t *i32, int len);
void mont_mulf_noconv(uint32_t *result, double *dm1, double *dm2, double *dt,
    double *dn, uint32_t *nint, int nlen, double dn0);
#endif

BIG_ERR_CODE ncp_big_init(BIGNUM *number, int size);
BIG_ERR_CODE ncp_big_extend(BIGNUM *number, int size);
void ncp_big_finish(BIGNUM *number);
int ncp_big_is_zero(BIGNUM *n);
void ncp_kcl2bignum(BIGNUM *bn, uchar_t *kn, size_t len);
void ncp_bignum2kcl(uchar_t *kn, BIGNUM *bn, size_t len);
BIG_ERR_CODE ncp_DSA_key_init(DSAkey *key, int size);
void ncp_DSA_key_finish(DSAkey *key);
BIG_ERR_CODE ncp_RSA_key_init(RSAkey *key, int psize, int qsize);
void ncp_RSA_key_finish(RSAkey *key);
BIG_ERR_CODE ncp_big_mont_rr(BIGNUM *result, BIGNUM *n);
BIG_ERR_CODE ncp_big_modexp(BIGNUM *result, BIGNUM *a, BIGNUM *e,
    BIGNUM *n, BIGNUM *n_rr, void *ncp);
BIG_ERR_CODE ncp_big_modexp_crt(BIGNUM *result, BIGNUM *a, BIGNUM *dmodpminus1,
    BIGNUM *dmodqminus1, BIGNUM *p, BIGNUM *q, BIGNUM *pinvmodq,
    BIGNUM *p_rr, BIGNUM *q_rr, void *ncp);
int ncp_big_cmp_abs(BIGNUM *a, BIGNUM *b);
BIG_ERR_CODE ncp_randombignum(BIGNUM *r, int lengthinbits);
BIG_ERR_CODE ncp_big_div_pos(BIGNUM *result, BIGNUM *remainder,
    BIGNUM *aa, BIGNUM *bb);
BIG_ERR_CODE ncp_big_ext_gcd_pos(BIGNUM *gcd, BIGNUM *cm, BIGNUM *ce,
    BIGNUM *m, BIGNUM *e);
BIG_ERR_CODE ncp_big_add(BIGNUM *result, BIGNUM *aa, BIGNUM *bb);
BIG_ERR_CODE ncp_big_mul(BIGNUM *result, BIGNUM *aa, BIGNUM *bb);
BIG_ERR_CODE ncp_big_nextprime_pos(BIGNUM *result, BIGNUM *n, void *ncp);
BIG_ERR_CODE ncp_big_sub_pos(BIGNUM *result, BIGNUM *aa, BIGNUM *bb);
BIG_ERR_CODE ncp_big_copy(BIGNUM *dest, BIGNUM *src);
BIG_ERR_CODE ncp_big_sub(BIGNUM *result, BIGNUM *aa, BIGNUM *bb);
int ncp_big_bitlength(BIGNUM *n);

#ifdef	__cplusplus
}
#endif

#endif	/* _BIGNUM_H */

/*-------------------------------------------------------------------------
 *
 * pg_popcount_avx512.c
 *	  Holds the pg_popcount() implementation that uses AVX512 instructions.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_popcount_avx512.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <immintrin.h>

#include "port/pg_bitutils.h"

/*
 * It's probably unlikely that TRY_POPCNT_FAST won't be set if we are able to
 * use AVX512 intrinsics, but we check it anyway to be sure.  We rely on
 * pg_popcount_fast() as our fallback implementation in pg_popcount_avx512().
 */
#ifdef TRY_POPCNT_FAST

/*
 * pg_popcount_avx512
 *		Returns the number of 1-bits in buf
 */
uint64
pg_popcount_avx512(const char *buf, int bytes)
{
	uint64		popcnt;
	__m512i		accum = _mm512_setzero_si512();

	for (; bytes >= sizeof(__m512i); bytes -= sizeof(__m512i))
	{
		const		__m512i val = _mm512_loadu_si512((const __m512i *) buf);
		const		__m512i cnt = _mm512_popcnt_epi64(val);

		accum = _mm512_add_epi64(accum, cnt);
		buf += sizeof(__m512i);
	}

	popcnt = _mm512_reduce_add_epi64(accum);
	return popcnt + pg_popcount_fast(buf, bytes);
}

/*
 * pg_popcount_masked_avx512
 *		Returns the number of 1-bits in buf after applying the mask to each byte
 */
uint64
pg_popcount_masked_avx512(const char *buf, int bytes, bits8 mask)
{
	uint64		popcnt;
	__m512i		accum = _mm512_setzero_si512();
	const		__m512i maskv = _mm512_set1_epi8(mask);

	for (; bytes >= sizeof(__m512i); bytes -= sizeof(__m512i))
	{
		const		__m512i val = _mm512_loadu_si512((const __m512i *) buf);
		const		__m512i vmasked = _mm512_and_si512(val, maskv);
		const		__m512i cnt = _mm512_popcnt_epi64(vmasked);

		accum = _mm512_add_epi64(accum, cnt);
		buf += sizeof(__m512i);
	}

	popcnt = _mm512_reduce_add_epi64(accum);
	return popcnt + pg_popcount_masked_fast(buf, bytes, mask);
}

#endif							/* TRY_POPCNT_FAST */

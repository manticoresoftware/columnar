// Copyright (c) 2025, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// The binary distance calculation implementations are based on Elasticsearch's Java implementation:
// https://github.com/elastic/elasticsearch/blob/1dd41ec2b683a7b7c9c16af404b842cf85cbd5bc/server/src/main/java/org/elasticsearch/index/codec/vectors/es816/ES816BinaryFlatVectorsScorer.java
// Modifications copyright (C) 2024 Elasticsearch B.V.
// Original implementation licensed under the Apache License, Version 2.0.
// The algorithm is based on the paper "RaBitQ" (https://arxiv.org/abs/2405.12497)

#include "space.h"

#include "util_private.h"

namespace knn
{

float DistFuncParamIP_t::CalcIP ( int iDotProduct ) const
{
	return m_fK*iDotProduct + m_fB;
}


static float DotProduct ( const std::vector<float> & tA, const std::vector<float> & tB )
{
	assert ( tA.size()==tB.size() );

	float fRes = 0.0f;
	for ( size_t i = 0; i < tA.size(); i++ )
		fRes += tA[i]*tB[i];

	return fRes;
}


#if defined(USE_AVX2)
static FORCE_INLINE float HSum256Ps ( __m256 v )
{
	alignas(32) float dBuffer[8];
	_mm256_store_ps ( dBuffer, v );
	return dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3] + dBuffer[4] + dBuffer[5] + dBuffer[6] + dBuffer[7];
}
#endif


float IPFloatDistance ( const void * pVect1, const void * pVect2, size_t, size_t, const void * pParam )
{
	const auto * pV1 = (const float *)pVect1;
	const auto * pV2 = (const float *)pVect2;
	size_t uDim = *(const size_t *)pParam;

	float fDot = 0.0f;
	size_t i = 0;

#if defined(USE_AVX512)
	__m512 vSum = _mm512_setzero_ps();
	size_t uLimit = uDim & ~size_t(15);
	for ( ; i < uLimit; i += 16 )
	{
		__m512 v1 = _mm512_loadu_ps ( pV1 + i );
		__m512 v2 = _mm512_loadu_ps ( pV2 + i );
		vSum = _mm512_fmadd_ps ( v1, v2, vSum );
	}
	fDot = _mm512_reduce_add_ps ( vSum );
#elif defined(USE_AVX2)
	__m256 vSum = _mm256_setzero_ps();
	size_t uLimit = uDim & ~size_t(7);
	for ( ; i < uLimit; i += 8 )
	{
		__m256 v1 = _mm256_loadu_ps ( pV1 + i );
		__m256 v2 = _mm256_loadu_ps ( pV2 + i );
		vSum = _mm256_add_ps ( vSum, _mm256_mul_ps ( v1, v2 ) );
	}
	fDot = HSum256Ps ( vSum );
#endif

	for ( size_t iTail = i; iTail < uDim; iTail++ )
		fDot += pV1[iTail]*pV2[iTail];

	return 1.0f - fDot;
}


void IPFloatDistanceBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t, size_t, size_t, const void * pParam, float & fDistA, float & fDistB )
{
	const auto * pV1 = (const float *)pVect1;
	const auto * pVA = (const float *)pVect2A;
	const auto * pVB = (const float *)pVect2B;
	size_t uDim = *(const size_t *)pParam;

	float fDotA = 0.0f;
	float fDotB = 0.0f;
	size_t i = 0;

#if defined(USE_AVX512)
	__m512 vSumA = _mm512_setzero_ps();
	__m512 vSumB = _mm512_setzero_ps();
	size_t uLimit = uDim & ~size_t(15);
	for ( ; i < uLimit; i += 16 )
	{
		__m512 v1 = _mm512_loadu_ps ( pV1 + i );
		__m512 vA = _mm512_loadu_ps ( pVA + i );
		__m512 vB = _mm512_loadu_ps ( pVB + i );
		vSumA = _mm512_fmadd_ps ( v1, vA, vSumA );
		vSumB = _mm512_fmadd_ps ( v1, vB, vSumB );
	}
	fDotA = _mm512_reduce_add_ps ( vSumA );
	fDotB = _mm512_reduce_add_ps ( vSumB );
#elif defined(USE_AVX2)
	__m256 vSumA = _mm256_setzero_ps();
	__m256 vSumB = _mm256_setzero_ps();
	size_t uLimit = uDim & ~size_t(7);
	for ( ; i < uLimit; i += 8 )
	{
		__m256 v1 = _mm256_loadu_ps ( pV1 + i );
		__m256 vA = _mm256_loadu_ps ( pVA + i );
		__m256 vB = _mm256_loadu_ps ( pVB + i );
		vSumA = _mm256_add_ps ( vSumA, _mm256_mul_ps ( v1, vA ) );
		vSumB = _mm256_add_ps ( vSumB, _mm256_mul_ps ( v1, vB ) );
	}
	fDotA = HSum256Ps ( vSumA );
	fDotB = HSum256Ps ( vSumB );
#endif

	for ( size_t iTail = i; iTail < uDim; iTail++ )
	{
		fDotA += pV1[iTail]*pVA[iTail];
		fDotB += pV1[iTail]*pVB[iTail];
	}

	fDistA = 1.0f - fDotA;
	fDistB = 1.0f - fDotB;
}


float L2FloatDistance ( const void * pVect1, const void * pVect2, size_t, size_t, const void * pParam )
{
	const auto * pV1 = (const float *)pVect1;
	const auto * pV2 = (const float *)pVect2;
	size_t uDim = *(const size_t *)pParam;

	float fDist = 0.0f;
	size_t i = 0;

#if defined(USE_AVX512)
	__m512 vSum = _mm512_setzero_ps();
	size_t uLimit = uDim & ~size_t(15);
	for ( ; i < uLimit; i += 16 )
	{
		__m512 v1 = _mm512_loadu_ps ( pV1 + i );
		__m512 v2 = _mm512_loadu_ps ( pV2 + i );
		__m512 vDiff = _mm512_sub_ps ( v1, v2 );
		vSum = _mm512_fmadd_ps ( vDiff, vDiff, vSum );
	}
	fDist = _mm512_reduce_add_ps ( vSum );
#elif defined(USE_AVX2)
	__m256 vSum = _mm256_setzero_ps();
	size_t uLimit = uDim & ~size_t(7);
	for ( ; i < uLimit; i += 8 )
	{
		__m256 v1 = _mm256_loadu_ps ( pV1 + i );
		__m256 v2 = _mm256_loadu_ps ( pV2 + i );
		__m256 vDiff = _mm256_sub_ps ( v1, v2 );
		vSum = _mm256_add_ps ( vSum, _mm256_mul_ps ( vDiff, vDiff ) );
	}
	fDist = HSum256Ps ( vSum );
#endif

	for ( size_t iTail = i; iTail < uDim; iTail++ )
	{
		float fDiff = pV1[iTail] - pV2[iTail];
		fDist += fDiff*fDiff;
	}

	return fDist;
}


void L2FloatDistanceBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t, size_t, size_t, const void * pParam, float & fDistA, float & fDistB )
{
	const auto * pV1 = (const float *)pVect1;
	const auto * pVA = (const float *)pVect2A;
	const auto * pVB = (const float *)pVect2B;
	size_t uDim = *(const size_t *)pParam;

	float fSumA = 0.0f;
	float fSumB = 0.0f;
	size_t i = 0;

#if defined(USE_AVX512)
	__m512 vSumA = _mm512_setzero_ps();
	__m512 vSumB = _mm512_setzero_ps();
	size_t uLimit = uDim & ~size_t(15);
	for ( ; i < uLimit; i += 16 )
	{
		__m512 v1 = _mm512_loadu_ps ( pV1 + i );
		__m512 vA = _mm512_loadu_ps ( pVA + i );
		__m512 vB = _mm512_loadu_ps ( pVB + i );
		__m512 vDiffA = _mm512_sub_ps ( v1, vA );
		__m512 vDiffB = _mm512_sub_ps ( v1, vB );
		vSumA = _mm512_fmadd_ps ( vDiffA, vDiffA, vSumA );
		vSumB = _mm512_fmadd_ps ( vDiffB, vDiffB, vSumB );
	}
	fSumA = _mm512_reduce_add_ps ( vSumA );
	fSumB = _mm512_reduce_add_ps ( vSumB );
#elif defined(USE_AVX2)
	__m256 vSumA = _mm256_setzero_ps();
	__m256 vSumB = _mm256_setzero_ps();
	size_t uLimit = uDim & ~size_t(7);
	for ( ; i < uLimit; i += 8 )
	{
		__m256 v1 = _mm256_loadu_ps ( pV1 + i );
		__m256 vA = _mm256_loadu_ps ( pVA + i );
		__m256 vB = _mm256_loadu_ps ( pVB + i );
		__m256 vDiffA = _mm256_sub_ps ( v1, vA );
		__m256 vDiffB = _mm256_sub_ps ( v1, vB );
		vSumA = _mm256_add_ps ( vSumA, _mm256_mul_ps ( vDiffA, vDiffA ) );
		vSumB = _mm256_add_ps ( vSumB, _mm256_mul_ps ( vDiffB, vDiffB ) );
	}
	fSumA = HSum256Ps ( vSumA );
	fSumB = HSum256Ps ( vSumB );
#endif

	for ( size_t iTail = i; iTail < uDim; iTail++ )
	{
		float fDiffA = pV1[iTail] - pVA[iTail];
		float fDiffB = pV1[iTail] - pVB[iTail];
		fSumA += fDiffA*fDiffA;
		fSumB += fDiffB*fDiffB;
	}

	fDistA = fSumA;
	fDistB = fSumB;
}

///////////////////////////////////////////////////////////////////////////////

static FORCE_INLINE int L2Sqr8Bit ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pQty )
{
	size_t uQty = *((size_t*)pQty);
	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	int iRes = 0;
	for ( size_t i = 0; i < uQty; i++ )
	{
		int iDiff = int(*pV1) - int(*pV2);
		iRes += iDiff*iDiff;
		pV1++;
		pV2++;
	}

	return iRes;
}


static float L2Sqr8BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr8Bit ( pVect1, pVect2, (size_t)-1, (size_t)-1, &(pDistFuncParam->m_uDim) );
	return pDistFuncParam->m_fA*iDist;
}


static FORCE_INLINE int L2Sqr8BitSIMD16 ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty )
{
	size_t uQty = *((size_t *)pQty);
	uQty >>= 4;

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	__m128i iSum = _mm_setzero_si128();

	for ( size_t i = 0; i < uQty; i++ )
	{
		__m128i iV1 = _mm_loadu_si128 ( (const __m128i*)pV1 );
		__m128i iV2 = _mm_loadu_si128 ( (const __m128i*)pV2 );

		__m128i iV1Lo = _mm_unpacklo_epi8 ( iV1, _mm_setzero_si128() );
		__m128i iV1Hi = _mm_unpackhi_epi8 ( iV1, _mm_setzero_si128() );
		__m128i iV2Lo = _mm_unpacklo_epi8 ( iV2, _mm_setzero_si128() );
		__m128i iV2Hi = _mm_unpackhi_epi8 ( iV2, _mm_setzero_si128() );

		__m128i iDiffLo = _mm_sub_epi16 ( iV1Lo, iV2Lo );
		__m128i iDiffHi = _mm_sub_epi16 ( iV1Hi, iV2Hi );

		__m128i iSqrLo = _mm_madd_epi16 ( iDiffLo, iDiffLo );
		__m128i iSqrHi = _mm_madd_epi16 ( iDiffHi, iDiffHi );

		iSum = _mm_add_epi32 ( iSum, iSqrLo );
		iSum = _mm_add_epi32 ( iSum, iSqrHi );

		pV1 += 16;
		pV2 += 16;
	}

	alignas(16) int dBuffer[4];
	_mm_store_si128 ( (__m128i*)dBuffer, iSum );
	return dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];
}


static float L2Sqr8BitSIMD16FloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr8BitSIMD16 ( pVect1, pVect2, &(pDistFuncParam->m_uDim) );
	return pDistFuncParam->m_fA*iDist;
}


static float L2Sqr8BitSIMD16FloatResiduals ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	size_t uQty = pDistFuncParam->m_uDim;
	size_t uQty16 = uQty >> 4 << 4;

	int iDist1 = L2Sqr8BitSIMD16 ( pVect1, pVect2, &uQty16 );

	auto pV1 = (uint8_t *)pVect1 + uQty16;
	auto pV2 = (uint8_t *)pVect2 + uQty16;
	size_t uQtyLeft = uQty - uQty16;
	int iDist2 = L2Sqr8Bit ( pV1, pV2, (size_t)-1, (size_t)-1, &uQtyLeft );

	return pDistFuncParam->m_fA*(iDist1 + iDist2);
}


L2Space8BitFloat_c::L2Space8BitFloat_c ( size_t uDim )
	: Space_c ( uDim )
{
	m_tDistFuncParam.m_uDim = uDim;

	if ( uDim % 16 == 0)
		m_fnDist = L2Sqr8BitSIMD16FloatDistance;
	else if ( uDim > 16)
		m_fnDist = L2Sqr8BitSIMD16FloatResiduals;
	else
		m_fnDist = L2Sqr8BitFloatDistance;
}


void L2Space8BitFloat_c::SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer )
{
	const QuantizationSettings_t & tSettings = tQuantizer.GetSettings();
	float fAlpha = CalcAlpha(tSettings);
	m_tDistFuncParam.m_fA = fAlpha*fAlpha;
}

///////////////////////////////////////////////////////////////////////////////
static FORCE_INLINE int L2Sqr1Bit ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uQty )
{
	size_t uLenBytes = (uQty + 7) >> 3;

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;
	int iDistance = 0;
	for ( size_t i = 0; i < uLenBytes; i++ )
		iDistance += __builtin_popcount (pV1[i] ^ pV2[i]);

	return iDistance;
}


static float L2Sqr1BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iMismatchingBits = L2Sqr1Bit ( pVect1, pVect2, pDistFuncParam->m_uDim );
	int iMatchingBits = int(pDistFuncParam->m_uDim) - iMismatchingBits;
	return iMismatchingBits - iMatchingBits;
}

#if !defined(USE_SIMDE)
static FORCE_INLINE int L2Sqr1Bit8x ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uQty )
{
	size_t uLenBytes = (uQty + 7) >> 3;
	size_t uQty8 = uLenBytes >> 3;

	auto pV1 = (uint64_t*)pVect1;
	auto pV2 = (uint64_t*)pVect2;

	int iDistance = 0;
	for ( size_t i = 0; i < uQty8; i++ )
		iDistance += _mm_popcnt_u64 ( pV1[i] ^ pV2[i] );

	return iDistance;
}


static float L2Sqr1Bit8xFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iMismatchingBits = L2Sqr1Bit8x ( pVect1, pVect2, pDistFuncParam->m_uDim );
	int iMatchingBits = int(pDistFuncParam->m_uDim) - iMismatchingBits;
	return iMismatchingBits - iMatchingBits;
}


static float L2Sqr1Bit8xFloatResiduals ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	size_t uQty = pDistFuncParam->m_uDim;
	size_t uLenBytes = (uQty + 7) >> 3;
	size_t uQty8 = uLenBytes >> 3;
	size_t uLenBytes8 = uQty8 << 3;

	int iMismatchingBits = L2Sqr1Bit8x ( pVect1, pVect2, uQty8 );

	auto pV1 = (uint8_t *)pVect1 + uLenBytes8;
	auto pV2 = (uint8_t *)pVect2 + uLenBytes8;
	iMismatchingBits += L2Sqr1Bit ( pV1, pV2, uQty - uQty8 );
	int iMatchingBits = int(pDistFuncParam->m_uDim) - iMismatchingBits;
	return iMismatchingBits - iMatchingBits;
}
#endif


L2Space1BitFloat_c::L2Space1BitFloat_c ( size_t uDim )
	: L2Space8BitFloat_c ( uDim )
{
	size_t uBytes = get_data_size();

#if !defined(USE_SIMDE)
	if ( uBytes % 8 == 0)
		m_fnDist = L2Sqr1Bit8xFloatDistance;
	else if ( uBytes > 8 )
		m_fnDist = L2Sqr1Bit8xFloatResiduals;
	else
#endif
		m_fnDist = L2Sqr1BitFloatDistance;
}

///////////////////////////////////////////////////////////////////////////////

static FORCE_INLINE int IP8Bit ( const uint8_t * __restrict pVect1, const uint8_t * __restrict pVect2, size_t uQty )
{
	int iRes = 0;
	for ( size_t i = 0; i < uQty; i++ )
		iRes += (int)pVect1[i]*pVect2[i];

	return iRes;
}


static float IP8BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	float fVect1B = *(float*)pVect1;
	float fVect2B = *(float*)pVect2;
	int iDotProduct = IP8Bit ( (uint8_t*)pVect1 + sizeof(float), (uint8_t*)pVect2 + sizeof(float), pDistFuncParam->m_uDim );
	return pDistFuncParam->CalcIP ( iDotProduct ) + fVect1B + fVect2B;
}


static FORCE_INLINE int IP8BitSIMD16 ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uQty )
{
	uQty >>= 4;
	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	__m128i iSum = _mm_setzero_si128();

	for ( size_t i = 0; i < uQty; i++ )
	{
		__m128i iV1 = _mm_loadu_si128 ( (const __m128i*)pV1 );
		__m128i iV2 = _mm_loadu_si128 ( (const __m128i*)pV2 );

		__m128i iV1Lo = _mm_unpacklo_epi8 ( iV1, _mm_setzero_si128() );
		__m128i iV1Hi = _mm_unpackhi_epi8 ( iV1, _mm_setzero_si128() );
		__m128i iV2Lo = _mm_unpacklo_epi8 ( iV2, _mm_setzero_si128() );
		__m128i iV2Hi = _mm_unpackhi_epi8 ( iV2, _mm_setzero_si128() );

		__m128i iV1Lo32Lo = _mm_cvtepu16_epi32(iV1Lo);
		__m128i iV1Lo32Hi = _mm_cvtepu16_epi32 ( _mm_srli_si128 ( iV1Lo, 8 ) );
		__m128i iV2Lo32Lo = _mm_cvtepu16_epi32(iV2Lo);
		__m128i iV2Lo32Hi = _mm_cvtepu16_epi32 ( _mm_srli_si128 ( iV2Lo, 8 ) );

		__m128i iProdLoLo = _mm_mullo_epi32 ( iV1Lo32Lo, iV2Lo32Lo );
		__m128i iProdLoHi = _mm_mullo_epi32 ( iV1Lo32Hi, iV2Lo32Hi );

		__m128i iV1Hi32Lo = _mm_cvtepu16_epi32(iV1Hi);
		__m128i iV1Hi32Hi = _mm_cvtepu16_epi32 ( _mm_srli_si128 ( iV1Hi, 8 ) );
		__m128i iV2Hi32Lo = _mm_cvtepu16_epi32(iV2Hi);
		__m128i iV2Hi32Hi = _mm_cvtepu16_epi32 ( _mm_srli_si128 ( iV2Hi, 8 ) );

		__m128i iProdHiLo = _mm_mullo_epi32 ( iV1Hi32Lo, iV2Hi32Lo );
		__m128i iProdHiHi = _mm_mullo_epi32 ( iV1Hi32Hi, iV2Hi32Hi );

		__m128i iSumLo = _mm_add_epi32 ( iProdLoLo, iProdLoHi );
		__m128i iSumHi = _mm_add_epi32 ( iProdHiLo, iProdHiHi );

		iSum = _mm_add_epi32 ( iSum, iSumLo );
		iSum = _mm_add_epi32 ( iSum, iSumHi );

		pV1 += 16;
		pV2 += 16;
	}

	alignas(16) int dBuffer[4];
	_mm_store_si128 ( (__m128i*)dBuffer, iSum );
	return dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];
}


static float IP8BitSIMD16FloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	float fVect1B = *(float*)pVect1;
	float fVect2B = *(float*)pVect2;
	int iDotProduct = IP8BitSIMD16 ( (uint8_t*)pVect1 + sizeof(float), (uint8_t*)pVect2 + sizeof(float), pDistFuncParam->m_uDim );
	return pDistFuncParam->CalcIP(iDotProduct) + fVect1B + fVect2B;
}


static float IP8BitSIMD16FloatResiduals ( const void * pVect1, const void * pVect2, size_t, size_t, const void * pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;

	size_t uQty = pDistFuncParam->m_uDim;
	size_t uQty16 = uQty >> 4 << 4;

	auto pV1 = (uint8_t *)pVect1;
	auto pV2 = (uint8_t *)pVect2;
	float fVect1B = *(float*)pV1;
	float fVect2B = *(float*)pV2;
	pV1 += sizeof(float);
	pV2 += sizeof(float);
	int iDotProduct = IP8BitSIMD16 ( pV1, pV2, uQty16 );
	iDotProduct += IP8Bit ( pV1 + uQty16, pV2 + uQty16, uQty - uQty16 );
	return pDistFuncParam->CalcIP(iDotProduct) + fVect1B + fVect2B;
}

///////////////////////////////////////////////////////////////////////////////

IPSpace8BitFloat_c::IPSpace8BitFloat_c ( size_t uDim )
	: Space_c ( uDim )
{
	m_tDistFuncParam.m_uDim = uDim;

	if ( uDim % 16 == 0)
		m_fnDist = IP8BitSIMD16FloatDistance;
	else if ( uDim > 16)
		m_fnDist = IP8BitSIMD16FloatResiduals;
	else
		m_fnDist = IP8BitFloatDistance;
}


void IPSpace8BitFloat_c::SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer )
{
	const QuantizationSettings_t & tSettings = tQuantizer.GetSettings();
	m_tDistFuncParam.m_fK = tSettings.m_fK;
	m_tDistFuncParam.m_fB = tSettings.m_fB;
}

///////////////////////////////////////////////////////////////////////////////

static float IP1BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iMismatchingBits = L2Sqr1Bit ( pVect1, pVect2, pDistFuncParam->m_uDim );
	int iMatchingBits = int(pDistFuncParam->m_uDim) - iMismatchingBits;
	return iMismatchingBits - iMatchingBits;
}

#if !defined(USE_SIMDE)
static float IP1Bit8xFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iMismatchingBits = L2Sqr1Bit8x ( pVect1, pVect2, pDistFuncParam->m_uDim );
	int iMatchingBits = int(pDistFuncParam->m_uDim) - iMismatchingBits;
	return iMismatchingBits - iMatchingBits;
}


static float IP1Bit8xFloatResiduals ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	size_t uQty = pDistFuncParam->m_uDim;
	size_t uLenBytes = (uQty + 7) >> 3;
	size_t uQty8 = uLenBytes >> 3;
	size_t uLenBytes8 = uQty8 << 3;

	int iMismatchingBits = L2Sqr1Bit8x ( pVect1, pVect2, uQty8 );

	auto pV1 = (uint8_t *)pVect1 + uLenBytes8;
	auto pV2 = (uint8_t *)pVect2 + uLenBytes8;
	iMismatchingBits += L2Sqr1Bit ( pV1, pV2, uQty - uQty8 );
	int iMatchingBits = int(pDistFuncParam->m_uDim) - iMismatchingBits;
	return iMismatchingBits - iMatchingBits;
}
#endif

IPSpace1BitFloat_c::IPSpace1BitFloat_c ( size_t uDim )
	: IPSpace8BitFloat_c ( uDim )
{
	size_t uBytes = get_data_size();

#if !defined(USE_SIMDE)
	if ( uBytes % 8 == 0)
		m_fnDist = IP1Bit8xFloatDistance;
	else if ( uBytes > 8 )
		m_fnDist = IP1Bit8xFloatResiduals;
	else
#endif
		m_fnDist = IP1BitFloatDistance;
}

///////////////////////////////////////////////////////////////////////////////

static int64_t BinaryDotProduct ( const uint8_t * pVec4Bit, const uint8_t * pVec1Bit, int iBytes )
{
	int64_t iResult = 0;
	auto pVec4BitPtr = pVec4Bit;
	for ( int i = 0; i < 4; i++ )
	{
		int64_t iPopCntSum = 0;
		
		int j = 0;        
		const int iLimit4Bytes = iBytes & ~( sizeof(uint32_t) - 1 );
		for ( ; j < iLimit4Bytes; j += sizeof(uint32_t) )
		{
			uint32_t uVal4Bit = *(const uint32_t*)pVec4BitPtr;
			uint32_t uVal1Bit = *(const uint32_t*)( &pVec1Bit[j] );

			iPopCntSum += __builtin_popcount (uVal4Bit & uVal1Bit );
			pVec4BitPtr += sizeof(uint32_t);
		}
		
		for ( ; j < iBytes; j++ )
		{
			iPopCntSum += __builtin_popcount ((*pVec4BitPtr & pVec1Bit[j] ) & 0xFF );
			pVec4BitPtr++;
		}

		iResult += iPopCntSum << i;
	}
	
	return iResult;
}

#if !defined(USE_SIMDE)

#if defined(USE_AVX2) || defined(USE_AVX512)
FORCE_INLINE __m256i PopCnt256_epi8 ( __m256i iValue )
{
	const __m256i iLowMask = _mm256_set1_epi8(0x0f);
	const __m256i iLookup = _mm256_setr_epi8 (
		0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,
		0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 );

	__m256i iLo = _mm256_and_si256 ( iValue, iLowMask );
	__m256i iHi = _mm256_and_si256 ( _mm256_srli_epi16 ( iValue, 4 ), iLowMask );
	return _mm256_add_epi8 ( _mm256_shuffle_epi8 ( iLookup, iLo ), _mm256_shuffle_epi8 ( iLookup, iHi ) );
}

FORCE_INLINE int64_t HSum256 ( __m256i v )
{
	__m128i lo = _mm256_castsi256_si128(v);
	__m128i hi = _mm256_extracti128_si256(v, 1);
	__m128i sum = _mm_add_epi64(lo, hi);
	return _mm_extract_epi64(sum, 0) + _mm_extract_epi64(sum, 1);
}
#endif

// AVX-512 VPOPCNTQ accumulate: AND query vec VEC with 4 doc planes iPlane0..3, popcnt, accumulate into A0..3
#define BINARYDOTPRODUCT_AVX512_ACCUM(VEC, A0, A1, A2, A3) \
	A0 = _mm512_add_epi64 ( A0, _mm512_popcnt_epi64 ( _mm512_and_si512 ( VEC, iPlane0 ) ) ); \
	A1 = _mm512_add_epi64 ( A1, _mm512_popcnt_epi64 ( _mm512_and_si512 ( VEC, iPlane1 ) ) ); \
	A2 = _mm512_add_epi64 ( A2, _mm512_popcnt_epi64 ( _mm512_and_si512 ( VEC, iPlane2 ) ) ); \
	A3 = _mm512_add_epi64 ( A3, _mm512_popcnt_epi64 ( _mm512_and_si512 ( VEC, iPlane3 ) ) );

// AVX2 SAD-popcount accumulate: AND query vec VEC with 4 doc planes iPlane0_s..3_s, popcnt, accumulate into A0..3 (uses iZero)
#define BINARYDOTPRODUCT_AVX2_ACCUM(VEC, A0, A1, A2, A3) \
	A0 = _mm256_add_epi64 ( A0, _mm256_sad_epu8 ( PopCnt256_epi8 ( _mm256_and_si256 ( VEC, iPlane0_s ) ), iZero ) ); \
	A1 = _mm256_add_epi64 ( A1, _mm256_sad_epu8 ( PopCnt256_epi8 ( _mm256_and_si256 ( VEC, iPlane1_s ) ), iZero ) ); \
	A2 = _mm256_add_epi64 ( A2, _mm256_sad_epu8 ( PopCnt256_epi8 ( _mm256_and_si256 ( VEC, iPlane2_s ) ), iZero ) ); \
	A3 = _mm256_add_epi64 ( A3, _mm256_sad_epu8 ( PopCnt256_epi8 ( _mm256_and_si256 ( VEC, iPlane3_s ) ), iZero ) );

// AVX2 32-byte step: load query + 4 doc planes, AND/popcnt/accumulate
#define BINARYDOTPRODUCT16_STEP(OFFS) \
	{ \
		__m256i iVec1Bit_s = _mm256_loadu_si256((__m256i*)(pVec1Bit0 + OFFS)); \
		__m256i iPlane0_s = _mm256_loadu_si256((__m256i*)(pVec4Bit0 + OFFS)); \
		__m256i iPlane1_s = _mm256_loadu_si256((__m256i*)(pVec4Bit1 + OFFS)); \
		__m256i iPlane2_s = _mm256_loadu_si256((__m256i*)(pVec4Bit2 + OFFS)); \
		__m256i iPlane3_s = _mm256_loadu_si256((__m256i*)(pVec4Bit3 + OFFS)); \
		const __m256i iZero = _mm256_setzero_si256(); \
		BINARYDOTPRODUCT_AVX2_ACCUM ( iVec1Bit_s, vPopCnt0, vPopCnt1, vPopCnt2, vPopCnt3 ) \
	}

template <bool RESIDUALS>
static int64_t BinaryDotProduct16 ( const uint8_t * pVec4Bit, const uint8_t * pVec1Bit, int iBytes )
{
	int64_t iPopCnt0 = 0;
	int64_t iPopCnt1 = 0;
	int64_t iPopCnt2 = 0;
	int64_t iPopCnt3 = 0;
	size_t i = 0;

#if defined(USE_AVX512)
	__m512i vPopCnt512_0 = _mm512_setzero_si512();
	__m512i vPopCnt512_1 = _mm512_setzero_si512();
	__m512i vPopCnt512_2 = _mm512_setzero_si512();
	__m512i vPopCnt512_3 = _mm512_setzero_si512();

	size_t uLimit64 = iBytes & ~63;
	for ( ; i < uLimit64; i += 64 )
	{
		__m512i iVec1Bit = _mm512_loadu_si512(pVec1Bit + i);

		auto pVec4Bit0 = pVec4Bit + i;
		__m512i iPlane0 = _mm512_loadu_si512(pVec4Bit0);
		__m512i iPlane1 = _mm512_loadu_si512(pVec4Bit0 + iBytes);
		__m512i iPlane2 = _mm512_loadu_si512(pVec4Bit0 + iBytes * 2);
		__m512i iPlane3 = _mm512_loadu_si512(pVec4Bit0 + iBytes * 3);

		BINARYDOTPRODUCT_AVX512_ACCUM ( iVec1Bit, vPopCnt512_0, vPopCnt512_1, vPopCnt512_2, vPopCnt512_3 )
	}

	iPopCnt0 = _mm512_reduce_add_epi64(vPopCnt512_0);
	iPopCnt1 = _mm512_reduce_add_epi64(vPopCnt512_1);
	iPopCnt2 = _mm512_reduce_add_epi64(vPopCnt512_2);
	iPopCnt3 = _mm512_reduce_add_epi64(vPopCnt512_3);

	// AVX2 remainder after AVX-512 (32-byte chunks)
	{
		__m256i vPopCnt0 = _mm256_setzero_si256();
		__m256i vPopCnt1 = _mm256_setzero_si256();
		__m256i vPopCnt2 = _mm256_setzero_si256();
		__m256i vPopCnt3 = _mm256_setzero_si256();

		size_t uLimit32 = iBytes & ~31;
		for ( ; i < uLimit32; i += 32 )
		{
			auto pVec1Bit0 = pVec1Bit + i;

			auto pVec4Bit0 = pVec4Bit + i;
			auto pVec4Bit1 = pVec4Bit0 + iBytes;
			auto pVec4Bit2 = pVec4Bit1 + iBytes;
			auto pVec4Bit3 = pVec4Bit2 + iBytes;

			BINARYDOTPRODUCT16_STEP(0);
		}

		iPopCnt0 += HSum256(vPopCnt0);
		iPopCnt1 += HSum256(vPopCnt1);
		iPopCnt2 += HSum256(vPopCnt2);
		iPopCnt3 += HSum256(vPopCnt3);
	}

#elif defined(USE_AVX2)
	__m256i vPopCnt0 = _mm256_setzero_si256();
	__m256i vPopCnt1 = _mm256_setzero_si256();
	__m256i vPopCnt2 = _mm256_setzero_si256();
	__m256i vPopCnt3 = _mm256_setzero_si256();

	size_t uLimit128 = iBytes & ~127;
	for ( ; i < uLimit128; i += 128 )
	{
		auto pVec1Bit0 = pVec1Bit + i;

		auto pVec4Bit0 = pVec4Bit + i;
		auto pVec4Bit1 = pVec4Bit0 + iBytes;
		auto pVec4Bit2 = pVec4Bit1 + iBytes;
		auto pVec4Bit3 = pVec4Bit2 + iBytes;

		BINARYDOTPRODUCT16_STEP(0);
		BINARYDOTPRODUCT16_STEP(32);
		BINARYDOTPRODUCT16_STEP(64);
		BINARYDOTPRODUCT16_STEP(96);
	}

	size_t uLimit64 = iBytes & ~63;
	for ( ; i < uLimit64; i += 64 )
	{
		auto pVec1Bit0 = pVec1Bit + i;

		auto pVec4Bit0 = pVec4Bit + i;
		auto pVec4Bit1 = pVec4Bit0 + iBytes;
		auto pVec4Bit2 = pVec4Bit1 + iBytes;
		auto pVec4Bit3 = pVec4Bit2 + iBytes;

		BINARYDOTPRODUCT16_STEP(0);
		BINARYDOTPRODUCT16_STEP(32);
	}

	iPopCnt0 = HSum256(vPopCnt0);
	iPopCnt1 = HSum256(vPopCnt1);
	iPopCnt2 = HSum256(vPopCnt2);
	iPopCnt3 = HSum256(vPopCnt3);
#endif

	size_t uLimit16 = iBytes & ~15;
	for ( ; i < uLimit16; i += 16 )
	{
		auto pVec1Bit0 = pVec1Bit + i;

		auto pVec4Bit0 = pVec4Bit + i;
		auto pVec4Bit1 = pVec4Bit0 + iBytes;
		auto pVec4Bit2 = pVec4Bit1 + iBytes;
		auto pVec4Bit3 = pVec4Bit2 + iBytes;

		__m128i iVec1Bit = _mm_loadu_si128 ( (__m128i*)pVec1Bit0 );

		__m128i iVec4Bit0 = _mm_loadu_si128 ( (__m128i*)pVec4Bit0 );
		__m128i iVec4Bit1 = _mm_loadu_si128 ( (__m128i*)pVec4Bit1 );
		__m128i iVec4Bit2 = _mm_loadu_si128 ( (__m128i*)pVec4Bit2 );
		__m128i iVec4Bit3 = _mm_loadu_si128 ( (__m128i*)pVec4Bit3 );

		__m128i iAnd0 = _mm_and_si128 ( iVec1Bit, iVec4Bit0 );
		__m128i iAnd1 = _mm_and_si128 ( iVec1Bit, iVec4Bit1 );
		__m128i iAnd2 = _mm_and_si128 ( iVec1Bit, iVec4Bit2 );
		__m128i iAnd3 = _mm_and_si128 ( iVec1Bit, iVec4Bit3 );

		uint64_t uLow0 = _mm_cvtsi128_si64(iAnd0);
		uint64_t uHigh0 = _mm_cvtsi128_si64 ( _mm_srli_si128 ( iAnd0, 8 ) );
		uint64_t uLow1 = _mm_cvtsi128_si64(iAnd1);
		uint64_t uHigh1 = _mm_cvtsi128_si64 ( _mm_srli_si128 ( iAnd1, 8 ) );
		uint64_t uLow2 = _mm_cvtsi128_si64(iAnd2);
		uint64_t uHigh2 = _mm_cvtsi128_si64 ( _mm_srli_si128 ( iAnd2, 8 ) );
		uint64_t uLow3 = _mm_cvtsi128_si64(iAnd3);
		uint64_t uHigh3 = _mm_cvtsi128_si64 ( _mm_srli_si128 ( iAnd3, 8 ) );

		iPopCnt0 += _mm_popcnt_u64(uLow0) + _mm_popcnt_u64(uHigh0);
		iPopCnt1 += _mm_popcnt_u64(uLow1) + _mm_popcnt_u64(uHigh1);
		iPopCnt2 += _mm_popcnt_u64(uLow2) + _mm_popcnt_u64(uHigh2);
		iPopCnt3 += _mm_popcnt_u64(uLow3) + _mm_popcnt_u64(uHigh3);
	}

	if constexpr ( RESIDUALS )
	{
		for ( ; i < iBytes; i++ )
		{
			uint8_t uValue = pVec1Bit[i];
			auto pVec4Bit0 = pVec4Bit + i;
			auto pVec4Bit1 = pVec4Bit0 + iBytes;
			auto pVec4Bit2 = pVec4Bit1 + iBytes;
			auto pVec4Bit3 = pVec4Bit2 + iBytes;

			iPopCnt0 += _mm_popcnt_u32 ( ( uValue & *pVec4Bit0 ) & 0xFF );
			iPopCnt1 += _mm_popcnt_u32 ( ( uValue & *pVec4Bit1 ) & 0xFF );
			iPopCnt2 += _mm_popcnt_u32 ( ( uValue & *pVec4Bit2 ) & 0xFF );
			iPopCnt3 += _mm_popcnt_u32 ( ( uValue & *pVec4Bit3 ) & 0xFF );
		}
	}

	return iPopCnt0 + ( iPopCnt1 << 1 ) + ( iPopCnt2 << 2 ) + ( iPopCnt3 << 3 );
}

template <bool RESIDUALS>
static void BinaryDotProductBatch2 ( const uint8_t * pVec4Bit, const uint8_t * pVec1BitA, const uint8_t * pVec1BitB, int iBytes, int64_t & iResultA, int64_t & iResultB )
{
	int64_t iPopCntA0 = 0, iPopCntA1 = 0, iPopCntA2 = 0, iPopCntA3 = 0;
	int64_t iPopCntB0 = 0, iPopCntB1 = 0, iPopCntB2 = 0, iPopCntB3 = 0;
	size_t i = 0;

#if defined(USE_AVX512)
	__m512i vPopCntA0 = _mm512_setzero_si512();
	__m512i vPopCntA1 = _mm512_setzero_si512();
	__m512i vPopCntA2 = _mm512_setzero_si512();
	__m512i vPopCntA3 = _mm512_setzero_si512();
	__m512i vPopCntB0 = _mm512_setzero_si512();
	__m512i vPopCntB1 = _mm512_setzero_si512();
	__m512i vPopCntB2 = _mm512_setzero_si512();
	__m512i vPopCntB3 = _mm512_setzero_si512();

	size_t uLimit64 = iBytes & ~63;
	for ( ; i < uLimit64; i += 64 )
	{
		__m512i iVec1BitA = _mm512_loadu_si512 ( pVec1BitA + i );
		__m512i iVec1BitB = _mm512_loadu_si512 ( pVec1BitB + i );

		auto pVec4Bit0 = pVec4Bit + i;
		__m512i iPlane0 = _mm512_loadu_si512 ( pVec4Bit0 );
		__m512i iPlane1 = _mm512_loadu_si512 ( pVec4Bit0 + iBytes );
		__m512i iPlane2 = _mm512_loadu_si512 ( pVec4Bit0 + iBytes * 2 );
		__m512i iPlane3 = _mm512_loadu_si512 ( pVec4Bit0 + iBytes * 3 );

		BINARYDOTPRODUCT_AVX512_ACCUM ( iVec1BitA, vPopCntA0, vPopCntA1, vPopCntA2, vPopCntA3 )
		BINARYDOTPRODUCT_AVX512_ACCUM ( iVec1BitB, vPopCntB0, vPopCntB1, vPopCntB2, vPopCntB3 )
	}

	iPopCntA0 = _mm512_reduce_add_epi64 ( vPopCntA0 );
	iPopCntA1 = _mm512_reduce_add_epi64 ( vPopCntA1 );
	iPopCntA2 = _mm512_reduce_add_epi64 ( vPopCntA2 );
	iPopCntA3 = _mm512_reduce_add_epi64 ( vPopCntA3 );
	iPopCntB0 = _mm512_reduce_add_epi64 ( vPopCntB0 );
	iPopCntB1 = _mm512_reduce_add_epi64 ( vPopCntB1 );
	iPopCntB2 = _mm512_reduce_add_epi64 ( vPopCntB2 );
	iPopCntB3 = _mm512_reduce_add_epi64 ( vPopCntB3 );

	// AVX2 remainder after AVX-512 (32-byte chunks)
	{
		__m256i vPopCntA0 = _mm256_setzero_si256(), vPopCntA1 = _mm256_setzero_si256(), vPopCntA2 = _mm256_setzero_si256(), vPopCntA3 = _mm256_setzero_si256();
		__m256i vPopCntB0 = _mm256_setzero_si256(), vPopCntB1 = _mm256_setzero_si256(), vPopCntB2 = _mm256_setzero_si256(), vPopCntB3 = _mm256_setzero_si256();

		size_t uLimit32 = iBytes & ~31;
		for ( ; i < uLimit32; i += 32 )
		{
			auto pVec4Bit0 = pVec4Bit + i;
			auto pVec4Bit1 = pVec4Bit0 + iBytes;
			auto pVec4Bit2 = pVec4Bit1 + iBytes;
			auto pVec4Bit3 = pVec4Bit2 + iBytes;

			__m256i iVec1BitA_s = _mm256_loadu_si256 ( (__m256i*)(pVec1BitA + i) );
			__m256i iVec1BitB_s = _mm256_loadu_si256 ( (__m256i*)(pVec1BitB + i) );
			__m256i iPlane0_s = _mm256_loadu_si256 ( (__m256i*)pVec4Bit0 );
			__m256i iPlane1_s = _mm256_loadu_si256 ( (__m256i*)pVec4Bit1 );
			__m256i iPlane2_s = _mm256_loadu_si256 ( (__m256i*)pVec4Bit2 );
			__m256i iPlane3_s = _mm256_loadu_si256 ( (__m256i*)pVec4Bit3 );
			const __m256i iZero = _mm256_setzero_si256();

			BINARYDOTPRODUCT_AVX2_ACCUM ( iVec1BitA_s, vPopCntA0, vPopCntA1, vPopCntA2, vPopCntA3 )
			BINARYDOTPRODUCT_AVX2_ACCUM ( iVec1BitB_s, vPopCntB0, vPopCntB1, vPopCntB2, vPopCntB3 )
		}

		iPopCntA0 += HSum256(vPopCntA0); iPopCntA1 += HSum256(vPopCntA1); iPopCntA2 += HSum256(vPopCntA2); iPopCntA3 += HSum256(vPopCntA3);
		iPopCntB0 += HSum256(vPopCntB0); iPopCntB1 += HSum256(vPopCntB1); iPopCntB2 += HSum256(vPopCntB2); iPopCntB3 += HSum256(vPopCntB3);
	}

	#elif defined(USE_AVX2)
		__m256i vPopCntA0 = _mm256_setzero_si256(), vPopCntA1 = _mm256_setzero_si256(), vPopCntA2 = _mm256_setzero_si256(), vPopCntA3 = _mm256_setzero_si256();
		__m256i vPopCntB0 = _mm256_setzero_si256(), vPopCntB1 = _mm256_setzero_si256(), vPopCntB2 = _mm256_setzero_si256(), vPopCntB3 = _mm256_setzero_si256();
		const __m256i iZero = _mm256_setzero_si256();
		size_t uLimit128 = iBytes & ~127;
		for ( ; i < uLimit128; i += 128 )
		{
			auto pVec4Bit0 = pVec4Bit + i;
			auto pVec4Bit1 = pVec4Bit0 + iBytes;
			auto pVec4Bit2 = pVec4Bit1 + iBytes;
			auto pVec4Bit3 = pVec4Bit2 + iBytes;

			for ( size_t uOff = 0; uOff < 128; uOff += 32 )
			{
				__m256i iVec1BitA_s = _mm256_loadu_si256 ( (__m256i*)(pVec1BitA + i + uOff) );
				__m256i iVec1BitB_s = _mm256_loadu_si256 ( (__m256i*)(pVec1BitB + i + uOff) );
				__m256i iPlane0_s = _mm256_loadu_si256 ( (__m256i*)(pVec4Bit0 + uOff) );
				__m256i iPlane1_s = _mm256_loadu_si256 ( (__m256i*)(pVec4Bit1 + uOff) );
				__m256i iPlane2_s = _mm256_loadu_si256 ( (__m256i*)(pVec4Bit2 + uOff) );
				__m256i iPlane3_s = _mm256_loadu_si256 ( (__m256i*)(pVec4Bit3 + uOff) );

				BINARYDOTPRODUCT_AVX2_ACCUM ( iVec1BitA_s, vPopCntA0, vPopCntA1, vPopCntA2, vPopCntA3 )
				BINARYDOTPRODUCT_AVX2_ACCUM ( iVec1BitB_s, vPopCntB0, vPopCntB1, vPopCntB2, vPopCntB3 )
			}
		}

		size_t uLimit64 = iBytes & ~63;
		for ( ; i < uLimit64; i += 64 )
		{
			auto pVec4Bit0 = pVec4Bit + i;
			auto pVec4Bit1 = pVec4Bit0 + iBytes;
			auto pVec4Bit2 = pVec4Bit1 + iBytes;
			auto pVec4Bit3 = pVec4Bit2 + iBytes;

			for ( size_t uOff = 0; uOff < 64; uOff += 32 )
			{
				__m256i iVec1BitA_s = _mm256_loadu_si256 ( (__m256i*)(pVec1BitA + i + uOff) );
				__m256i iVec1BitB_s = _mm256_loadu_si256 ( (__m256i*)(pVec1BitB + i + uOff) );
				__m256i iPlane0_s = _mm256_loadu_si256 ( (__m256i*)(pVec4Bit0 + uOff) );
				__m256i iPlane1_s = _mm256_loadu_si256 ( (__m256i*)(pVec4Bit1 + uOff) );
				__m256i iPlane2_s = _mm256_loadu_si256 ( (__m256i*)(pVec4Bit2 + uOff) );
				__m256i iPlane3_s = _mm256_loadu_si256 ( (__m256i*)(pVec4Bit3 + uOff) );

				BINARYDOTPRODUCT_AVX2_ACCUM ( iVec1BitA_s, vPopCntA0, vPopCntA1, vPopCntA2, vPopCntA3 )
				BINARYDOTPRODUCT_AVX2_ACCUM ( iVec1BitB_s, vPopCntB0, vPopCntB1, vPopCntB2, vPopCntB3 )
			}
		}

		size_t uLimit32 = iBytes & ~31;
		for ( ; i < uLimit32; i += 32 )
		{
			auto pVec4Bit0 = pVec4Bit + i;
			auto pVec4Bit1 = pVec4Bit0 + iBytes;
			auto pVec4Bit2 = pVec4Bit1 + iBytes;
			auto pVec4Bit3 = pVec4Bit2 + iBytes;

			__m256i iVec1BitA_s = _mm256_loadu_si256 ( (__m256i*)(pVec1BitA + i) );
			__m256i iVec1BitB_s = _mm256_loadu_si256 ( (__m256i*)(pVec1BitB + i) );
			__m256i iPlane0_s = _mm256_loadu_si256 ( (__m256i*)pVec4Bit0 );
			__m256i iPlane1_s = _mm256_loadu_si256 ( (__m256i*)pVec4Bit1 );
			__m256i iPlane2_s = _mm256_loadu_si256 ( (__m256i*)pVec4Bit2 );
			__m256i iPlane3_s = _mm256_loadu_si256 ( (__m256i*)pVec4Bit3 );

			BINARYDOTPRODUCT_AVX2_ACCUM ( iVec1BitA_s, vPopCntA0, vPopCntA1, vPopCntA2, vPopCntA3 )
			BINARYDOTPRODUCT_AVX2_ACCUM ( iVec1BitB_s, vPopCntB0, vPopCntB1, vPopCntB2, vPopCntB3 )
		}

		iPopCntA0 = HSum256(vPopCntA0); iPopCntA1 = HSum256(vPopCntA1); iPopCntA2 = HSum256(vPopCntA2); iPopCntA3 = HSum256(vPopCntA3);
		iPopCntB0 = HSum256(vPopCntB0); iPopCntB1 = HSum256(vPopCntB1); iPopCntB2 = HSum256(vPopCntB2); iPopCntB3 = HSum256(vPopCntB3);
	#endif

	size_t uLimit16 = iBytes & ~15;
	for ( ; i < uLimit16; i += 16 )
	{
		auto pVec4Bit0 = pVec4Bit + i;
		auto pVec4Bit1 = pVec4Bit0 + iBytes;
		auto pVec4Bit2 = pVec4Bit1 + iBytes;
		auto pVec4Bit3 = pVec4Bit2 + iBytes;

		__m128i iVec1BitA = _mm_loadu_si128 ( (__m128i*)(pVec1BitA + i) );
		__m128i iVec1BitB = _mm_loadu_si128 ( (__m128i*)(pVec1BitB + i) );
		__m128i iVec4Bit0 = _mm_loadu_si128 ( (__m128i*)pVec4Bit0 );
		__m128i iVec4Bit1 = _mm_loadu_si128 ( (__m128i*)pVec4Bit1 );
		__m128i iVec4Bit2 = _mm_loadu_si128 ( (__m128i*)pVec4Bit2 );
		__m128i iVec4Bit3 = _mm_loadu_si128 ( (__m128i*)pVec4Bit3 );

		auto tPopCnt128 = [] ( __m128i iValue ) -> int64_t
		{
			uint64_t uLow = _mm_cvtsi128_si64(iValue);
			uint64_t uHigh = _mm_cvtsi128_si64 ( _mm_srli_si128 ( iValue, 8 ) );
			return _mm_popcnt_u64(uLow) + _mm_popcnt_u64(uHigh);
		};

		iPopCntA0 += tPopCnt128 ( _mm_and_si128 ( iVec1BitA, iVec4Bit0 ) );
		iPopCntA1 += tPopCnt128 ( _mm_and_si128 ( iVec1BitA, iVec4Bit1 ) );
		iPopCntA2 += tPopCnt128 ( _mm_and_si128 ( iVec1BitA, iVec4Bit2 ) );
		iPopCntA3 += tPopCnt128 ( _mm_and_si128 ( iVec1BitA, iVec4Bit3 ) );
		iPopCntB0 += tPopCnt128 ( _mm_and_si128 ( iVec1BitB, iVec4Bit0 ) );
		iPopCntB1 += tPopCnt128 ( _mm_and_si128 ( iVec1BitB, iVec4Bit1 ) );
		iPopCntB2 += tPopCnt128 ( _mm_and_si128 ( iVec1BitB, iVec4Bit2 ) );
		iPopCntB3 += tPopCnt128 ( _mm_and_si128 ( iVec1BitB, iVec4Bit3 ) );
	}

	if constexpr ( RESIDUALS )
	{
		for ( ; i < iBytes; i++ )
		{
			uint8_t uValueA = pVec1BitA[i];
			uint8_t uValueB = pVec1BitB[i];
			auto pVec4Bit0 = pVec4Bit + i;
			auto pVec4Bit1 = pVec4Bit0 + iBytes;
			auto pVec4Bit2 = pVec4Bit1 + iBytes;
			auto pVec4Bit3 = pVec4Bit2 + iBytes;

			iPopCntA0 += _mm_popcnt_u32 ( ( uValueA & *pVec4Bit0 ) & 0xFF );
			iPopCntA1 += _mm_popcnt_u32 ( ( uValueA & *pVec4Bit1 ) & 0xFF );
			iPopCntA2 += _mm_popcnt_u32 ( ( uValueA & *pVec4Bit2 ) & 0xFF );
			iPopCntA3 += _mm_popcnt_u32 ( ( uValueA & *pVec4Bit3 ) & 0xFF );
			iPopCntB0 += _mm_popcnt_u32 ( ( uValueB & *pVec4Bit0 ) & 0xFF );
			iPopCntB1 += _mm_popcnt_u32 ( ( uValueB & *pVec4Bit1 ) & 0xFF );
			iPopCntB2 += _mm_popcnt_u32 ( ( uValueB & *pVec4Bit2 ) & 0xFF );
			iPopCntB3 += _mm_popcnt_u32 ( ( uValueB & *pVec4Bit3 ) & 0xFF );
		}
	}

	iResultA = iPopCntA0 + ( iPopCntA1 << 1 ) + ( iPopCntA2 << 2 ) + ( iPopCntA3 << 3 );
	iResultB = iPopCntB0 + ( iPopCntB1 << 1 ) + ( iPopCntB2 << 2 ) + ( iPopCntB3 << 3 );
}
#endif

// This binary distance calculation is derived from Elasticsearch's Java implementation
// in org.elasticsearch.index.codec.vectors.es816.ES816BinaryFlatVectorsScorer
// Permalink: https://github.com/elastic/elasticsearch/blob/1dd41ec2b683a7b7c9c16af404b842cf85cbd5bc/server/src/main/java/org/elasticsearch/index/codec/vectors/es816/ES816BinaryFlatVectorsScorer.java
static FORCE_INLINE float IPBinaryFloatDistanceFromHammingDist ( const Binary4BitFactors_t & tFactors4Bit, const Binary1BitFactorsIP_t & tFactors1Bit, int64_t iHammingDist, const DistFuncParamBinary_t & tBinaryParam )
{
	float fDist = 0.0f;
	if ( tFactors1Bit.m_fVecMinusCentroidNorm==0.0f || tFactors1Bit.m_fQuality==0.0f )
		fDist = tFactors1Bit.m_fVecDocCentroid + tFactors4Bit.m_fVecDotCentroid - tBinaryParam.m_fCentroidDotCentroid;
	else
	{
		assert ( std::isfinite ( tFactors1Bit.m_fQuality ) );
		//float fEstimatedDot = ( 2.0f*tFactors4Bit.m_fRange/tBinaryParam.m_fSqrtDim * iHammingDist + 2.0f*tFactors4Bit.m_fMin/tBinaryParam.m_fSqrtDim*tFactors1Bit.m_fPopCnt - tFactors4Bit.m_fRange/tBinaryParam.m_fSqrtDim*tFactors4Bit.m_fQuantizedSum - tBinaryParam.m_fSqrtDim*tFactors4Bit.m_fMin ) / tFactors1Bit.m_fQuality;
		//fDist = tFactors4Bit.m_fVecMinusCentroidNorm*tFactors1Bit.m_fVecMinusCentroidNorm*fEstimatedDot + tFactors1Bit.m_fVecDocCentroid + tFactors4Bit.m_fVecDotCentroid - tBinaryParam.m_fCentroidDotCentroid;

		// FIXME! these can be calculated once per query
		const float fA = tFactors4Bit.m_fRange	* tBinaryParam.m_fDoubleInvSqrtDim;	
		const float fB = tFactors4Bit.m_fMin	* tBinaryParam.m_fDoubleInvSqrtDim;
		const float fC = tFactors4Bit.m_fRange	* tBinaryParam.m_fInvSqrtDim;
		const float fD = tFactors4Bit.m_fMin	* tBinaryParam.m_fSqrtDim;

		float fTmp = std::fma ( fA, (float)iHammingDist, fB * tFactors1Bit.m_fPopCnt );		// (fA * iHammingDist) + (fB * PopCnt)
		fTmp = std::fma ( -fC, tFactors4Bit.m_fQuantizedSum, fTmp );						// fTmp - (fC * QuantizedSum)
		fTmp = fTmp - fD;      
		float fEstimatedDot = fTmp / tFactors1Bit.m_fQuality;

		float fProd = tFactors4Bit.m_fVecMinusCentroidNorm * tFactors1Bit.m_fVecMinusCentroidNorm;
		float fSum1 = std::fma ( fProd, fEstimatedDot, tFactors1Bit.m_fVecDocCentroid );
		float fSum2 = std::fma ( 1.0f, tFactors4Bit.m_fVecDotCentroid, fSum1 );		// fSum1 + VecDotCentroid
		fDist = fSum2 - tBinaryParam.m_fCentroidDotCentroid;
	}

	assert ( std::isfinite(fDist) );

	float fQualitySqr = tFactors1Bit.m_fQuality*tFactors1Bit.m_fQuality;
	float fErrorBound = tFactors4Bit.m_fVecMinusCentroidNorm*tFactors1Bit.m_fVecMinusCentroidNorm*( tBinaryParam.m_fMaxError*std::sqrt ( ( 1.0f - fQualitySqr )/fQualitySqr ) );
	float fAdjustedDist = std::isfinite(fErrorBound) ? fDist - fErrorBound : fDist;
	return 1.0f - std::max ( ( 1.0f + fAdjustedDist ) / 2.0f, 0.0f );
}


static FORCE_INLINE float L2BinaryFloatDistanceFromHammingDist ( const Binary4BitFactors_t & tFactors4Bit, const Binary1BitFactorsL2_t & tFactors1Bit, int64_t iHammingDist, const DistFuncParamBinary_t & tBinaryParam )
{
	float fDistanceToCentroid2Sqr = tFactors1Bit.m_fDistanceToCentroid * tFactors1Bit.m_fDistanceToCentroid;
	float fCentroidDistToMagnitude2Ratio = tFactors1Bit.m_fDistanceToCentroid / tFactors1Bit.m_fVectorMagnitude;
	float fIPCoeff = -tBinaryParam.m_fDoubleInvSqrtDim * fCentroidDistToMagnitude2Ratio;
	float fPopCntCoeff = std::fma ( 2.0f, tFactors1Bit.m_fPopCnt, -float(tBinaryParam.m_uDim) ); // 2*fPopCnt - uDim
	fPopCntCoeff = fIPCoeff * fPopCntCoeff;

	//float fDist = fDistanceToCentroid2Sqr + tFactors4Bit.m_fDistanceToCentroidSq + fPopCntCoeff * tFactors4Bit.m_fMin + ( iHammingDist*2 - tFactors4Bit.m_fQuantizedSum )*fIPCoeff*tFactors4Bit.m_fRange;

	float fDoubleHammingMinusQuant = std::fma ( 2.0f, float(iHammingDist), -tFactors4Bit.m_fQuantizedSum );
	float fFmaTerm = std::fma ( fIPCoeff * tFactors4Bit.m_fRange, fDoubleHammingMinusQuant, 0.0f );

	float fDist = fDistanceToCentroid2Sqr;
	fDist = std::fma ( 1.0f, tFactors4Bit.m_fDistanceToCentroidSq, fDist );	// + DistanceToCentroidSq
	fDist = std::fma ( fPopCntCoeff, tFactors4Bit.m_fMin, fDist );			// + fPopCntCoeff * fMin
	fDist = std::fma ( 1.0f, fFmaTerm, fDist );								// + main term

	// (d/m)^2 - d^2 = d^2 * (1 - m^2) / m^2.
	// This algebraic form avoids catastrophic cancellation between two nearly-equal
	// large floats when the vector magnitude is close to 1: (1 - m*m) is exactly 0
	// when m == 1, regardless of FP contraction/rounding. The naive form
	// (fCentroidDistToMagnitude2Ratio*fCentroidDistToMagnitude2Ratio - fDistanceToCentroid2Sqr)
	// can drift to ~1e-7 with AVX2 codegen, producing a spurious 1e-4 error-bound bias.
	float fOneMinusMagSqr = std::fma ( -tFactors1Bit.m_fVectorMagnitude, tFactors1Bit.m_fVectorMagnitude, 1.0f );
	float fInvMagSqr = 1.0f / ( tFactors1Bit.m_fVectorMagnitude * tFactors1Bit.m_fVectorMagnitude );
	float fProjectionDiff = fDistanceToCentroid2Sqr * fOneMinusMagSqr * fInvMagSqr;
	float fProjectionDist = fProjectionDiff>0.0f ? std::sqrt(fProjectionDiff) : 0.0f;
	float fError = 2.0f*tBinaryParam.m_fMaxError*fProjectionDist;
	float fErrorBound = fError*std::sqrt(tFactors4Bit.m_fDistanceToCentroidSq);
	if ( std::isfinite(fErrorBound) )
		fDist += fErrorBound;

	return fDist;
}


template<bool BUILD, int64_t (*DOTPRODUCT_FN)( const uint8_t * pVec4Bit, const uint8_t * pVec1Bit, int iBytes )>
static float IPBinaryFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uRowID1, size_t uRowID2, const void * __restrict pParam )
{
	const auto & tBinaryParam = *(const DistFuncParamBinary_t*)pParam;

	auto pV1 = (const uint8_t *)pVect1;
	auto pV2 = (const uint8_t *)pVect2;

	// ignore uRowID2, pull uRowID1 from the pool
	if constexpr (BUILD)
	{
		// we are getting 1-bit data as first argument, but we need 4-bit data
		if ( uRowID1!=(size_t)-1 )
		{	
			// fetch 4-bit data from the pool
			pV1 = tBinaryParam.m_fnFetcher(uRowID1);
		}
	}

	assert ( uRowID2!=(size_t)-1 );

	auto tFactors4Bit = *(Binary4BitFactors_t*)pV1;
	pV1 += sizeof(Binary4BitFactors_t);

	auto tFactors1Bit = *(Binary1BitFactorsIP_t*)pV2;
	pV2 += sizeof(Binary1BitFactorsIP_t);
	
	int iBytes = ( tBinaryParam.m_uDim+7 ) >> 3;
	int64_t iHammingDist = DOTPRODUCT_FN ( (const uint8_t*)pV1, (const uint8_t*)pV2, iBytes );
	return IPBinaryFloatDistanceFromHammingDist ( tFactors4Bit, tFactors1Bit, iHammingDist, tBinaryParam );
}

// This binary distance calculation is derived from Elasticsearch's Java implementation
// in org.elasticsearch.index.codec.vectors.es816.ES816BinaryFlatVectorsScorer
// Permalink: https://github.com/elastic/elasticsearch/blob/1dd41ec2b683a7b7c9c16af404b842cf85cbd5bc/server/src/main/java/org/elasticsearch/index/codec/vectors/es816/ES816BinaryFlatVectorsScorer.java
template<bool BUILD, int64_t (*DOTPRODUCT_FN)( const uint8_t * pVec4Bit, const uint8_t * pVec1Bit, int iBytes )>
static float L2BinaryFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uRowID1, size_t uRowID2, const void * __restrict pParam )
{
	const auto & tBinaryParam = *(const DistFuncParamBinary_t*)pParam;

	auto pV1 = (const uint8_t *)pVect1;
	auto pV2 = (const uint8_t *)pVect2;

	// ignore uRowID2, pull uRowID1 from the pool
	if constexpr (BUILD)
	{
		// we are getting 1-bit data as first argument, but we need 4-bit data
		if ( uRowID1!=(size_t)-1 )
		{	
			// fetch 4-bit data from the pool
			pV1 = tBinaryParam.m_fnFetcher(uRowID1);
		}
	}

	assert ( uRowID2!=(size_t)-1 );

	auto tFactors4Bit = *(Binary4BitFactors_t*)pV1;
	pV1 += sizeof(Binary4BitFactors_t);

	auto tFactors1Bit = *(Binary1BitFactorsL2_t*)pV2;
	pV2 += sizeof(Binary1BitFactorsL2_t);

	int iBytes = ( tBinaryParam.m_uDim+7 ) >> 3;
	int64_t iHammingDist = DOTPRODUCT_FN ( (const uint8_t*)pV1, (const uint8_t*)pV2, iBytes );
	return L2BinaryFloatDistanceFromHammingDist ( tFactors4Bit, tFactors1Bit, iHammingDist, tBinaryParam );
}

///////////////////////////////////////////////////////////////////////////////

float IPBinaryFloatDistanceGeneric ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam )
{
	return IPBinaryFloatDistance<false,BinaryDotProduct> ( pVect1, pVect2, uRowID1, uRowID2, pParam );
}

float IPBinaryFloatDistanceSIMD16 ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam )
{
	return IPBinaryFloatDistance<false,BinaryDotProduct16<false>> ( pVect1, pVect2, uRowID1, uRowID2, pParam );
}

float IPBinaryFloatDistanceSIMD16Residuals ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam )
{
	return IPBinaryFloatDistance<false,BinaryDotProduct16<true>> ( pVect1, pVect2, uRowID1, uRowID2, pParam );
}

template <bool RESIDUALS>
static void IPBinaryFloatDistanceBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
{
	const auto & tBinaryParam = *(const DistFuncParamBinary_t*)pParam;

	auto pV1 = (const uint8_t *)pVect1;
	auto pVA = (const uint8_t *)pVect2A;
	auto pVB = (const uint8_t *)pVect2B;

	assert ( uRowID2A!=(size_t)-1 );
	assert ( uRowID2B!=(size_t)-1 );

	auto tFactors4Bit = *(Binary4BitFactors_t*)pV1;
	pV1 += sizeof(Binary4BitFactors_t);

	auto tFactors1BitA = *(Binary1BitFactorsIP_t*)pVA;
	pVA += sizeof(Binary1BitFactorsIP_t);
	auto tFactors1BitB = *(Binary1BitFactorsIP_t*)pVB;
	pVB += sizeof(Binary1BitFactorsIP_t);

	int iBytes = ( tBinaryParam.m_uDim+7 ) >> 3;
	int64_t iHammingDistA = 0, iHammingDistB = 0;
	BinaryDotProductBatch2<RESIDUALS> ( pV1, pVA, pVB, iBytes, iHammingDistA, iHammingDistB );

	fDistA = IPBinaryFloatDistanceFromHammingDist ( tFactors4Bit, tFactors1BitA, iHammingDistA, tBinaryParam );
	fDistB = IPBinaryFloatDistanceFromHammingDist ( tFactors4Bit, tFactors1BitB, iHammingDistB, tBinaryParam );
}

void IPBinaryFloatDistanceSIMD16Batch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
{
	IPBinaryFloatDistanceBatch2<false> ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
}

void IPBinaryFloatDistanceSIMD16ResidualsBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
{
	IPBinaryFloatDistanceBatch2<true> ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
}

float L2BinaryFloatDistanceGeneric ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam )
{
	return L2BinaryFloatDistance<false,BinaryDotProduct> ( pVect1, pVect2, uRowID1, uRowID2, pParam );
}

float L2BinaryFloatDistanceSIMD16 ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam )
{
	return L2BinaryFloatDistance<false,BinaryDotProduct16<false>> ( pVect1, pVect2, uRowID1, uRowID2, pParam );
}

float L2BinaryFloatDistanceSIMD16Residuals ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam )
{
	return L2BinaryFloatDistance<false,BinaryDotProduct16<true>> ( pVect1, pVect2, uRowID1, uRowID2, pParam );
}

template <bool RESIDUALS>
static void L2BinaryFloatDistanceBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
{
	const auto & tBinaryParam = *(const DistFuncParamBinary_t*)pParam;

	auto pV1 = (const uint8_t *)pVect1;
	auto pVA = (const uint8_t *)pVect2A;
	auto pVB = (const uint8_t *)pVect2B;

	assert ( uRowID2A!=(size_t)-1 );
	assert ( uRowID2B!=(size_t)-1 );

	auto tFactors4Bit = *(Binary4BitFactors_t*)pV1;
	pV1 += sizeof(Binary4BitFactors_t);

	auto tFactors1BitA = *(Binary1BitFactorsL2_t*)pVA;
	pVA += sizeof(Binary1BitFactorsL2_t);
	auto tFactors1BitB = *(Binary1BitFactorsL2_t*)pVB;
	pVB += sizeof(Binary1BitFactorsL2_t);

	int iBytes = ( tBinaryParam.m_uDim+7 ) >> 3;
	int64_t iHammingDistA = 0, iHammingDistB = 0;
	BinaryDotProductBatch2<RESIDUALS> ( pV1, pVA, pVB, iBytes, iHammingDistA, iHammingDistB );

	fDistA = L2BinaryFloatDistanceFromHammingDist ( tFactors4Bit, tFactors1BitA, iHammingDistA, tBinaryParam );
	fDistB = L2BinaryFloatDistanceFromHammingDist ( tFactors4Bit, tFactors1BitB, iHammingDistB, tBinaryParam );
}

void L2BinaryFloatDistanceSIMD16Batch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
{
	L2BinaryFloatDistanceBatch2<false> ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
}

void L2BinaryFloatDistanceSIMD16ResidualsBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
{
	L2BinaryFloatDistanceBatch2<true> ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
}

///////////////////////////////////////////////////////////////////////////////

IPSpace32BitFloat_c::IPSpace32BitFloat_c ( size_t uDim )
	: Space_c ( uDim )
{
	m_fnDist = IPFloatDistance;
	m_eDistFuncId = DistFuncId_e::IP_FLOAT32;
}

///////////////////////////////////////////////////////////////////////////////

L2Space32BitFloat_c::L2Space32BitFloat_c ( size_t uDim )
	: Space_c ( uDim )
{
	m_fnDist = L2FloatDistance;
	m_eDistFuncId = DistFuncId_e::L2_FLOAT32;
}

///////////////////////////////////////////////////////////////////////////////

IPSpaceBinaryFloat_c::IPSpaceBinaryFloat_c ( size_t uDim, bool bBuild )
	: Space_c ( uDim )
	, m_tDistFuncParam ( uDim )
{
#if defined(USE_SIMDE)
	if ( bBuild )
		m_fnDist = IPBinaryFloatDistance<true,BinaryDotProduct>;
	else
	{
		m_fnDist = IPBinaryFloatDistance<false,BinaryDotProduct>;
		m_eDistFuncId = DistFuncId_e::IP_BINARY_GENERIC;
	}
#else
	int iBytes = ( uDim+7 ) >> 3;
	bool bUseSSE = iBytes>=16;
	bool bNeedResiduals = (iBytes % 16) != 0;

	int iSwitch = (bBuild ? 1 : 0)*4 + (bUseSSE ? 1 : 0)*2 + (bNeedResiduals ? 1 : 0)*1;

	switch ( iSwitch )
	{
	case 0:	m_fnDist = IPBinaryFloatDistance<false,	BinaryDotProduct>; break;
	case 1:	m_fnDist = IPBinaryFloatDistance<false,	BinaryDotProduct>; break;
	case 2: m_fnDist = IPBinaryFloatDistance<false,	BinaryDotProduct16<false>>; break;
	case 3: m_fnDist = IPBinaryFloatDistance<false,	BinaryDotProduct16<true>>; break;
	case 4: m_fnDist = IPBinaryFloatDistance<true,	BinaryDotProduct>; break;
	case 5: m_fnDist = IPBinaryFloatDistance<true,	BinaryDotProduct>; break;
	case 6: m_fnDist = IPBinaryFloatDistance<true,	BinaryDotProduct16<false>>; break;
	case 7: m_fnDist = IPBinaryFloatDistance<true,	BinaryDotProduct16<true>>; break;
	}

	if ( !bBuild )
	{
		if ( bUseSSE )
			m_eDistFuncId = bNeedResiduals ? DistFuncId_e::IP_BINARY_SIMD16_RESIDUALS : DistFuncId_e::IP_BINARY_SIMD16;
		else
			m_eDistFuncId = DistFuncId_e::IP_BINARY_GENERIC;
	}
#endif
}


void IPSpaceBinaryFloat_c::SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer )
{
	m_tDistFuncParam.m_fCentroidDotCentroid	= DotProduct ( tQuantizer.GetSettings().m_dCentroid, tQuantizer.GetSettings().m_dCentroid );
	m_tDistFuncParam.m_fnFetcher			= tQuantizer.GetPoolFetcher();
}

///////////////////////////////////////////////////////////////////////////////

L2SpaceBinaryFloat_c::L2SpaceBinaryFloat_c ( size_t uDim, bool bBuild )
	: Space_c ( uDim )
	, m_tDistFuncParam ( uDim )
{
#if defined(USE_SIMDE)
	if ( bBuild )
		m_fnDist = L2BinaryFloatDistance<true,BinaryDotProduct>;
	else
	{
		m_fnDist = L2BinaryFloatDistance<false,BinaryDotProduct>;
		m_eDistFuncId = DistFuncId_e::L2_BINARY_GENERIC;
	}
#else
	int iBytes = ( uDim+7 ) >> 3;
	bool bUseSSE = iBytes>=16;
	bool bNeedResiduals = (iBytes % 16) != 0;

	int iSwitch = (bBuild ? 1 : 0)*4 + (bUseSSE ? 1 : 0)*2 + (bNeedResiduals ? 1 : 0)*1;

	switch ( iSwitch )
	{
	case 0:	m_fnDist = L2BinaryFloatDistance<false,	BinaryDotProduct>; break;
	case 1:	m_fnDist = L2BinaryFloatDistance<false,	BinaryDotProduct>; break;
	case 2: m_fnDist = L2BinaryFloatDistance<false,	BinaryDotProduct16<false>>; break;
	case 3: m_fnDist = L2BinaryFloatDistance<false,	BinaryDotProduct16<true>>; break;
	case 4: m_fnDist = L2BinaryFloatDistance<true,	BinaryDotProduct>; break;
	case 5: m_fnDist = L2BinaryFloatDistance<true,	BinaryDotProduct>; break;
	case 6: m_fnDist = L2BinaryFloatDistance<true,	BinaryDotProduct16<false>>; break;
	case 7: m_fnDist = L2BinaryFloatDistance<true,	BinaryDotProduct16<true>>; break;
	}

	if ( !bBuild )
	{
		if ( bUseSSE )
			m_eDistFuncId = bNeedResiduals ? DistFuncId_e::L2_BINARY_SIMD16_RESIDUALS : DistFuncId_e::L2_BINARY_SIMD16;
		else
			m_eDistFuncId = DistFuncId_e::L2_BINARY_GENERIC;
	}
#endif
}


void L2SpaceBinaryFloat_c::SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer )
{
	m_tDistFuncParam.m_fnFetcher = tQuantizer.GetPoolFetcher();
}

} // namespace knn

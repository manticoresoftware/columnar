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

static FORCE_INLINE int L2Sqr4Bit ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty )
{
	size_t uQty = *((size_t *)pQty);
	uQty = ( uQty+1 ) >> 1;

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	int iSum = 0;
	for ( size_t i = 0; i < uQty; i++ )
	{
		int iV1Lo = pV1[i] & 0x0F;
		int iV1Hi = ( pV1[i] >> 4 ) & 0x0F;

		int iV2Lo = pV2[i] & 0x0F;
		int iV2Hi = ( pV2[i] >> 4 ) & 0x0F;

		int iDiffLo = iV1Lo - iV2Lo;
		int iDiffHi = iV1Hi - iV2Hi;

		iSum += iDiffLo*iDiffLo + iDiffHi*iDiffHi;
	}

	return iSum;
}


static float L2Sqr4BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr4Bit ( pVect1, pVect2, &(pDistFuncParam->m_uDim) );
	return pDistFuncParam->m_fA*iDist;
}


static FORCE_INLINE int L2Sqr4BitSIMD16 ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pQty )
{
	size_t uQty = *((size_t *)pQty);
	uQty = uQty >> 5;  // 32x 4-bit components

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	__m128i iSum = _mm_setzero_si128();
	__m128i iMask= _mm_set1_epi8(0x0F);

	for ( size_t i = 0; i < uQty; i++ )
	{
		__m128i iV1 = _mm_loadu_si128 ( (const __m128i*)pV1 );
		__m128i iV2 = _mm_loadu_si128 ( (const __m128i*)pV2 );

		__m128i iV1Hi = _mm_and_si128 ( _mm_srli_epi16 ( iV1, 4 ), iMask );
		__m128i iV2Hi = _mm_and_si128 ( _mm_srli_epi16 ( iV2, 4 ), iMask );
		__m128i iV1Lo = _mm_and_si128 ( iV1, iMask );
		__m128i iV2Lo = _mm_and_si128 ( iV2, iMask );

		__m128i iV1Lo16Lo = _mm_unpacklo_epi8 ( iV1Lo, _mm_setzero_si128() );
		__m128i iV1Hi16Lo = _mm_unpackhi_epi8 ( iV1Lo, _mm_setzero_si128() );
		__m128i iV2Lo16Lo = _mm_unpacklo_epi8 ( iV2Lo, _mm_setzero_si128() );
		__m128i iV2Hi16Lo = _mm_unpackhi_epi8 ( iV2Lo, _mm_setzero_si128() );

		__m128i iV1Lo16Hi = _mm_unpacklo_epi8 ( iV1Hi, _mm_setzero_si128() );
		__m128i iV1Hi16Hi = _mm_unpackhi_epi8 ( iV1Hi, _mm_setzero_si128() );
		__m128i iV2Lo16Hi = _mm_unpacklo_epi8 ( iV2Hi, _mm_setzero_si128() );
		__m128i iV2Hi16Hi = _mm_unpackhi_epi8 ( iV2Hi, _mm_setzero_si128() );

		__m128i iDiffLo16Lo = _mm_sub_epi16 ( iV1Lo16Lo, iV2Lo16Lo );
		__m128i iDiffHi16Lo = _mm_sub_epi16 ( iV1Hi16Lo, iV2Hi16Lo );
		__m128i iDiffLo16Hi = _mm_sub_epi16 ( iV1Lo16Hi, iV2Lo16Hi );
		__m128i iDiffHi16Hi = _mm_sub_epi16 ( iV1Hi16Hi, iV2Hi16Hi );

		__m128i iSqrLo16Lo = _mm_madd_epi16 ( iDiffLo16Lo, iDiffLo16Lo );
		__m128i iSqrHi16Lo = _mm_madd_epi16 ( iDiffHi16Lo, iDiffHi16Lo );
		__m128i iSqrLo16Hi = _mm_madd_epi16 ( iDiffLo16Hi, iDiffLo16Hi );
		__m128i iSqrHi16Hi = _mm_madd_epi16 ( iDiffHi16Hi, iDiffHi16Hi );

		iSum = _mm_add_epi32 ( iSum, iSqrLo16Lo );
		iSum = _mm_add_epi32 ( iSum, iSqrHi16Lo );
		iSum = _mm_add_epi32 ( iSum, iSqrLo16Hi );
		iSum = _mm_add_epi32 ( iSum, iSqrHi16Hi );

		pV1 += 16;
		pV2 += 16;
	}

	alignas(16) int dBuffer[4];
	_mm_store_si128 ( (__m128i*)dBuffer, iSum );
	return dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];
}


static float L2Sqr4BitSIMD16FloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr4BitSIMD16 ( pVect1, pVect2, (size_t)-1, (size_t)-1, &(pDistFuncParam->m_uDim) );
	return pDistFuncParam->m_fA*iDist;
}


static float L2Sqr4BitSIMD16FloatResiduals ( const void * pVect1, const void * pVect2, size_t, size_t, const void * pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	size_t uQty = pDistFuncParam->m_uDim;
	size_t uQty16 = uQty >> 4 << 4;
	size_t uNumBytes16 = uQty16 >> 1;

	int iDist1 = L2Sqr4BitSIMD16 ( pVect1, pVect2, (size_t)-1, (size_t)-1, &uQty16 );

	auto pV1 = (uint8_t *)pVect1 + uNumBytes16;
	auto pV2 = (uint8_t *)pVect2 + uNumBytes16;
	size_t uQtyLeft = uQty - uQty16;
	int iDist2 = L2Sqr4Bit ( pV1, pV2, &uQtyLeft );

	return pDistFuncParam->m_fA*(iDist1 + iDist2);
}


L2Space4BitFloat_c::L2Space4BitFloat_c ( size_t uDim )
	: L2Space8BitFloat_c ( uDim )
{
	size_t uBytes = get_data_size();
	if ( uBytes % 16 == 0)
		m_fnDist = L2Sqr4BitSIMD16FloatDistance;
	else if ( uBytes > 16 )
		m_fnDist = L2Sqr4BitSIMD16FloatResiduals;
	else
		m_fnDist = L2Sqr4BitFloatDistance;
}

///////////////////////////////////////////////////////////////////////////////

FORCE_INLINE uint32_t PopCnt32 ( uint32_t uVal )
{
#if defined(USE_SIMDE)
	return __builtin_popcount(uVal);
#else
	return _mm_popcnt_u32(uVal);
#endif
}


static FORCE_INLINE int L2Sqr1Bit ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uQty )
{
	size_t uLenBytes = (uQty + 7) >> 3;

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;
	int iDistance = 0;
	for ( size_t i = 0; i < uLenBytes; i++ )
		iDistance += PopCnt32 ( pV1[i] ^ pV2[i] );

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

static FORCE_INLINE int IP4Bit ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uQty )
{
	uQty = ( uQty+1 ) >> 1;

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	int iSum = 0;
	for ( size_t i = 0; i < uQty; i++ )
	{
		int iV1Lo = pV1[i] & 0x0F;
		int iV1Hi = ( pV1[i] >> 4 ) & 0x0F;

		int iV2Lo = pV2[i] & 0x0F;
		int iV2Hi = ( pV2[i] >> 4 ) & 0x0F;

		iSum += iV1Lo*iV2Lo + iV1Hi*iV2Hi;
	}

	return iSum;
}


static float IP4BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	float fVect1B = *(float*)pVect1;
	float fVect2B = *(float*)pVect2;
	int iDotProduct = IP4Bit ( (uint8_t*)pVect1 + sizeof(float), (uint8_t*)pVect2 + sizeof(float), pDistFuncParam->m_uDim );
	return pDistFuncParam->CalcIP(iDotProduct) + fVect1B + fVect2B;
}


static FORCE_INLINE int IP4BitSIMD16 ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uQty )
{
	uQty = uQty >> 5;  // 32x 4-bit components

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	__m128i iSum	= _mm_setzero_si128();
	__m128i iUnit16	= _mm_set1_epi16(1);
	__m128i iMask	= _mm_set1_epi8(0x0F);

	for ( size_t i = 0; i < uQty; i++ )
	{
		__m128i iV1 = _mm_loadu_si128 ( (const __m128i*)pV1 );
		__m128i iV2 = _mm_loadu_si128 ( (const __m128i*)pV2 );

		__m128i iV1Hi = _mm_and_si128 ( _mm_srli_epi16 ( iV1, 4 ), iMask );
		__m128i iV2Hi = _mm_and_si128 ( _mm_srli_epi16 ( iV2, 4 ), iMask );
		__m128i iV1Lo = _mm_and_si128 ( iV1, iMask );
		__m128i iV2Lo = _mm_and_si128 ( iV2, iMask );

		__m128i iV1Lo16Lo = _mm_unpacklo_epi8 ( iV1Lo, _mm_setzero_si128() );
		__m128i iV1Hi16Lo = _mm_unpackhi_epi8 ( iV1Lo, _mm_setzero_si128() );
		__m128i iV2Lo16Lo = _mm_unpacklo_epi8 ( iV2Lo, _mm_setzero_si128() );
		__m128i iV2Hi16Lo = _mm_unpackhi_epi8 ( iV2Lo, _mm_setzero_si128() );

		__m128i iV1Lo16Hi = _mm_unpacklo_epi8 ( iV1Hi, _mm_setzero_si128() );
		__m128i iV1Hi16Hi = _mm_unpackhi_epi8 ( iV1Hi, _mm_setzero_si128() );
		__m128i iV2Lo16Hi = _mm_unpacklo_epi8 ( iV2Hi, _mm_setzero_si128() );
		__m128i iV2Hi16Hi = _mm_unpackhi_epi8 ( iV2Hi, _mm_setzero_si128() );

		__m128i iProdLo16Lo = _mm_mullo_epi16 ( iV1Lo16Lo, iV2Lo16Lo );
		__m128i iProdLo16Hi = _mm_mullo_epi16 ( iV1Lo16Hi, iV2Lo16Hi );
		__m128i iProdHi16Lo = _mm_mullo_epi16 ( iV1Hi16Lo, iV2Hi16Lo );
		__m128i iProdHi16Hi = _mm_mullo_epi16 ( iV1Hi16Hi, iV2Hi16Hi );

		__m128i iSumLo = _mm_add_epi16 ( iProdLo16Lo, iProdLo16Hi );
		__m128i iSumHi = _mm_add_epi16 ( iProdHi16Lo, iProdHi16Hi );
		__m128i iSum16 = _mm_add_epi16 ( iSumLo, iSumHi );
		__m128i iSum32 = _mm_madd_epi16 ( iSum16, iUnit16 );

		iSum = _mm_add_epi32 ( iSum, iSum32 );

		pV1 += 16;
		pV2 += 16;
	}

	alignas(16) int dBuffer[4];
	_mm_store_si128 ( (__m128i*)dBuffer, iSum );
	return dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];
}


static float IP4BitSIMD16FloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	float fVect1B = *(float*)pVect1;
	float fVect2B = *(float*)pVect2;
	int iDotProduct = IP4BitSIMD16 ( (uint8_t*)pVect1 + sizeof(float), (uint8_t*)pVect2 + sizeof(float), pDistFuncParam->m_uDim );
	return pDistFuncParam->CalcIP(iDotProduct) + fVect1B + fVect2B;
}


static float IP4BitSIMD16FloatResiduals ( const void * __restrict pVect1, const void * __restrict pVect2, size_t, size_t, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	size_t uQty = pDistFuncParam->m_uDim;
	size_t uQty16 = uQty >> 4 << 4;
	size_t uNumBytes16 = uQty16 >> 1;

	auto pV1 = (uint8_t *)pVect1;
	auto pV2 = (uint8_t *)pVect2;
	float fVect1B = *(float*)pV1;
	float fVect2B = *(float*)pV2;
	pV1 += sizeof(float);
	pV2 += sizeof(float);

	int iDotProduct = IP4BitSIMD16 ( pV1, pV1, uQty16 );

	pV1 += uNumBytes16;
	pV2 += uNumBytes16;
	iDotProduct += IP4Bit ( pV1, pV2, uQty - uQty16 );
	return pDistFuncParam->CalcIP(iDotProduct) + fVect1B + fVect2B;
}

///////////////////////////////////////////////////////////////////////////////

IPSpace4BitFloat_c::IPSpace4BitFloat_c ( size_t uDim )
	: IPSpace8BitFloat_c ( uDim )
{
	size_t uBytes = (uDim+3)>>1;
	if ( uBytes % 16 == 0 )
		m_fnDist = IP4BitSIMD16FloatDistance;
	else if ( uBytes > 16 )
		m_fnDist = IP4BitSIMD16FloatResiduals;
	else
		m_fnDist = IP4BitFloatDistance;
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

            iPopCntSum += PopCnt32 ( uVal4Bit & uVal1Bit );
			pVec4BitPtr += sizeof(uint32_t);
        }
        
        for ( ; j < iBytes; j++ )
		{
            iPopCntSum += PopCnt32 ( ( *pVec4BitPtr & pVec1Bit[j] ) & 0xFF );
			pVec4BitPtr++;
		}

        iResult += iPopCntSum << i;
    }
    
    return iResult;
}

#if !defined(USE_SIMDE)
template <bool RESIDUALS>
static int64_t BinaryDotProduct16 ( const uint8_t * pVec4Bit, const uint8_t * pVec1Bit, int iBytes )
{
    int64_t iPopCnt0 = 0;
    int64_t iPopCnt1 = 0;
    int64_t iPopCnt2 = 0;
    int64_t iPopCnt3 = 0;
    int i = 0;

    const int iLimit16Bytes = iBytes & ~15;
    for ( ; i < iLimit16Bytes; i += 16 )
    {
        __m128i iVec1Bit = _mm_loadu_si128 ( (__m128i*)&pVec1Bit[i] );
        
        __m128i iVec4Bit0 = _mm_loadu_si128 ( (__m128i*)&pVec4Bit[i] );
        __m128i iVec4Bit1 = _mm_loadu_si128 ( (__m128i*)&pVec4Bit[i + iBytes] );
        __m128i iVec4Bit2 = _mm_loadu_si128 ( (__m128i*)&pVec4Bit[i + 2 * iBytes] );
        __m128i iVec4Bit3 = _mm_loadu_si128 ( (__m128i*)&pVec4Bit[i + 3 * iBytes] );

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
        for (; i < iBytes; i++)
        {
            uint8_t uValue = pVec1Bit[i];
            iPopCnt0 += _mm_popcnt_u32 ( ( uValue & pVec4Bit[i] ) & 0xFF );
            iPopCnt1 += _mm_popcnt_u32 ( ( uValue & pVec4Bit[i + iBytes] ) & 0xFF );
            iPopCnt2 += _mm_popcnt_u32 ( ( uValue & pVec4Bit[i + 2 * iBytes] ) & 0xFF );
            iPopCnt3 += _mm_popcnt_u32 ( ( uValue & pVec4Bit[i + 3 * iBytes] ) & 0xFF );
        }
    }

    return iPopCnt0 + ( iPopCnt1 << 1 ) + ( iPopCnt2 << 2 ) + ( iPopCnt3 << 3 );
}
#endif

// This binary distance calculation is derived from Elasticsearch's Java implementation
// in org.elasticsearch.index.codec.vectors.es816.ES816BinaryFlatVectorsScorer
// Permalink: https://github.com/elastic/elasticsearch/blob/1dd41ec2b683a7b7c9c16af404b842cf85cbd5bc/server/src/main/java/org/elasticsearch/index/codec/vectors/es816/ES816BinaryFlatVectorsScorer.java
template<bool BUILD, int64_t (*DOTPRODUCT_FN)( const uint8_t * pVec4Bit, const uint8_t * pVec1Bit, int iBytes )>
static float IPBinaryFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uRowID1, size_t uRowID2, const void * __restrict pParam )
{
	auto tBinaryParam = *(const DistFuncParamBinary_t*)pParam;

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

	float * pV1f = (float*)pV1;
	float fQuantizedSum			= *pV1f++;
	pV1f++;	// skip fDistanceToCentroidSq
	float fMin					= *pV1f++;
	float fRange				= *pV1f++;
	float fVecMinusCentroidNorm	= *pV1f++;
	float fVecDotCentroid		= *pV1f++;

	float * pV2f = (float*)pV2;
	float fQuality				= *pV2f++;
	float fVec2MinusCentroidNorm= *pV2f++;
	float fVec2DocCentroid		= *pV2f++;
	float fPopCnt2				= *pV2f++;

	int iBytes = ( tBinaryParam.m_uDim+7 ) >> 3;
	int64_t iHammingDist = DOTPRODUCT_FN ( (const uint8_t*)pV1f, (const uint8_t*)pV2f, iBytes );

	float fDist = 0.0f;
	if ( fVec2MinusCentroidNorm==0.0f || fQuality==0.0f )
		fDist = fVec2DocCentroid + fVecDotCentroid - tBinaryParam.m_fCentroidDotCentroid;
	else
	{
		assert ( std::isfinite(fQuality) );
		float fEstimatedDot = ( 2.0f*fRange/tBinaryParam.m_fSqrtDim * iHammingDist + 2.0f*fMin/tBinaryParam.m_fSqrtDim*fPopCnt2 - fRange/tBinaryParam.m_fSqrtDim*fQuantizedSum - tBinaryParam.m_fSqrtDim*fMin ) / fQuality;
		fDist = fVecMinusCentroidNorm*fVec2MinusCentroidNorm*fEstimatedDot + fVec2DocCentroid + fVecDotCentroid - tBinaryParam.m_fCentroidDotCentroid;
	}

	assert ( std::isfinite(fDist) );

	float fQualitySqr = fQuality*fQuality;
	float fErrorBound = fVecMinusCentroidNorm*fVec2MinusCentroidNorm*( tBinaryParam.m_fMaxError*std::sqrt ( ( 1.0f - fQualitySqr )/fQualitySqr ) );
	float fAdjustedDist = std::isfinite(fErrorBound) ? fDist - fErrorBound : fDist;
	return 1.0f - std::max ( ( 1.0f + fAdjustedDist ) / 2.0f, 0.0f );
}

// This binary distance calculation is derived from Elasticsearch's Java implementation
// in org.elasticsearch.index.codec.vectors.es816.ES816BinaryFlatVectorsScorer
// Permalink: https://github.com/elastic/elasticsearch/blob/1dd41ec2b683a7b7c9c16af404b842cf85cbd5bc/server/src/main/java/org/elasticsearch/index/codec/vectors/es816/ES816BinaryFlatVectorsScorer.java
template<bool BUILD, int64_t (*DOTPRODUCT_FN)( const uint8_t * pVec4Bit, const uint8_t * pVec1Bit, int iBytes )>
static float L2BinaryFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, size_t uRowID1, size_t uRowID2, const void * __restrict pParam )
{
	auto tBinaryParam = *(const DistFuncParamBinary_t*)pParam;

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

	float * pV1f = (float*)pV1;
	float fQuantizedSum			= *pV1f++;
	float fDistanceToCentroidSq	= *pV1f++;
	float fMin					= *pV1f++;
	float fRange				= *pV1f++;
	pV1f += 2;	// skip fVecMinusCentroidNorm, fVecDotCentroid

	float * pV2f = (float*)pV2;
	float fDistanceToCentroid2	= *pV2f++;
	float fVectorMagnitude2		= *pV2f++;
	float fPopCnt2				= *pV2f++;

    float fDistanceToCentroid2Sqr = fDistanceToCentroid2 * fDistanceToCentroid2;
    double fCentroidDistToMagnitude2Ratio = fDistanceToCentroid2 / fVectorMagnitude2;

    float fPopCntCoeff = -2.0f / tBinaryParam.m_fSqrtDim * fCentroidDistToMagnitude2Ratio * (fPopCnt2 * 2.0f - tBinaryParam.m_uDim );
    float fIPCoeff = -2.0f / tBinaryParam.m_fSqrtDim * fCentroidDistToMagnitude2Ratio;

	int iBytes = ( tBinaryParam.m_uDim+7 ) >> 3;
    int64_t iHammingDist = DOTPRODUCT_FN ( (const uint8_t*)pV1f, (const uint8_t*)pV2f, iBytes );
    float fDist = fDistanceToCentroid2Sqr + fDistanceToCentroidSq + fPopCntCoeff * fMin + ( iHammingDist*2 - fQuantizedSum )*fIPCoeff*fRange;

    float fProjectionDist = std::sqrt ( fCentroidDistToMagnitude2Ratio*fCentroidDistToMagnitude2Ratio - fDistanceToCentroid2Sqr );
    float fError = 2.0f*tBinaryParam.m_fMaxError*fProjectionDist;
    float fErrorBound = fError*std::sqrt(fDistanceToCentroidSq);
    if ( std::isfinite(fErrorBound) )
        fDist += fErrorBound;

    return 1.0f - std::max ( 1.0f / ( 1.0f + fDist ), 0.0f );
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
	m_fnDist = IPBinaryFloatDistance<false,BinaryDotProduct>;
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
	m_fnDist = L2BinaryFloatDistance<false,BinaryDotProduct>;
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
#endif
}


void L2SpaceBinaryFloat_c::SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer )
{
	m_tDistFuncParam.m_fnFetcher = tQuantizer.GetPoolFetcher();
}

} // namespace knn
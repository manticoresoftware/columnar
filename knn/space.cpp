// Copyright (c) 2025, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
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

#include "space.h"

#include "util_private.h"

namespace knn
{

float DistFuncParamIP_t::CalcIP ( int iDotProduct, int iSumVec1, int iSumVec2 ) const
{
	return m_fA + m_fB*( iSumVec1 + iSumVec2 ) + m_fC*iDotProduct;
}

///////////////////////////////////////////////////////////////////////////////

static FORCE_INLINE int L2Sqr8Bit ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty )
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


static float L2Sqr8BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr8Bit ( pVect1, pVect2, &(pDistFuncParam->m_uDim) );
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


static float L2Sqr8BitSIMD16FloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr8BitSIMD16 ( pVect1, pVect2, &(pDistFuncParam->m_uDim) );
	return pDistFuncParam->m_fA*iDist;
}


static float L2Sqr8BitSIMD16FloatResiduals ( const void * pVect1, const void * pVect2, const void * pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	size_t uQty = pDistFuncParam->m_uDim;
	size_t uQty16 = uQty >> 4 << 4;

	int iDist1 = L2Sqr8BitSIMD16 ( pVect1, pVect2, &uQty16 );

	auto pV1 = (uint8_t *)pVect1 + uQty16;
	auto pV2 = (uint8_t *)pVect2 + uQty16;
	size_t uQtyLeft = uQty - uQty16;
	int iDist2 = L2Sqr8Bit ( pV1, pV2, &uQtyLeft );

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


void L2Space8BitFloat_c::SetQuantizationSettings ( const QuantizationSettings_t & tSettings )
{
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


static float L2Sqr4BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr4Bit ( pVect1, pVect2, &(pDistFuncParam->m_uDim) );
	return pDistFuncParam->m_fA*iDist;
}


static FORCE_INLINE int L2Sqr4BitSIMD16 ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty )
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


static float L2Sqr4BitSIMD16FloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr4BitSIMD16 ( pVect1, pVect2, &(pDistFuncParam->m_uDim) );
	return pDistFuncParam->m_fA*iDist;
}


static float L2Sqr4BitSIMD16FloatResiduals ( const void * pVect1, const void * pVect2, const void * pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	size_t uQty = pDistFuncParam->m_uDim;
	size_t uQty16 = uQty >> 4 << 4;
	size_t uNumBytes16 = uQty16 >> 1;

	int iDist1 = L2Sqr4BitSIMD16 ( pVect1, pVect2, &uQty16 );

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


static FORCE_INLINE int L2Sqr1Bit ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty )
{
	size_t uQty = *((size_t *)pQty);
	size_t uLenBytes = (uQty + 7) >> 3;

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;
	int iDistance = 0;
	for ( size_t i = 0; i < uLenBytes; i++ )
		iDistance += PopCnt32 ( pV1[i] ^ pV2[i] );

	return iDistance;
}


static float L2Sqr1BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr1Bit ( pVect1, pVect2, &(pDistFuncParam->m_uDim) );
	return pDistFuncParam->m_fA*iDist;
}

#if !defined(USE_SIMDE)
static FORCE_INLINE int L2Sqr1Bit8x ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty )
{
	size_t uQty = *((size_t *)pQty);
	size_t uLenBytes = (uQty + 7) >> 3;
	size_t uQty8 = uLenBytes >> 3;

	auto pV1 = (uint64_t*)pVect1;
	auto pV2 = (uint64_t*)pVect2;

	int iDistance = 0;
	for ( size_t i = 0; i < uQty8; i++ )
		iDistance += _mm_popcnt_u64 ( pV1[i] ^ pV2[i] );

	return iDistance;
}


static float L2Sqr1Bit8xFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	int iDist = L2Sqr1Bit8x ( pVect1, pVect2, &(pDistFuncParam->m_uDim) );
	return pDistFuncParam->m_fA*iDist;
}


static float L2Sqr1Bit8xFloatResiduals ( const void * pVect1, const void * pVect2, const void * pParam )
{
	auto pDistFuncParam = (const DistFuncParamL2_t*)pParam;
	size_t uQty = pDistFuncParam->m_uDim;
	size_t uLenBytes = (uQty + 7) >> 3;
	size_t uQty8 = uLenBytes >> 3;
	size_t uLenBytes8 = uQty8 << 3;

	int iDist1 = L2Sqr1Bit8x ( pVect1, pVect2, &uQty8 );

	auto pV1 = (uint8_t*)pVect1 + uLenBytes8;
	auto pV2 = (uint8_t*)pVect2 + uLenBytes8;
	size_t uQtyLeft = uQty - uQty8;
	int iDist2 = L2Sqr1Bit ( pV1, pV2, &uQtyLeft );

	return pDistFuncParam->m_fA*( iDist1 + iDist2 );
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

static FORCE_INLINE int IP8Bit ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty, int & iSumVec1, int & iSumVec2 )
{
	size_t uQty = *((size_t*)pQty);
	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	int iRes = 0;
	for ( size_t i = 0; i < uQty; i++ )
	{
		int iV1 = pV1[i];
		int iV2 = pV2[i];
		iSumVec1 += iV1;
		iSumVec2 += iV2;
		iRes += iV1*iV2;
	}

	return iRes;
}


static float IP8BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iSumVec1 = 0;
	int iSumVec2 = 0;
	int iDotProduct = IP8Bit ( pVect1, pVect2, &(pDistFuncParam->m_uDim), iSumVec1, iSumVec2 );
	return 1.0f - pDistFuncParam->CalcIP ( iDotProduct, iSumVec1, iSumVec2 );
}


static FORCE_INLINE int IP8BitSIMD16 ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty, int & iSumVec1, int & iSumVec2 )
{
	size_t uQty = *((size_t*)pQty);
	uQty >>= 4;
	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	__m128i iSum = _mm_setzero_si128();
	__m128i iVec1SumPacked = _mm_setzero_si128();
	__m128i iVec2SumPacked = _mm_setzero_si128();

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

		__m128i iElementSumVec1 = _mm_add_epi32 ( _mm_add_epi32 ( iV1Lo32Lo, iV1Lo32Hi ), _mm_add_epi32 ( iV1Hi32Lo, iV1Hi32Hi ) );
		iVec1SumPacked = _mm_add_epi32 ( iVec1SumPacked, iElementSumVec1 );

		__m128i iElementSumVec2 = _mm_add_epi32 ( _mm_add_epi32 ( iV2Lo32Lo, iV2Lo32Hi ), _mm_add_epi32 ( iV2Hi32Lo, iV2Hi32Hi ) );
		iVec2SumPacked = _mm_add_epi32 ( iVec2SumPacked, iElementSumVec2 );

		iSum = _mm_add_epi32 ( iSum, iSumLo );
		iSum = _mm_add_epi32 ( iSum, iSumHi );

		pV1 += 16;
		pV2 += 16;
	}

	alignas(16) int dBuffer[4];
	_mm_store_si128 ( (__m128i*)dBuffer, iVec1SumPacked );
	iSumVec1 += dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];

	_mm_store_si128 ( (__m128i*)dBuffer, iVec2SumPacked );
	iSumVec2 += dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];

	_mm_store_si128 ( (__m128i*)dBuffer, iSum );
	return dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];
}


static float IP8BitSIMD16FloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iSumVec1 = 0;
	int iSumVec2 = 0;
	int iDotProduct = IP8BitSIMD16 ( pVect1, pVect2, &(pDistFuncParam->m_uDim), iSumVec1, iSumVec2 );
	return 1.0f - pDistFuncParam->CalcIP ( iDotProduct, iSumVec1, iSumVec2 );
}


static float IP8BitSIMD16FloatResiduals ( const void * pVect1, const void * pVect2, const void * pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iSumVec1 = 0;
	int iSumVec2 = 0;

	size_t uQty = pDistFuncParam->m_uDim;
	size_t uQty16 = uQty >> 4 << 4;

	int iDotProduct = IP8BitSIMD16 ( pVect1, pVect2, &uQty16, iSumVec1, iSumVec2 );

	auto pV1 = (uint8_t *)pVect1 + uQty16;
	auto pV2 = (uint8_t *)pVect2 + uQty16;
	size_t uQtyLeft = uQty - uQty16;
	iDotProduct += IP8Bit ( pV1, pV2, &uQtyLeft, iSumVec1, iSumVec2 );
	return 1.0f - pDistFuncParam->CalcIP ( iDotProduct, iSumVec1, iSumVec2 );
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


void IPSpace8BitFloat_c::SetQuantizationSettings ( const QuantizationSettings_t & tSettings )
{
	float fAlpha = CalcAlpha(tSettings);
	m_tDistFuncParam.m_fA = tSettings.m_fMin*tSettings.m_fMin*m_uDim;
	m_tDistFuncParam.m_fB = fAlpha*tSettings.m_fMin;
	m_tDistFuncParam.m_fC = fAlpha*fAlpha;
}

///////////////////////////////////////////////////////////////////////////////

static FORCE_INLINE int IP4Bit ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty, int & iSumVec1, int & iSumVec2 )
{
	size_t uQty = *((size_t*)pQty);
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
		iSumVec1 += iV1Lo + iV1Hi;
		iSumVec2 += iV2Lo + iV2Hi;
	}

	return iSum;
}


static float IP4BitDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iSumVec1 = 0;
	int iSumVec2 = 0;
	int iDotProduct = IP4Bit ( pVect1, pVect2, &(pDistFuncParam->m_uDim), iSumVec1, iSumVec2 );
	return 1.0f - pDistFuncParam->CalcIP ( iDotProduct, iSumVec1, iSumVec2 );
}


static FORCE_INLINE int IP4BitSIMD16 ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty, int & iSumVec1, int & iSumVec2 )
{
	size_t uQty = *((size_t*)pQty);
	uQty = uQty >> 5;  // 32x 4-bit components

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;

	__m128i iSum	= _mm_setzero_si128();
	__m128i iUnit16	= _mm_set1_epi16(1);
	__m128i iMask	= _mm_set1_epi8(0x0F);
	__m128i iVec1SumPacked = _mm_setzero_si128();
	__m128i iVec2SumPacked = _mm_setzero_si128();

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

		__m128i iElementSumVec1 = _mm_add_epi16 ( _mm_add_epi16 ( iV1Lo16Lo, iV1Hi16Lo ), _mm_add_epi16 ( iV1Lo16Hi, iV1Hi16Hi ) );
		__m128i iVec1Sum32 = _mm_madd_epi16 ( iElementSumVec1, iUnit16 );
		iVec1SumPacked = _mm_add_epi32 ( iVec1SumPacked, iVec1Sum32 );

		__m128i iElementSumVec2 = _mm_add_epi16 ( _mm_add_epi16 ( iV2Lo16Lo, iV2Hi16Lo ), _mm_add_epi16 ( iV2Lo16Hi, iV2Hi16Hi ) );
		__m128i iVec2Sum32 = _mm_madd_epi16 ( iElementSumVec2, iUnit16 );
		iVec2SumPacked = _mm_add_epi32 ( iVec2SumPacked, iVec2Sum32 );

		iSum = _mm_add_epi32 ( iSum, iSum32 );

		pV1 += 16;
		pV2 += 16;
	}

	alignas(16) int dBuffer[4];
	_mm_store_si128 ( (__m128i*)dBuffer, iVec1SumPacked );
	iSumVec1 += dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];

	_mm_store_si128 ( (__m128i*)dBuffer, iVec2SumPacked );
	iSumVec2 += dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];

	_mm_store_si128 ( (__m128i*)dBuffer, iSum );
	return dBuffer[0] + dBuffer[1] + dBuffer[2] + dBuffer[3];
}


static float IP4BitSIMD16FloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iSumVec1 = 0;
	int iSumVec2 = 0;
	int iDotProduct = IP4BitSIMD16 ( pVect1, pVect2, &(pDistFuncParam->m_uDim), iSumVec1, iSumVec2 );
	return 1.0f - pDistFuncParam->CalcIP ( iDotProduct, iSumVec1, iSumVec2 );
}


static float IP4BitSIMD16FloatResiduals ( const void * pVect1, const void * pVect2, const void * pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iSumVec1 = 0;
	int iSumVec2 = 0;

	size_t uQty = pDistFuncParam->m_uDim;
	size_t uQty16 = uQty >> 4 << 4;
	size_t uNumBytes16 = uQty16 >> 1;

	int iDotProduct = IP4BitSIMD16 ( pVect1, pVect2, &uQty16, iSumVec1, iSumVec2 );

	auto pV1 = (uint8_t *)pVect1 + uNumBytes16;
	auto pV2 = (uint8_t *)pVect2 + uNumBytes16;
	size_t uQtyLeft = uQty - uQty16;
	iDotProduct += IP4Bit ( pV1, pV2, &uQtyLeft, iSumVec1, iSumVec2 );
	return 1.0f - pDistFuncParam->CalcIP ( iDotProduct, iSumVec1, iSumVec2 );
}

///////////////////////////////////////////////////////////////////////////////

IPSpace4BitFloat_c::IPSpace4BitFloat_c ( size_t uDim )
	: IPSpace8BitFloat_c ( uDim )
{
	size_t uBytes = get_data_size();
	if ( uBytes % 16 == 0)
		m_fnDist = IP4BitSIMD16FloatDistance;
	else if ( uBytes > 16 )
		m_fnDist = IP4BitSIMD16FloatResiduals;
	else
		m_fnDist = IP4BitDistance;
}

///////////////////////////////////////////////////////////////////////////////

static FORCE_INLINE int IP1Bit ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty, int & iSumVec1, int & iSumVec2 )
{
	size_t uQty = *((size_t *)pQty);
	size_t uLenBytes = (uQty + 7) >> 3;

	auto pV1 = (uint8_t*)pVect1;
	auto pV2 = (uint8_t*)pVect2;
	int iDistance = 0;
	for ( size_t i = 0; i < uLenBytes; i++ )
	{
		uint32_t uV1 = pV1[i];
		uint32_t uV2 = pV2[i];
		iSumVec1 += PopCnt32(uV1);
		iSumVec2 += PopCnt32(uV2);
		iDistance += PopCnt32( uV1 & uV2 );
	}

	return iDistance;
}


static float IP1BitFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iSumVec1 = 0;
	int iSumVec2 = 0;
	int iDotProduct = IP1Bit ( pVect1, pVect2, &(pDistFuncParam->m_uDim), iSumVec1, iSumVec2 );
	return 1.0f - pDistFuncParam->CalcIP ( iDotProduct, iSumVec1, iSumVec2 );
}

#if !defined(USE_SIMDE)
static FORCE_INLINE int IP1Bit8x ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pQty, int & iSumVec1, int & iSumVec2 )
{
	size_t uQty = *((size_t *)pQty);
	size_t uLenBytes = (uQty + 7) >> 3;
	size_t uQty8 = uLenBytes >> 3;

	auto pV1 = (uint64_t*)pVect1;
	auto pV2 = (uint64_t*)pVect2;

	int iRes = 0;
	for ( size_t i = 0; i < uQty8; i++ )
	{
		uint64_t uV1 = pV1[i];
		uint64_t uV2 = pV2[i];
		iSumVec1 += _mm_popcnt_u64(uV1);
		iSumVec2 += _mm_popcnt_u64(uV2);
		iRes += _mm_popcnt_u64 ( uV1 & uV2 );
	}

	return iRes;
}


static float IP1Bit8xFloatDistance ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iSumVec1 = 0;
	int iSumVec2 = 0;
	int iDotProduct = IP1Bit8x ( pVect1, pVect2, &(pDistFuncParam->m_uDim), iSumVec1, iSumVec2 );
	return 1.0f - pDistFuncParam->CalcIP ( iDotProduct, iSumVec1, iSumVec2 );
}


static float IP1Bit8xFloatResiduals ( const void * __restrict pVect1, const void * __restrict pVect2, const void * __restrict pParam )
{
	auto pDistFuncParam = (const DistFuncParamIP_t*)pParam;
	int iSumVec1 = 0;
	int iSumVec2 = 0;

	size_t uQty = pDistFuncParam->m_uDim;
	size_t uLenBytes = (uQty + 7) >> 3;
	size_t uQty8 = uLenBytes >> 3;
	size_t uLenBytes8 = uQty8 << 3;

	int iDotProduct = IP1Bit8x ( pVect1, pVect2, &uQty8, iSumVec1, iSumVec2 );

	auto pV1 = (uint8_t *)pVect1 + uLenBytes8;
	auto pV2 = (uint8_t *)pVect2 + uLenBytes8;
	size_t uQtyLeft = uQty - uQty8;
	iDotProduct += IP1Bit ( pV1, pV2, &uQtyLeft, iSumVec1, iSumVec2 );
	return 1.0f - pDistFuncParam->CalcIP ( iDotProduct, iSumVec1, iSumVec2 );
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

} // namespace knn
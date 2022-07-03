// Copyright (c) 2021-2022, Manticore Software LTD (https://manticoresearch.com)
// Copyright (c) Daniel Lemire, http://lemire.me/en/
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

#include "delta.h"

#if defined(USE_SIMDE)
	#define SIMDE_ENABLE_NATIVE_ALIASES 1
	#include <simde/x86/sse4.1.h>
#elif _MSC_VER
	#include <intrin.h>
#else
	#include <x86intrin.h>
#endif

#include "deltautil.h"

namespace util
{

template <class T>
static void Delta (T *data, const size_t size)
{
	for (size_t i = size - 1; i > 0; --i)
		data[i] -= data[i - 1];
}


void FastDeltaUnaligned ( uint32_t * pData, size_t iTotalQty )
{
	if ( iTotalQty < 5 )
	{
		Delta ( pData, iTotalQty ); // no SIMD
		return;
	}

	const size_t iQty4 = iTotalQty >> 2;
	__m128i * pCurr = reinterpret_cast<__m128i *>(pData);
	const __m128i *pEnd = pCurr + iQty4;

	__m128i last = _mm_setzero_si128();
	while ( pCurr < pEnd )
	{
		__m128i a0 = _mm_loadu_si128(pCurr);
		__m128i a1 = _mm_sub_epi32 ( a0, _mm_srli_si128(last, 12) );
		a1 = _mm_sub_epi32 ( a1, _mm_slli_si128(a0, 4) );
		last = a0;

		_mm_storeu_si128 ( pCurr++, a1 );
	}

	if ( ( iQty4 << 2 ) < iTotalQty )
	{
		uint32_t uLastVal = _mm_cvtsi128_si32 ( _mm_srli_si128(last, 12) );
		for ( size_t i = iQty4 << 2; i < iTotalQty; ++i )
		{
			uint32_t uNewVal = pData[i];
			pData[i] -= uLastVal;
			uLastVal = uNewVal;
		}
	}
}


template <class T>
static void InverseDelta ( T * pData, size_t iSize )
{
	if ( !iSize )
		return;

	size_t i = 1;
	for ( ; i < iSize - 1; i += 2 )
	{
		pData[i] += pData[i-1];
		pData[i+1] += pData[i];
	}

	for ( ; i != iSize; ++i )
		pData[i] += pData[i-1];
}


static void FastInverseDeltaUnaligned ( uint32_t * pData, const size_t iTotalQty )
{
	if ( iTotalQty < 5 )
	{
		InverseDelta ( pData, iTotalQty ); // no SIMD
		return;
	}

	const size_t iQty4 = iTotalQty >> 2;
	__m128i runningCount = _mm_setzero_si128();
	__m128i * pCurr = reinterpret_cast<__m128i *>(pData);
	const __m128i * pEnd = pCurr + iQty4;
	while (pCurr < pEnd)
	{
		__m128i a0 = _mm_loadu_si128(pCurr);
		__m128i a1 = _mm_add_epi32(_mm_slli_si128(a0, 8), a0);
		__m128i a2 = _mm_add_epi32(_mm_slli_si128(a1, 4), a1);
		a0 = _mm_add_epi32(a2, runningCount);
		runningCount = _mm_shuffle_epi32(a0, 0xFF);
		_mm_storeu_si128(pCurr++, a0);
	}

	for ( size_t i = iQty4 << 2; i < iTotalQty; ++i )
		pData[i] += pData[i-1];
}

template <typename T>
static void CalcDescDeltas ( T * pData, size_t tLength )
{
	// FIXME: move to SSE
	T tPrevValue = pData[0];
	T tCurValue = (T)0;
	for ( size_t i = 1; i < tLength; i++ )
	{
		tCurValue = pData[i];
		pData[i] = tPrevValue - tCurValue;
		tPrevValue = tCurValue;
	}
}


static void DeltaCalc ( uint32_t * pData, size_t tLength, bool bAsc )
{
	if ( bAsc )
		FastDeltaUnaligned ( pData, tLength );
	else
		CalcDescDeltas ( pData, tLength );
}


static void DeltaCalc ( uint64_t * pData, size_t tLength, bool bAsc )
{
	if ( bAsc )
		Delta ( pData, tLength );
	else
		CalcDescDeltas ( pData, tLength );
}


static inline void CalcInverseDelta64 ( uint64_t * pData, size_t tSize )
{
	if ( tSize & ( sizeof(__m128i)/sizeof(uint64_t) - 1 ) )
	{
		InverseDelta ( pData, tSize );
		return;
	}

	__m128i tRunningCount = _mm_setzero_si128();
	__m128i * pCurr = reinterpret_cast<__m128i *>(pData);
	const __m128i * pEnd = pCurr + ( tSize>>1 );
	while ( pCurr < pEnd )
	{
		__m128i a0 = _mm_loadu_si128(pCurr);
		__m128i a1 = _mm_add_epi64 ( _mm_slli_si128 ( a0, 8 ), a0 );
		a0 = _mm_add_epi64 ( a1, tRunningCount );
		tRunningCount = _mm_shuffle_epi32(a0, _MM_SHUFFLE(3,2,3,2));
		_mm_storeu_si128 ( pCurr++, a0 );
	}
}


template<typename T>
static inline void CalcInverseDescDeltas ( T * pData, size_t tSize )
{
	for ( size_t i = 1; i < tSize; i++ )
		pData[i] = pData[i-1] - pData[i];
}


static inline void CalcInverseDelta32Desc ( uint32_t * pData, size_t tSize )
{
	if ( tSize & ( sizeof(__m128i)/sizeof(uint32_t) - 1 ) )
	{
		CalcInverseDescDeltas ( pData, tSize );
		return;
	}

	__m128i tRunningCount = _mm_set1_epi32 ( *pData );
	*pData = 0;

	__m128i * pCurr = reinterpret_cast<__m128i *>(pData);
	const __m128i * pEnd = pCurr + ( tSize>>2 );
	while ( pCurr < pEnd )
	{
		__m128i a0 = _mm_loadu_si128(pCurr);
		__m128i a1 = _mm_add_epi32(_mm_slli_si128(a0, 8), a0);
		__m128i a2 = _mm_add_epi32(_mm_slli_si128(a1, 4), a1);
		a0 = _mm_sub_epi64 ( tRunningCount, a2 );
		tRunningCount = _mm_shuffle_epi32(a0, 0xFF);
		_mm_storeu_si128 ( pCurr++, a0 );
	}
}


static inline void CalcInverseDelta64Desc ( uint64_t * pData, size_t tSize )
{
	if ( tSize & ( sizeof(__m128i)/sizeof(uint64_t) - 1 ) )
	{
		CalcInverseDescDeltas ( pData, tSize );
		return;
	}

	__m128i tRunningCount = _mm_set1_epi64x ( *pData );
	*pData = 0;

	__m128i * pCurr = reinterpret_cast<__m128i *>(pData);
	const __m128i * pEnd = pCurr + ( tSize>>1 );
	while ( pCurr < pEnd )
	{
		__m128i a0 = _mm_loadu_si128(pCurr);
		__m128i a1 = _mm_add_epi64 ( _mm_slli_si128 ( a0, 8 ), a0 );
		a0 = _mm_sub_epi64 ( tRunningCount, a1 );
		tRunningCount = _mm_shuffle_epi32(a0, _MM_SHUFFLE(3,2,3,2));
		_mm_storeu_si128 ( pCurr++, a0 );
	}
}


void ComputeDeltas ( uint32_t * pData, int iLength, bool bAsc )
{
	DeltaCalc ( pData, iLength, bAsc );
}


void ComputeDeltas ( uint64_t * pData, int iLength, bool bAsc )
{
	DeltaCalc ( pData, iLength, bAsc );
}


void ComputeInverseDeltas ( Span_T<uint32_t> & dData, bool bAsc )
{
	if ( bAsc )
		FastInverseDeltaUnaligned ( dData.data(), dData.size() );
	else
		CalcInverseDelta32Desc ( dData.data(), dData.size() );
}


void ComputeInverseDeltas ( Span_T<uint64_t> & dData, bool bAsc )
{
	if ( bAsc )
		CalcInverseDelta64 ( dData.data(), dData.size() );
	else
		CalcInverseDelta64Desc ( dData.data(), dData.size() );
}


void ComputeInverseDeltas ( std::vector<uint32_t> & dData, bool bAsc )
{
	Span_T<uint32_t> tSpan(dData);
	ComputeInverseDeltas ( tSpan, bAsc );
}


void ComputeInverseDeltas ( std::vector<uint64_t> & dData, bool bAsc )
{
	Span_T<uint64_t> tSpan(dData);
	ComputeInverseDeltas ( tSpan, bAsc );
}

} // namespace util
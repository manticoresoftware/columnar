// Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
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

#include "buildertraits.h"

#if _WIN32
	#include "intrin.h"
#else
	#include <x86intrin.h>
#endif

#if _WIN32
	#pragma warning ( push )
	#pragma warning ( disable : 4267)
#endif

#include "deltautil.h"

//#include "vsencoding.h"
#include "simple16.h"
#include "simple9.h"
#include "simple9_rle.h"
#include "simple8b_rle.h"
//#include "optpfor.h"
//#include "simdoptpfor.h"
#include "fastpfor.h"
#include "simdfastpfor.h"
#include "variablebyte.h"
#include "compositecodec.h"
#include "pfor.h"
#include "simdpfor.h"
#include "pfor2008.h"
#include "simdbinarypacking.h"
#include "varintgb.h"
#include "simdvariablebyte.h"
#include "streamvariablebyte.h"
#include "simdgroupsimple.h"


#if _WIN32
	#pragma warning( pop )
#endif

namespace columnar
{

class IntCodec_c : public IntCodec_i
{
public:
			IntCodec_c ( const std::string & sCodec32, const std::string & szCodec64 );

	void	Encode ( const Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) override;
	void	Encode ( const Span_T<uint64_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) override;
	bool	Decode ( const Span_T<uint32_t> & dCompressed, std::vector<uint32_t> & dDecompressed ) override;
	bool	Decode ( const Span_T<uint32_t> & dCompressed, std::vector<uint64_t> & dDecompressed ) override;

private:
	std::unique_ptr<FastPForLib::IntegerCODEC> m_pCodec32;
	std::unique_ptr<FastPForLib::IntegerCODEC> m_pCodec64;

	template <typename T>
	FORCE_INLINE void	Encode ( const Span_T<T> & dUncompressed, std::vector<uint32_t> & dCompressed, FastPForLib::IntegerCODEC & tCodec );

	template <typename T>
	FORCE_INLINE bool	Decode ( const Span_T<uint32_t> & dCompressed, std::vector<T> & dDecompressed, FastPForLib::IntegerCODEC & tCodec );

	FastPForLib::IntegerCODEC *	CreateCodec ( const std::string & sName );
};


IntCodec_c::IntCodec_c ( const std::string & sCodec32, const std::string & sCodec64 )
	: m_pCodec32 ( CreateCodec(sCodec32) )
	, m_pCodec64 ( CreateCodec(sCodec64) )
{}


void IntCodec_c::Encode ( const Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed )
{
	Encode ( dUncompressed, dCompressed, *m_pCodec32 );
}


void IntCodec_c::Encode ( const Span_T<uint64_t> & dUncompressed, std::vector<uint32_t> & dCompressed )
{
	Encode ( dUncompressed, dCompressed, *m_pCodec64 );
}


bool IntCodec_c::Decode ( const Span_T<uint32_t> & dCompressed, std::vector<uint32_t> & dDecompressed )
{
	return Decode ( dCompressed, dDecompressed, *m_pCodec32 );
}


bool IntCodec_c::Decode ( const Span_T<uint32_t> & dCompressed, std::vector<uint64_t> & dDecompressed )
{
	return Decode ( dCompressed, dDecompressed, *m_pCodec64 );
}

template <typename T>
void IntCodec_c::Encode ( const Span_T<T> & dUncompressed, std::vector<uint32_t> & dCompressed, FastPForLib::IntegerCODEC & tCodec )
{
	const size_t EXTRA_GAP = 1024;
	dCompressed.resize ( dUncompressed.size() + EXTRA_GAP );
	size_t uCompressedSize = dCompressed.size();
	tCodec.encodeArray ( dUncompressed.data(), dUncompressed.size(), dCompressed.data(), uCompressedSize );
	dCompressed.resize(uCompressedSize);
}

template <typename T>
bool IntCodec_c::Decode ( const Span_T<uint32_t> & dCompressed, std::vector<T> & dDecompressed, FastPForLib::IntegerCODEC & tCodec )
{
	const int MAX_DECODED_SIZE = 1024;
	if ( dDecompressed.size()<MAX_DECODED_SIZE )
		dDecompressed.resize(MAX_DECODED_SIZE);

	size_t uDecompressedSize = dDecompressed.size();
	const uint32_t * pOut = tCodec.decodeArray ( dCompressed.data(), dCompressed.size(), dDecompressed.data(), uDecompressedSize );
	dDecompressed.resize(uDecompressedSize);

	return pOut-(const uint32_t*)dCompressed.data()==dCompressed.size();
}


FastPForLib::IntegerCODEC * IntCodec_c::CreateCodec ( const std::string & sName )
{
	using namespace FastPForLib;

	if ( sName=="fastbinarypacking8" )		return new CompositeCodec<FastBinaryPacking<8>, VariableByte>;
	if ( sName=="fastbinarypacking16" )		return new CompositeCodec<FastBinaryPacking<16>, VariableByte>;
	if ( sName=="fastbinarypacking32" )		return new CompositeCodec<FastBinaryPacking<32>, VariableByte>;
	if ( sName=="BP32" )					return new CompositeCodec<BP32, VariableByte>;
//	if ( sName=="vsencoding" )				return new vsencoding::VSEncodingBlocks(1U << 16);
	if ( sName=="fastpfor128" )				return new CompositeCodec<FastPFor<4>, VariableByte>;
	if ( sName=="fastpfor256" )				return new CompositeCodec<FastPFor<8>, VariableByte>;
	if ( sName=="simdfastpfor128" )			return new CompositeCodec<SIMDFastPFor<4>, VariableByte>;
	if ( sName=="simdfastpfor256" )			return new CompositeCodec<SIMDFastPFor<8>, VariableByte>;
	if ( sName=="simplepfor" )				return new CompositeCodec<SimplePFor<>, VariableByte>;
	if ( sName=="simdsimplepfor" )			return new CompositeCodec<SIMDSimplePFor<>, VariableByte>;
	if ( sName=="pfor" )					return new CompositeCodec<PFor, VariableByte>;
	if ( sName=="simdpfor" )				return new CompositeCodec<SIMDPFor, VariableByte>;
	if ( sName=="pfor2008" )				return new CompositeCodec<PFor2008, VariableByte>;
//	if ( sName=="simdnewpfor" )				return new CompositeCodec<SIMDNewPFor<4, Simple16<false>>, VariableByte>;
//	if ( sName=="newpfor" )					return new CompositeCodec<NewPFor<4, Simple16<false>>, VariableByte>;
//	if ( sName=="optpfor" )					return new CompositeCodec<OPTPFor<4, Simple16<false>>, VariableByte>;
//	if ( sName=="simdoptpfor" )				return new CompositeCodec<SIMDOPTPFor<4, Simple16<false>>, VariableByte>;
	if ( sName=="varint" )					return new VariableByte;
	if ( sName=="vbyte" )					return new VByte;
	if ( sName=="maskedvbyte" )				return new MaskedVByte;
	if ( sName=="streamvbyte" )				return new StreamVByte;
	if ( sName=="varintgb" )				return new VarIntGB<>;
	if ( sName=="simple16" )				return new Simple16<true>;
	if ( sName=="simple9" )					return new Simple9<true>;
	if ( sName=="simple9_rle" )				return new Simple9_RLE<true>;
	if ( sName=="simple8b" )				return new Simple8b<true>;
	if ( sName=="simple8b_rle" )			return new Simple8b_RLE<true>;
	if ( sName=="simdbinarypacking" )		return new CompositeCodec<SIMDBinaryPacking, VariableByte>;
	if ( sName=="simdgroupsimple" )			return new CompositeCodec<SIMDGroupSimple<false, false>, VariableByte>;
	if ( sName=="simdgroupsimple_ringbuf" )	return new CompositeCodec<SIMDGroupSimple<true, true>, VariableByte>;
	if ( sName=="copy" )					return new JustCopy;

	assert ( 0 && "Unknown integer codec" );
	return nullptr;
}

//////////////////////////////////////////////////////////////////////////

AttributeHeaderBuilder_c::AttributeHeaderBuilder_c ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType )
	: m_sName ( sName )
	, m_eType ( eType )
	, m_tSettings ( tSettings )
{}


bool AttributeHeaderBuilder_c::Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError )
{
	m_tSettings.Save(tWriter);

	tWriter.Write_string(m_sName);

	// store base offset to correct it later
	tBaseOffset = tWriter.GetPos();

	tWriter.Write_uint64 ( 0 ); // stub
	tWriter.Pack_uint32 ( (uint32_t)m_dBlocks.size() );
	int64_t tPrevOffset = 0;

	// no offset for 1st block
	for ( size_t i=1; i < m_dBlocks.size(); i++ )
	{
		tWriter.Pack_uint64 ( m_dBlocks[i]-tPrevOffset );
		tPrevOffset = m_dBlocks[i];
	}

	return !tWriter.IsError();
}

//////////////////////////////////////////////////////////////////////////

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
		FastPForLib::Delta::fastDelta ( pData, tLength );
	else
		CalcDescDeltas ( pData, tLength );
}


static void DeltaCalc ( uint64_t * pData, size_t tLength, bool bAsc )
{
	if ( bAsc )
		FastPForLib::Delta::delta ( pData, tLength );
	else
		CalcDescDeltas ( pData, tLength );
}


static inline void CalcInverseDelta64 ( uint64_t * pData, size_t tSize )
{
	if ( tSize & ( sizeof(__m128i)/sizeof(uint64_t) - 1 ) )
	{
		FastPForLib::Delta::inverseDelta ( pData, tSize );
		return;
	}

	__m128i tRunningCount = _mm_setzero_si128();
	__m128i * pCurr = reinterpret_cast<__m128i *>(pData);
	const __m128i * pEnd = pCurr + ( tSize>>1 );
	while ( pCurr < pEnd )
	{
		__m128i a0 = _mm_load_si128(pCurr);
		__m128i a1 = _mm_add_epi64 ( _mm_slli_si128 ( a0, 8 ), a0 );
		a0 = _mm_add_epi64 ( a1, tRunningCount );
		tRunningCount = _mm_shuffle_epi32(a0, _MM_SHUFFLE(3,2,3,2));
		_mm_store_si128 ( pCurr++, a0 );
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
		__m128i a0 = _mm_load_si128(pCurr);
		__m128i a1 = _mm_add_epi32(_mm_slli_si128(a0, 8), a0);
		__m128i a2 = _mm_add_epi32(_mm_slli_si128(a1, 4), a1);
		a0 = _mm_sub_epi64 ( tRunningCount, a2 );
		tRunningCount = _mm_shuffle_epi32(a0, 0xFF);
		_mm_store_si128 ( pCurr++, a0 );
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
		__m128i a0 = _mm_load_si128(pCurr);
		__m128i a1 = _mm_add_epi64 ( _mm_slli_si128 ( a0, 8 ), a0 );
		a0 = _mm_sub_epi64 ( tRunningCount, a1 );
		tRunningCount = _mm_shuffle_epi32(a0, _MM_SHUFFLE(3,2,3,2));
		_mm_store_si128 ( pCurr++, a0 );
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


void ComputeInverseDeltas ( std::vector<uint32_t> & dData, bool bAsc )
{
	if ( bAsc )
		FastPForLib::Delta::fastinverseDelta2 ( dData.data(), dData.size() );
	else
		CalcInverseDelta32Desc ( dData.data(), dData.size() );
}


void ComputeInverseDeltas ( std::vector<uint64_t> & dData, bool bAsc )
{
	if ( bAsc )
		CalcInverseDelta64 ( dData.data(), dData.size() );
	else
		CalcInverseDelta64Desc ( dData.data(), dData.size() );
}


void BitPack128 ( const std::vector<uint32_t> & dValues, std::vector<uint32_t> & dPacked, int iBits )
{
	assert ( dValues.size()==128 );
	FastPForLib::SIMD_fastpack_32 ( dValues.data(), (__m128i *)dPacked.data(), iBits );
}


void BitUnpack128 ( const std::vector<uint32_t> & dPacked, std::vector<uint32_t> & dValues, int iBits )
{
	assert ( dValues.size()==128 );
	FastPForLib::SIMD_fastunpack_32 ( (__m128i *)dPacked.data(), dValues.data(), iBits );
}


IntCodec_i * CreateIntCodec ( const std::string & sCodec32, const std::string & sCodec64 )
{
	return new IntCodec_c ( sCodec32, sCodec64 );
}

} // namespace columnar
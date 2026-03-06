// Copyright (c) 2020-2025, Manticore Software LTD (https://manticoresearch.com)
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

#include "codec.h"

#include <utility>

#if _WIN32
	#pragma warning ( push )
	#pragma warning ( disable : 4267 )
#endif

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
#include "util/delta.h"
#include "streamvbyte.h"
#include "streamvbytedelta.h"


#if _WIN32
	#pragma warning( pop )
#endif

namespace util
{

template <typename T>
static FORCE_INLINE size_t ReserveSpaceForDecoded ( util::SpanResizeable_T<T> & dDecompressed )
{
	const int MAX_DECODED_SIZE = 32768;
	if ( dDecompressed.size()<MAX_DECODED_SIZE )
		dDecompressed.resize(MAX_DECODED_SIZE);

	return dDecompressed.size();
}

template <typename T>
static FORCE_INLINE void Encode ( const util::Span_T<T> & dUncompressed, std::vector<uint32_t> & dCompressed, FastPForLib::IntegerCODEC & tCodec )
{
	const size_t EXTRA_GAP = 1024;
	dCompressed.resize ( dUncompressed.size()*sizeof(dUncompressed[0])/sizeof(dCompressed[0]) + EXTRA_GAP );
	size_t uCompressedSize = dCompressed.size();
	tCodec.encodeArray ( dUncompressed.data(), dUncompressed.size(), dCompressed.data(), uCompressedSize );
	dCompressed.resize(uCompressedSize);
}

template <typename T>
static FORCE_INLINE bool Decode ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<T> & dDecompressed, FastPForLib::IntegerCODEC & tCodec )
{
	size_t uDecompressedSize = ReserveSpaceForDecoded(dDecompressed);
	const uint32_t * pOut = tCodec.decodeArray ( dCompressed.data(), dCompressed.size(), dDecompressed.data(), uDecompressedSize );
	assert ( uDecompressedSize<=dDecompressed.size() );
	dDecompressed.resize(uDecompressedSize);

	return pOut-(const uint32_t*)dCompressed.data()==dCompressed.size();
}


FastPForLib::IntegerCODEC * CreateFastPFORCodec ( const std::string & sName )
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

class Int32FastPFORCodec_c
{
public:
		Int32FastPFORCodec_c ( const std::string & sCodec32 ) : m_pCodec32 ( CreateFastPFORCodec(sCodec32) ) {}

	FORCE_INLINE void	Encode ( const util::Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed )			{ util::Encode ( dUncompressed, dCompressed, *m_pCodec32 ); }
	FORCE_INLINE void	EncodeDelta ( util::Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed );
	FORCE_INLINE void	Decode ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint32_t> & dDecompressed )	{ util::Decode ( dCompressed, dDecompressed, *m_pCodec32 ); }
	FORCE_INLINE void	DecodeDelta ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint32_t> & dDecompressed );

private:
	std::unique_ptr<FastPForLib::IntegerCODEC> m_pCodec32;
};


void Int32FastPFORCodec_c::EncodeDelta ( util::Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed )
{
	dCompressed.resize(0);
	ComputeDeltas ( dUncompressed.data(), (int)dUncompressed.size(), true );
	util::Encode ( dUncompressed, dCompressed, *m_pCodec32 );
}


void Int32FastPFORCodec_c::DecodeDelta ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint32_t> & dDecompressed )
{
	util::Decode ( dCompressed, dDecompressed, *m_pCodec32 );
	ComputeInverseDeltasAsc ( dDecompressed );
}

//////////////////////////////////////////////////////////////////////////

class Int64FastPFORCodec_c
{
public:
		Int64FastPFORCodec_c ( const std::string & sCodec64 )  : m_pCodec64 ( CreateFastPFORCodec(sCodec64) ) {}

	FORCE_INLINE void	Encode ( const util::Span_T<uint64_t> & dUncompressed, std::vector<uint32_t> & dCompressed )			{ util::Encode ( dUncompressed, dCompressed, *m_pCodec64 ); }
	FORCE_INLINE void	EncodeDelta ( util::Span_T<uint64_t> & dUncompressed, std::vector<uint32_t> & dCompressed );
	FORCE_INLINE void	Decode ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint64_t> & dDecompressed ) { util::Decode ( dCompressed, dDecompressed, *m_pCodec64 ); }
	FORCE_INLINE void	DecodeDelta ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint64_t> & dDecompressed );

private:
	std::unique_ptr<FastPForLib::IntegerCODEC> m_pCodec64;
};


void Int64FastPFORCodec_c::EncodeDelta ( util::Span_T<uint64_t> & dUncompressed, std::vector<uint32_t> & dCompressed )
{
	ComputeDeltas ( dUncompressed.data(), (int)dUncompressed.size(), true );
	util::Encode ( dUncompressed, dCompressed, *m_pCodec64 );
}


void Int64FastPFORCodec_c::DecodeDelta ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint64_t> & dDecompressed )
{
	util::Decode ( dCompressed, dDecompressed, *m_pCodec64 );
	ComputeInverseDeltasAsc ( dDecompressed );
}

//////////////////////////////////////////////////////////////////////////

class Int32SVBCodec_c
{
public:
	Int32SVBCodec_c ( const std::string & sCodec )  {}

	FORCE_INLINE void Encode ( const util::Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed )
	{
		auto uNumValues = dUncompressed.size();
		dCompressed.resize ( ( streamvbyte_max_compressedbytes(uNumValues) + sizeof(uint32_t)-1 ) / sizeof(uint32_t) );
		size_t uBytesWritten = streamvbyte_encode ( dUncompressed.data(), uNumValues, (uint8_t*)dCompressed.data() );
		dCompressed.resize ( ( uBytesWritten + sizeof(uint32_t)-1 ) / sizeof(uint32_t) );
	}

	FORCE_INLINE void EncodeDelta ( const util::Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed )
	{
		auto uNumValues = dUncompressed.size();
		dCompressed.resize ( ( streamvbyte_max_compressedbytes(uNumValues) + sizeof(uint32_t)-1 ) / sizeof(uint32_t) );
		size_t uBytesWritten = streamvbyte_delta_encode ( dUncompressed.data(), uNumValues, (uint8_t*)dCompressed.data(), 0 );
		dCompressed.resize ( ( uBytesWritten + sizeof(uint32_t)-1 ) / sizeof(uint32_t) );
	}

	FORCE_INLINE void Decode ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint32_t> & dDecompressed )		{ streamvbyte_decode ( (const uint8_t*)dCompressed.data(), dDecompressed.data(), dDecompressed.size() ); }
	FORCE_INLINE void DecodeDelta ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint32_t> & dDecompressed )	{ streamvbyte_delta_decode ( (const uint8_t*)dCompressed.data(), dDecompressed.data(), dDecompressed.size(), 0 ); }
};

//////////////////////////////////////////////////////////////////////////

template<typename CODEC32, typename CODEC64>
class IntCodec_T : public IntCodec_i, public CODEC32, public CODEC64
{
public:
			IntCodec_T ( const std::string & sCodec32, const std::string & szCodec64 );

	void	Encode ( const util::Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) override	{ CODEC32::Encode ( dUncompressed, dCompressed ); }
	void	EncodeDelta ( util::Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) override	{ CODEC32::EncodeDelta ( dUncompressed, dCompressed ); }

	void	Encode ( const util::Span_T<uint64_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) override	{ CODEC64::Encode ( dUncompressed, dCompressed ); }
	void	EncodeDelta ( util::Span_T<uint64_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) override	{ CODEC64::EncodeDelta ( dUncompressed, dCompressed ); }

	void	Decode ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint32_t> & dDecompressed ) override		{ CODEC32::Decode ( dCompressed, dDecompressed ); }
	void	DecodeDelta ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint32_t> & dDecompressed ) override	{ CODEC32::DecodeDelta ( dCompressed, dDecompressed ); }

	void	Decode ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint64_t> & dDecompressed ) override		{ CODEC64::Decode ( dCompressed, dDecompressed ); }
	void	DecodeDelta ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint64_t> & dDecompressed ) override	{ CODEC64::DecodeDelta ( dCompressed, dDecompressed ); }
};

template<typename CODEC32, typename CODEC64>
IntCodec_T<CODEC32,CODEC64>::IntCodec_T( const std::string & sCodec32, const std::string & sCodec64 )
	: CODEC32(sCodec32)
	, CODEC64(sCodec64)
{}

//////////////////////////////////////////////////////////////////////////

void BitPack ( const std::vector<uint32_t> & dValues, std::vector<uint32_t> & dPacked, int iBits )
{
	assert ( !( dValues.size() & 127 ) );

	int iNumPacks = dValues.size()>>7;
	int iStep = iBits<<2;
	const uint32_t * pIn = &dValues[0];
	uint32_t * pOut = &dPacked[0];

	for ( int i = 0; i < iNumPacks; i++ )
	{
		FastPForLib::SIMD_fastpack_32 ( pIn, (__m128i *)pOut, iBits );
		pIn += 128;
		pOut += iStep;
	}
}


static FORCE_INLINE void BitUnpack ( const uint32_t * pIn, uint32_t * pOut, int iNumValues, int iBits )
{
	assert ( !(iNumValues & 127 ) );

	int iNumPacks = iNumValues>>7;
	int iStep = iBits<<2;

	for ( int i = 0; i < iNumPacks; i++ )
	{
		FastPForLib::SIMD_fastunpack_32 ( (__m128i *)pIn, pOut, iBits );
		pIn += iStep;
		pOut += 128;
	}
}


void BitUnpack ( const std::vector<uint32_t> & dPacked, std::vector<uint32_t> & dValues, int iBits )
{
	BitUnpack ( &dPacked[0], &dValues[0], (int)dValues.size(), iBits );
}


void BitUnpack ( const util::Span_T<uint32_t> & dPacked, util::Span_T<uint32_t> & dValues, int iBits )
{
	BitUnpack ( &dPacked[0], &dValues[0], (int)dValues.size(), iBits );
}

////////////////////////////////////////////////////////////////////////////////
class IntCodecPool_c
{
public:
	std::unique_ptr<IntCodec_i>	Acquire ( const std::string & sCodec32, const std::string & sCodec64 );
	void						Release ( const std::string & sCodec32, const std::string & sCodec64, std::unique_ptr<IntCodec_i> pCodec );
	static IntCodecPool_c &		Get();

private:
	struct ThreadPoolEntry_t
	{
		CodecKey_t m_tKey;
		std::vector<std::unique_ptr<IntCodec_i>> m_dCodecs;
	};

	static constexpr size_t MAX_CODECS_PER_PAIR = 128;
	static std::vector<ThreadPoolEntry_t> & GetThreadPool();
};


std::unique_ptr<IntCodec_i> IntCodecPool_c::Acquire ( const std::string & sCodec32, const std::string & sCodec64 )
{
	auto & dPool = GetThreadPool();
	for ( auto & tEntry : dPool )
	{
		if ( tEntry.m_tKey.first!=sCodec32 || tEntry.m_tKey.second!=sCodec64 )
			continue;

		if ( tEntry.m_dCodecs.empty() )
			return nullptr;

		std::unique_ptr<IntCodec_i> pCodec = std::move ( tEntry.m_dCodecs.back() );
		tEntry.m_dCodecs.pop_back();
		return pCodec;
	}

	return nullptr;
}


void IntCodecPool_c::Release ( const std::string & sCodec32, const std::string & sCodec64, std::unique_ptr<IntCodec_i> pCodec )
{
	if ( !pCodec )
		return;

	auto & dPool = GetThreadPool();
	for ( auto & tEntry : dPool )
	{
		if ( tEntry.m_tKey.first!=sCodec32 || tEntry.m_tKey.second!=sCodec64 )
			continue;

		if ( tEntry.m_dCodecs.size()<MAX_CODECS_PER_PAIR )
			tEntry.m_dCodecs.push_back ( std::move(pCodec) );
		return;
	}

	ThreadPoolEntry_t & tNew = dPool.emplace_back();
	tNew.m_tKey = { sCodec32, sCodec64 };
	tNew.m_dCodecs.reserve(MAX_CODECS_PER_PAIR);
	tNew.m_dCodecs.push_back ( std::move(pCodec) );
}


IntCodecPool_c & IntCodecPool_c::Get()
{
	static IntCodecPool_c tPool;
	return tPool;
}


std::vector<IntCodecPool_c::ThreadPoolEntry_t> & IntCodecPool_c::GetThreadPool()
{
	thread_local std::vector<ThreadPoolEntry_t> dPool;
	return dPool;
}

////////////////////////////////////////////////////////////////////////////////
void CodecPoolDeleter_t::operator() ( IntCodec_i * pCodec ) const
{
	if ( !pCodec )
		return;

	if ( !m_pKey )
	{
		delete pCodec;
		return;
	}

	IntCodecPool_c::Get().Release ( m_pKey->first, m_pKey->second, std::unique_ptr<IntCodec_i>(pCodec) );
}

IntCodecPooledPtr_t CreateIntCodec ( const std::string & sCodec32, const std::string & sCodec64 )
{
	std::unique_ptr<IntCodec_i> pCodec = IntCodecPool_c::Get().Acquire ( sCodec32, sCodec64 );
	if ( !pCodec )
	{
		if ( sCodec32=="libstreamvbyte" )
			pCodec = std::make_unique<IntCodec_T<Int32SVBCodec_c, Int64FastPFORCodec_c>> ( sCodec32, sCodec64 );
		else
			pCodec = std::make_unique<IntCodec_T<Int32FastPFORCodec_c, Int64FastPFORCodec_c>> ( sCodec32, sCodec64 );
	}

	return IntCodecPooledPtr_t ( pCodec.release(), CodecPoolDeleter_t ( sCodec32, sCodec64 ) );
}

std::shared_ptr<IntCodec_i> CreateIntCodecShared ( const std::string & sCodec32, const std::string & sCodec64 )
{
	auto pCodec = CreateIntCodec ( sCodec32, sCodec64 );
	auto tDeleter = pCodec.get_deleter();
	return std::shared_ptr<IntCodec_i> ( pCodec.release(), tDeleter );
}

} // namespace util

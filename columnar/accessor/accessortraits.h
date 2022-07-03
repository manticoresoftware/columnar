// Copyright (c) 2020-2022, Manticore Software LTD (https://manticoresearch.com)
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

#pragma once

#include "accessor.h"
#include "buildertraits.h"
#include "builderint.h"
#include "reader.h"
#include "delta.h"
#include <cassert>

#if defined(USE_SIMDE)
	#define SIMDE_ENABLE_NATIVE_ALIASES 1
	#include <simde/x86/sse4.1.h>
#elif _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace columnar
{

struct SubblockCalc_t
{
	int			m_iSubblockSize = 0;
	int			m_iSubblockShift = 0;
	int			m_iSubblocksPerBlock = 0;

				SubblockCalc_t ( int iSubblockSize );

	FORCE_INLINE int GetSubblockId ( uint32_t uIdInBlock ) const
	{
		return uIdInBlock >> m_iSubblockShift;
	}

	FORCE_INLINE int SubblockId2BlockId ( uint32_t uSubblockId ) const
	{
		return uSubblockId >> (BLOCK_ID_BITS-m_iSubblockShift);
	}

	FORCE_INLINE int SubblockId2RowId ( uint32_t uSubblockId ) const
	{
		return uSubblockId << m_iSubblockShift;
	}

	FORCE_INLINE int GetValueIdInSubblock ( uint32_t uIdInBlock ) const
	{
		return uIdInBlock & (m_iSubblockSize-1);
	}

	FORCE_INLINE int GetSubblockIdInBlock ( uint32_t uSubblockId ) const
	{
		return uSubblockId & (m_iSubblocksPerBlock-1);
	}
};


FORCE_INLINE uint32_t RowId2BlockId ( uint32_t tRowID )
{
	return tRowID >> BLOCK_ID_BITS;
}


FORCE_INLINE uint32_t BlockId2RowId ( uint32_t uBlockId )
{
	return uBlockId << BLOCK_ID_BITS;
}

const uint32_t INVALID_ROW_ID = 0xFFFFFFFF;
const uint32_t INVALID_BLOCK_ID = 0xFFFFFFFF;

struct StoredBlockTraits_t : SubblockCalc_t
{
	using SubblockCalc_t::SubblockCalc_t;

	uint32_t	m_tRequestedRowID = INVALID_ROW_ID;
	uint32_t	m_uBlockId = INVALID_BLOCK_ID;
	uint32_t	m_tStartBlockRowId = INVALID_ROW_ID;
	int			m_iNumSubblocks = 0;
	uint32_t	m_uNumDocsInBlock = 0;

	void		SetBlockId ( uint32_t uBlockId, uint32_t uNumDocsInBlock );

	FORCE_INLINE uint32_t GetNumSubblockValues ( int iSubblockId ) const
	{
		if ( m_uNumDocsInBlock==DOCS_PER_BLOCK )
			return m_iSubblockSize;

		if ( iSubblockId<m_iNumSubblocks-1 )
			return m_iSubblockSize;

		int iLeftover = m_uNumDocsInBlock & (m_iSubblockSize-1);
		return iLeftover ? iLeftover : m_iSubblockSize;
	}
};

// common traits of all columnar analyzers
template <bool HAVE_MATCHING_BLOCKS>
class Analyzer_T : public Analyzer_i
{
public:
				Analyzer_T ( int iSubblockSize );

	int64_t		GetNumProcessed() const final { return m_iNumProcessed; }
	void		Setup ( SharedBlocks_c & pBlocks, uint32_t uTotalDocs ) final;
	bool		HintRowID ( uint32_t tRowID ) final;

protected:
	int			m_iNumProcessed = 0;
	uint32_t	m_tRowID = INVALID_ROW_ID;
	int			m_iCurSubblock = 0;
	int			m_iCurBlockId = -1;
	int			m_iTotalSubblocks = 0;

	std::vector<uint32_t> m_dCollected {0};
	SharedBlocks_c		m_pMatchingSubblocks;

	SubblockCalc_t		m_tSubblockCalc;

	FORCE_INLINE bool	MoveToSubblock ( int iSubblock );
	virtual bool		MoveToBlock ( int iBlock ) = 0;

	template <typename ACCESSOR, typename PROCESSSUBBLOCK>
	FORCE_INLINE bool	GetNextRowIdBlock ( ACCESSOR & tAccessor, util::Span_T<uint32_t> & dRowIdBlock, PROCESSSUBBLOCK && fnProcessSubblock );

	template <typename ACCESSOR>
	FORCE_INLINE void	StartBlockProcessing ( ACCESSOR & tAccessor, int iNextBlock );

	template <typename ACCESSOR>
	FORCE_INLINE bool	RewindToNextBlock ( ACCESSOR & tAccessor, int & iNextBlock );
};

template <bool HAVE_MATCHING_BLOCKS>
Analyzer_T<HAVE_MATCHING_BLOCKS>::Analyzer_T ( int iSubblockSize )
	: m_tSubblockCalc ( iSubblockSize )
{
	m_dCollected.resize ( iSubblockSize*2 );
}

template <bool HAVE_MATCHING_BLOCKS>
void Analyzer_T<HAVE_MATCHING_BLOCKS>::Setup ( SharedBlocks_c & pBlocks, uint32_t uTotalDocs )
{
	if ( HAVE_MATCHING_BLOCKS )
	{
		m_pMatchingSubblocks = pBlocks;
		m_iTotalSubblocks = m_pMatchingSubblocks->GetNumBlocks();
	}
	else
		m_iTotalSubblocks = ( uTotalDocs+m_tSubblockCalc.m_iSubblockSize-1 ) / m_tSubblockCalc.m_iSubblockSize;

	// reject everything? signal the end
	if ( !MoveToSubblock(0) )
		m_iCurSubblock = m_iTotalSubblocks;
}

template <bool HAVE_MATCHING_BLOCKS>
bool Analyzer_T<HAVE_MATCHING_BLOCKS>::MoveToSubblock ( int iSubblock )
{
	m_iCurSubblock = iSubblock;

	if ( iSubblock>=m_iTotalSubblocks )
		return false;

	int iNextSubblockId;
	if ( HAVE_MATCHING_BLOCKS )
		iNextSubblockId = m_pMatchingSubblocks->GetBlock(m_iCurSubblock);
	else
		iNextSubblockId = m_iCurSubblock;

	int iNextBlock = m_tSubblockCalc.SubblockId2BlockId(iNextSubblockId);
	if ( iNextBlock==m_iCurBlockId )
	{
		m_tRowID = m_tSubblockCalc.SubblockId2RowId(iNextSubblockId);
		return true;
	}

	if ( !MoveToBlock ( iNextBlock ) )
		return false;

	if ( HAVE_MATCHING_BLOCKS )
		m_tRowID = m_tSubblockCalc.SubblockId2RowId ( m_pMatchingSubblocks->GetBlock(m_iCurSubblock) );
	else
		m_tRowID = m_tSubblockCalc.SubblockId2RowId(m_iCurSubblock);

	return true;
}

template <bool HAVE_MATCHING_BLOCKS>
bool Analyzer_T<HAVE_MATCHING_BLOCKS>::HintRowID ( uint32_t tRowID )
{
	int iNextSubblock = m_iCurSubblock;

	// we assume that we are only advancing forward
	while ( iNextSubblock<m_iTotalSubblocks )
	{
		int iSubblockID;
		if ( HAVE_MATCHING_BLOCKS )
			iSubblockID = m_pMatchingSubblocks->GetBlock(iNextSubblock);
		else
			iSubblockID = iNextSubblock;

		uint32_t tSubblockStart = m_tSubblockCalc.SubblockId2RowId(iSubblockID);
		uint32_t tSubblockEnd = tSubblockStart + m_tSubblockCalc.m_iSubblockSize;

		if ( tRowID<tSubblockStart || ( tRowID>=tSubblockStart && tRowID<tSubblockEnd ) )
		{
			if ( iNextSubblock!=m_iCurSubblock )
				return MoveToSubblock(iNextSubblock);

			return true;
		}

		iNextSubblock++;
	}

	return false;
}

template <bool HAVE_MATCHING_BLOCKS>
template <typename ACCESSOR, typename PROCESSSUBBLOCK>
bool Analyzer_T<HAVE_MATCHING_BLOCKS>::GetNextRowIdBlock ( ACCESSOR & tAccessor, util::Span_T<uint32_t> & dRowIdBlock, PROCESSSUBBLOCK && fnProcessSubblock )
{
	if ( m_iCurSubblock>=m_iTotalSubblocks )
		return false;

	uint32_t * pRowIdStart = m_dCollected.data();
	uint32_t * pRowID = pRowIdStart;
	uint32_t * pRowIdMax = pRowIdStart + tAccessor.m_iSubblockSize;

	// we scan until we find at least 128 (subblock size) matches.
	// this might lead to this analyzer scanning the whole index
	// a more responsive version would return after processing each 128 docs
	// (even if it doesn't find any matches)
	while ( pRowID<pRowIdMax )
	{
		int iSubblockIdInBlock;
		if ( HAVE_MATCHING_BLOCKS )
			iSubblockIdInBlock = tAccessor.GetSubblockIdInBlock ( m_pMatchingSubblocks->GetBlock(m_iCurSubblock) );
		else
			iSubblockIdInBlock = tAccessor.GetSubblockIdInBlock ( m_iCurSubblock );

		m_iNumProcessed += fnProcessSubblock ( pRowID, iSubblockIdInBlock );

		if ( !MoveToSubblock ( m_iCurSubblock+1 ) )
			break;
	}

	return CheckEmptySpan ( pRowID, pRowIdStart, dRowIdBlock );
}

template <bool HAVE_MATCHING_BLOCKS>
template <typename ACCESSOR>
void Analyzer_T<HAVE_MATCHING_BLOCKS>::StartBlockProcessing ( ACCESSOR & tAccessor, int iNextBlock )
{
	m_iCurBlockId = iNextBlock;
	tAccessor.SetCurBlock ( m_iCurBlockId );
}

template <bool HAVE_MATCHING_BLOCKS>
template <typename ACCESSOR>
bool Analyzer_T<HAVE_MATCHING_BLOCKS>::RewindToNextBlock ( ACCESSOR & tAccessor, int & iNextBlock )
{
	if ( !HAVE_MATCHING_BLOCKS )
	{
		iNextBlock = m_iCurBlockId+1;
		m_iCurSubblock = tAccessor.GetSubblockId ( BlockId2RowId(iNextBlock) );
		return m_iCurSubblock<m_iTotalSubblocks;
	}

	while ( iNextBlock==m_iCurBlockId && m_iCurSubblock<m_iTotalSubblocks )
		iNextBlock = tAccessor.SubblockId2BlockId ( m_pMatchingSubblocks->GetBlock ( m_iCurSubblock++ ) );

	if ( iNextBlock!=m_iCurBlockId )
	{
		m_iCurSubblock--;
		return true;
	}

	return false;
}


FORCE_INLINE void AddMinValue ( util::Span_T<uint32_t> & dValues, uint32_t uMin )
{
	int nValues = (int)dValues.size();
	if ( nValues & ( sizeof(__m128i)/sizeof(uint32_t) - 1 ) )
	{
		for ( auto & i : dValues )
			i += uMin;
	}
	else
	{
		uint32_t dMins[4] = { uMin, uMin, uMin, uMin };
		__m128i tMin = _mm_loadu_si128 ( reinterpret_cast<__m128i *>(&dMins[0]) );
		__m128i * pStart = reinterpret_cast<__m128i *>( dValues.data() );
		__m128i * pEnd = reinterpret_cast<__m128i *>( dValues.data()+nValues );
		while ( pStart < pEnd )
		{
			__m128i tLength = _mm_loadu_si128(pStart);
			_mm_store_si128 ( pStart, _mm_add_epi32(tLength, tMin) );
			pStart++;
		}
	}
}


FORCE_INLINE void AddMinValue ( util::Span_T<uint64_t> & dValues, uint64_t uMin )
{
	int nValues = (int)dValues.size();
	if ( nValues & ( sizeof(__m128i)/sizeof(uint64_t) - 1 ) )
	{
		for ( auto & i : dValues )
			i += uMin;
	}
	else
	{
		uint64_t dMins[2] = { uMin, uMin };
		__m128i tMin = _mm_loadu_si128 ( reinterpret_cast<__m128i *>(&dMins[0]) );
		__m128i * pStart = reinterpret_cast<__m128i *>( dValues.data() );
		__m128i * pEnd = reinterpret_cast<__m128i *>( dValues.data()+nValues );
		while ( pStart < pEnd )
		{
			__m128i tLength = _mm_loadu_si128(pStart);
			_mm_store_si128 ( pStart, _mm_add_epi64(tLength, tMin) );
			pStart++;
		}
	}
}


template <typename T>
FORCE_INLINE void DecodeValues_Delta_PFOR ( util::SpanResizeable_T<T> & dValues, util::FileReader_c & tReader, util::IntCodec_i & tCodec, util::SpanResizeable_T<uint32_t> & dTmp, uint32_t uTotalSize, bool bReadFlag )
{
	int64_t tStart = tReader.GetPos();
	uint8_t uFlags = util::to_underlying ( IntDeltaPacking_e::DELTA_ASC );
	if ( bReadFlag )
		uFlags = tReader.Read_uint8();

	T uMin = (T)tReader.Unpack_uint64();
	uint32_t uPFOREncodedSize = uint32_t ( uTotalSize - ( tReader.GetPos() - tStart ) );
	assert ( uPFOREncodedSize % 4 == 0 );

	dTmp.resize ( uPFOREncodedSize>>2 );
	tReader.Read ( (uint8_t*)dTmp.data(), (int)dTmp.size()*sizeof(dTmp[0]) );

	tCodec.Decode ( dTmp, dValues );

	assert ( !dValues[0] );
	dValues[0] = uMin;
	ComputeInverseDeltas ( dValues, uFlags==util::to_underlying ( IntDeltaPacking_e::DELTA_ASC ) );
}

template <typename T>
FORCE_INLINE void DecodeValues_PFOR ( util::SpanResizeable_T<T> & dValues, util::FileReader_c & tReader, util::IntCodec_i & tCodec, util::SpanResizeable_T<uint32_t> & dTmp, uint32_t uTotalSize )
{
	int64_t tStart = tReader.GetPos();
	T uMin = (T)tReader.Unpack_uint64();
	uint32_t uPFOREncodedSize = uint32_t ( uTotalSize - ( tReader.GetPos() - tStart ) );
	assert ( uPFOREncodedSize % 4 == 0 );

	dTmp.resize ( uPFOREncodedSize>>2 );
	tReader.Read ( (uint8_t*)dTmp.data(), (int)dTmp.size()*sizeof(dTmp[0]) );

	tCodec.Decode ( dTmp, dValues );

	AddMinValue ( dValues, uMin );
}

template <typename T, bool PACK>
FORCE_INLINE uint32_t PackValue ( const util::Span_T<T> & dValue, uint8_t * & pValue )
{
	if ( PACK )
		pValue = util::ByteCodec_c::PackData(dValue);
	else
		pValue = (uint8_t*)dValue.data();

	return uint32_t ( dValue.size()*sizeof(T) );
}

} // namespace columnar

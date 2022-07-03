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

#include "iterator.h"
#include "secondary.h"
#include "delta.h"
#include "interval.h"

namespace SI
{

using namespace util;
using namespace common;

template <bool ROWID_RANGE>
class RowidIterator_T : public BlockIterator_i
{
public:
				RowidIterator_T ( const std::string & sAttr, Packing_e eType, uint64_t uStartOffset, uint32_t uMinRowID, uint32_t uMaxRowID, std::shared_ptr<FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t * pBounds=nullptr );

	bool		HintRowID ( uint32_t tRowID ) override;
	bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) override;
	int64_t		GetNumProcessed() const override { return 0; }

	void		AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const override { dDesc.push_back ( { m_sAttr, "secondary" } ); }

private:
	std::string			m_sAttr;
	Packing_e			m_eType = Packing_e::TOTAL;
	uint64_t			m_uRowStart = 0;
	std::shared_ptr<FileReader_c> m_pReader { nullptr };
	std::shared_ptr<IntCodec_i>	m_pCodec { nullptr };
	uint32_t			m_uMinRowID = 0;
	uint32_t			m_uMaxRowID = 0;
	int64_t				m_iMetaOffset = 0;
	int64_t				m_iDataOffset = 0;
	RowidRange_t		m_tBounds;

	bool				m_bStarted = false;
	bool				m_bStopped = false;
	bool				m_bNeedToRewind = true;

	int					m_iCurBlock = 0;
	SpanResizeable_T<uint32_t>	m_dRows;
	SpanResizeable_T<uint32_t>	m_dMinMax;
	SpanResizeable_T<uint32_t>	m_dBlockOffsets;
	SpanResizeable_T<uint32_t>	m_dTmp;
	BitVec_T<uint64_t>	m_dMatchingBlocks{0};

	bool		StartBlock ( Span_T<uint32_t> & dRowIdBlock );
	bool		ReadNextBlock ( Span_T<uint32_t> & dRowIdBlock );

	FORCE_INLINE void DecodeDeltaVector ( SpanResizeable_T<uint32_t> & dDecoded, int iRsetSize=0 );
	uint32_t	MarkMatchingBlocks();
	bool		RewindToNextMatchingBlock();
};

template <bool ROWID_RANGE>
RowidIterator_T<ROWID_RANGE>::RowidIterator_T ( const std::string & sAttr, Packing_e eType, uint64_t uStartOffset, uint32_t uMinRowID, uint32_t uMaxRowID, std::shared_ptr<FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t * pBounds )
	: m_sAttr ( sAttr )
	, m_eType ( eType )
	, m_uRowStart ( uStartOffset )
	, m_pReader ( pReader )
	, m_pCodec ( pCodec )
	, m_uMinRowID ( uMinRowID )
	, m_uMaxRowID ( uMaxRowID )
{
	if ( pBounds )
		m_tBounds = *pBounds;

	if ( eType!=Packing_e::ROW )
		m_iMetaOffset = m_pReader->GetPos();
}

template <bool ROWID_RANGE>
bool RowidIterator_T<ROWID_RANGE>::HintRowID ( uint32_t tRowID )
{
	if ( !m_bStarted )	return true;
	if ( m_bStopped )	return false;

	switch ( m_eType )
	{
	case Packing_e::ROW:		return tRowID<=m_dRows[0];
	case Packing_e::ROW_BLOCK:	return tRowID<=m_uMaxRowID;
	case Packing_e::ROW_BLOCKS_LIST:
		if ( tRowID<=m_uMinRowID )
			return true;

		if ( tRowID>m_uMaxRowID )
		{
			m_bStopped = true;
			return false;
		}

		do
		{
			if ( tRowID<=m_dMinMax[(m_iCurBlock<<1)+1] )
			{
				m_bNeedToRewind = false;
				return true;
			}
		}
		while ( RewindToNextMatchingBlock() );
		return false;

	default:
		assert ( 0 && "Unknown block encoding" );
		return false;
	}
}

template <bool ROWID_RANGE>
bool RowidIterator_T<ROWID_RANGE>::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	if ( m_bStopped )
		return false;

	if ( !m_bStarted )
		return StartBlock ( dRowIdBlock );

	if ( m_bNeedToRewind && !RewindToNextMatchingBlock() )
		return false;

	m_bNeedToRewind = true;
	return ReadNextBlock ( dRowIdBlock );
}

template<>
uint32_t RowidIterator_T<false>::MarkMatchingBlocks()
{
	m_dMatchingBlocks.Resize ( m_dBlockOffsets.size() );
	m_dMatchingBlocks.SetAllBits();
	return m_dBlockOffsets.size();
}

template<>
uint32_t RowidIterator_T<true>::MarkMatchingBlocks()
{
	uint32_t uSet = 0;
	m_dMatchingBlocks.Resize ( m_dBlockOffsets.size() );
	Interval_T<uint32_t> tRowidBounds ( m_tBounds.m_uMin, m_tBounds.m_uMax );
	for ( size_t i = 0; i < m_dBlockOffsets.size(); i++ )
		if ( tRowidBounds.Overlaps ( { m_dMinMax[i<<1], m_dMinMax[(i<<1)+1] } ) )
		{
			m_dMatchingBlocks.BitSet(i);
			uSet++;
		}

	return uSet;
}

template <bool ROWID_RANGE>
bool RowidIterator_T<ROWID_RANGE>::StartBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	m_bStarted = true;
	switch ( m_eType )
	{
	case Packing_e::ROW:
		m_bStopped = true;

		// no range checks here; they are done before creating the iterator
		m_dRows.resize(1);
		m_dRows[0] = m_uMinRowID;	// min==max==value
		break;

	case Packing_e::ROW_BLOCK:
		m_pReader->Seek ( m_iMetaOffset + m_uRowStart );
		m_bStopped = true;
		DecodeDeltaVector ( m_dRows );
		break;

	case Packing_e::ROW_BLOCKS_LIST:
	{
		m_pReader->Seek ( m_iMetaOffset + m_uRowStart );
		int iBlocks = m_pReader->Unpack_uint32();
		DecodeDeltaVector ( m_dMinMax, iBlocks*2 );
		DecodeDeltaVector ( m_dBlockOffsets, iBlocks );
		m_iDataOffset = m_pReader->GetPos();

		if ( !MarkMatchingBlocks() )
		{
			m_bStopped = true;
			return false;
		}

		m_iCurBlock = 0;
		return ReadNextBlock(dRowIdBlock);
	}

	default:
		m_bStopped = true;
		break;
	}

	dRowIdBlock = Span_T<uint32_t> ( m_dRows );
	return ( !dRowIdBlock.empty() );
}

template <bool ROWID_RANGE>
bool RowidIterator_T<ROWID_RANGE>::RewindToNextMatchingBlock()
{
	m_iCurBlock++;
	if ( m_iCurBlock>=m_dMatchingBlocks.GetLength() )
	{
		m_bStopped = true;
		return false;
	}

	m_iCurBlock = m_dMatchingBlocks.Scan(m_iCurBlock);
	if ( m_iCurBlock>=m_dMatchingBlocks.GetLength() )
	{
		m_bStopped = true;
		return false;
	}

	return true;
}

template <bool ROWID_RANGE>
bool RowidIterator_T<ROWID_RANGE>::ReadNextBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	assert ( m_bStarted && !m_bStopped );
	assert ( m_eType==Packing_e::ROW_BLOCKS_LIST );

	int64_t iBlockSize = m_dBlockOffsets[m_iCurBlock];
	int64_t iBlockOffset = m_iCurBlock ? ( m_dBlockOffsets[m_iCurBlock-1]): 0;
	iBlockSize -= iBlockOffset;

	m_pReader->Seek ( m_iDataOffset + ( iBlockOffset << 2 ) );

	m_dTmp.resize(iBlockSize);
	ReadVectorData ( m_dTmp, *m_pReader );
	m_pCodec->Decode ( m_dTmp, m_dRows );
	ComputeInverseDeltas ( m_dRows, true );

	dRowIdBlock = Span_T<uint32_t>(m_dRows);
	return ( !dRowIdBlock.empty() );
}

template <bool ROWID_RANGE>
void RowidIterator_T<ROWID_RANGE>::DecodeDeltaVector ( SpanResizeable_T<uint32_t> & dDecoded, int iRsetSize )
{
	dDecoded.resize(iRsetSize);
	m_dTmp.resize(0);
	ReadVectorLen32 ( m_dTmp, *m_pReader );
	m_pCodec->Decode ( m_dTmp, dDecoded );
	ComputeInverseDeltas ( dDecoded, true );
}

/////////////////////////////////////////////////////////////////////

BlockIterator_i * CreateRowidIterator ( const std::string & sAttr, Packing_e eType, uint64_t uStartOffset, uint32_t uMinRowID, uint32_t uMaxRowID, std::shared_ptr<FileReader_c> & pSharedReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t * pBounds )
{
	static const int BLOCK_READER_BUFFER = 4096;

	std::shared_ptr<FileReader_c> pReader;
	switch ( eType )
	{
	case Packing_e::ROW_BLOCK:
		pReader = pSharedReader;
		break;

	case Packing_e::ROW_BLOCKS_LIST:
		pReader = std::make_shared<FileReader_c>( pSharedReader->GetFD(), BLOCK_READER_BUFFER );
		pReader->Seek ( pSharedReader->GetPos() );
		break;

	default:
		break;
	}

	if ( pBounds )
	{
		Interval_T<uint32_t> tRowidBounds ( pBounds->m_uMin, pBounds->m_uMax );
		if ( !tRowidBounds.Overlaps ( { uMinRowID, uMaxRowID } ) )
			return nullptr;

		return new RowidIterator_T<true> ( sAttr, eType, uStartOffset, uMinRowID, uMaxRowID, pReader, pCodec, pBounds );
	}

	return new RowidIterator_T<false> ( sAttr, eType, uStartOffset, uMinRowID, uMaxRowID, pReader, pCodec );
}

}
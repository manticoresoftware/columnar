// Copyright (c) 2020-2024, Manticore Software LTD (https://manticoresearch.com)
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
#include "bitvec.h"

namespace SI
{

using namespace util;
using namespace common;

template <bool ROWID_RANGE>
class RowidIterator_T : public BlockIteratorWithSetup_i
{
public:
				RowidIterator_T ( const std::string & sAttr, Packing_e eType, int64_t iStartOffset, uint32_t uMinRowID, uint32_t uMaxRowID, uint32_t uCount, uint32_t uRowidsPerBlock, std::shared_ptr<FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t * pBounds, bool bBitmap );

	bool		HintRowID ( uint32_t tRowID ) override;
	bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) override;
	int64_t		GetNumProcessed() const override { return 0; }

	void		AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const override { dDesc.push_back ( { m_sAttr, "SecondaryIndex" } ); }

	void		SetCutoff ( int iCutoff ) override {}
	bool		WasCutoffHit() const override { return false; }

	void		Setup ( Packing_e eType, uint64_t uStartOffset, uint32_t uMinRowID, uint32_t uMaxRowID, uint32_t uCount ) override;

private:
	std::string			m_sAttr;
	Packing_e			m_eType = Packing_e::TOTAL;
	int64_t				m_iStartOffset = 0;
	std::shared_ptr<FileReader_c> m_pReader { nullptr };
	std::shared_ptr<IntCodec_i>	m_pCodec { nullptr };
	uint32_t			m_uMinRowID = 0;
	uint32_t			m_uMaxRowID = 0;
	uint32_t			m_uCount = 0;
	uint32_t			m_uRowidsPerBlock = 0;
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

	bool				StartBlock ( Span_T<uint32_t> & dRowIdBlock );
	FORCE_INLINE bool	ReadNextBlock ( Span_T<uint32_t> & dRowIdBlock );

	FORCE_INLINE void	DecodeDeltaVector ( SpanResizeable_T<uint32_t> & dDecoded, int iRsetSize );
	uint32_t			MarkMatchingBlocks();
	bool				RewindToNextMatchingBlock();
	FORCE_INLINE uint32_t CalcNumBlockRowids() const;
};

template <bool ROWID_RANGE>
RowidIterator_T<ROWID_RANGE>::RowidIterator_T ( const std::string & sAttr, Packing_e eType, int64_t iStartOffset, uint32_t uMinRowID, uint32_t uMaxRowID, uint32_t uCount, uint32_t uRowidsPerBlock, std::shared_ptr<FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t * pBounds, bool bBitmap )
	: m_sAttr ( sAttr )
	, m_eType ( eType )
	, m_iStartOffset ( iStartOffset )
	, m_pCodec ( pCodec )
	, m_uMinRowID ( uMinRowID )
	, m_uMaxRowID ( uMaxRowID )
	, m_uCount ( uCount )
	, m_uRowidsPerBlock ( uRowidsPerBlock )
{
	if ( pBounds )
		m_tBounds = *pBounds;

	// use shared reader for all operations if working with a bitmap iterator
	// otherwise clone a new reader
	if ( !bBitmap && eType==Packing_e::ROW_BLOCKS_LIST )
		m_pReader = std::make_shared<FileReader_c>( pReader->GetFD(), pReader->GetBufferSize() );
	else
		m_pReader = pReader;
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
		{
			if ( tRowID<=m_uMinRowID )
				return true;

			if ( tRowID>m_uMaxRowID )
			{
				m_bStopped = true;
				return false;
			}

			int iOldBlock = m_iCurBlock;
			do
			{
				if ( tRowID<=m_dMinMax[(m_iCurBlock<<1)+1] )
				{
					if ( m_iCurBlock!=iOldBlock )
						m_bNeedToRewind = false; // reset flag only if we moved to a new block

					return true;
				}
			}
			while ( RewindToNextMatchingBlock() );
		}
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
	m_iCurBlock = 0;
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
			if ( !uSet )
				m_iCurBlock = i;

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
		// no range checks here; they are done before creating the iterator
		m_bStopped = true;
		dRowIdBlock = Span_T<uint32_t> ( &m_uMinRowID, 1 ); // min==max==value
		return true;

	case Packing_e::ROW_BLOCK:
		m_pReader->Seek ( m_iStartOffset );
		m_bStopped = true;
		DecodeDeltaVector ( m_dRows, m_uCount );
		break;

	case Packing_e::ROW_BLOCKS_LIST:
	{
		m_pReader->Seek ( m_iStartOffset );
		int iBlocks = m_pReader->Unpack_uint32();
		DecodeDeltaVector ( m_dMinMax, iBlocks*2 );
		DecodeDeltaVector ( m_dBlockOffsets, iBlocks );
		m_iDataOffset = m_pReader->GetPos();

		if ( !MarkMatchingBlocks() )
		{
			m_bStopped = true;
			return false;
		}

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
	m_dRows.resize ( CalcNumBlockRowids() );
	m_pCodec->DecodeDelta ( m_dTmp, m_dRows );

	dRowIdBlock = Span_T<uint32_t>(m_dRows);
	return ( !dRowIdBlock.empty() );
}

template <bool ROWID_RANGE>
void RowidIterator_T<ROWID_RANGE>::DecodeDeltaVector ( SpanResizeable_T<uint32_t> & dDecoded, int iRsetSize )
{
	dDecoded.resize(iRsetSize);
	ReadVectorLen32 ( m_dTmp, *m_pReader );
	m_pCodec->DecodeDelta ( m_dTmp, dDecoded );
}

template <bool ROWID_RANGE>
uint32_t RowidIterator_T<ROWID_RANGE>::CalcNumBlockRowids() const
{
	if ( m_iCurBlock < (int)m_dBlockOffsets.size()-1 )
		return m_uRowidsPerBlock;

	uint32_t uLeftover = m_uCount % m_uRowidsPerBlock;
	return uLeftover ? uLeftover : m_uRowidsPerBlock;
}

template <bool ROWID_RANGE>
void RowidIterator_T<ROWID_RANGE>::Setup ( Packing_e eType, uint64_t uStartOffset, uint32_t uMinRowID, uint32_t uMaxRowID, uint32_t uCount )
{
	m_eType = eType;
	m_iStartOffset = uStartOffset;

	m_uMinRowID = uMinRowID;
	m_uMaxRowID = uMaxRowID;
	m_uCount = uCount;

	m_iDataOffset = 0;

	m_bStarted = false;
	m_bStopped = false;
	m_bNeedToRewind = true;

	m_iCurBlock = 0;
	m_dRows.resize(0);
	m_dMinMax.resize(0);
	m_dBlockOffsets.resize(0);
	m_dTmp.resize(0);

	m_dMatchingBlocks.Resize(0);
}

/////////////////////////////////////////////////////////////////////

BlockIteratorWithSetup_i * CreateRowidIterator ( const std::string & sAttr, Packing_e eType, uint64_t uStartOffset, uint32_t uMinRowID, uint32_t uMaxRowID, uint32_t uCount, uint32_t uRowidsPerBlock, std::shared_ptr<FileReader_c> & pSharedReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t * pBounds, bool bBitmap )
{
	if ( pBounds )
	{
		Interval_T<uint32_t> tRowidBounds ( pBounds->m_uMin, pBounds->m_uMax );
		if ( !tRowidBounds.Overlaps ( { uMinRowID, uMaxRowID } ) )
			return nullptr;

		return new RowidIterator_T<true> ( sAttr, eType, uStartOffset, uMinRowID, uMaxRowID, uCount, uRowidsPerBlock, pSharedReader, pCodec, pBounds, bBitmap );
	}

	return new RowidIterator_T<false> ( sAttr, eType, uStartOffset, uMinRowID, uMaxRowID, uCount, uRowidsPerBlock, pSharedReader, pCodec, nullptr, bBitmap );
}


bool SetupRowidIterator ( BlockIteratorWithSetup_i * pIterator, Packing_e eType, uint64_t uStartOffset, uint32_t uMinRowID, uint32_t uMaxRowID, uint32_t uCount, const RowidRange_t * pBounds )
{
	assert(pIterator);

	if ( pBounds )
	{
		Interval_T<uint32_t> tRowidBounds ( pBounds->m_uMin, pBounds->m_uMax );
		if ( !tRowidBounds.Overlaps ( { uMinRowID, uMaxRowID } ) )
			return false;
	}

	pIterator->Setup ( eType, uStartOffset, uMinRowID, uMaxRowID, uCount );
	return true;
}

}
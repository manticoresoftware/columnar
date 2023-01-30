// Copyright (c) 2020-2023, Manticore Software LTD (https://manticoresearch.com)
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

#include "blockreader.h"
#include "secondary.h"

#include "pgm.h"
#include "iterator.h"
#include "delta.h"
#include "interval.h"
#include "bitvec.h"

#include <functional>

namespace SI
{

static const int READER_BUFFER_SIZE = 1024;

using namespace util;
using namespace common;

static uint64_t GetValueBlock ( uint64_t uPos, int iValuesPerBlock )
{
	return uPos / iValuesPerBlock;
}

BlockIter_t::BlockIter_t ( const ApproxPos_t & tFrom, uint64_t uVal, uint64_t uBlocksCount, int iValuesPerBlock )
	: m_uVal ( uVal )
{
	m_iStart = GetValueBlock ( tFrom.m_iLo, iValuesPerBlock );
	m_iPos = GetValueBlock ( tFrom.m_iPos, iValuesPerBlock ) - m_iStart;
	m_iLast = GetValueBlock ( tFrom.m_iHi, iValuesPerBlock );

	if ( m_iStart+m_iPos>=uBlocksCount )
		m_iPos = uBlocksCount - 1 - m_iStart;
	if ( m_iLast>=uBlocksCount )
		m_iLast = uBlocksCount - m_iStart;
}

struct FindValueResult_t
{
	int m_iMatchedItem { -1 };
	int m_iCmp { 0 };
};

/////////////////////////////////////////////////////////////////////
class SplitBitmap_c
{
public:
				SplitBitmap_c ( uint32_t uNumValues );

	inline void BitSet ( int iBit );
	inline int	Scan ( int iStart );
	inline int	GetLength() const { return m_iSize; }

	template <typename RESULT>
	inline void	Fetch ( int & iIterator, int iBase, RESULT * & pRes, RESULT * pMax );

private:
	using BITMAP_TYPE = BitVec_T<uint64_t>;
	const int	BITMAP_BITS = 16;
	const int	VALUES_PER_BITMAP = 1 << BITMAP_BITS;

	std::vector<std::unique_ptr<BITMAP_TYPE>> m_dBitmaps;
	int			m_iSize = 0;
};


SplitBitmap_c::SplitBitmap_c ( uint32_t uNumValues )
	: m_iSize ( uNumValues )
{
	m_dBitmaps.resize ( ( uNumValues+VALUES_PER_BITMAP-1 ) >> BITMAP_BITS );
}


void SplitBitmap_c::BitSet ( int iBit )
{
	int iBitmap = iBit >> BITMAP_BITS;
	auto & pBitmap = m_dBitmaps[iBitmap];
	if ( !pBitmap )
		pBitmap = std::make_unique<BITMAP_TYPE>(VALUES_PER_BITMAP);

	pBitmap->BitSet( iBit - ( iBitmap << BITMAP_BITS ) );
}


int SplitBitmap_c::Scan ( int iStart )
{
	if ( iStart>=m_iSize )
		return m_iSize;

	int iBitmap = iStart >> BITMAP_BITS;
	auto & pBitmap = m_dBitmaps[iBitmap];
	if ( !pBitmap )
	{
		iBitmap++;
		while ( iBitmap<m_dBitmaps.size() && !m_dBitmaps[iBitmap] )
			iBitmap++;

		if ( iBitmap==m_dBitmaps.size() )
			return m_iSize;

		// since this bitmap exists, its guaranteed to have set bits
		return m_dBitmaps[iBitmap]->Scan(0) + ( iBitmap << BITMAP_BITS );
	}

	// we have a valid bitmap, but there's no guarantee we still have set bits
	int iBitmapStart = iBitmap << BITMAP_BITS;
	int iRes = pBitmap->Scan ( iStart - iBitmapStart );
	if ( iRes==pBitmap->GetLength() )
		return Scan ( iBitmapStart + VALUES_PER_BITMAP );

	return iRes + iBitmapStart;
}

template <typename RESULT>
void SplitBitmap_c::Fetch ( int & iIterator, int iBase, RESULT * & pRes, RESULT * pMax )
{
	int iBitmap = iIterator >> ( BITMAP_BITS-6 );
	if ( iBitmap>=m_dBitmaps.size() )
		return;

	auto & pBitmap = m_dBitmaps[iBitmap];
	if ( pBitmap )
	{
		// we have a valid bitmap, but there's no guarantee we still have set bits
		auto * pResStart = pRes;
		int iBitmapStartIterator = iBitmap << ( BITMAP_BITS-6 );
		int iIteratorInBitmap = iIterator - iBitmapStartIterator;
		m_dBitmaps[iBitmap]->Fetch ( iIteratorInBitmap, iBitmapStartIterator << 6, pRes, pMax );
		if ( pRes==pResStart )
		{
			iIterator = iBitmapStartIterator + ( VALUES_PER_BITMAP>>6 );
			Fetch ( iIterator, 0, pRes, pMax );
		}
		else
			iIterator = iIteratorInBitmap + iBitmapStartIterator;
	}
	else
	{
		iBitmap++;
		while ( iBitmap<m_dBitmaps.size() && !m_dBitmaps[iBitmap] )
			iBitmap++;

		if ( iBitmap==m_dBitmaps.size() )
			return;

		// since this bitmap exists, its guaranteed to have set bits
		int iIteratorInBitmap = 0;
		m_dBitmaps[iBitmap]->Fetch ( iIteratorInBitmap, iBitmap << BITMAP_BITS, pRes, pMax );
		iIterator = iIteratorInBitmap + ( iBitmap << ( BITMAP_BITS-6 ) );
	}
}

/////////////////////////////////////////////////////////////////////
class BitmapIterator_i : public BlockIterator_i
{
public:
	virtual void Add ( BlockIterator_i * pIterator ) = 0;
};

template <typename BITMAP>
class BitmapIterator_T : public BitmapIterator_i
{
public:
				BitmapIterator_T ( uint32_t uNumValues );

	bool		HintRowID ( uint32_t tRowID ) override;
	bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) override;
	int64_t		GetNumProcessed() const override	{ return m_iNumProcessed; }
	void		AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const override;

	void		SetCutoff ( int iCutoff ) override	{ m_iRowsLeft = iCutoff; }
	bool		WasCutoffHit() const override		{ return !m_iRowsLeft; }

	void		Add ( BlockIterator_i * pIterator ) override;

private:
	static const int			RESULT_BLOCK_SIZE = 1024;
	BITMAP						m_tBitmap;
	std::vector<IteratorDesc_t> m_dDesc;
	int64_t						m_iNumProcessed = 0;
	int							m_iIndex = 0;
	int							m_iRowsLeft = INT_MAX;
	SpanResizeable_T<uint32_t>	m_dRows;
};

template <typename BITMAP>
BitmapIterator_T<BITMAP>::BitmapIterator_T ( uint32_t uNumValues )
	: m_tBitmap(uNumValues)
{
	m_dRows.resize(RESULT_BLOCK_SIZE);
}

template <typename BITMAP>
void BitmapIterator_T<BITMAP>::AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const
{
	for ( const auto & i : m_dDesc )
		dDesc.push_back(i);
}

template <typename BITMAP>
void BitmapIterator_T<BITMAP>::Add ( BlockIterator_i * pIterator )
{
	assert(pIterator);

	if ( m_dDesc.empty() )
		pIterator->AddDesc(m_dDesc);

	Span_T<uint32_t> dRowIdBlock;
	while ( pIterator->GetNextRowIdBlock(dRowIdBlock) && m_iRowsLeft>0 )
	{
		for ( auto i : dRowIdBlock )
			m_tBitmap.BitSet(i);

		m_iNumProcessed += dRowIdBlock.size();
		m_iRowsLeft -= dRowIdBlock.size();
	}

	m_iRowsLeft = std::max ( m_iRowsLeft, 0 );
}

template <typename BITMAP>
bool BitmapIterator_T<BITMAP>::HintRowID ( uint32_t tRowID )
{
	m_iIndex = tRowID >> 6;
	return m_iIndex < m_tBitmap.GetLength();
}

template <typename BITMAP>
bool BitmapIterator_T<BITMAP>::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	uint32_t * pData = m_dRows.data();
	uint32_t * pPtr = pData;
	
	m_tBitmap.Fetch ( m_iIndex, 0, pPtr, m_dRows.end() );
	dRowIdBlock = Span_T<uint32_t>( pData, pPtr-pData );

	return !dRowIdBlock.empty();
}

/////////////////////////////////////////////////////////////////////

template<typename VEC>
void DecodeBlock ( VEC & dDst, IntCodec_i * pCodec, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader )
{
	dBuf.resize ( 0 );
	ReadVectorLen32 ( dBuf, tReader );
	pCodec->Decode ( dBuf, dDst );
	ComputeInverseDeltas ( dDst, true );
}

template<typename VEC>
void DecodeBlockWoDelta ( VEC & dDst, IntCodec_i * pCodec, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader )
{
	dBuf.resize ( 0 );
	ReadVectorLen32 ( dBuf, tReader );
	pCodec->Decode ( dBuf, dDst );
}

/////////////////////////////////////////////////////////////////////

struct Int64ValueCmp_t
{
	bool operator()( const uint64_t & uVal1, const uint64_t & uVal2 ) const
	{
		return ( (int64_t)uVal1<(int64_t)uVal2 );
	}
};


struct FloatValueCmp_t
{
	float AsFloat ( const uint32_t & tVal ) const { return UintToFloat ( tVal ); }
	float AsFloat ( const float & fVal ) const { return fVal; }

	template< typename T1, typename T2 >
	bool operator()( const T1 & tVal1, const T2 & tVal2 ) const
	{
		return IsLess ( tVal1, tVal2 );
	}

	template< typename T1, typename T2 >
	bool IsLess ( const T1 & tVal1, const T2 & tVal2 ) const
	{
		return AsFloat( tVal1 ) < AsFloat( tVal2 );
	}
};

/////////////////////////////////////////////////////////////////////

class ReaderTraits_c : public BlockReader_i
{
public:
						ReaderTraits_c ( const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tRsetInfo, int iCutoff );

protected:
	std::string					m_sAttr;
	std::shared_ptr<IntCodec_i>	m_pCodec { nullptr };
	uint64_t					m_uBlockBaseOff = 0;
	int64_t						m_iMetaOffset = 0;

	RowidRange_t				m_tBounds;
	bool						m_bHaveBounds = false;

	SpanResizeable_T<uint32_t>	m_dTypes;
	SpanResizeable_T<uint32_t>	m_dMin;
	SpanResizeable_T<uint32_t>	m_dMax;
	SpanResizeable_T<uint32_t>	m_dRowStart;

	SpanResizeable_T<uint32_t>	m_dBufTmp;
	RsetInfo_t					m_tRsetInfo;
	int							m_iCutoff = 0;

	bool				NeedBitmapIterator() const;
	BitmapIterator_i *	SpawnBitmapIterator() const;
};


ReaderTraits_c::ReaderTraits_c ( const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tRsetInfo, int iCutoff )
	: m_sAttr ( sAttr )
	, m_pCodec ( pCodec )
	, m_uBlockBaseOff ( uBlockBaseOff )
	, m_tRsetInfo ( tRsetInfo )
	, m_iCutoff ( iCutoff )
{
	assert ( m_pCodec.get() );
	m_bHaveBounds = !!pBounds;
	if ( m_bHaveBounds )
		m_tBounds = *pBounds;
}


bool ReaderTraits_c::NeedBitmapIterator() const
{
	const size_t BITMAP_ITERATOR_THRESH = 8;
	const int QUEUE_RSET_THRESH = 4096;
	return m_tRsetInfo.m_iNumIterators>BITMAP_ITERATOR_THRESH && m_tRsetInfo.m_iRsetSize>QUEUE_RSET_THRESH;
}


BitmapIterator_i * ReaderTraits_c::SpawnBitmapIterator() const
{
	if ( !NeedBitmapIterator() )
		return nullptr;

	const uint32_t SMALL_INDEX_THRESH = 262144;
	if ( m_tRsetInfo.m_uRowsCount<=SMALL_INDEX_THRESH ) 
		return new BitmapIterator_T<BitVec_T<uint64_t>> ( m_tRsetInfo.m_uRowsCount );

	const float LARGE_BITMAP_RATIO = 0.01f;
	if ( float(m_tRsetInfo.m_iRsetSize)/m_tRsetInfo.m_uRowsCount<=LARGE_BITMAP_RATIO )
		return new BitmapIterator_T<SplitBitmap_c> ( m_tRsetInfo.m_uRowsCount );

	return new BitmapIterator_T<BitVec_T<uint64_t>> ( m_tRsetInfo.m_uRowsCount );
}

/////////////////////////////////////////////////////////////////////

class BlockReader_c : public ReaderTraits_c
{
public:
			BlockReader_c ( int iFD, const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff );

	void	CreateBlocksIterator ( const std::vector<BlockIter_t> & dIt, std::vector<BlockIterator_i *> & dRes ) override;
	void	CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tVal, std::vector<BlockIterator_i *> & dRes ) override { assert ( 0 && "Requesting range iterators from block reader" ); }

protected:
	std::shared_ptr<FileReader_c> m_pFileReader { nullptr };
		
	std::vector<uint64_t>	m_dBlockOffsets;
	int						m_iLoadedBlock { -1 };
	int						m_iStartBlock { -1 };
	int64_t					m_iOffPastValues { -1 };

	// interface for value related methods
	virtual void			LoadValues () = 0;
	virtual FindValueResult_t FindValue ( uint64_t uRefVal ) const = 0;

	BlockIterator_i	*		CreateIterator ( int iItem );
	int						BlockLoadCreateIterator ( int iBlock, uint64_t uVal, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator );
	bool					AddIterator ( int iItem, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator );
	void					CreateBlocksIterator ( const BlockIter_t & tIt, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator );
};


BlockReader_c::BlockReader_c ( int iFD, const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff )
	: ReaderTraits_c ( sAttr, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff )
	, m_pFileReader ( std::make_shared<FileReader_c>( iFD, READER_BUFFER_SIZE ) )
{}


bool BlockReader_c::AddIterator ( int iItem, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator )
{
	std::unique_ptr<BlockIterator_i> pIterator ( CreateIterator(iItem) );
	if ( !pIterator )
		return true;

	if ( pBitmapIterator )
	{
		pBitmapIterator->Add ( pIterator.get() );
		return !pBitmapIterator->WasCutoffHit();
	}
	
	dRes.push_back ( pIterator.release() );
	return true;
}


int BlockReader_c::BlockLoadCreateIterator ( int iBlock, uint64_t uVal, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator )
{
	if ( iBlock!=-1 )
	{
		m_pFileReader->Seek ( m_dBlockOffsets[iBlock] );
		LoadValues();
		m_iLoadedBlock = m_iStartBlock + iBlock;
	}

	auto [iItem, iCmp] = FindValue ( uVal );
	if ( iItem!=-1 )
		AddIterator ( iItem, dRes, pBitmapIterator );

	return iCmp;
}


void BlockReader_c::CreateBlocksIterator ( const BlockIter_t & tIt, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator )
{
	m_iStartBlock = tIt.m_iStart;

	// load offsets of all blocks for the range
	m_dBlockOffsets.resize ( tIt.m_iLast - tIt.m_iStart + 1 );
	m_pFileReader->Seek ( m_uBlockBaseOff + tIt.m_iStart * sizeof ( uint64_t) );
	for ( int iBlock=0; iBlock<m_dBlockOffsets.size(); iBlock++ )
		m_dBlockOffsets[iBlock] = m_pFileReader->Read_uint64();

	// first check already loadded block in case it fits the range and it is not the best block that will be checked
	int iLastBlockChecked = -1;
	if ( m_iLoadedBlock!=m_iStartBlock+tIt.m_iPos && m_iStartBlock>=m_iLoadedBlock && m_iLoadedBlock<=tIt.m_iLast )
	{
		// if current block fits - exit even no matches
		if ( BlockLoadCreateIterator ( -1, tIt.m_uVal, dRes, pBitmapIterator )==0 )
			return;

		iLastBlockChecked = m_iLoadedBlock;
	}

	// if best block fits - exit even no matches
	if ( BlockLoadCreateIterator ( tIt.m_iPos, tIt.m_uVal, dRes, pBitmapIterator )==0 )
		return;

	for ( int iBlock=0; iBlock<=tIt.m_iLast - tIt.m_iStart; iBlock++ )
	{
		if ( iBlock==tIt.m_iPos || ( iLastBlockChecked!=-1 && m_iStartBlock+iBlock==iLastBlockChecked ) )
			continue;

		int iCmp = BlockLoadCreateIterator ( iBlock, tIt.m_uVal, dRes, pBitmapIterator );

		// stop ckecking blocks in case
		// - found block where the value in values range
		// - checked block with the greater value
		if ( iCmp==0 || iCmp>0 )
			return;
	}
}


void BlockReader_c::CreateBlocksIterator ( const std::vector<BlockIter_t> & dIt, std::vector<BlockIterator_i *> & dRes )
{
	// add bitmap iterator as 1st element of dRes on exit
	std::function<void( BitmapIterator_i * pIterator )> fnDeleter = [&]( BitmapIterator_i * pIterator ){ if ( pIterator ) { assert(dRes.empty()); dRes.push_back(pIterator); } };
	std::unique_ptr<BitmapIterator_i, decltype(fnDeleter)> pBitmapIterator ( SpawnBitmapIterator(), fnDeleter );
	if ( pBitmapIterator && m_iCutoff>=0 )
		pBitmapIterator->SetCutoff(m_iCutoff);

	for ( auto & i : dIt )
		CreateBlocksIterator ( i, dRes, pBitmapIterator.get() );
}


BlockIterator_i * BlockReader_c::CreateIterator ( int iItem )
{
	if ( m_iOffPastValues!=-1 )
	{
		// seek right after values to load the rest of the block content as only values could be loaded
		m_pFileReader->Seek ( m_iOffPastValues );
		m_iOffPastValues = -1;

		DecodeBlockWoDelta ( m_dTypes, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
		DecodeBlock ( m_dMin, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
		DecodeBlock ( m_dMax, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
		DecodeBlock ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );

		m_iMetaOffset = m_pFileReader->GetPos();
	}

	return CreateRowidIterator ( m_sAttr, (Packing_e)m_dTypes[iItem], m_iMetaOffset+m_dRowStart[iItem], m_dMin[iItem], m_dMax[iItem], m_pFileReader, m_pCodec, m_bHaveBounds ? &m_tBounds : nullptr, false );
}

/////////////////////////////////////////////////////////////////////

template<typename VALUE, bool FLOAT_VALUE>
class BlockReader_T : public BlockReader_c
{
public:
			BlockReader_T ( int iFD, const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff );

private:
	SpanResizeable_T<VALUE> m_dValues;

	void	LoadValues() override;
	FindValueResult_t FindValue ( uint64_t uRefVal ) const override;

	void	AddIterator ( int iValCur, bool bLoad, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator );
};

template<typename VALUE, bool FLOAT_VALUE>
BlockReader_T<VALUE, FLOAT_VALUE>::BlockReader_T ( int iFD, const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff )
	: BlockReader_c ( iFD, sAttr, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff )
{}

template<typename VALUE, bool FLOAT_VALUE>
void BlockReader_T<VALUE, FLOAT_VALUE>::LoadValues()
{
	DecodeBlock ( m_dValues, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
	m_iOffPastValues = m_pFileReader->GetPos();
}

template<>
FindValueResult_t BlockReader_T<uint32_t, false>::FindValue ( uint64_t uRefVal ) const
{
	uint32_t uVal = uRefVal;
	int iItem = binary_search_idx ( m_dValues, uVal );
	if ( iItem!=-1 )
		return FindValueResult_t { iItem, 0 };

	if ( !m_dValues.size() || ( m_dValues.size() && m_dValues.front()<=uVal && uVal<=m_dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, m_dValues.back()<uVal ? 1 : -1 };
}

template<>
FindValueResult_t BlockReader_T<uint64_t, false>::FindValue ( uint64_t uRefVal ) const
{
	const auto tFirst = m_dValues.begin();
	const auto tLast = m_dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, uRefVal, Int64ValueCmp_t() );
	if ( tFound!=tLast && *tFound==uRefVal )
		return FindValueResult_t { (int)( tFound-tFirst ), 0 };

	if ( !m_dValues.size() || ( m_dValues.size() && (int64_t)m_dValues.front()<=(int64_t)uRefVal && (int64_t)uRefVal<=(int64_t)m_dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, (int64_t)m_dValues.back()<(int64_t)uRefVal ? 1 : -1 };
}

template<>
FindValueResult_t BlockReader_T<uint32_t, true>::FindValue ( uint64_t uRefVal ) const
{
	float fVal = UintToFloat ( uRefVal );
	const auto tFirst = m_dValues.begin();
	const auto tLast = m_dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, fVal, FloatValueCmp_t() );

	if ( tFound!=tLast && FloatEqual ( *tFound, fVal ) )
		return FindValueResult_t { (int)( tFound-tFirst ), 0 };

	if ( !m_dValues.size() || ( m_dValues.size() && m_dValues.front()<=fVal && fVal<=m_dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, m_dValues.back()<fVal ? 1 : -1 };
}

/////////////////////////////////////////////////////////////////////

BlockReader_i * CreateBlockReader ( int iFD, const ColumnInfo_t & tCol, const Settings_t & tSettings, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff )
{
	auto pCodec { std::shared_ptr<IntCodec_i> ( CreateIntCodec ( tSettings.m_sCompressionUINT32, tSettings.m_sCompressionUINT64 ) ) };
	assert(pCodec);

	switch ( tCol.m_eType )
	{
		case AttrType_e::UINT32:
		case AttrType_e::TIMESTAMP:
		case AttrType_e::UINT32SET:
		case AttrType_e::BOOLEAN:
			return new BlockReader_T<uint32_t, false> ( iFD, tCol.m_sName, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff );
			break;

		case AttrType_e::FLOAT:
			return new BlockReader_T<uint32_t, true> ( iFD, tCol.m_sName, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff );
			break;

		case AttrType_e::STRING:
		case AttrType_e::INT64:
		case AttrType_e::INT64SET:
			return new BlockReader_T<uint64_t, false> ( iFD, tCol.m_sName, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff );
			break;

		default: return nullptr;
	}
}

/////////////////////////////////////////////////////////////////////

template<typename T>
int CmpRange ( T tStart, T tEnd, const Filter_t & tRange )
{
	Interval_T<T> tIntBlock ( tStart, tEnd );

	Interval_T<T> tIntRange;
	if ( std::is_floating_point<T>::value )
		tIntRange = Interval_T<T> ( tRange.m_fMinValue, tRange.m_fMaxValue );
	else
		tIntRange = Interval_T<T> ( tRange.m_iMinValue, tRange.m_iMaxValue );

	if ( tRange.m_bLeftUnbounded )
		tIntRange.m_tStart = std::numeric_limits<T>::min();
	if ( tRange.m_bRightUnbounded )
		tIntRange.m_tEnd = std::numeric_limits<T>::max();

	if ( tIntBlock.Overlaps ( tIntRange ) )
		return 0;

	return ( tIntBlock<tIntRange ? -1 : 1 );
}

/////////////////////////////////////////////////////////////////////

class RangeReader_c : public ReaderTraits_c
{
public:
			RangeReader_c ( int iFD, const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff );

	void	CreateBlocksIterator ( const std::vector<BlockIter_t> & dIt, std::vector<BlockIterator_i *> & dRes ) override { assert ( 0 && "Requesting block iterators from range reader" ); }
	void	CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tVal, std::vector<BlockIterator_i *> & dRes ) override;

protected:
	struct BlockCtx_t
	{
		int m_iValCur = 0;
		int m_iValCount = 0;
		int m_iBlockItCreated = -1;
		int m_iBlockCur = 0;
		
		BlockCtx_t ( int iStart ) : m_iBlockCur(iStart) {}
	};

	std::shared_ptr<FileReader_c> m_pOffReader { nullptr };
	std::shared_ptr<FileReader_c> m_pBlockReader { nullptr };
		
	// interface for value related methods
	virtual int			LoadValues () = 0;
	virtual bool		EvalRangeValue ( int iItem, const Filter_t & tRange ) const = 0;
	virtual int			CmpBlock ( const Filter_t & tRange ) const = 0;

	BlockIterator_i *	CreateIterator ( int iItem, bool bLoad, bool bBitmap );
	bool				AddIterator ( int iValCur, bool bLoad, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator );

	bool				WarmupBlockIterators ( BlockCtx_t & tCtx, const Filter_t & tRange, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator );
	bool				AddBlockIterators ( BlockCtx_t & tCtx, const Filter_t & tRange, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator );
};


RangeReader_c::RangeReader_c ( int iFD, const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff  )
 	: ReaderTraits_c ( sAttr, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff )
 	, m_pOffReader ( std::make_shared<FileReader_c>( iFD, READER_BUFFER_SIZE ) )
 	, m_pBlockReader ( std::make_shared<FileReader_c>( iFD, READER_BUFFER_SIZE ) )
{}


bool RangeReader_c::AddIterator ( int iValCur, bool bLoad, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator )
{
	std::unique_ptr<BlockIterator_i> pIterator ( CreateIterator ( iValCur, bLoad, !!pBitmapIterator ) );
	if ( !pIterator )
		return true;

	if ( pBitmapIterator )
	{
		pBitmapIterator->Add ( pIterator.get() );
		return !pBitmapIterator->WasCutoffHit();
	}

	dRes.push_back ( pIterator.release() );
	return true;
}


bool RangeReader_c::WarmupBlockIterators ( BlockCtx_t & tCtx, const Filter_t & tRange, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator )
{
	for ( tCtx.m_iValCur=0; tCtx.m_iValCur<tCtx.m_iValCount; tCtx.m_iValCur++ )
		if ( EvalRangeValue ( tCtx.m_iValCur, tRange ) )
		{
			if ( !AddIterator ( tCtx.m_iValCur, true, dRes, pBitmapIterator ) )
				return false;

			tCtx.m_iBlockItCreated = tCtx.m_iBlockCur;
			tCtx.m_iValCur++;
			break;
		}

	return true;
}


bool RangeReader_c::AddBlockIterators ( BlockCtx_t & tCtx, const Filter_t & tRange, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator )
{
	if ( tCtx.m_iValCur>=tCtx.m_iValCount )
		return true;

	// case: values only inside the block matched, need to check every value
	if ( !EvalRangeValue ( tCtx.m_iValCount-1, tRange ) )
	{
		for ( ; tCtx.m_iValCur<tCtx.m_iValCount; tCtx.m_iValCur++ )
		{
			if ( !EvalRangeValue ( tCtx.m_iValCur, tRange ) )
				return false;

			if ( !AddIterator  ( tCtx.m_iValCur, tCtx.m_iBlockItCreated!=tCtx.m_iBlockCur, dRes, pBitmapIterator ) )
				return false;

			tCtx.m_iBlockItCreated = tCtx.m_iBlockCur;
		}

		return false;
	}

	// case: all values till the end of the block matched
	for ( ; tCtx.m_iValCur<tCtx.m_iValCount; tCtx.m_iValCur++ )
	{
		if ( !AddIterator  ( tCtx.m_iValCur, tCtx.m_iBlockItCreated!=tCtx.m_iBlockCur, dRes, pBitmapIterator ) )
			return false;

		tCtx.m_iBlockItCreated = tCtx.m_iBlockCur;
	}

	return true;
}


void RangeReader_c::CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tRange, std::vector<BlockIterator_i *> & dRes )
{
	BlockCtx_t tCtx ( tIt.m_iStart );
	m_pOffReader->Seek ( m_uBlockBaseOff + tCtx.m_iBlockCur*sizeof(uint64_t) );

	// add bitmap iterator as 1st element of dRes on exit
	std::function<void( BitmapIterator_i * pIterator )> fnDeleter = [&]( BitmapIterator_i * pIterator ){ if ( pIterator ) { assert(dRes.empty()); dRes.push_back(pIterator); } };
	std::unique_ptr<BitmapIterator_i, decltype(fnDeleter)> pBitmapIterator ( SpawnBitmapIterator(), fnDeleter );
	if ( pBitmapIterator && m_iCutoff>=0 )
		pBitmapIterator->SetCutoff(m_iCutoff);

	// warmup
	for ( ; tCtx.m_iBlockCur<=tIt.m_iLast; tCtx.m_iBlockCur++ )
	{
		uint64_t uBlockOff = m_pOffReader->Read_uint64();
		m_pBlockReader->Seek ( uBlockOff );

		tCtx.m_iValCount = LoadValues();

		int iCmpLast = CmpBlock ( tRange );
		if ( iCmpLast==1 )
			break;
		if ( iCmpLast==-1 )
			continue;

		if ( !WarmupBlockIterators ( tCtx, tRange, dRes, pBitmapIterator.get() ) )
			return;

		// get into search in case current block has values matched
		if ( tCtx.m_iBlockItCreated!=-1 )
			break;
	}

	if ( tCtx.m_iBlockItCreated==-1 )
		return;

	// check end block value vs EvalRange then add all values from block as iterators
	// for openleft find start via linear scan
	// cases :
	// - whole block
	// - skip left part of values in block 
	// - stop on scan all values in block
	// FIXME!!! stop checking on range passed values 
	for ( ;; )
	{
		if ( !AddBlockIterators ( tCtx, tRange, dRes, pBitmapIterator.get() ) )
			return;

		tCtx.m_iBlockCur++;
		if ( tCtx.m_iBlockCur>tIt.m_iLast )
			break;

		uint64_t uBlockOff = m_pOffReader->Read_uint64();
		m_pBlockReader->Seek ( uBlockOff );

		tCtx.m_iValCount = LoadValues();
		tCtx.m_iValCur = 0;
		assert ( tCtx.m_iValCount );

		// matching is over
		if ( !EvalRangeValue ( 0, tRange ) )
			break;
	}
}


BlockIterator_i * RangeReader_c::CreateIterator ( int iItem, bool bLoad, bool bBitmap )
{
	if ( bLoad )
	{
		DecodeBlockWoDelta ( m_dTypes, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
		DecodeBlock ( m_dMin, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
		DecodeBlock ( m_dMax, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
		DecodeBlock ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
		m_iMetaOffset = m_pBlockReader->GetPos();
	}

	return CreateRowidIterator ( m_sAttr, (Packing_e)m_dTypes[iItem], m_iMetaOffset + m_dRowStart[iItem], m_dMin[iItem], m_dMax[iItem], m_pBlockReader, m_pCodec, m_bHaveBounds ? &m_tBounds : nullptr, bBitmap );
}

/////////////////////////////////////////////////////////////////////

template<typename STORE_VALUE, typename DST_VALUE>
class RangeReader_T : public RangeReader_c
{
public:
	RangeReader_T ( int iFD, const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff );

private:
	SpanResizeable_T<STORE_VALUE> m_dValues;

	int LoadValues () override
	{
		DecodeBlock ( m_dValues, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
		return m_dValues.size();
	}

	bool EvalRangeValue ( int iItem, const Filter_t & tRange ) const override
	{
		if ( std::is_floating_point<DST_VALUE>::value )
			return ValueInInterval<float> ( UintToFloat ( m_dValues[iItem] ), tRange );
		else
			return ValueInInterval<DST_VALUE> ( (DST_VALUE)m_dValues[iItem], tRange );
	}

	int CmpBlock ( const Filter_t & tRange ) const override
	{
		if ( std::is_floating_point<DST_VALUE>::value )
			return CmpRange<float> ( UintToFloat ( m_dValues.front() ), UintToFloat ( m_dValues.back() ), tRange );
		else
			return CmpRange<DST_VALUE> ( m_dValues.front(), m_dValues.back(), tRange );
	}
};

template<typename STORE_VALUE, typename DST_VALUE>
RangeReader_T<STORE_VALUE,DST_VALUE>::RangeReader_T ( int iFD, const std::string & sAttr, std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff )
	: RangeReader_c ( iFD, sAttr, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff )
{}

/////////////////////////////////////////////////////////////////////

BlockReader_i * CreateRangeReader ( int iFD, const ColumnInfo_t & tCol, const Settings_t & tSettings, uint64_t uBlockBaseOff, const common::RowidRange_t * pBounds, const RsetInfo_t & tInfo, int iCutoff )
{
	auto pCodec { std::shared_ptr<IntCodec_i> ( CreateIntCodec ( tSettings.m_sCompressionUINT32, tSettings.m_sCompressionUINT64 ) ) };
	assert(pCodec);

	switch ( tCol.m_eType )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
	case AttrType_e::UINT32SET:
	case AttrType_e::BOOLEAN:
		return new RangeReader_T<uint32_t, uint32_t> ( iFD, tCol.m_sName, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff );
		break;

	case AttrType_e::FLOAT:
		return new RangeReader_T<uint32_t, float> ( iFD, tCol.m_sName, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff );
		break;

	case AttrType_e::INT64:
	case AttrType_e::INT64SET:
		return new RangeReader_T<uint64_t, int64_t> ( iFD, tCol.m_sName, pCodec, uBlockBaseOff, pBounds, tInfo, iCutoff );
		break;

	default: return nullptr;
	}
}

}
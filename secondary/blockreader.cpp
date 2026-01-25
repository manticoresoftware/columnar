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

#include "blockreader.h"
#include "secondary.h"

#include "pgm.h"
#include "iterator.h"
#include "delta.h"
#include "interval.h"
#include "bitvec.h"
#include <atomic>

#include <functional>

namespace SI
{

using namespace util;
using namespace common;

void ColumnInfo_t::Load ( util::FileReader_c & tReader, uint32_t uVersion )
{
	m_sName = tReader.Read_string();
	m_eType = (AttrType_e)tReader.Unpack_uint32();
	m_uCountDistinct = tReader.Unpack_uint32();

	if ( uVersion>=9 )
	{
		m_tMin = tReader.Unpack_uint64();
		m_tMax = tReader.Unpack_uint64();
	}

	auto iJsonFieldPos = m_sName.find ( "['" );
	if ( iJsonFieldPos!=std::string::npos )
		m_sJsonParentName = m_sName.substr ( 0, iJsonFieldPos );
}


void ColumnInfo_t::Save ( util::FileWriter_c & tWriter ) const
{
	tWriter.Write_string ( m_sName );
	tWriter.Pack_uint32 ( (int)m_eType );
	tWriter.Pack_uint32 ( m_uCountDistinct );
	tWriter.Pack_uint64 ( m_tMin );
	tWriter.Pack_uint64 ( m_tMax );
}

/////////////////////////////////////////////////////////////////////

void Settings_t::Load ( FileReader_c & tReader, uint32_t uVersion )
{
	m_sCompressionUINT32 = tReader.Read_string();
	m_sCompressionUINT64 = tReader.Read_string();
}


void Settings_t::Save ( FileWriter_c & tWriter ) const
{
	tWriter.Write_string(m_sCompressionUINT32);
	tWriter.Write_string(m_sCompressionUINT64);
}

/////////////////////////////////////////////////////////////////////

static FORCE_INLINE uint64_t GetValueBlock ( uint64_t uPos, uint32_t uValuesPerBlockShift )
{
	return uPos >> uValuesPerBlockShift;
}

BlockIter_t::BlockIter_t ( const ApproxPos_t & tFrom, uint64_t uVal, uint64_t uBlocksCount, uint32_t uValuesPerBlockShift )
	: m_uVal ( uVal )
{
	m_iStart= GetValueBlock ( tFrom.m_iLo, uValuesPerBlockShift );
	m_iPos	= GetValueBlock ( tFrom.m_iPos, uValuesPerBlockShift ) - m_iStart;
	m_iLast	= GetValueBlock ( tFrom.m_iHi, uValuesPerBlockShift );

	if ( m_iStart+m_iPos>=uBlocksCount )
		m_iPos = 0;

	if ( m_iLast>=uBlocksCount )
		m_iLast = uBlocksCount - 1;
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

	FORCE_INLINE void	BitSet ( int iBit );
	FORCE_INLINE int	Scan ( int iStart );
	FORCE_INLINE int	GetLength() const { return m_iSize; }

	void				Invert ( int iMinBit=-1, int iMaxBit=-1 ) { assert ( 0 && "Unsupported by SplitBitmap_c" ); }
	void				Resize ( int iSize ) { assert ( 0 && "Unsupported by SplitBitmap_c" ); }

	template <typename RESULT>
	void				Fetch ( int & iIterator, int iBase, RESULT * & pRes, RESULT * pMax );

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
	virtual void Invert ( RowidRange_t * pBounds ) = 0;
};

template <typename BITMAP, bool ROWID_RANGE>
class BitmapIterator_T : public BitmapIterator_i
{
public:
				BitmapIterator_T ( const std::string & sAttr, uint32_t uNumValues, const RowidRange_t * pBounds=nullptr );

	bool		HintRowID ( uint32_t tRowID ) override;
	bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) override;
	int64_t		GetNumProcessed() const override	{ return m_iNumProcessed; }
	void		AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const override { dDesc.push_back ( { m_sAttr, "SecondaryIndex" } ); }

	void		SetCutoff ( int iCutoff ) override	{ m_iRowsLeft = iCutoff; m_bHasCutoff = true; }
	bool		WasCutoffHit() const override		{ return ( m_bHasCutoff && !m_iRowsLeft ); }

	void		Add ( BlockIterator_i * pIterator ) override;
	void		Invert ( RowidRange_t * pBounds ) override	{ m_tBitmap.Invert ( pBounds ? pBounds->m_uMin : -1, pBounds ? pBounds->m_uMax : -1 ); }

private:
	static const int			RESULT_BLOCK_SIZE = 1024;
	BITMAP						m_tBitmap;
	std::string					m_sAttr;
	int64_t						m_iNumProcessed = 0;
	int							m_iIndex = 0;
	int							m_iRowsLeft = INT_MAX;
	bool						m_bHasCutoff = false;
	RowidRange_t				m_tBounds;
	SpanResizeable_T<uint32_t>	m_dRows;
};

template <typename BITMAP, bool ROWID_RANGE>
BitmapIterator_T<BITMAP,ROWID_RANGE>::BitmapIterator_T ( const std::string & sAttr, uint32_t uNumValues, const RowidRange_t * pBounds )
	: m_tBitmap ( uNumValues )
	, m_sAttr ( sAttr )
{
	if ( pBounds )
		m_tBounds = *pBounds;

	m_dRows.resize(RESULT_BLOCK_SIZE);
}

template <typename BITMAP, bool ROWID_RANGE>
void BitmapIterator_T<BITMAP,ROWID_RANGE>::Add ( BlockIterator_i * pIterator )
{
	assert(pIterator);

	Span_T<uint32_t> dRowIdBlock;
	while ( pIterator->GetNextRowIdBlock(dRowIdBlock) && m_iRowsLeft>0 )
	{
		uint32_t * pPtr = dRowIdBlock.data();
		uint32_t * pEnd = dRowIdBlock.end();

		if constexpr ( ROWID_RANGE )
		{
			// we need additional filtering only on first and last blocks
			// per-block filtering is performed inside the iterator
			bool bCutFront = dRowIdBlock.front() < m_tBounds.m_uMin;
			bool bCutBack = dRowIdBlock.back() > m_tBounds.m_uMax;
			if ( bCutFront || bCutBack )
			{
				if ( bCutFront )
					pPtr = std::lower_bound ( pPtr, pEnd, m_tBounds.m_uMin );
					
				if ( bCutBack )
					pEnd = std::upper_bound ( pPtr, pEnd, m_tBounds.m_uMax );
			}
		}

		bool bWasCutoff = false;
		int iProcessed = ( pEnd - pPtr );
		if ( iProcessed>m_iRowsLeft )
		{
			pEnd = pPtr + m_iRowsLeft;
			iProcessed = m_iRowsLeft;
			bWasCutoff = true;
		}
		while ( pPtr<pEnd )
			m_tBitmap.BitSet ( *pPtr++ );

		m_iNumProcessed += iProcessed;
		m_iRowsLeft -= iProcessed;

		// cutoff lower processed count that keep some bits cleared
		// then tFilter.m_bExclude cause bitvec to invert thouse bits
		if ( m_bHasCutoff && bWasCutoff )
		{
			uint32_t iLastRow = *(--pPtr);
			// should only srink but not extend
			assert ( iLastRow+1<=m_tBitmap.GetLength() );
			m_tBitmap.Resize ( iLastRow+1 );
		}
	}

	m_iRowsLeft = std::max ( m_iRowsLeft, 0 );
}

template <typename BITMAP, bool ROWID_RANGE>
bool BitmapIterator_T<BITMAP,ROWID_RANGE>::HintRowID ( uint32_t tRowID )
{
	int iNewIndex = tRowID >> 6;
	if ( iNewIndex > m_iIndex )
		m_iIndex = iNewIndex;

	return m_iIndex < m_tBitmap.GetLength();
}

template <typename BITMAP, bool ROWID_RANGE>
bool BitmapIterator_T<BITMAP,ROWID_RANGE>::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	uint32_t * pData = m_dRows.data();
	uint32_t * pPtr = pData;
	
	m_tBitmap.Fetch ( m_iIndex, 0, pPtr, m_dRows.end() );
	dRowIdBlock = Span_T<uint32_t>( pData, pPtr-pData );

	return !dRowIdBlock.empty();
}

/////////////////////////////////////////////////////////////////////

template<typename VEC>
static FORCE_INLINE void ReadBlock ( VEC & dDst, int iNumValues, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader )
{
	dDst.resize(iNumValues);
	ReadVectorLen32 ( dBuf, tReader );
}

template<typename VEC>
static FORCE_INLINE void DecodeBlock ( VEC & dDst, int iNumValues, IntCodec_i * pCodec, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader )
{
	ReadBlock ( dDst, iNumValues, dBuf, tReader );
	pCodec->DecodeDelta ( dBuf, dDst );
}

template<typename VEC>
static FORCE_INLINE void DecodeBlockWoDelta ( VEC & dDst, int iNumValues, IntCodec_i * pCodec, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader )
{
	ReadBlock ( dDst, iNumValues, dBuf, tReader );
	pCodec->Decode ( dBuf, dDst );	
}


static FORCE_INLINE void SkipBlockUint32 ( FileReader_c & tReader )
{
	SkipVectorLen32<uint32_t>(tReader);
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

template <typename T>
static void CopyVec ( T & tDst, const T & tSrc )
{
	tDst.resize ( tSrc.size() );
	memcpy ( tDst.data(), tSrc.data(), tSrc.size()*sizeof(tSrc[0]) );
}

static const int OFF_READER_BUFFER_SIZE = 16384;
static const int READER_BUFFER_SIZE = 262144;

template <typename VALUE>
struct BlockValues_T
{
	SpanResizeable_T<VALUE>		m_dValues;
	int64_t						m_iOffPastValues = -1;

	void		Load ( uint32_t uNumValues, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader, IntCodec_i * pCodec );
	void		CopyFrom ( const BlockValues_T<VALUE> & dRhs );
	uint64_t	CalcSize() const;
};

template <typename VALUE>
void BlockValues_T<VALUE>::Load ( uint32_t uNumValues, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader, IntCodec_i * pCodec )
{
	DecodeBlock ( m_dValues, uNumValues, pCodec, dBuf, tReader );
	m_iOffPastValues = tReader.GetPos();
}

template <typename VALUE>
void BlockValues_T<VALUE>::CopyFrom ( const BlockValues_T<VALUE> & dRhs )
{
	// rhs contains vectors resized for decoding, we need a more compact represntation
	CopyVec ( m_dValues, dRhs.m_dValues );
	m_iOffPastValues = dRhs.m_iOffPastValues;
}

template <typename VALUE>
uint64_t BlockValues_T<VALUE>::CalcSize() const
{
	return sizeof(*this) + m_dValues.capacity()*sizeof(m_dValues[0]);
}


struct BlockData_t
{
	SpanResizeable_T<uint32_t>	m_dTypes;
	SpanResizeable_T<uint32_t>	m_dMin;
	SpanResizeable_T<uint32_t>	m_dMax;
	SpanResizeable_T<uint32_t>	m_dRowStart;
	SpanResizeable_T<uint32_t>	m_dCount;
	int64_t						m_iMetaOffset = 0;

	void		Load ( uint32_t uNumValues, uint32_t uVersion, bool bOnlyCount, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader, IntCodec_i * pCodec );
	void		CopyFrom ( const BlockData_t & dRhs );
	uint64_t	CalcSize() const;
};


void BlockData_t::Load ( uint32_t uNumValues, uint32_t uVersion, bool bOnlyCount, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader, IntCodec_i * pCodec )
{
	if ( !bOnlyCount )
	{
		DecodeBlockWoDelta ( m_dTypes, uNumValues, pCodec, dBuf, tReader );
		DecodeBlock ( m_dMin, uNumValues, pCodec, dBuf, tReader );
		DecodeBlock ( m_dMax, uNumValues, pCodec, dBuf, tReader );
		DecodeBlock ( m_dRowStart, uNumValues, pCodec, dBuf, tReader );
	}
	else
	{
		SkipBlockUint32(tReader); // m_dTypes
		SkipBlockUint32(tReader); // m_dMin
		SkipBlockUint32(tReader); // m_dMax
		SkipBlockUint32(tReader); // m_dRowStart
	}

	if ( uVersion > 6 )
		DecodeBlockWoDelta ( m_dCount, uNumValues, pCodec, dBuf, tReader );

	m_iMetaOffset = tReader.GetPos();
}


void BlockData_t::CopyFrom ( const BlockData_t & dRhs )
{
	// rhs contains vectors resized for decoding, we need a more compact represntation
	CopyVec ( m_dTypes,		dRhs.m_dTypes );
	CopyVec ( m_dMin,		dRhs.m_dMin );
	CopyVec ( m_dMax,		dRhs.m_dMax );
	CopyVec ( m_dRowStart,	dRhs.m_dRowStart );
	CopyVec ( m_dCount,		dRhs.m_dCount );
	m_iMetaOffset = dRhs.m_iMetaOffset;
}


uint64_t BlockData_t::CalcSize() const
{
	return sizeof(*this) + m_dTypes.capacity()*sizeof(m_dTypes[0]) + m_dMin.capacity()*sizeof(m_dMin[0]) + m_dMax.capacity()*sizeof(m_dMax[0]) + m_dRowStart.capacity()*sizeof(m_dRowStart[0]) + m_dCount.capacity()*sizeof(m_dCount[0]);
}

/////////////////////////////////////////////////////////////////////

class ReaderTraits_c : public BlockReader_i
{
public:
	ReaderTraits_c ( const ReaderFactory_c & tCtx, std::shared_ptr<IntCodec_i> & pCodec );

protected:
	std::shared_ptr<FileReader_c> m_pReader { nullptr };
	std::shared_ptr<FileReader_c> m_pOffsetReader { nullptr };

	std::string					m_sAttr;
	uint32_t					m_uVersion;
	std::shared_ptr<IntCodec_i>	m_pCodec { nullptr };
	uint64_t					m_uBlockBaseOff = 0;
	uint64_t					m_uBlocksCount = 0;
	uint32_t					m_uTotalValues = 0;
	uint32_t					m_uValuesPerBlock = 0;
	uint32_t					m_uRowidsPerBlock = 0;

	RowidRange_t				m_tBounds;
	bool						m_bHaveBounds = false;

	BlockData_t					m_tBlockData;

	SpanResizeable_T<uint32_t>	m_dBufTmp;
	RsetInfo_t					m_tRsetInfo;
	int							m_iCutoff = 0;

	bool				NeedBitmapIterator() const;
	BitmapIterator_i *	SpawnBitmapIterator ( const RowidRange_t * pBounds, bool bExclude ) const;
	uint32_t			CalcNumBlockValues ( int iBlock ) const;
};


ReaderTraits_c::ReaderTraits_c ( const ReaderFactory_c & tCtx, std::shared_ptr<IntCodec_i> & pCodec )
	: m_pReader ( std::make_shared<FileReader_c> ( tCtx.m_iFD, READER_BUFFER_SIZE ) )
	, m_pOffsetReader ( std::make_shared<FileReader_c> ( tCtx.m_iFD, READER_BUFFER_SIZE/2 ) )
	, m_sAttr ( tCtx.m_tCol.m_sName )
	, m_uVersion ( tCtx.m_uVersion )
	, m_pCodec ( pCodec )
	, m_uBlockBaseOff ( tCtx.m_uBlockBaseOff )
	, m_uBlocksCount ( tCtx.m_uBlocksCount )
	, m_uTotalValues ( tCtx.m_tCol.m_uCountDistinct )
	, m_uValuesPerBlock ( tCtx.m_uValuesPerBlock )
	, m_uRowidsPerBlock ( tCtx.m_uRowidsPerBlock )
	, m_tRsetInfo ( tCtx.m_tRsetInfo )
	, m_iCutoff ( tCtx.m_iCutoff )
{
	assert ( m_pCodec.get() );
	m_bHaveBounds = !!tCtx.m_pBounds;
	if ( m_bHaveBounds )
		m_tBounds = *tCtx.m_pBounds;
}


bool ReaderTraits_c::NeedBitmapIterator() const
{
	const size_t BITMAP_ITERATOR_THRESH = 8;
	return m_tRsetInfo.m_iNumIterators>BITMAP_ITERATOR_THRESH;
}


BitmapIterator_i * ReaderTraits_c::SpawnBitmapIterator ( const RowidRange_t * pBounds, bool bExclude ) const
{
	// force bitmap iterator for exclude filters
	// FIXME! make invertable split bitmaps
	if ( bExclude )
	{
		if ( pBounds )
			return new BitmapIterator_T<BitVec_T<uint64_t>, true> ( m_sAttr, m_tRsetInfo.m_uRowsCount, pBounds );
		else
			return new BitmapIterator_T<BitVec_T<uint64_t>, false> ( m_sAttr, m_tRsetInfo.m_uRowsCount );
	}

	if ( !NeedBitmapIterator() )
		return nullptr;

	float fRatio = 1.0f;
	if ( pBounds )
		fRatio = float ( pBounds->m_uMax - pBounds->m_uMin + 1  ) / m_tRsetInfo.m_uRowsCount;

	int64_t iRsetSize = (int64_t) ( m_tRsetInfo.m_iRsetSize*fRatio );

	const int64_t SMALL_INDEX_THRESH = 262144;
	const float LARGE_BITMAP_RATIO = 0.01f;
	if ( m_tRsetInfo.m_uRowsCount>SMALL_INDEX_THRESH && float(iRsetSize)/m_tRsetInfo.m_uRowsCount<=LARGE_BITMAP_RATIO )
	{
		if ( pBounds )
			return new BitmapIterator_T<SplitBitmap_c, true> ( m_sAttr, m_tRsetInfo.m_uRowsCount, pBounds );
		else
			return new BitmapIterator_T<SplitBitmap_c, false> ( m_sAttr, m_tRsetInfo.m_uRowsCount );
	}

	if ( pBounds )
		return new BitmapIterator_T<BitVec_T<uint64_t>, true> ( m_sAttr, m_tRsetInfo.m_uRowsCount, pBounds );
	else
		return new BitmapIterator_T<BitVec_T<uint64_t>, false> ( m_sAttr, m_tRsetInfo.m_uRowsCount );
}


uint32_t ReaderTraits_c::CalcNumBlockValues ( int iBlock ) const
{
	if ( iBlock < (int)m_uBlocksCount-1 )
		return m_uValuesPerBlock;

	uint32_t uLeftover = m_uTotalValues % m_uValuesPerBlock;
	return uLeftover ? uLeftover : m_uValuesPerBlock;
}

/////////////////////////////////////////////////////////////////////

template<typename VALUE>
class BlockCache_T : public BlockCache_i
{
public:
				BlockCache_T ( uint32_t uNumBlocks, uint64_t uMaxSize );
				~BlockCache_T();

	const BlockValues_T<VALUE> * GetValues ( int iBlock ) const;
	const BlockData_t * GetData ( int iBlock ) const	{ return m_dBlockData[iBlock].load ( std::memory_order_acquire ); }
	void		AddData ( int iBlock, const BlockData_t & tData );
	void		AddValues ( int iBlock, const BlockValues_T<VALUE> & tValues );

	uint64_t	GetMaxSize() const override				{ return m_uMaxSize; }
	float		GetReuseRatio() const override			{ return float ( m_uReuse.load ( std::memory_order_acquire ) ) / m_dBlockValues.size(); };
	bool		IsCacheFull() const override			{ return m_uSize.load ( std::memory_order_acquire ) >= m_uMaxSize; }
	void		ClearAll() override;

private:
	std::vector<std::atomic<BlockValues_T<VALUE>*>> m_dBlockValues;
	std::vector<std::atomic<BlockData_t*>>			m_dBlockData;
	std::atomic<uint64_t>			m_uSize;
	mutable std::atomic<uint64_t>	m_uReuse;
	mutable std::atomic<bool>		m_bStopCounting;
	uint64_t						m_uMaxSize = 0;
};

template<typename VALUE>
BlockCache_T<VALUE>::BlockCache_T ( uint32_t uNumBlocks, uint64_t uMaxSize )
	: m_dBlockValues ( uNumBlocks )
	, m_dBlockData ( uNumBlocks )
	, m_uMaxSize ( uMaxSize )
{
    for ( auto & i : m_dBlockValues )
        i.store ( nullptr, std::memory_order_relaxed );

    for ( auto & i : m_dBlockData )
        i.store ( nullptr, std::memory_order_relaxed );
}

template<typename VALUE>
BlockCache_T<VALUE>::~BlockCache_T()
{
	for ( auto & i : m_dBlockValues )
	    delete i.load ( std::memory_order_relaxed );

	for ( auto & i : m_dBlockData )
		delete i.load ( std::memory_order_relaxed );
}

template<typename VALUE>
void BlockCache_T<VALUE>::ClearAll()
{
	if ( m_uSize.load ( std::memory_order_acquire )==0 )
		return;

	for ( auto & i : m_dBlockValues )
		delete i.exchange ( nullptr, std::memory_order_relaxed );

	for ( auto & i : m_dBlockData )
		delete i.exchange ( nullptr, std::memory_order_relaxed );

	m_uSize.store ( 0, std::memory_order_release );
}

template<typename VALUE>
const BlockValues_T<VALUE> * BlockCache_T<VALUE>::GetValues ( int iBlock ) const
{
	auto pRes = m_dBlockValues[iBlock].load ( std::memory_order_acquire );
	if ( pRes && !m_bStopCounting.load ( std::memory_order_acquire ) )
	{
		uint64_t uOldReuse = m_uReuse.fetch_add ( 1, std::memory_order_relaxed );
		if ( uOldReuse >= std::numeric_limits<int64_t>::max() )
			m_bStopCounting.store ( true, std::memory_order_release );
	}

	return pRes;
}

template<typename VALUE>
void BlockCache_T<VALUE>::AddData ( int iBlock, const BlockData_t & tData )
{
	if ( m_dBlockData[iBlock].load ( std::memory_order_acquire ) )
        return; // already cached by another thread

	if ( m_uMaxSize && m_uSize.load ( std::memory_order_acquire )>=m_uMaxSize )
		return;

	auto pNewBlock = new BlockData_t;
	pNewBlock->CopyFrom(tData);

	BlockData_t * pExpected = nullptr;
	if ( !m_dBlockData[iBlock].compare_exchange_strong ( pExpected, pNewBlock, std::memory_order_release, std::memory_order_relaxed ) )
	{
		delete pNewBlock; // cached by another thread, discard our copy
		return;
	}

	m_uSize.fetch_add ( pNewBlock->CalcSize(), std::memory_order_relaxed );
}

template<typename VALUE>
void BlockCache_T<VALUE>::AddValues ( int iBlock, const BlockValues_T<VALUE> & tValues )
{
	if ( m_dBlockValues[iBlock].load ( std::memory_order_acquire ) )
        return; // already cached by another thread

	if ( m_uMaxSize && m_uSize.load ( std::memory_order_acquire )>=m_uMaxSize )
		return;

	auto pNewValues = new BlockValues_T<VALUE>;
	pNewValues->CopyFrom(tValues);

	BlockValues_T<VALUE> * pExpected = nullptr;
	if ( !m_dBlockValues[iBlock].compare_exchange_strong ( pExpected, pNewValues, std::memory_order_release, std::memory_order_relaxed ) )
	{
		delete pNewValues; // cached by another thread, discard our copy
		return;
	}

	m_uSize.fetch_add ( pNewValues->CalcSize(), std::memory_order_relaxed );
}

/////////////////////////////////////////////////////////////////////

template<typename VALUE, typename STORED_VALUE>
class BlockReader_T : public ReaderTraits_c
{
public:
	BlockReader_T ( const ReaderFactory_c & tCtx, std::shared_ptr<IntCodec_i> & pCodec )
		: ReaderTraits_c ( tCtx, pCodec ), m_pBlockCache ( tCtx.m_pBlockCache ? static_cast<BlockCache_T<VALUE>*>(tCtx.m_pBlockCache) : nullptr )
	{}

	void		CreateBlocksIterator ( const std::vector<BlockIter_t> & dIt, const Filter_t & tFilter, std::vector<BlockIterator_i *> & dRes ) override;
	void		CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tFilter, std::vector<BlockIterator_i *> & dRes ) override { assert ( 0 && "Requesting range iterators from block reader" ); }
	uint32_t	CalcValueCount ( const std::vector<BlockIter_t> & dIt ) override;
	uint32_t	CalcValueCount ( const BlockIter_t & tIt, const common::Filter_t & tVal ) override { assert ( 0 && "Requesting range iterators from block reader" ); return 0; }

protected:
	std::vector<uint64_t>	m_dBlockOffsets;
	int						m_iLoadedValuesBlockId = -1;
	int						m_iLoadedDataBlockId = -1;
	uint64_t				m_uBlockOffsetsOff = 0;

	BlockValues_T<VALUE>	m_tBlockValues;
	const BlockValues_T<VALUE> * m_pBlockValues = nullptr;
	const BlockData_t *		m_pBlockData = nullptr;
	BlockCache_T<VALUE> *	m_pBlockCache = nullptr;

	FORCE_INLINE FindValueResult_t FindValue ( uint64_t uRefVal ) const;

	void					AddIterator ( int iValCur, bool bLoad, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator );

	BlockIteratorWithSetup_i * CreateIterator ( int iBlock, int iItem, bool bBitmap );
	bool					SetupExistingIterator ( BlockIteratorWithSetup_i * pIterator, int iBlock, int iItem );
	uint32_t				CountValues ( int iBlock, int iItem );
	FORCE_INLINE void		LoadBlockValuesToCache ( int iStart, int iBlock );
	void					LoadBlockDataToCache ( int iBlock, bool bOnlyCount );

	template <typename ADDITERATOR>	int LoadBlockAndCreateIterator ( int iStart, int iBlock, uint64_t uVal, ADDITERATOR && fnAddIterator );
	template <typename ADDITERATOR>	int	CreateIterator ( int iBlock, uint64_t uVal, ADDITERATOR && fnAddIterator );
	bool					AddIterator ( int iBlock, int iItem, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator, std::unique_ptr<BlockIteratorWithSetup_i> & pCommonIterator );

	template <typename ADDITERATOR>
	void					CreateBlocksIterator ( const BlockIter_t & tIt, ADDITERATOR && fnAddIterator );

	void					LoadBlockOffsets ( const BlockIter_t & tIt );
};

template<typename VALUE, typename STORED_VALUE>
bool BlockReader_T<VALUE, STORED_VALUE>::AddIterator ( int iBlock, int iItem, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator, std::unique_ptr<BlockIteratorWithSetup_i> & pCommonIterator )
{
	// reuse previous iterator if pBitmapIterator is present
	if ( pBitmapIterator )
	{
		if ( !pCommonIterator )
		{
			pCommonIterator = std::unique_ptr<BlockIteratorWithSetup_i> ( CreateIterator ( iBlock, iItem, !!pBitmapIterator ) );
			if ( !pCommonIterator )
				return true;
		}
		else
		{
			if ( !SetupExistingIterator ( pCommonIterator.get(), iBlock, iItem ) )
				return true;
		}

		pBitmapIterator->Add ( pCommonIterator.get() );
		return !pBitmapIterator->WasCutoffHit();
	}

	std::unique_ptr<BlockIterator_i> pIterator ( CreateIterator ( iBlock, iItem, !!pBitmapIterator ) );
	if ( !pIterator )
		return true;

	dRes.push_back ( pIterator.release() );
	return true;
}

template<typename VALUE, typename STORED_VALUE>
void BlockReader_T<VALUE, STORED_VALUE>::LoadBlockValuesToCache ( int iStart, int iBlock )
{
	if ( m_iLoadedValuesBlockId==iBlock )
		return;

	if ( m_pBlockCache )
	{
		m_pBlockValues = m_pBlockCache->GetValues(iBlock);
		if ( m_pBlockValues )
		{
			m_iLoadedValuesBlockId = iBlock;
			return;
		}
	}

	m_pReader->Seek ( m_dBlockOffsets[iBlock-iStart] );
	m_tBlockValues.Load ( CalcNumBlockValues(iBlock), m_dBufTmp, *m_pReader.get(), m_pCodec.get() );
	m_iLoadedValuesBlockId = iBlock;

	if ( m_pBlockCache )
		m_pBlockCache->AddValues ( iBlock, m_tBlockValues );

	m_pBlockValues = &m_tBlockValues;
}

template<typename VALUE, typename STORED_VALUE>
template<typename ADDITERATOR>
int BlockReader_T<VALUE,STORED_VALUE>::CreateIterator ( int iBlock, uint64_t uVal, ADDITERATOR && fnAddIterator )
{
	auto [iItem, iCmp] = FindValue(uVal);
	if ( iItem!=-1 )
		fnAddIterator ( iBlock, iItem );

	return iCmp;
}

template<typename VALUE, typename STORED_VALUE>
template<typename ADDITERATOR>
int BlockReader_T<VALUE,STORED_VALUE>::LoadBlockAndCreateIterator ( int iStart, int iBlock, uint64_t uVal, ADDITERATOR && fnAddIterator )
{
	LoadBlockValuesToCache ( iStart, iBlock );
	return CreateIterator ( iBlock, uVal, fnAddIterator );
}

template<typename VALUE, typename STORED_VALUE>
void BlockReader_T<VALUE, STORED_VALUE>::LoadBlockOffsets ( const BlockIter_t & tIt )
{
	int iNumOffsetsToLoad = tIt.m_iLast - tIt.m_iStart + 1;
	uint64_t uBlockOffsetsOff = m_uBlockBaseOff + tIt.m_iStart * sizeof(uint64_t);

	if ( m_dBlockOffsets.size()==size_t(iNumOffsetsToLoad) && m_uBlockOffsetsOff==uBlockOffsetsOff )
		return;

	m_dBlockOffsets.resize(iNumOffsetsToLoad);
	m_pOffsetReader->Seek(uBlockOffsetsOff);
	for ( int iBlock=0; iBlock<m_dBlockOffsets.size(); iBlock++ )
		m_dBlockOffsets[iBlock] = m_pOffsetReader->Read_uint64();

	m_uBlockOffsetsOff = uBlockOffsetsOff;
}

template<typename VALUE, typename STORED_VALUE>
template<typename ADDITERATOR>
void BlockReader_T<VALUE, STORED_VALUE>::CreateBlocksIterator ( const BlockIter_t & tIt, ADDITERATOR && fnAddIterator )
{
	// load offsets of all blocks for the range
	LoadBlockOffsets(tIt);

	// first check already loaded block in case it fits the range and it is not the best block that will be checked
	int iLastBlockChecked = -1;
	if ( m_iLoadedValuesBlockId!=tIt.m_iStart+tIt.m_iPos && tIt.m_iStart>=m_iLoadedValuesBlockId && m_iLoadedValuesBlockId<=tIt.m_iLast )
	{
		// if current block fits - exit even no matches
		if ( CreateIterator ( m_iLoadedValuesBlockId, tIt.m_uVal, fnAddIterator )==0 )
			return;

		iLastBlockChecked = m_iLoadedValuesBlockId;
	}

	// try to find value in predicted "most probable" block
	if ( LoadBlockAndCreateIterator ( tIt.m_iStart, tIt.m_iStart+tIt.m_iPos, tIt.m_uVal, fnAddIterator )==0 )
		return;

	for ( int iBlock = tIt.m_iStart; iBlock<=tIt.m_iLast; iBlock++ )
	{
		// skip the "most probable" block and the last loaded block as we already checked those
		if ( iBlock-tIt.m_iStart == tIt.m_iPos || ( iLastBlockChecked!=-1 && iBlock==iLastBlockChecked ) )
			continue;

		int iCmp = LoadBlockAndCreateIterator ( tIt.m_iStart, iBlock, tIt.m_uVal, fnAddIterator );

		// stop checking blocks in case
		// - found block where the value in values range
		// - checked block with the greater value
		if ( iCmp==0 || iCmp>0 )
			return;
	}
}

template<typename VALUE, typename STORED_VALUE>
void BlockReader_T<VALUE, STORED_VALUE>::CreateBlocksIterator ( const std::vector<BlockIter_t> & dIt, const Filter_t & tFilter, std::vector<BlockIterator_i *> & dRes )
{
	// add bitmap iterator as 1st element of dRes on exit
	std::function<void( BitmapIterator_i * pIterator )> fnDeleter = [&]( BitmapIterator_i * pIterator ){ if ( pIterator ) { assert(dRes.empty()); dRes.push_back(pIterator); } };
	RowidRange_t * pBounds = m_bHaveBounds ? &m_tBounds : nullptr;
	std::unique_ptr<BitmapIterator_i, decltype(fnDeleter)> pBitmapIterator ( SpawnBitmapIterator ( pBounds, tFilter.m_bExclude ), fnDeleter );
	if ( pBitmapIterator && m_iCutoff>=0 )
		pBitmapIterator->SetCutoff(m_iCutoff);

	std::unique_ptr<BlockIteratorWithSetup_i> pCommonIterator;
	for ( auto & i : dIt )
		CreateBlocksIterator ( i, [this, &dRes, &pBitmapIterator, &pCommonIterator]( int iBlock, int iItem ){ AddIterator ( iBlock, iItem, dRes, pBitmapIterator.get(), pCommonIterator ); } );

	if ( tFilter.m_bExclude )
		pBitmapIterator->Invert(pBounds);
}

template<typename VALUE, typename STORED_VALUE>
uint32_t BlockReader_T<VALUE, STORED_VALUE>::CalcValueCount ( const std::vector<BlockIter_t> & dIt )
{
	uint32_t uCount = 0;
	for ( auto & i : dIt )
		CreateBlocksIterator ( i, [this, &uCount]( int iBlock, int iItem ){ uCount += CountValues ( iBlock, iItem ); } );

	return uCount;
}

template<typename VALUE, typename STORED_VALUE>
void BlockReader_T<VALUE, STORED_VALUE>::LoadBlockDataToCache ( int iBlock, bool bOnlyCount )
{
	if ( m_iLoadedDataBlockId==m_iLoadedValuesBlockId )
		return;

	if ( m_pBlockCache )
	{
		m_pBlockData = m_pBlockCache->GetData(iBlock);
		if ( m_pBlockData )
		{
			m_iLoadedDataBlockId = m_iLoadedValuesBlockId;
			return;
		}
	}

	// seek right after values to load the rest of the block content as only values could be loaded
	assert(m_pBlockValues);
	m_pReader->Seek ( m_pBlockValues->m_iOffPastValues );
	m_tBlockData.Load ( m_pBlockValues->m_dValues.size(), m_uVersion, bOnlyCount, m_dBufTmp, *m_pReader.get(), m_pCodec.get() );
	m_iLoadedDataBlockId = m_iLoadedValuesBlockId;

	if ( m_pBlockCache )
		m_pBlockCache->AddData ( iBlock, m_tBlockData );

	m_pBlockData = &m_tBlockData;
}

template<typename VALUE, typename STORED_VALUE>
BlockIteratorWithSetup_i * BlockReader_T<VALUE, STORED_VALUE>::CreateIterator ( int iBlock, int iItem, bool bBitmap )
{
	LoadBlockDataToCache ( iBlock, false );
	return CreateRowidIterator ( m_sAttr, (Packing_e)m_pBlockData->m_dTypes[iItem], m_pBlockData->m_iMetaOffset + m_pBlockData->m_dRowStart[iItem], m_pBlockData->m_dMin[iItem], m_pBlockData->m_dMax[iItem], m_pBlockData->m_dCount[iItem], m_uRowidsPerBlock, m_pReader, m_pCodec, m_bHaveBounds ? &m_tBounds : nullptr, bBitmap );
}

template<typename VALUE, typename STORED_VALUE>
bool BlockReader_T<VALUE, STORED_VALUE>::SetupExistingIterator ( BlockIteratorWithSetup_i * pIterator, int iBlock, int iItem )
{
	LoadBlockDataToCache ( iBlock, false );
	return SetupRowidIterator ( pIterator, (Packing_e)m_pBlockData->m_dTypes[iItem], m_pBlockData->m_iMetaOffset+m_pBlockData->m_dRowStart[iItem], m_pBlockData->m_dMin[iItem], m_pBlockData->m_dMax[iItem], m_pBlockData->m_dCount[iItem], m_bHaveBounds ? &m_tBounds : nullptr );
}

template<typename VALUE, typename STORED_VALUE>
uint32_t BlockReader_T<VALUE, STORED_VALUE>::CountValues ( int iBlock, int iItem )
{
	LoadBlockDataToCache ( iBlock, true );
	return m_pBlockData->m_dCount[iItem];
}

template<>
FindValueResult_t BlockReader_T<uint32_t, uint32_t>::FindValue ( uint64_t uRefVal ) const
{
	assert(m_pBlockValues);
	const auto & dValues = m_pBlockValues->m_dValues;

	uint32_t uVal = uRefVal;
	int iItem = binary_search_idx ( dValues, uVal );
	if ( iItem!=-1 )
		return FindValueResult_t { iItem, 0 };

	if ( !dValues.size() || ( dValues.front()<=uVal && uVal<=dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, dValues.back()<uVal ? 1 : -1 };
}

template<>
FindValueResult_t BlockReader_T<uint64_t, int64_t>::FindValue ( uint64_t uRefVal ) const
{
	assert(m_pBlockValues);
	const auto & dValues = m_pBlockValues->m_dValues;

	const auto tFirst = dValues.begin();
	const auto tLast = dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, uRefVal, Int64ValueCmp_t() );
	if ( tFound!=tLast && *tFound==uRefVal )
		return FindValueResult_t { (int)( tFound-tFirst ), 0 };

	if ( !dValues.size() || ( (int64_t)dValues.front()<=(int64_t)uRefVal && (int64_t)uRefVal<=(int64_t)dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, (int64_t)dValues.back()<(int64_t)uRefVal ? 1 : -1 };
}

template<>
FindValueResult_t BlockReader_T<uint64_t, uint64_t>::FindValue ( uint64_t uRefVal ) const
{
	assert(m_pBlockValues);
	const auto & dValues = m_pBlockValues->m_dValues;

	const auto tFirst = dValues.begin();
	const auto tLast = dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, uRefVal );
	if ( tFound!=tLast && *tFound==uRefVal )
		return FindValueResult_t { (int)( tFound-tFirst ), 0 };

	if ( !dValues.size() || ( dValues.front()<=uRefVal && uRefVal<=dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, dValues.back()<uRefVal ? 1 : -1 };
}

template<>
FindValueResult_t BlockReader_T<uint32_t, float>::FindValue ( uint64_t uRefVal ) const
{
	assert(m_pBlockValues);
	const auto & dValues = m_pBlockValues->m_dValues;

	float fVal = UintToFloat ( uRefVal );
	const auto tFirst = dValues.begin();
	const auto tLast = dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, fVal, FloatValueCmp_t() );

	if ( tFound!=tLast && FloatEqual ( *tFound, fVal ) )
		return FindValueResult_t { (int)( tFound-tFirst ), 0 };

	if ( !dValues.size() || ( dValues.front()<=fVal && fVal<=dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, dValues.back()<fVal ? 1 : -1 };
}

/////////////////////////////////////////////////////////////////////

template<typename T>
static int CmpRange ( T tStart, T tEnd, const Filter_t & tRange )
{
	if ( tRange.m_bLeftUnbounded && tRange.m_bRightUnbounded )
		return 0;

	Interval_T<T> tIntBlock ( tStart, tEnd );

	Interval_T<T> tIntRange;
	if ( std::is_floating_point<T>::value )
		tIntRange = Interval_T<T> ( tRange.m_fMinValue, tRange.m_fMaxValue );
	else
		tIntRange = Interval_T<T> ( tRange.m_iMinValue, tRange.m_iMaxValue );

	if ( tRange.m_bLeftUnbounded )
		tIntRange.m_tStart = std::numeric_limits<T>::lowest();
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
				RangeReader_c ( const ReaderFactory_c & tCtx, std::shared_ptr<IntCodec_i> & pCodec );

	void		CreateBlocksIterator ( const std::vector<BlockIter_t> & dIt, const Filter_t & tFilter, std::vector<BlockIterator_i *> & dRes ) override { assert ( 0 && "Requesting block iterators from range reader" ); }
	void		CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tFilter, std::vector<BlockIterator_i *> & dRes ) override;
	uint32_t	CalcValueCount ( const std::vector<BlockIter_t> & dIt ) override { assert ( 0 && "Requesting block iterators from range reader" ); return 0; }
	uint32_t	CalcValueCount ( const BlockIter_t & tIt, const common::Filter_t & tRange ) override;

protected:
	struct BlockCtx_t
	{
		int m_iValCur = 0;
		int m_iValCount = 0;
		int m_iBlockItCreated = -1;
		int m_iBlockCur = 0;
		
		BlockCtx_t ( int iStart ) : m_iBlockCur(iStart) {}
	};

	uint32_t	m_uNumValues = 0;
	std::shared_ptr<FileReader_c> m_pOffReader { nullptr };
		
	// interface for value related methods
	virtual int			LoadValues ( uint32_t uNumValues ) = 0;
	virtual bool		EvalRangeValue ( int iItem, const Filter_t & tRange ) const = 0;
	virtual int			CmpBlock ( const Filter_t & tRange ) const = 0;

	BlockIteratorWithSetup_i * CreateIterator ( int iItem, bool bLoad, bool bBitmap );
	bool				SetupExistingIterator ( BlockIteratorWithSetup_i * pIterator, int iItem, bool bLoad );
	uint32_t			CountValues ( int iItem, bool bLoad );
	bool				AddIterator ( int iValCur, bool bLoad, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator, std::unique_ptr<BlockIteratorWithSetup_i> & pCommonIterator );

	template <typename ADDITERATOR>
	bool				WarmupBlockIterators ( BlockCtx_t & tCtx, const Filter_t & tRange, ADDITERATOR && fnAddIterator );

	template <typename ADDITERATOR>
	bool				AddBlockIterators ( BlockCtx_t & tCtx, const Filter_t & tRange, ADDITERATOR && fnAddIterator );

	template <typename ADDITERATOR>
	void				CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tRange, ADDITERATOR && fnAddIterator );
};


RangeReader_c::RangeReader_c ( const ReaderFactory_c & tCtx, std::shared_ptr<IntCodec_i> & pCodec )
 	: ReaderTraits_c ( tCtx, pCodec )
 	, m_pOffReader ( std::make_shared<FileReader_c>( tCtx.m_iFD, OFF_READER_BUFFER_SIZE ) )
{}


bool RangeReader_c::AddIterator ( int iValCur, bool bLoad, std::vector<BlockIterator_i *> & dRes, BitmapIterator_i * pBitmapIterator, std::unique_ptr<BlockIteratorWithSetup_i> & pCommonIterator )
{
	// reuse previous iterator if pBitmapIterator is present
	bool bBitmap = !!pBitmapIterator;
	if ( bBitmap )
	{
		if ( !pCommonIterator )
		{
			pCommonIterator = std::unique_ptr<BlockIteratorWithSetup_i> ( CreateIterator ( iValCur, bLoad, bBitmap ) );
			if ( !pCommonIterator )
				return true;
		}
		else
		{
			if ( !SetupExistingIterator ( pCommonIterator.get(), iValCur, bLoad ) )
				return true;
		}

		pBitmapIterator->Add ( pCommonIterator.get() );
		return !pBitmapIterator->WasCutoffHit();
	}

	std::unique_ptr<BlockIterator_i> pIterator ( CreateIterator ( iValCur, bLoad, bBitmap ) );
	if ( !pIterator )
		return true;

	dRes.push_back ( pIterator.release() );
	return true;
}

template <typename ADDITERATOR>
bool RangeReader_c::WarmupBlockIterators ( BlockCtx_t & tCtx, const Filter_t & tRange, ADDITERATOR && fnAddIterator )
{
	for ( tCtx.m_iValCur=0; tCtx.m_iValCur<tCtx.m_iValCount; tCtx.m_iValCur++ )
		if ( EvalRangeValue ( tCtx.m_iValCur, tRange ) )
		{
			if ( !fnAddIterator ( tCtx.m_iValCur, true ) )
				return false;

			tCtx.m_iBlockItCreated = tCtx.m_iBlockCur;
			tCtx.m_iValCur++;
			break;
		}

	return true;
}

template <typename ADDITERATOR>
bool RangeReader_c::AddBlockIterators ( BlockCtx_t & tCtx, const Filter_t & tRange, ADDITERATOR && fnAddIterator )
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

			if ( !fnAddIterator ( tCtx.m_iValCur, tCtx.m_iBlockItCreated!=tCtx.m_iBlockCur ) )
				return false;

			tCtx.m_iBlockItCreated = tCtx.m_iBlockCur;
		}

		return false;
	}

	// case: all values till the end of the block matched
	for ( ; tCtx.m_iValCur<tCtx.m_iValCount; tCtx.m_iValCur++ )
	{
		if ( !fnAddIterator ( tCtx.m_iValCur, tCtx.m_iBlockItCreated!=tCtx.m_iBlockCur ) )
			return false;

		tCtx.m_iBlockItCreated = tCtx.m_iBlockCur;
	}

	return true;
}

template <typename ADDITERATOR>
void RangeReader_c::CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tRange, ADDITERATOR && fnAddIterator )
{
	BlockCtx_t tCtx ( tIt.m_iStart );
	m_pOffReader->Seek ( m_uBlockBaseOff + tCtx.m_iBlockCur*sizeof(uint64_t) );

	// warmup
	for ( ; tCtx.m_iBlockCur<=tIt.m_iLast; tCtx.m_iBlockCur++ )
	{
		uint64_t uBlockOff = m_pOffReader->Read_uint64();
		m_pReader->Seek ( uBlockOff );

		tCtx.m_iValCount = LoadValues ( CalcNumBlockValues ( tCtx.m_iBlockCur ) );

		int iCmpLast = CmpBlock ( tRange );
		if ( iCmpLast==1 )
			break;
		if ( iCmpLast==-1 )
			continue;

		if ( !WarmupBlockIterators ( tCtx, tRange, fnAddIterator ) )
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
		if ( !AddBlockIterators ( tCtx, tRange, fnAddIterator ) )
			return;

		tCtx.m_iBlockCur++;
		if ( tCtx.m_iBlockCur>tIt.m_iLast )
			break;

		uint64_t uBlockOff = m_pOffReader->Read_uint64();
		m_pReader->Seek ( uBlockOff );

		tCtx.m_iValCount = LoadValues ( CalcNumBlockValues ( tCtx.m_iBlockCur ) );
		tCtx.m_iValCur = 0;
		assert ( tCtx.m_iValCount );

		// matching is over
		if ( !EvalRangeValue ( 0, tRange ) )
			break;
	}
}


void RangeReader_c::CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tFilter, std::vector<BlockIterator_i *> & dRes )
{
	// add bitmap iterator as 1st element of dRes on exit
	std::function<void( BitmapIterator_i * pIterator )> fnDeleter = [&]( BitmapIterator_i * pIterator ){ if ( pIterator ) { assert(dRes.empty()); dRes.push_back(pIterator); } };
	RowidRange_t * pBounds = m_bHaveBounds ? &m_tBounds : nullptr;
	std::unique_ptr<BitmapIterator_i, decltype(fnDeleter)> pBitmapIterator ( SpawnBitmapIterator ( pBounds, tFilter.m_bExclude ), fnDeleter );
	if ( pBitmapIterator && m_iCutoff>=0 )
		pBitmapIterator->SetCutoff(m_iCutoff);

	std::unique_ptr<BlockIteratorWithSetup_i> pCommonIterator;
	CreateBlocksIterator ( tIt, tFilter, [this, &dRes, &pBitmapIterator, &pCommonIterator]( int iValCur, bool bLoad ){ return AddIterator ( iValCur, bLoad, dRes, pBitmapIterator.get(), pCommonIterator ); } );

	if ( tFilter.m_bExclude )
		pBitmapIterator->Invert(pBounds);
}


uint32_t RangeReader_c::CalcValueCount ( const BlockIter_t & tIt, const common::Filter_t & tRange )
{
	uint32_t uCount = 0;
	CreateBlocksIterator ( tIt, tRange, [this, &uCount]( int iItem, bool bLoad ){ uCount += CountValues ( iItem, bLoad ); return true; } );
	return uCount;
}


BlockIteratorWithSetup_i * RangeReader_c::CreateIterator ( int iItem, bool bLoad, bool bBitmap )
{
	if ( bLoad )
		m_tBlockData.Load ( m_uNumValues, m_uVersion, false, m_dBufTmp, *m_pReader.get(), m_pCodec.get() );

	return CreateRowidIterator ( m_sAttr, (Packing_e)m_tBlockData.m_dTypes[iItem], m_tBlockData.m_iMetaOffset + m_tBlockData.m_dRowStart[iItem], m_tBlockData.m_dMin[iItem], m_tBlockData.m_dMax[iItem], m_tBlockData.m_dCount[iItem], m_uRowidsPerBlock, m_pReader, m_pCodec, m_bHaveBounds ? &m_tBounds : nullptr, bBitmap );
}


bool RangeReader_c::SetupExistingIterator ( BlockIteratorWithSetup_i * pIterator, int iItem, bool bLoad )
{
	if ( bLoad )
		m_tBlockData.Load ( m_uNumValues, m_uVersion, false, m_dBufTmp, *m_pReader.get(), m_pCodec.get() );

	return SetupRowidIterator ( pIterator, (Packing_e)m_tBlockData.m_dTypes[iItem], m_tBlockData.m_iMetaOffset + m_tBlockData.m_dRowStart[iItem], m_tBlockData.m_dMin[iItem], m_tBlockData.m_dMax[iItem], m_tBlockData.m_dCount[iItem], m_bHaveBounds ? &m_tBounds : nullptr );
}


uint32_t RangeReader_c::CountValues ( int iItem, bool bLoad )
{
	if ( bLoad )
		m_tBlockData.Load ( m_uNumValues, m_uVersion, true, m_dBufTmp, *m_pReader.get(), m_pCodec.get() );

	return m_tBlockData.m_dCount[iItem];
}

/////////////////////////////////////////////////////////////////////

template<typename STORE_VALUE, typename DST_VALUE>
class RangeReader_T : public RangeReader_c
{
public:
	RangeReader_T ( const ReaderFactory_c & tCtx, std::shared_ptr<IntCodec_i> & pCodec );

private:
	SpanResizeable_T<STORE_VALUE> m_dValues;

	int LoadValues ( uint32_t uNumValues ) override
	{
		DecodeBlock ( m_dValues, uNumValues, m_pCodec.get(), m_dBufTmp, *m_pReader.get() );
		m_uNumValues = uNumValues;
		return m_dValues.size();
	}

	bool EvalRangeValue ( int iItem, const Filter_t & tRange ) const override
	{
		if ( tRange.m_bLeftUnbounded && tRange.m_bRightUnbounded )
			return true;

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
RangeReader_T<STORE_VALUE,DST_VALUE>::RangeReader_T ( const ReaderFactory_c & tCtx, std::shared_ptr<IntCodec_i> & pCodec )
	: RangeReader_c ( tCtx, pCodec )
{}

/////////////////////////////////////////////////////////////////////

BlockReader_i * ReaderFactory_c::CreateBlockReader()
{
	auto pCodec { std::shared_ptr<IntCodec_i> ( CreateIntCodec ( m_tSettings.m_sCompressionUINT32, m_tSettings.m_sCompressionUINT64 ) ) };
	assert(pCodec);

	switch ( m_tCol.m_eType )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
	case AttrType_e::UINT32SET:
	case AttrType_e::BOOLEAN:
		return new BlockReader_T<uint32_t, uint32_t> ( *this, pCodec );

	case AttrType_e::FLOAT:
		return new BlockReader_T<uint32_t, float> ( *this, pCodec );

	case AttrType_e::STRING:
		return new BlockReader_T<uint64_t, uint64_t> ( *this, pCodec );

	case AttrType_e::INT64:
	case AttrType_e::INT64SET:
		return new BlockReader_T<uint64_t, int64_t> ( *this, pCodec );

	default: return nullptr;
	}
}


BlockReader_i * ReaderFactory_c::CreateRangeReader()
{
	auto pCodec { std::shared_ptr<IntCodec_i> ( CreateIntCodec ( m_tSettings.m_sCompressionUINT32, m_tSettings.m_sCompressionUINT64 ) ) };
	assert(pCodec);

	switch ( m_tCol.m_eType )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
	case AttrType_e::UINT32SET:
	case AttrType_e::BOOLEAN:
		return new RangeReader_T<uint32_t, uint32_t> ( *this, pCodec );

	case AttrType_e::FLOAT:
		return new RangeReader_T<uint32_t, float> ( *this, pCodec );

	case AttrType_e::STRING:
		return new RangeReader_T<uint64_t, uint64_t> ( *this, pCodec );

	case AttrType_e::INT64:
	case AttrType_e::INT64SET:
		return new RangeReader_T<uint64_t, int64_t> ( *this, pCodec );

	default: return nullptr;
	}
}


BlockCache_i * CreateBlockCache ( common::AttrType_e eType, uint32_t uBlocksCount, uint64_t uMaxSize )
{
	switch ( eType )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
	case AttrType_e::UINT32SET:
	case AttrType_e::BOOLEAN:
		return new BlockCache_T<uint32_t> ( uBlocksCount, uMaxSize );

	case AttrType_e::STRING:
	case AttrType_e::INT64:
	case AttrType_e::INT64SET:
		return new BlockCache_T<uint64_t> ( uBlocksCount, uMaxSize );

	default: return nullptr;
	}
}

}
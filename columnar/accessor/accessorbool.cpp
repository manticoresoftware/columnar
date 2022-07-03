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

#include "accessorbool.h"
#include "accessortraits.h"
#include "builderbool.h"

#include "columnar.h"
#include "check.h"
#include "interval.h"
#include "reader.h"
#include <algorithm>

namespace columnar
{

using namespace util;
using namespace common;

class StoredBlock_Bool_Const_c
{
public:
	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader ) { m_bValue = !!tReader.Read_uint8(); }
	FORCE_INLINE int64_t	GetValue() const { return m_bValue; }

private:
	bool	m_bValue = false;
};


class StoredBlock_Bool_Bitmap_c
{
public:
							StoredBlock_Bool_Bitmap_c ( int iSubblockSize );

	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader, uint32_t uDocsInBlock );
	FORCE_INLINE void		ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader );
	FORCE_INLINE int64_t	GetValue ( int iIdInSubblock );
	FORCE_INLINE const Span_T<uint32_t> & GetValues() const { return m_tValuesRead; }

private:
	std::vector<uint32_t>	m_dValues;
	std::vector<uint32_t>	m_dEncoded;
	int64_t					m_iValuesOffset = 0;
	int						m_iSubblockId = -1;
	Span_T<uint32_t>		m_tValuesRead;
};


StoredBlock_Bool_Bitmap_c::StoredBlock_Bool_Bitmap_c ( int iSubblockSize )
{
	assert ( !( iSubblockSize & 127 ) );
	m_dValues.resize(iSubblockSize);
	m_dEncoded.resize ( iSubblockSize >> 5 );
}


void StoredBlock_Bool_Bitmap_c::ReadHeader ( FileReader_c & tReader, uint32_t uDocsInBlock )
{
	m_iValuesOffset = tReader.GetPos();
	m_iSubblockId = -1;
}


void StoredBlock_Bool_Bitmap_c::ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;

	size_t uPackedSize = m_dEncoded.size()*sizeof ( m_dEncoded[0] );
	tReader.Seek ( m_iValuesOffset + uPackedSize*iSubblockId );
	tReader.Read ( (uint8_t*)m_dEncoded.data(), uPackedSize );
	BitUnpack ( m_dEncoded, m_dValues, 1 );

	m_tValuesRead = { m_dValues.data(), (size_t)iNumValues };
}


int64_t StoredBlock_Bool_Bitmap_c::GetValue ( int iIdInSubblock )
{
	return m_dValues[iIdInSubblock];
}

//////////////////////////////////////////////////////////////////////////

class Accessor_Bool_c : public StoredBlockTraits_t
{
public:
						Accessor_Bool_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader );

	FORCE_INLINE void	SetCurBlock ( uint32_t uBlockId );

protected:
	const AttributeHeader_i &		m_tHeader;
	std::unique_ptr<FileReader_c>	m_pReader;

	StoredBlock_Bool_Const_c		m_tBlockConst;
	StoredBlock_Bool_Bitmap_c		m_tBlockBitmap;

	int64_t (Accessor_Bool_c::*m_fnReadValue)() = nullptr;

	BoolPacking_e		m_ePacking = BoolPacking_e::CONST;

	int64_t			ReadValue_Const();
	int64_t			ReadValue_Bitmap();
};


Accessor_Bool_c::Accessor_Bool_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader )
	: StoredBlockTraits_t ( tHeader.GetSettings().m_iSubblockSize )
	, m_tHeader ( tHeader )
	, m_pReader ( pReader )
	, m_tBlockBitmap ( tHeader.GetSettings().m_iSubblockSize )
{
	assert(pReader);
}


void Accessor_Bool_c::SetCurBlock ( uint32_t uBlockId )
{
	m_pReader->Seek ( m_tHeader.GetBlockOffset(uBlockId) );
	m_ePacking = (BoolPacking_e)m_pReader->Unpack_uint32();

	m_tRequestedRowID = INVALID_ROW_ID;

	uint32_t uDocsInBlock = m_tHeader.GetNumDocs(uBlockId);

	switch ( m_ePacking )
	{
	case BoolPacking_e::CONST:
		m_fnReadValue = &Accessor_Bool_c::ReadValue_Const;
		m_tBlockConst.ReadHeader ( *m_pReader );
		break;

	case BoolPacking_e::BITMAP:
		m_fnReadValue = &Accessor_Bool_c::ReadValue_Bitmap;
		m_tBlockBitmap.ReadHeader ( *m_pReader, uDocsInBlock );
		break;

	default:
		assert ( 0 && "Packing not implemented yet" );
	}

	SetBlockId ( uBlockId, uDocsInBlock );
}


int64_t Accessor_Bool_c::ReadValue_Const()
{
	return m_tBlockConst.GetValue();
}


int64_t Accessor_Bool_c::ReadValue_Bitmap()
{
	uint32_t uIdInBlock = m_tRequestedRowID - m_tStartBlockRowId;
	int iSubblockId = StoredBlockTraits_t::GetSubblockId(uIdInBlock);
	m_tBlockBitmap.ReadSubblock ( iSubblockId, StoredBlockTraits_t::GetNumSubblockValues(iSubblockId), *m_pReader );
	return m_tBlockBitmap.GetValue ( GetValueIdInSubblock(uIdInBlock) );
}

//////////////////////////////////////////////////////////////////////////

class Iterator_Bool_c : public Iterator_i, public Accessor_Bool_c
{
	using BASE = Accessor_Bool_c;
	using BASE::Accessor_Bool_c;

public:
	uint32_t	AdvanceTo ( uint32_t tRowID ) final		{ return DoAdvance(tRowID); }
	int64_t		Get() final								{ return DoGet(); }

	void		Fetch ( const Span_T<uint32_t> & dRowIDs, Span_T<int64_t> & dValues ) final;

	int			Get ( const uint8_t * & pData ) final	{ assert ( 0 && "INTERNAL ERROR: requesting blob from bool iterator" ); return 0; }
	uint8_t *	GetPacked() final						{ assert ( 0 && "INTERNAL ERROR: requesting blob from bool iterator" ); return nullptr; }
	int			GetLength() final						{ assert ( 0 && "INTERNAL ERROR: requesting string length from bool iterator" ); return 0; }

	void		AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const override { dDesc.push_back ( { m_tHeader.GetName(), "iterator" } ); };

private:
	FORCE_INLINE uint32_t	DoAdvance ( uint32_t tRowID );
	FORCE_INLINE int64_t	DoGet();
};


uint32_t Iterator_Bool_c::DoAdvance ( uint32_t tRowID )
{
	assert ( tRowID < BASE::m_tHeader.GetNumDocs() );

	uint32_t uBlockId = RowId2BlockId(tRowID);
	if ( uBlockId!=BASE::m_uBlockId )
		BASE::SetCurBlock(uBlockId);

	BASE::m_tRequestedRowID = tRowID;

	return tRowID;
}


void Iterator_Bool_c::Fetch ( const Span_T<uint32_t> & dRowIDs, Span_T<int64_t> & dValues )
{
	uint32_t * pRowID = dRowIDs.begin();
	uint32_t * pRowIDEnd = dRowIDs.end();
	int64_t * pValue = dValues.begin();
	while ( pRowID<pRowIDEnd )
	{
		DoAdvance ( *pRowID++ );
		*pValue++ = DoGet();
	}
}


int64_t Iterator_Bool_c::DoGet()
{
	assert ( BASE::m_fnReadValue );
	return (*this.*BASE::m_fnReadValue)();
}

//////////////////////////////////////////////////////////////////////////

class AnalyzerBlock_Bool_Const_c
{
public:
						AnalyzerBlock_Bool_Const_c ( uint32_t & tRowID ) : m_tRowID ( tRowID ) {}

	FORCE_INLINE bool	SetupNextBlock ( const StoredBlock_Bool_Const_c & tBlock );
	FORCE_INLINE int	ProcessSubblock ( uint32_t * & pRowID, int iNumValues );
	void				Setup ( bool bFilterValue ) { m_bFilterValue=bFilterValue; }

private:
	uint32_t &			m_tRowID;
	bool				m_bFilterValue = false;
};


bool AnalyzerBlock_Bool_Const_c::SetupNextBlock ( const StoredBlock_Bool_Const_c & tBlock )
{
	int64_t tValue = tBlock.GetValue();
	return m_bFilterValue==!!tValue;
}


int AnalyzerBlock_Bool_Const_c::ProcessSubblock ( uint32_t * & pRowID, int iNumValues )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	for ( int i = 0; i < iNumValues; i++ )
		*pRowID++ = tRowID++;

	m_tRowID = tRowID;
	return iNumValues;
}

//////////////////////////////////////////////////////////////////////////

class AnalyzerBlock_Bool_Bitmap_c
{
public:
						AnalyzerBlock_Bool_Bitmap_c ( uint32_t & tRowID ) : m_tRowID ( tRowID ) {}

	FORCE_INLINE int	ProcessSubblock ( uint32_t * & pRowID, const Span_T<uint32_t> & dValues );
	void				Setup ( bool bFilterValue ) { m_bFilterValue=bFilterValue; }

private:
	uint32_t &			m_tRowID;
	bool				m_bFilterValue = false;
};


int AnalyzerBlock_Bool_Bitmap_c::ProcessSubblock ( uint32_t * & pRowID, const Span_T<uint32_t> & dValues )
{
	uint32_t tRowID = m_tRowID;

	uint32_t uFilterValue = m_bFilterValue ? 1 : 0;
	for ( auto i : dValues )
	{
		if ( i == uFilterValue )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;

	return (int)dValues.size();
}

//////////////////////////////////////////////////////////////////////////

template <bool HAVE_MATCHING_BLOCKS>
class Analyzer_Bool_T : public Analyzer_T<HAVE_MATCHING_BLOCKS>, public Accessor_Bool_c
{
	using ANALYZER = Analyzer_T<HAVE_MATCHING_BLOCKS>;
	using ACCESSOR = Accessor_Bool_c;

public:
				Analyzer_Bool_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings );

	bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) final;
	void		AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const final { dDesc.push_back ( { ACCESSOR::m_tHeader.GetName(), "analyzer" } ); }

private:
	bool		m_bAcceptFalse = false;
	bool		m_bAcceptTrue = false;

	AnalyzerBlock_Bool_Const_c	m_tBlockConst;
	AnalyzerBlock_Bool_Bitmap_c	m_tBlockBitmap;

	const Filter_t & m_tSettings;

	typedef int (Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::*ProcessSubblock_fn)( uint32_t * & pRowID, int iSubblockIdInBlock );
	std::array<ProcessSubblock_fn,to_underlying(BoolPacking_e::TOTAL)> m_dProcessingFuncs;
	ProcessSubblock_fn	m_fnProcessSubblock = nullptr;

	void		AnalyzeFilter();
	void		SetupPackingFuncs();

	int			ProcessSubblockConst ( uint32_t * & pRowID, int iSubblockIdInBlock );
	int			ProcessSubblockBitmap ( uint32_t * & pRowID, int iSubblockIdInBlock );
	int			ProcessSubblockAny ( uint32_t * & pRowID, int iSubblockIdInBlock )	{ return m_tBlockConst.ProcessSubblock ( pRowID, ACCESSOR::GetNumSubblockValues(iSubblockIdInBlock) ); }
	int			ProcessSubblockNone ( uint32_t * & pRowID, int iSubblockIdInBlock )	{ return iSubblockIdInBlock; }

	bool		MoveToBlock ( int iNextBlock ) final;
};

template <bool HAVE_MATCHING_BLOCKS>
Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::Analyzer_Bool_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings )
	: ANALYZER ( tHeader.GetSettings().m_iSubblockSize )
	, ACCESSOR ( tHeader, pReader )
	, m_tBlockConst ( ANALYZER::m_tRowID )
	, m_tBlockBitmap ( ANALYZER::m_tRowID )
	, m_tSettings ( tSettings )
{
	SetupPackingFuncs();
}

template <bool HAVE_MATCHING_BLOCKS>
void Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::AnalyzeFilter()
{
	m_bAcceptFalse = false;
	m_bAcceptTrue = false;

	switch ( m_tSettings.m_eType )
	{
	case FilterType_e::VALUES:
		for ( auto i : m_tSettings.m_dValues )
		{
			m_bAcceptFalse |= i==0;
			m_bAcceptTrue  |= i!=0;
		}
		break;

	case FilterType_e::RANGE:
		m_bAcceptFalse = ValueInInterval ( 0, m_tSettings );
		m_bAcceptTrue =  ValueInInterval ( 1, m_tSettings );
		break;

	default:
		assert ( 0 && "Unknown filter type");
		break;
	}

	if ( m_tSettings.m_bExclude )
	{
		m_bAcceptFalse = !m_bAcceptFalse;
		m_bAcceptTrue = !m_bAcceptTrue;
	}
}

template <bool HAVE_MATCHING_BLOCKS>
void Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::SetupPackingFuncs()
{
	auto & dFuncs = m_dProcessingFuncs;
	for ( auto & i : dFuncs )
		i = nullptr;

	AnalyzeFilter();

	if ( m_bAcceptFalse && m_bAcceptTrue )
	{
		dFuncs[ to_underlying ( BoolPacking_e::CONST ) ] = &Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::ProcessSubblockAny;
		dFuncs[ to_underlying ( BoolPacking_e::BITMAP) ] = &Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::ProcessSubblockAny;
	}
	else if ( !m_bAcceptFalse && !m_bAcceptTrue )
	{
		dFuncs [ to_underlying ( BoolPacking_e::CONST  ) ] = &Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::ProcessSubblockNone;
		dFuncs [ to_underlying ( BoolPacking_e::BITMAP ) ] = &Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::ProcessSubblockNone;
	}
	else
	{
		dFuncs [ to_underlying ( BoolPacking_e::CONST  ) ] = &Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::ProcessSubblockConst;
		dFuncs [ to_underlying ( BoolPacking_e::BITMAP ) ] = &Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::ProcessSubblockBitmap;

		m_tBlockConst.Setup(m_bAcceptTrue);
		m_tBlockBitmap.Setup(m_bAcceptTrue);
	}
}

template <bool HAVE_MATCHING_BLOCKS>
int Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::ProcessSubblockConst ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	return m_tBlockConst.ProcessSubblock ( pRowID, ACCESSOR::GetNumSubblockValues(iSubblockIdInBlock) );
}

template <bool HAVE_MATCHING_BLOCKS>
int Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::ProcessSubblockBitmap ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockBitmap.ReadSubblock ( iSubblockIdInBlock, StoredBlockTraits_t::GetNumSubblockValues(iSubblockIdInBlock), *ACCESSOR::m_pReader );
	return m_tBlockBitmap.ProcessSubblock ( pRowID, ACCESSOR::m_tBlockBitmap.GetValues() );
}

template <bool HAVE_MATCHING_BLOCKS>
bool Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	return ANALYZER::GetNextRowIdBlock ( (ACCESSOR&)*this, dRowIdBlock, [this] ( uint32_t * & pRowID, int iSubblockIdInBlock ){ return (*this.*m_fnProcessSubblock) ( pRowID, iSubblockIdInBlock ); } );
}

template <bool HAVE_MATCHING_BLOCKS>
bool Analyzer_Bool_T<HAVE_MATCHING_BLOCKS>::MoveToBlock ( int iNextBlock )
{
	while(true)
	{
		ANALYZER::StartBlockProcessing ( (ACCESSOR&)*this, iNextBlock );

		if ( m_bAcceptFalse && m_bAcceptTrue )
			break;

		if ( !m_bAcceptFalse && !m_bAcceptTrue )
			return false;

		if ( ACCESSOR::m_ePacking!=BoolPacking_e::CONST )
			break;

		if ( m_tBlockConst.SetupNextBlock ( ACCESSOR::m_tBlockConst ) )
			break;

		if ( !ANALYZER::RewindToNextBlock ( (ACCESSOR&)*this, iNextBlock ) )
			return false;
	}

	m_fnProcessSubblock = m_dProcessingFuncs [ to_underlying ( ACCESSOR::m_ePacking ) ];
	assert ( m_fnProcessSubblock );

	return true;
}


Iterator_i * CreateIteratorBool ( const AttributeHeader_i & tHeader, FileReader_c * pReader )
{
	return new Iterator_Bool_c ( tHeader, pReader );
}


Analyzer_i * CreateAnalyzerBool ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings, bool bHaveMatchingBlocks )
{
	if ( bHaveMatchingBlocks )
		return new Analyzer_Bool_T<true> ( tHeader, pReader, tSettings );
	else
		return new Analyzer_Bool_T<false> ( tHeader, pReader, tSettings );
}

//////////////////////////////////////////////////////////////////////////

class Checker_Bool_c : public Checker_c
{
	using BASE=Checker_c;
	using BASE::BASE;

private:
	bool	CheckBlockHeader ( uint32_t uBlockId ) override;
};


bool Checker_Bool_c::CheckBlockHeader ( uint32_t uBlockId )
{
	uint32_t uPacking = m_pReader->Unpack_uint32();
	if ( uPacking!=(uint32_t)BoolPacking_e::CONST && uPacking!=(uint32_t)BoolPacking_e::BITMAP )
	{
		m_fnError ( FormatStr ( "Unknown encoding of block %u: %u", uBlockId, uPacking ).c_str() );
		return false;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////

Checker_i * CreateCheckerBool ( const AttributeHeader_i & tHeader, FileReader_c * pReader, Reporter_fn & fnProgress, Reporter_fn & fnError )
{
	return new Checker_Bool_c ( tHeader, pReader, fnProgress, fnError );
}

} // namespace columnar
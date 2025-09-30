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

#include "secondary.h"

#include "pgm.h"
#include "delta.h"
#include "codec.h"
#include "blockreader.h"
#include "bitvec.h"

#include <unordered_map>

namespace SI
{

using namespace util;
using namespace common;

template <typename T>
static void LoadTypeLimits ( PGM_i * pPGM, ApproxPos_t & tFoundMin, ApproxPos_t & tFoundMax )
{
	assert(pPGM);
	tFoundMin = pPGM->Search ( std::numeric_limits<T>::min() );
	tFoundMax = pPGM->Search ( std::numeric_limits<T>::max() );
}

template <>
void LoadTypeLimits<float> ( PGM_i * pPGM, ApproxPos_t & tFoundMin, ApproxPos_t & tFoundMax )
{
	assert(pPGM);
	tFoundMin = pPGM->Search ( FloatToUint ( std::numeric_limits<float>::min() ) );
	tFoundMax = pPGM->Search ( FloatToUint ( std::numeric_limits<float>::max() ) );
}


static void SetupFullscanLimits ( AttrType_e eType, PGM_i * pPGM, ApproxPos_t & tFoundMin, ApproxPos_t & tFoundMax )
{
	assert(pPGM);

	switch ( eType )
	{
	case AttrType_e::FLOAT:
	case AttrType_e::FLOATVEC:
		LoadTypeLimits<float> ( pPGM, tFoundMin, tFoundMax );
		break;

	case AttrType_e::STRING:
		LoadTypeLimits<uint64_t> ( pPGM, tFoundMin, tFoundMax );
		break;

	case AttrType_e::INT64:
	case AttrType_e::INT64SET:
		LoadTypeLimits<int64_t> ( pPGM, tFoundMin, tFoundMax );
		break;

	default:
		LoadTypeLimits<uint32_t> ( pPGM, tFoundMin, tFoundMax );
		break;
	}
}

////////////////////////////////////////////////////////////////////

class SecondaryIndex_c : public Index_i
{
public:
				SecondaryIndex_c ( const SI::IndexSettings_t & tSettings ) : m_uMaxBlockCacheSize ( tSettings.m_uBlockCacheSize ) {}

	bool		Setup ( const std::string & sFile, std::string & sError );

	bool		CreateIterators ( std::vector<BlockIterator_i *> & dIterators, const Filter_t & tFilter, const IteratorSettings_t & tSettings, std::string & sError ) const override;
	bool		CalcCount ( uint32_t & uCount, const common::Filter_t & tFilter, uint32_t uMaxValues, std::string & sError ) const override;
	uint32_t	GetNumIterators ( const common::Filter_t & tFilter ) const override;
	bool		IsEnabled ( const std::string & sName ) const override;
	int64_t		GetCountDistinct ( const std::string & sName ) const override;
	bool		SaveMeta ( std::string & sError ) override;
	void		ColumnUpdated ( const char * sName ) override;
	void		GetAttrInfo ( std::vector<IndexAttrInfo_t> & dAttrs ) const override;

private:
	Settings_t	m_tSettings;
	uint64_t	m_uMaxBlockCacheSize = 0;
	uint32_t	m_uValuesPerBlock = 1;
	uint32_t	m_uValuesPerBlockShift = 0;
	uint32_t	m_uRowidsPerBlock = 1024;

	uint64_t	m_uMetaOff { 0 };
	uint64_t	m_uNextMetaOff { 0 };

	util::FileReader_c m_tReader;

	std::vector<ColumnInfo_t> m_dAttrs;
	std::unordered_map<std::string, int> m_hAttrs;
	std::vector<std::unique_ptr<BlockCache_c>> m_dBlockCaches;
	std::vector<uint64_t> m_dBlockStartOff;			// per attribute vector of offsets to every block of values-rows-meta
	std::vector<uint64_t> m_dBlocksCount;			// per attribute vector of blocks count
	std::vector<std::shared_ptr<PGM_i>> m_dIdx;
	bool		m_bUpdated = false;
	int64_t		m_iBlocksBase = 0;					// start of offsets at file
	uint32_t	m_uVersion = 0;						// storage version

	std::string m_sFileName;

	int64_t		GetValsRows ( std::vector<BlockIterator_i *> * pIterators, const Filter_t & tFilter, const IteratorSettings_t & tSettings ) const;
	int64_t		GetRangeRows ( std::vector<BlockIterator_i *> * pIterators, const Filter_t & tFilter, const IteratorSettings_t & tSettings ) const;
	uint32_t	CalcValsRows ( const Filter_t & tFilter ) const;
	uint32_t	CalcRangeRows ( const Filter_t & tFilter ) const;
	bool		PrepareBlocksValues ( const Filter_t & tFilter, std::vector<BlockIter_t> * pBlocksIt, uint64_t & uBlockBaseOff, int64_t & iNumIterators, uint64_t & uBlocksCount ) const;
	bool		PrepareBlocksRange ( const Filter_t & tFilter, ApproxPos_t & tPos, uint64_t & uBlockBaseOff, uint64_t & uBlocksCount, int64_t & iNumIterators ) const;
	int			GetColumnId ( const std::string & sName ) const;
	const ColumnInfo_t * GetAttr ( const Filter_t & tFilter, std::string & sError ) const;
};

bool SecondaryIndex_c::Setup ( const std::string & sFile, std::string & sError )
{
	if ( !m_tReader.Open ( sFile, sError ) )
		return false;

	m_uVersion = m_tReader.Read_uint32();

	// starting with v.6 we have backward compatibility
	if ( m_uVersion<6 || m_uVersion>STORAGE_VERSION )
	{
		sError = FormatStr ( "Unable to load inverted index: %s is v.%d, binary is v.%d", sFile.c_str(), m_uVersion, STORAGE_VERSION );
		return false;
	}
		
	m_sFileName = sFile;
	m_uMetaOff = m_tReader.Read_uint64();
		
	m_tReader.Seek ( m_uMetaOff );

	// raw non packed data first
	m_uNextMetaOff = m_tReader.Read_uint64();
	int iAttrsCount = m_tReader.Read_uint32();

	BitVec_c dAttrsEnabled ( iAttrsCount );
	ReadVectorData ( dAttrsEnabled.GetData(), m_tReader );

	m_tSettings.Load ( m_tReader, m_uVersion );
	m_uValuesPerBlock = m_tReader.Read_uint32();
	m_uValuesPerBlockShift = Log2(m_uValuesPerBlock)-1;

	if ( m_uVersion>=8 )
		m_uRowidsPerBlock = m_tReader.Read_uint32();

	m_dAttrs.resize ( iAttrsCount );
	for ( int i=0; i<iAttrsCount; i++ )
	{
		ColumnInfo_t & tAttr = m_dAttrs[i];
		tAttr.Load ( m_tReader, m_uVersion );
		tAttr.m_bEnabled = dAttrsEnabled.BitGet(i);
	}

	ReadVectorPacked ( m_dBlockStartOff, m_tReader );
	ComputeInverseDeltas ( m_dBlockStartOff, true );
	ReadVectorPacked ( m_dBlocksCount, m_tReader );

	if ( m_uMaxBlockCacheSize )
	{
		for ( size_t i = 0; i < m_dAttrs.size(); i++ )
			m_dBlockCaches.push_back ( std::unique_ptr<BlockCache_c> ( CreateBlockCache ( m_dAttrs[i].m_eType, m_dBlocksCount[i], m_uMaxBlockCacheSize ) ) );
	}

	m_dIdx.resize ( m_dAttrs.size() );
	for ( int i=0; i<m_dIdx.size(); i++ )
	{
		const ColumnInfo_t & tCol = m_dAttrs[i];
		switch ( tCol.m_eType )
		{
			case AttrType_e::UINT32:
			case AttrType_e::TIMESTAMP:
			case AttrType_e::UINT32SET:
			case AttrType_e::BOOLEAN:
				m_dIdx[i].reset ( new PGM_T<uint32_t>() );
				break;

			case AttrType_e::FLOAT:
			case AttrType_e::FLOATVEC:
				m_dIdx[i].reset ( new PGM_T<float>() );
				break;

			case AttrType_e::STRING:
				m_dIdx[i].reset ( new PGM_T<uint64_t>() );
				break;

			case AttrType_e::INT64:
			case AttrType_e::INT64SET:
				m_dIdx[i].reset ( new PGM_T<int64_t>() );
				break;

			default:
				sError = FormatStr ( "Unknown attribute '%s'(%d) with type %d", tCol.m_sName.c_str(), i, tCol.m_eType );
				return false;
		}

		int64_t iPgmLen = m_tReader.Unpack_uint64();
		int64_t iPgmEnd = m_tReader.GetPos() + iPgmLen;
		m_dIdx[i]->Load ( m_tReader );
		if ( m_tReader.GetPos()!=iPgmEnd )
		{
			sError = FormatStr ( "Out of bounds on loading PGM for attribute '%s'(%d), end expected %ll got %ll", tCol.m_sName.c_str(), i, iPgmEnd, m_tReader.GetPos() );
			return false;
		}

		m_hAttrs.insert ( { tCol.m_sName, i } );
		if ( !tCol.m_sJsonParentName.empty() )
			m_hAttrs.emplace ( tCol.m_sJsonParentName, i );
	}

	m_iBlocksBase = m_tReader.GetPos();

	if ( m_tReader.IsError() )
	{
		sError = m_tReader.GetError();
		return false;
	}

	return true;
}


int SecondaryIndex_c::GetColumnId ( const std::string & sName ) const
{
	auto tIt = m_hAttrs.find ( sName );
	if ( tIt==m_hAttrs.end() )
		return -1;
		
	return tIt->second;
}


bool SecondaryIndex_c::IsEnabled ( const std::string & sName ) const
{
	int iId = GetColumnId(sName);
	if ( iId<0 )
		return false;

	auto & tCol = m_dAttrs[iId];
	return tCol.m_eType!=AttrType_e::NONE && tCol.m_bEnabled;
}


int64_t SecondaryIndex_c::GetCountDistinct ( const std::string & sName ) const
{
	int iId = GetColumnId(sName);
	if ( iId<0 )
		return -1;

	auto & tCol = m_dAttrs[iId];
	return tCol.m_bEnabled ? tCol.m_uCountDistinct : -1;
}


bool SecondaryIndex_c::SaveMeta ( std::string & sError )
{
	if ( !m_bUpdated || !m_dAttrs.size() )
		return true;

	BitVec_c dAttrEnabled ( m_dAttrs.size() );
	for ( int i=0; i<m_dAttrs.size(); i++ )
	{
		const ColumnInfo_t & tAttr = m_dAttrs[i];
		if ( tAttr.m_bEnabled )
			dAttrEnabled.BitSet ( i );
	}

	FileWriter_c tDstFile;
	if ( !tDstFile.Open ( m_sFileName, false, false, false, sError ) )
		return false;

	// seek to meta offset and skip attrbutes count
	tDstFile.Seek ( m_uMetaOff + sizeof(uint64_t) + sizeof(uint32_t) );
	WriteVector ( dAttrEnabled.GetData(), tDstFile );
	return true;
}
	
void SecondaryIndex_c::ColumnUpdated ( const char * sName )
{
	int iAttr = GetColumnId ( sName );
	if ( iAttr==-1 )
		return;
		
	ColumnInfo_t & tCol = m_dAttrs[iAttr];
	m_bUpdated |= tCol.m_bEnabled; // already disabled indexes should not cause flush
	bool bWasUpdated = tCol.m_bEnabled;
	tCol.m_bEnabled = false;

	// need to disable all fields at this JSON attribute
	if ( bWasUpdated && !tCol.m_sJsonParentName.empty() )
	{
		for ( auto & tSibling : m_dAttrs )
		{
			if ( tSibling.m_sJsonParentName==tCol.m_sJsonParentName )
				tSibling.m_bEnabled = false;
		}
	}
}


void SecondaryIndex_c::GetAttrInfo ( std::vector<IndexAttrInfo_t> & dAttrs ) const
{
	for ( const auto & i : m_dAttrs )
		dAttrs.push_back ( { i.m_sName, i.m_eType, i.m_bEnabled } );
}


bool SecondaryIndex_c::PrepareBlocksValues ( const Filter_t & tFilter, std::vector<BlockIter_t> * pBlocksIt, uint64_t & uBlockBaseOff, int64_t & iNumIterators, uint64_t & uBlocksCount ) const
{
	iNumIterators = 0;

	int iCol = GetColumnId ( tFilter.m_sName );
	assert ( iCol>=0 );

	if ( m_dIdx[iCol]->IsEmpty() )
		return false;

	// m_dBlockStartOff is 0based need to set to start of offsets vector
	uBlockBaseOff = m_iBlocksBase + m_dBlockStartOff[iCol];
	uBlocksCount = m_dBlocksCount[iCol];

	if ( pBlocksIt )
		pBlocksIt->reserve ( tFilter.m_dValues.size() );

	for ( const uint64_t uVal : tFilter.m_dValues )
	{
		ApproxPos_t tPos = m_dIdx[iCol]->Search(uVal);
		iNumIterators += tPos.m_iHi-tPos.m_iLo;
		if ( pBlocksIt )
			pBlocksIt->emplace_back ( BlockIter_t ( tPos, uVal, uBlocksCount, m_uValuesPerBlockShift ) );
	}

	// sort by block start offset
	if ( pBlocksIt )
		std::sort ( pBlocksIt->begin(), pBlocksIt->end(), [] ( const BlockIter_t & tA, const BlockIter_t & tB ) { return tA.m_iStart<tB.m_iStart; } );

	return true;
}


int64_t SecondaryIndex_c::GetValsRows ( std::vector<BlockIterator_i *> * pIterators,  const Filter_t & tFilter, const IteratorSettings_t & tSettings ) const
{
	if ( tFilter.m_dValues.empty() )
		return 0;

	std::vector<BlockIter_t> dBlocksIt;
	int64_t iNumIterators = 0;
	uint64_t uBlockBaseOff = 0;
	uint64_t uBlocksCount = 0;
	if ( !PrepareBlocksValues ( tFilter, pIterators ? &dBlocksIt : nullptr, uBlockBaseOff, iNumIterators, uBlocksCount ) )
		return 0;

	iNumIterators = std::min ( (int64_t)tFilter.m_dValues.size(), iNumIterators );

	if ( !pIterators )
		return iNumIterators;

	RsetInfo_t tRsetInfo { iNumIterators, tSettings.m_uMaxValues, tSettings.m_iRsetSize };
	int iColumnId = GetColumnId ( tFilter.m_sName );
	auto pBlockCache = tSettings.m_bUseCache && m_dBlockCaches.size() ? m_dBlockCaches[iColumnId].get() : nullptr;

	ReaderFactory_c tReaderFactory = { .m_tCol = m_dAttrs[iColumnId], .m_tSettings = m_tSettings, .m_tRsetInfo = tRsetInfo, .m_iFD = m_tReader.GetFD(), .m_uVersion = m_uVersion, .m_uBlockBaseOff = uBlockBaseOff, .m_uBlocksCount = uBlocksCount, .m_uValuesPerBlock = m_uValuesPerBlock, .m_uRowidsPerBlock = m_uRowidsPerBlock, .m_pBounds = tSettings.m_pBounds, .m_iCutoff = tSettings.m_iCutoff, .m_pBlockCache = pBlockCache };
	std::unique_ptr<BlockReader_i> pBlockReader { tReaderFactory.CreateBlockReader() };
	if ( !pBlockReader )
		return 0;

	pBlockReader->CreateBlocksIterator ( dBlocksIt, tFilter, *pIterators );
	return iNumIterators;
}


uint32_t SecondaryIndex_c::CalcValsRows ( const Filter_t & tFilter ) const
{
	if ( tFilter.m_dValues.empty() )
		return 0;

	std::vector<BlockIter_t> dBlocksIt;
	uint64_t uBlockBaseOff = 0;
	int64_t iNumIterators = 0;
	uint64_t uBlocksCount = 0;
	if ( !PrepareBlocksValues ( tFilter, &dBlocksIt, uBlockBaseOff, iNumIterators, uBlocksCount ) )
		return 0;

	const auto & tCol = m_dAttrs[GetColumnId ( tFilter.m_sName )];

	ReaderFactory_c tReaderFactory = { .m_tCol = tCol, .m_tSettings = m_tSettings, .m_iFD = m_tReader.GetFD(), .m_uVersion = m_uVersion, .m_uBlockBaseOff = uBlockBaseOff, .m_uBlocksCount = uBlocksCount, .m_uValuesPerBlock = m_uValuesPerBlock, .m_uRowidsPerBlock = m_uRowidsPerBlock };
	std::unique_ptr<BlockReader_i> pBlockReader { tReaderFactory.CreateBlockReader() };
	if ( !pBlockReader )
		return 0;

	return pBlockReader->CalcValueCount(dBlocksIt);
}


bool SecondaryIndex_c::PrepareBlocksRange ( const Filter_t & tFilter, ApproxPos_t & tPos, uint64_t & uBlockBaseOff, uint64_t & uBlocksCount, int64_t & iNumIterators ) const
{
	iNumIterators = 0;

	int iCol = GetColumnId ( tFilter.m_sName );
	assert ( iCol>=0 );

	if ( m_dIdx[iCol]->IsEmpty() )
		return 0;

	const auto & tCol = m_dAttrs[iCol];

	uBlockBaseOff = m_iBlocksBase + m_dBlockStartOff[iCol];
	uBlocksCount = m_dBlocksCount[iCol];

	const bool bFloat = tCol.m_eType==AttrType_e::FLOAT;

	tPos = { 0, 0, ( uBlocksCount - 1 )*m_uValuesPerBlock };

	bool bFullscan = tFilter.m_bLeftUnbounded && tFilter.m_bRightUnbounded;
	if ( bFullscan || (!tFilter.m_bLeftUnbounded && !tFilter.m_bRightUnbounded ) )
	{
		ApproxPos_t tFoundMin, tFoundMax;

		if ( bFullscan )
			SetupFullscanLimits ( tCol.m_eType, m_dIdx[iCol].get(), tFoundMin, tFoundMax );
		else
		{
			tFoundMin = ( bFloat ? m_dIdx[iCol]->Search ( FloatToUint ( tFilter.m_fMinValue ) ) : m_dIdx[iCol]->Search ( tFilter.m_iMinValue ) );
			tFoundMax = ( bFloat ? m_dIdx[iCol]->Search ( FloatToUint ( tFilter.m_fMaxValue ) ) : m_dIdx[iCol]->Search ( tFilter.m_iMaxValue ) );
		}
		
		tPos.m_iLo = std::min ( tFoundMin.m_iLo, tFoundMax.m_iLo );
		tPos.m_iPos = std::min ( tFoundMin.m_iPos, tFoundMax.m_iPos );
		tPos.m_iHi = std::max ( tFoundMin.m_iHi, tFoundMax.m_iHi );
		iNumIterators = tFoundMax.m_iPos-tFoundMin.m_iPos+1;
	}
	else if ( tFilter.m_bRightUnbounded )
	{
		ApproxPos_t tFound =  ( bFloat ? m_dIdx[iCol]->Search ( FloatToUint ( tFilter.m_fMinValue ) ) : m_dIdx[iCol]->Search ( tFilter.m_iMinValue ) );
		tPos.m_iPos = tFound.m_iPos;
		tPos.m_iLo = tFound.m_iLo;
		iNumIterators = tPos.m_iHi-tPos.m_iPos;
	}
	else if ( tFilter.m_bLeftUnbounded )
	{
		ApproxPos_t tFound = ( bFloat ? m_dIdx[iCol]->Search ( FloatToUint ( tFilter.m_fMaxValue ) ) : m_dIdx[iCol]->Search ( tFilter.m_iMaxValue ) );
		tPos.m_iPos = tFound.m_iPos;
		tPos.m_iHi = tFound.m_iHi;
		iNumIterators = tPos.m_iPos-tPos.m_iLo;
	}

	iNumIterators = std::max ( iNumIterators, int64_t(0) );
	return true;
}


int64_t SecondaryIndex_c::GetRangeRows ( std::vector<BlockIterator_i *> * pIterators,  const Filter_t & tFilter, const IteratorSettings_t & tSettings ) const
{
	ApproxPos_t tPos;
	int64_t iNumIterators = 0;
	uint64_t uBlockBaseOff = 0;
	uint64_t uBlocksCount = 0;
	if ( !PrepareBlocksRange ( tFilter, tPos, uBlockBaseOff, uBlocksCount, iNumIterators ) )
		return 0;

	if ( !pIterators )
		return iNumIterators;

	BlockIter_t tPosIt ( tPos, 0, uBlocksCount, m_uValuesPerBlockShift );
	RsetInfo_t tRsetInfo { iNumIterators, tSettings.m_uMaxValues, tSettings.m_iRsetSize };
	const auto & tCol = m_dAttrs[GetColumnId ( tFilter.m_sName )];

	ReaderFactory_c tReaderFactory = { .m_tCol = tCol, .m_tSettings = m_tSettings, .m_tRsetInfo = tRsetInfo, .m_iFD = m_tReader.GetFD(), .m_uVersion = m_uVersion, .m_uBlockBaseOff = uBlockBaseOff, .m_uBlocksCount = uBlocksCount, .m_uValuesPerBlock = m_uValuesPerBlock, .m_uRowidsPerBlock = m_uRowidsPerBlock, .m_pBounds = tSettings.m_pBounds, .m_iCutoff = tSettings.m_iCutoff };
	std::unique_ptr<BlockReader_i> pReader { tReaderFactory.CreateRangeReader() };
	if ( !pReader )
		return 0;

	pReader->CreateBlocksIterator ( tPosIt, tFilter, *pIterators );

	return iNumIterators;
}


uint32_t SecondaryIndex_c::CalcRangeRows ( const Filter_t & tFilter ) const
{
	ApproxPos_t tPos;
	uint64_t uBlockBaseOff = 0;
	uint64_t uBlocksCount = 0;
	int64_t iNumIterators = 0;
	if ( !PrepareBlocksRange ( tFilter, tPos, uBlockBaseOff, uBlocksCount, iNumIterators ) )
		return 0;

	BlockIter_t tPosIt ( tPos, 0, uBlocksCount, m_uValuesPerBlockShift );
	const auto & tCol = m_dAttrs[GetColumnId ( tFilter.m_sName )];

	ReaderFactory_c tReaderFactory = { .m_tCol = tCol, .m_tSettings = m_tSettings, .m_iFD = m_tReader.GetFD(), .m_uVersion = m_uVersion, .m_uBlockBaseOff = uBlockBaseOff, .m_uBlocksCount = uBlocksCount, .m_uValuesPerBlock = m_uValuesPerBlock, .m_uRowidsPerBlock = m_uRowidsPerBlock };
	std::unique_ptr<BlockReader_i> pReader { tReaderFactory.CreateRangeReader() };
	if ( !pReader )
		return 0;

	return pReader->CalcValueCount ( tPosIt, tFilter );
}


const ColumnInfo_t * SecondaryIndex_c::GetAttr ( const Filter_t & tFilter, std::string & sError ) const
{
	int iCol = GetColumnId ( tFilter.m_sName );
	if ( iCol==-1 )
	{
		sError = FormatStr ( "secondary index not found for attribute '%s'", tFilter.m_sName.c_str() );
		return nullptr;
	}

	const auto & tCol = m_dAttrs[iCol];

	if ( tCol.m_eType==AttrType_e::NONE )
	{
		sError = FormatStr( "invalid attribute %s type %d", tCol.m_sName.c_str(), tCol.m_eType );
		return nullptr;
	}

	return &tCol;
}


static void RemoveOutOfRangeValues ( Filter_t & tFilter, const ColumnInfo_t & tCol, uint32_t uVersion )
{
	// versions prior to 9 don't have min/max
	if ( tFilter.m_dValues.empty() || uVersion<9 || tFilter.m_bExclude )
		return;

	static common::AttrType_e dSupportedTypes[] =
	{
		common::AttrType_e::UINT32,
		common::AttrType_e::TIMESTAMP,
		common::AttrType_e::INT64,
		common::AttrType_e::BOOLEAN,
		common::AttrType_e::UINT32SET,
		common::AttrType_e::INT64SET
	};

	bool bSupported = false;
	for ( auto i : dSupportedTypes )
		if ( tCol.m_eType==i )
			bSupported = true;

	if ( !bSupported )
		return;

	if ( tFilter.m_dValues.back() < (int64_t)tCol.m_tMin || tFilter.m_dValues.front() > (int64_t)tCol.m_tMax )
	{
		tFilter.m_dValues.resize(0);
		return;
	}

	tFilter.m_dValues.erase ( std::remove_if ( tFilter.m_dValues.begin(), tFilter.m_dValues.end(), [&tCol]( int64_t iValue ) { return iValue<(int64_t)tCol.m_tMin || iValue>(int64_t)tCol.m_tMax; } ), tFilter.m_dValues.end() );
}


static bool FixupFilter ( Filter_t & tFixedFilter, const Filter_t & tFilter, const ColumnInfo_t & tCol, uint32_t uVersion )
{
	tFixedFilter = tFilter;
	FixupFilterSettings ( tFixedFilter, tCol.m_eType );

	switch ( tFixedFilter.m_eType )
	{
	case FilterType_e::VALUES:
		RemoveOutOfRangeValues ( tFixedFilter, tCol, uVersion );
		break;

	case FilterType_e::STRINGS:
		if ( !tFixedFilter.m_fnCalcStrHash )
			return false;

		tFixedFilter = StringFilterToHashFilter ( tFixedFilter, false );
		break;

	case FilterType_e::NOTNULL:
		tFixedFilter.m_bLeftUnbounded = true;
		tFixedFilter.m_bRightUnbounded = true;
		break;

	default:
		break;
	}

	return true;
}


bool SecondaryIndex_c::CreateIterators ( std::vector<BlockIterator_i *> & dIterators, const Filter_t & tFilter, const IteratorSettings_t & tSettings, std::string & sError ) const
{
	const auto * pCol = GetAttr ( tFilter, sError );
	if ( !pCol )
		return false;

	Filter_t tFixedFilter;
	if ( !FixupFilter ( tFixedFilter, tFilter, *pCol, m_uVersion ) )
		return false;

	switch ( tFixedFilter.m_eType )
	{
	case FilterType_e::VALUES:
		GetValsRows ( &dIterators, tFixedFilter, tSettings );
		return true;

	case FilterType_e::RANGE:
	case FilterType_e::FLOATRANGE:
	case FilterType_e::NOTNULL:
		GetRangeRows ( &dIterators, tFixedFilter, tSettings );
		return true;

	default:
		sError = FormatStr ( "unhandled filter type '%d'", to_underlying ( tFixedFilter.m_eType ) );
		return false;
	}
}


bool SecondaryIndex_c::CalcCount ( uint32_t & uCount, const common::Filter_t & tFilter, uint32_t uMaxValues, std::string & sError ) const
{
	uCount = 0;

	if ( m_uVersion < 7 )
		return false;

	const auto * pCol = GetAttr ( tFilter, sError );
	if ( !pCol )
		return false;

	Filter_t tFixedFilter;
	if ( !FixupFilter ( tFixedFilter, tFilter, *pCol, m_uVersion ) )
		return false;

	bool bExclude = tFixedFilter.m_bExclude;
	tFixedFilter.m_bExclude = false;

	switch ( tFixedFilter.m_eType )
	{
	case FilterType_e::VALUES:
		uCount = CalcValsRows ( tFixedFilter );
		if ( bExclude )
			uCount = uMaxValues - uCount;
		return true;

	case FilterType_e::RANGE:
	case FilterType_e::FLOATRANGE:
	case FilterType_e::NOTNULL:
		uCount = CalcRangeRows ( tFixedFilter );
		if ( bExclude )
			uCount = uMaxValues - uCount;
		return true;

	default:
		sError = FormatStr ( "unhandled filter type '%d'", to_underlying ( tFixedFilter.m_eType ) );
		return false;
	}
}


uint32_t SecondaryIndex_c::GetNumIterators ( const common::Filter_t & tFilter ) const
{
	std::string sError;
	const auto * pCol = GetAttr ( tFilter, sError );
	if ( !pCol )
		return 0;

	Filter_t tFixedFilter;
	if ( !FixupFilter ( tFixedFilter, tFilter, *pCol, m_uVersion ) )
		return 0;

	switch ( tFixedFilter.m_eType )
	{
	case FilterType_e::VALUES:
		{
			// filters with lots of values are too slow to estimate
			const int BIG_FILTER_THRESH=100;
			if ( tFixedFilter.m_dValues.size()>=BIG_FILTER_THRESH )
				return tFixedFilter.m_dValues.size();

			return GetValsRows ( nullptr, tFixedFilter, {} );
		}

	case FilterType_e::RANGE:
	case FilterType_e::FLOATRANGE:
	case FilterType_e::NOTNULL:
		return GetRangeRows ( nullptr, tFixedFilter, {} );

	default:
		return 0;
	}
}

}

SI::Index_i * CreateSecondaryIndex ( const char * szFile, const SI::IndexSettings_t & tSettings, std::string & sError )
{
	std::unique_ptr<SI::SecondaryIndex_c> pIdx ( new SI::SecondaryIndex_c(tSettings) );

	if ( !pIdx->Setup ( szFile, sError ) )
		return nullptr;

	return pIdx.release();
}


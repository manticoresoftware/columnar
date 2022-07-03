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

#include "columnar.h"

#include "accessorbool.h"
#include "accessorint.h"
#include "accessorstr.h"
#include "accessormva.h"
#include "check.h"
#include "reader.h"

#include <unordered_map>
#include <algorithm>

namespace columnar
{

using namespace util;
using namespace common;

using HeaderWithLocator_t = std::pair<const AttributeHeader_i*, int>;

template <bool ROWID_LIMITS>
class MinMaxEval_T
{
public:
			MinMaxEval_T ( const std::vector<HeaderWithLocator_t> & dHeaders, const BlockTester_i & tBlockTester, SharedBlocks_c & pMatchingBlocks, uint32_t uMinRowID, uint32_t uMaxRowID );

	void	Eval();
	bool	EvalAll();

private:
	const std::vector<HeaderWithLocator_t> & m_dHeaders;
	const BlockTester_i &	m_tBlockTester;
	SharedBlocks_c			m_pMatchingBlocks;

	std::vector<int>		m_dBlocksOnLevel;
	MinMaxVec_t				m_dMinMax;
	int						m_iNumLevels = 0;
	int						m_iMinMaxLeafShift = 0;
	uint32_t				m_uMinRowID = 0;
	uint32_t				m_uMaxRowID = INVALID_ROW_ID;

	void					DoEval ( int iLevel, int iBlock );
	void					ResizeMinMax();
	FORCE_INLINE bool		FillMinMax ( int iLevel, int iBlock );
	FORCE_INLINE uint32_t	MinMaxBlockId2RowId ( int iBlockId ) const;
	FORCE_INLINE uint32_t	MinMaxBlockId2RowId ( int iBlockId, int iLevel ) const;
	FORCE_INLINE bool		RangesOverlap ( uint32_t uMin, uint32_t uMax ) const;
};

template <bool ROWID_LIMITS>
MinMaxEval_T<ROWID_LIMITS>::MinMaxEval_T ( const std::vector<HeaderWithLocator_t> & dHeaders, const BlockTester_i & tBlockTester, SharedBlocks_c & pMatchingBlocks, uint32_t uMinRowID, uint32_t uMaxRowID )
	: m_dHeaders ( dHeaders )
	, m_tBlockTester ( tBlockTester )
	, m_pMatchingBlocks ( pMatchingBlocks )
	, m_uMinRowID ( uMinRowID )
	, m_uMaxRowID ( uMaxRowID )
{
	assert ( !dHeaders.empty() );

	// do this to avoid multiple vcalls when evaluating
	m_iNumLevels = m_dHeaders[0].first->GetNumMinMaxLevels();
	m_iMinMaxLeafShift = CalcNumBits ( m_dHeaders[0].first->GetSettings().m_iSubblockSize ) - 1;

	m_dBlocksOnLevel.resize(m_iNumLevels);

	for ( size_t i=0; i <m_dBlocksOnLevel.size(); i++ )
		m_dBlocksOnLevel[i] = m_dHeaders[0].first->GetNumMinMaxBlocks ( (int)i );
}

template <bool ROWID_LIMITS>
void MinMaxEval_T<ROWID_LIMITS>::Eval()
{
	ResizeMinMax();
	DoEval ( 0, 0 );
}

template <bool ROWID_LIMITS>
bool MinMaxEval_T<ROWID_LIMITS>::EvalAll()
{
	ResizeMinMax();
	if ( !FillMinMax ( 0, 0 ) )
		return true;

	return m_tBlockTester.Test(m_dMinMax);
}

template <bool ROWID_LIMITS>
bool MinMaxEval_T<ROWID_LIMITS>::RangesOverlap ( uint32_t uMin, uint32_t uMax ) const
{
	return uMin<=m_uMaxRowID && uMax>=m_uMinRowID;
}

template <bool ROWID_LIMITS>
void MinMaxEval_T<ROWID_LIMITS>::DoEval ( int iLevel, int iBlock )
{
	if ( !FillMinMax ( iLevel, iBlock ) )
		return;

	if ( m_tBlockTester.Test ( m_dMinMax ) )
	{
		if ( iLevel==m_iNumLevels-1 )
		{
			if ( ROWID_LIMITS )
			{
				uint32_t uMinBlockRowID = MinMaxBlockId2RowId(iBlock);
				uint32_t uMaxBlockRowID = MinMaxBlockId2RowId(iBlock+1) - 1;

				if ( RangesOverlap ( uMinBlockRowID, uMaxBlockRowID ) )
					m_pMatchingBlocks->Add(iBlock);
			}
			else
				m_pMatchingBlocks->Add(iBlock);
		}
		else
		{
			int iLeftBlock = iBlock<<1;
			int iRightBlock = iLeftBlock+1;

			if ( ROWID_LIMITS )
			{
				uint32_t uMinLeftBlockRowID = MinMaxBlockId2RowId ( iLeftBlock, iLevel+1 );
				uint32_t uMaxLeftBlockRowID = MinMaxBlockId2RowId ( iLeftBlock+1, iLevel+1 ) - 1;
				uint32_t uMinRightBlockRowID = MinMaxBlockId2RowId ( iRightBlock, iLevel+1 );
				uint32_t uMaxRightBlockRowID = MinMaxBlockId2RowId ( iRightBlock+1, iLevel+1 ) - 1;

				if ( RangesOverlap ( uMinLeftBlockRowID, uMaxLeftBlockRowID ) )
					DoEval ( iLevel+1, iLeftBlock );

				if ( RangesOverlap ( uMinRightBlockRowID, uMaxRightBlockRowID ) )
					DoEval ( iLevel+1, iRightBlock );
			}
			else
			{
				DoEval ( iLevel+1, iLeftBlock );
				DoEval ( iLevel+1, iRightBlock );
			}
		}
	}
}

template <bool ROWID_LIMITS>
void MinMaxEval_T<ROWID_LIMITS>::ResizeMinMax()
{
	int iMaxLocator = 0;
	for ( const auto & i : m_dHeaders )
		iMaxLocator = std::max ( i.second, iMaxLocator );

	m_dMinMax.resize ( iMaxLocator+1 );
	for ( auto & i : m_dMinMax )
		i = {0,0};
}

template <bool ROWID_LIMITS>
bool MinMaxEval_T<ROWID_LIMITS>::FillMinMax ( int iLevel, int iBlock )
{
	int iNumBlocksOnLevel = m_dBlocksOnLevel[iLevel];
	if ( iBlock>=iNumBlocksOnLevel )
		return false;

	for ( const auto & tHeader : m_dHeaders )
	{
		assert ( tHeader.first->GetNumMinMaxBlocks(iLevel)==iNumBlocksOnLevel );
		m_dMinMax[tHeader.second] = tHeader.first->GetMinMax ( iLevel, iBlock );
	}

	return true;
}

template <bool ROWID_LIMITS>
uint32_t MinMaxEval_T<ROWID_LIMITS>::MinMaxBlockId2RowId ( int iBlockId ) const
{
	return iBlockId<<m_iMinMaxLeafShift;
}

template <bool ROWID_LIMITS>
uint32_t MinMaxEval_T<ROWID_LIMITS>::MinMaxBlockId2RowId ( int iBlockId, int iLevel ) const
{
	return iBlockId << ( m_iNumLevels - iLevel - 1 + m_iMinMaxLeafShift );
}

//////////////////////////////////////////////////////////////////////////

void Settings_t::Load ( FileReader_c & tReader )
{
	m_iSubblockSize		= tReader.Read_uint32();

	// FIXME: should be removed before release
	m_sCompressionUINT32 = tReader.Read_string();
	m_sCompressionUINT64 = tReader.Read_string();
}


void Settings_t::Save ( FileWriter_c & tWriter )
{
	tWriter.Write_uint32(m_iSubblockSize);
	tWriter.Write_string(m_sCompressionUINT32);
	tWriter.Write_string(m_sCompressionUINT64);
}


bool Settings_t::Check ( FileReader_c & tReader, Reporter_fn & fnError )
{
	if ( !CheckInt32 ( tReader, 0, 65536, "Subblock size", fnError ) )			return false;	// m_iSubblockSize
	if ( !CheckString ( tReader, 0, 128, "Uint32 compression algo", fnError ) )	return false;	// m_sCompressionUINT32
	if ( !CheckString ( tReader, 0, 128, "Uint64 compression algo", fnError ) )	return false;	// m_sCompressionUINT64

	return true;
}

//////////////////////////////////////////////////////////////////////////

class BlockIterator_c : public BlockIterator_i
{
public:
	bool			HintRowID ( uint32_t tRowID ) final;
	bool			GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) final;
	int64_t			GetNumProcessed() const final;
	bool			Setup ( const std::vector<HeaderWithLocator_t> & dHeaders, SharedBlocks_c & pMatchingBlocks );
	void			AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const final;

private:
	static const int MAX_COLLECTED = 128;

	std::shared_ptr<MatchingBlocks_c>	m_pMatchingBlocks;
	std::array<uint32_t,MAX_COLLECTED>	m_dCollected;

	std::vector<std::string>			m_dAttrs;

	int64_t		m_iTotalDocs = 0;
	int			m_iDoc = 0;
	int			m_iBlock = 0;
	int			m_iDocsInBlock = 0;
	uint32_t	m_tRowID = 0;
	int			m_iProcessed = 0;

	int			m_iNumBlocks = 0;
	int			m_iDocsPerBlock = 0;
	int			m_iDocsInLastBlock = 0;
	int			m_iMinMaxLeafShift = 0;
	int			m_iNumLevels = 0;

	inline bool	SetCurBlock ( int iBlock );
	FORCE_INLINE int GetNumDocs ( int iBlock ) const;
	FORCE_INLINE uint32_t MinMaxBlockId2RowId ( int iBlockId ) const;
};


bool BlockIterator_c::Setup ( const std::vector<HeaderWithLocator_t> & dHeaders, SharedBlocks_c & pMatchingBlocks )
{
	assert ( !dHeaders.empty() );

	for ( const auto & i : dHeaders )
		m_dAttrs.push_back ( i.first->GetName() );

	const AttributeHeader_i * pFirstAttr = dHeaders[0].first;
	m_iTotalDocs = pFirstAttr->GetNumDocs();
	m_iNumLevels = pFirstAttr->GetNumMinMaxLevels();
	m_iNumBlocks = pFirstAttr->GetNumMinMaxBlocks ( m_iNumLevels-1 );
	m_iDocsPerBlock = pFirstAttr->GetSettings().m_iSubblockSize;
	m_iMinMaxLeafShift = CalcNumBits(m_iDocsPerBlock)-1;

	int iLeftover = m_iTotalDocs % m_iDocsPerBlock;
	m_iDocsInLastBlock = iLeftover ? iLeftover : m_iDocsPerBlock;

	// 99% or more of leaves match? not worth spawning the iterator
	const float THRESH = 0.99f;
	if ( pMatchingBlocks->GetNumBlocks()>=(int)(m_iNumBlocks*THRESH) )
		return false;

	m_pMatchingBlocks = pMatchingBlocks;

	SetCurBlock(0);
	return true;
}

void BlockIterator_c::AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const
{
	for ( const auto & i : m_dAttrs )
		dDesc.push_back ( { i, "prefilter" } );
}

bool BlockIterator_c::HintRowID ( uint32_t tRowID )
{
	int iNextBlock = m_iBlock;
	int iNumBlocks = m_pMatchingBlocks->GetNumBlocks();

	// we assume that we are only advancing forward
	while ( iNextBlock<iNumBlocks )
	{
		uint32_t tSubblockStart = MinMaxBlockId2RowId ( m_pMatchingBlocks->GetBlock(iNextBlock) );
		uint32_t tSubblockEnd = tSubblockStart + m_iDocsPerBlock;

		if ( tRowID<tSubblockStart || ( tRowID>=tSubblockStart && tRowID<tSubblockEnd ) )
		{
			if ( iNextBlock!=m_iBlock )
				SetCurBlock(iNextBlock);

			return true;
		}

		iNextBlock++;
	}

	return false;
}


bool BlockIterator_c::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	uint32_t * pRowIdStart = m_dCollected.data();
	uint32_t * pRowIdMax = pRowIdStart + m_dCollected.size() - 1;
	uint32_t * pRowID = pRowIdStart;

	while ( pRowID<pRowIdMax )
	{
		if ( m_iDoc>=m_iDocsInBlock )
		{
			// this means that we don't have any more docs
			if ( !m_iDocsInBlock )
				return false;

			if ( !SetCurBlock ( m_iBlock+1 ) )
				break;
		}

		*pRowID++ = m_tRowID;

		m_iDoc++;
		m_tRowID++;
	}

	m_iProcessed += (int)(pRowID-pRowIdStart);
	return CheckEmptySpan ( pRowID, pRowIdStart, dRowIdBlock );
}


int64_t BlockIterator_c::GetNumProcessed() const
{
	return m_iProcessed;
}


bool BlockIterator_c::SetCurBlock ( int iBlock )
{
	if ( iBlock>=m_pMatchingBlocks->GetNumBlocks() )
	{
		m_iDocsInBlock = 0;
		return false;
	}

	m_iBlock = iBlock;
	int iMatchingBlockId = m_pMatchingBlocks->GetBlock(iBlock);
	m_iDocsInBlock = GetNumDocs ( iMatchingBlockId );
	m_tRowID = MinMaxBlockId2RowId ( iMatchingBlockId );
	m_iDoc = 0;

	return true;
}


int BlockIterator_c::GetNumDocs ( int iBlock ) const
{
	if ( iBlock<m_iNumBlocks-1 )
		return m_iDocsPerBlock;

	return m_iDocsInLastBlock;
}


uint32_t BlockIterator_c::MinMaxBlockId2RowId ( int iBlockId ) const
{
	return iBlockId<<m_iMinMaxLeafShift;
}

//////////////////////////////////////////////////////////////////////////

class Columnar_c final : public Columnar_i
{
public:
										Columnar_c ( const std::string & sFilename, uint32_t uTotalDocs );

	bool								Setup ( std::string & sError );

	Iterator_i *						CreateIterator ( const std::string & sName, const IteratorHints_t & tHints, columnar::IteratorCapabilities_t * pCapabilities, std::string & sError ) const final;
	std::vector<BlockIterator_i *>		CreateAnalyzerOrPrefilter ( const std::vector<Filter_t> & dFilters, std::vector<int> & dDeletedFilters, const BlockTester_i & tBlockTester ) const final;
	int									GetAttributeId ( const std::string & sName ) const final;
	AttrType_e							GetType ( const std::string & sName ) const final;

	bool								EarlyReject ( const std::vector<Filter_t> & dFilters, const BlockTester_i & tBlockTester ) const final;
	bool								IsFilterDegenerate ( const Filter_t & tFilter ) const final;

private:
	std::string							m_sFilename;
	uint32_t							m_uTotalDocs;
	std::vector<std::unique_ptr<AttributeHeader_i>>	m_dHeaders;
	std::unordered_map<std::string, HeaderWithLocator_t> m_hHeaders;
	FileReader_c						m_tReader;

	const AttributeHeader_i *			GetHeader ( const std::string & sName ) const;
	bool								LoadHeaders ( FileReader_c & tReader, int iNumAttrs, std::string & sError );
	FileReader_c *						CreateFileReader() const;
	std::vector<HeaderWithLocator_t>	GetHeadersForMinMax ( const std::vector<Filter_t> & dFilters ) const;

	Analyzer_i *						CreateAnalyzer ( const Filter_t & tSettings, bool bHaveMatchingBlocks ) const;
	std::vector<BlockIterator_i *>		TryToCreatePrefilter ( const std::vector<HeaderWithLocator_t> & dHeaders, SharedBlocks_c pMatchingBlocks ) const;
	std::vector<BlockIterator_i *>		TryToCreateAnalyzers ( const std::vector<Filter_t> & dFilters, std::vector<int> & dDeletedFilters, SharedBlocks_c & pMatchingBlocks ) const;
};

//////////////////////////////////////////////////////////////////////////

Columnar_c::Columnar_c ( const std::string & sFilename, uint32_t uTotalDocs )
	: m_sFilename ( sFilename )
	, m_uTotalDocs ( uTotalDocs )
{}


bool Columnar_c::Setup ( std::string & sError )
{
	if ( !m_tReader.Open ( m_sFilename, sError ) )
		return false;

	uint32_t uStorageVersion = m_tReader.Read_uint32();
	if ( uStorageVersion!=STORAGE_VERSION )
	{
		sError = FormatStr ( "Unable to load columnar storage: %s is v.%d, binary is v.%d", m_sFilename.c_str(), uStorageVersion, STORAGE_VERSION );
		return false;
	}

	int iNumAttrs = (int)m_tReader.Read_uint32();
	if ( !iNumAttrs )
		return true;

	if ( !LoadHeaders ( m_tReader, iNumAttrs, sError ) )
		return false;

	if ( m_tReader.IsError() )
	{
		sError = m_tReader.GetError();
		return false;
	}

	return true;
}


Iterator_i * Columnar_c::CreateIterator ( const std::string & sName, const IteratorHints_t & tHints, columnar::IteratorCapabilities_t * pCapabilities, std::string & sError ) const
{
	const AttributeHeader_i * pHeader = GetHeader(sName);
	if ( !pHeader )
		return nullptr;

	std::unique_ptr<FileReader_c> pReader ( CreateFileReader() );
	if ( !pReader )
		return nullptr;

	switch ( pHeader->GetType() )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
	case AttrType_e::FLOAT:
		return CreateIteratorUint32 ( *pHeader, pReader.release() );

	case AttrType_e::INT64:		return CreateIteratorUint64 ( *pHeader, pReader.release() );
	case AttrType_e::BOOLEAN:	return CreateIteratorBool ( *pHeader, pReader.release() );
	case AttrType_e::STRING:
		if ( tHints.m_bNeedStringHashes )
		{
			const AttributeHeader_i * pHashHeader = GetHeader ( GenerateHashAttrName(sName) );
			if ( pHashHeader )
			{
				if ( pCapabilities )
					pCapabilities->m_bStringHashes = true;

				return CreateIteratorUint64 ( *pHashHeader, pReader.release() );
			}
		}
		return CreateIteratorStr ( *pHeader, pReader.release() );
	
	case AttrType_e::UINT32SET:
	case AttrType_e::INT64SET:
		return CreateIteratorMVA ( *pHeader, pReader.release() );

	default:
		sError = "Unsupported columnar iterator type";
		return nullptr;
	}
}


FileReader_c * Columnar_c::CreateFileReader() const
{
	return new FileReader_c ( m_tReader.GetFD() );
}


Analyzer_i * Columnar_c::CreateAnalyzer ( const Filter_t & tSettings, bool bHaveMatchingBlocks ) const
{
	const AttributeHeader_i * pHeader = GetHeader ( tSettings.m_sName );
	if ( !pHeader )
		return nullptr;

	std::unique_ptr<FileReader_c> pReader ( CreateFileReader() );
	if ( !pReader )
		return nullptr;

	switch ( pHeader->GetType() )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
	case AttrType_e::FLOAT:
	case AttrType_e::INT64:
		assert ( bHaveMatchingBlocks );
		return CreateAnalyzerInt ( *pHeader, pReader.release(), tSettings );

	case AttrType_e::BOOLEAN:
		return CreateAnalyzerBool ( *pHeader, pReader.release(), tSettings, bHaveMatchingBlocks );

	case AttrType_e::UINT32SET:
	case AttrType_e::INT64SET:
		return CreateAnalyzerMVA ( *pHeader, pReader.release(), tSettings, bHaveMatchingBlocks );

	case AttrType_e::STRING:
		if ( tSettings.m_fnCalcStrHash )
		{
			const AttributeHeader_i * pHashHeader = GetHeader ( GenerateHashAttrName ( tSettings.m_sName ) );
			if ( pHashHeader )
				return CreateAnalyzerInt ( *pHashHeader, pReader.release(), StringFilterToHashFilter ( tSettings, true ) );
		}

		return CreateAnalyzerStr ( *pHeader, pReader.release(), tSettings, bHaveMatchingBlocks );

	default:
		return nullptr;
	}
}


std::vector<HeaderWithLocator_t> Columnar_c::GetHeadersForMinMax ( const std::vector<Filter_t> & dFilters ) const
{
	int iBlocks=0;
	std::vector<HeaderWithLocator_t> dHeaders;
	for ( const auto & i : dFilters )
	{
		int iAttrIndex = GetAttributeId ( i.m_sName );
		if ( iAttrIndex<0 )
			continue;

		const AttributeHeader_i * pHeader = GetHeader ( i.m_sName );
		if ( !pHeader || !pHeader->GetNumMinMaxLevels() )
			continue;

		dHeaders.push_back ( { pHeader, iAttrIndex } );
		iBlocks = dHeaders.back().first->GetNumBlocks();
	}

	if ( !iBlocks )
		dHeaders.resize(0);

	return dHeaders;
}


static void FetchRowIdLimits ( const Filter_t & tFilter, uint32_t uNumDocs, uint32_t & uMinRowID, uint32_t & uMaxRowID )
{
	uint32_t uMin = (uint32_t)tFilter.m_iMinValue;
	uint32_t uMax = (uint32_t)tFilter.m_iMaxValue;
	double fDelta = (double)uNumDocs / uMax;
	uMinRowID = uint32_t ( fDelta*uMin );
	uMaxRowID = (uMin==uMax-1) ? uNumDocs : uint32_t ( fDelta*(uMin+1) )-1;
}


static void PopulateMatchingBlocks ( MatchingBlocks_c & tBlocks, int iBlockSize, uint32_t uMinRowID, uint32_t uMaxRowID )
{
	int iStart = uMinRowID / iBlockSize;
	int iEnd = uMaxRowID / iBlockSize + 1;
	for ( int i = iStart; i < iEnd; i++ )
		tBlocks.Add(i);
}


std::vector<BlockIterator_i *> Columnar_c::CreateAnalyzerOrPrefilter ( const std::vector<Filter_t> & dFilters, std::vector<int> & dDeletedFilters, const BlockTester_i & tBlockTester ) const
{
	std::vector<HeaderWithLocator_t> dHeaders = GetHeadersForMinMax(dFilters);
	SharedBlocks_c pMatchingBlocks ( dHeaders.empty() ? nullptr : new MatchingBlocks_c );

	const Filter_t * pRowIdFilter = nullptr;
	for ( auto & i : dFilters )
		if ( i.m_sName=="@rowid" )
		{
			pRowIdFilter = &i;
			break;
		}

	uint32_t uMinRowID = 0;
	uint32_t uMaxRowID = INVALID_ROW_ID;
	if ( pRowIdFilter )
		FetchRowIdLimits ( *pRowIdFilter, m_dHeaders[0]->GetNumDocs(), uMinRowID, uMaxRowID );

	bool bMinMaxBlocks = !!pMatchingBlocks;
	if ( bMinMaxBlocks )
	{
		if ( pRowIdFilter )
		{
			MinMaxEval_T<true> tMinMaxEval ( dHeaders, tBlockTester, pMatchingBlocks, uMinRowID, uMaxRowID );
			tMinMaxEval.Eval();
		}
		else
		{
			MinMaxEval_T<false> tMinMaxEval ( dHeaders, tBlockTester, pMatchingBlocks, uMinRowID, uMaxRowID );
			tMinMaxEval.Eval();
		}
	}
	else
	{
		pMatchingBlocks = SharedBlocks_c ( new MatchingBlocks_c );
		PopulateMatchingBlocks ( *pMatchingBlocks, m_dHeaders[0]->GetSettings().m_iSubblockSize, uMinRowID, uMaxRowID );
	}

	std::vector<BlockIterator_i *> dAnalyzers = TryToCreateAnalyzers ( dFilters, dDeletedFilters, pMatchingBlocks );
	if ( !dAnalyzers.empty() )
		return dAnalyzers;

	if ( !bMinMaxBlocks )
		return {};

	return TryToCreatePrefilter ( dHeaders, pMatchingBlocks );
}


int Columnar_c::GetAttributeId ( const std::string & sName ) const
{
	const auto & tFound = m_hHeaders.find(sName);
	return tFound==m_hHeaders.end() ? -1 : tFound->second.second;
}


AttrType_e Columnar_c::GetType ( const std::string & sName ) const
{
	const auto & tFound = m_hHeaders.find(sName);
	return tFound==m_hHeaders.end() ? AttrType_e::NONE : tFound->second.first->GetType();
}


bool Columnar_c::EarlyReject ( const std::vector<Filter_t> & dFilters, const BlockTester_i & tBlockTester ) const
{
	std::vector<HeaderWithLocator_t> dHeaders = GetHeadersForMinMax(dFilters);
	if ( dHeaders.empty() )
		return false;

	SharedBlocks_c pShared(nullptr);
	MinMaxEval_T<false> tMinMaxEval ( dHeaders, tBlockTester, pShared, 0, INVALID_ROW_ID );
	return !tMinMaxEval.EvalAll();
}


bool Columnar_c::IsFilterDegenerate ( const Filter_t & tFilter ) const
{
	const AttributeHeader_i * pHeader = GetHeader ( tFilter.m_sName );
	if ( !pHeader )
		return false;

	// fixme! handle more cases
	if ( tFilter.m_eType==FilterType_e::VALUES && pHeader->GetType()==AttrType_e::BOOLEAN && tFilter.m_dValues.size()==2 && tFilter.m_dValues[0]==0 && tFilter.m_dValues[1]==1 )
		return true;

	return false;
}


std::vector<BlockIterator_i *> Columnar_c::TryToCreateAnalyzers ( const std::vector<Filter_t> & dFilters, std::vector<int> & dDeletedFilters, SharedBlocks_c & pMatchingBlocks ) const
{
	std::vector<BlockIterator_i*> dAnalyzers;

	for ( size_t i = 0; i<dFilters.size(); i++ )
	{
		const auto & tFilter = dFilters[i];

		int iAttrIndex = GetAttributeId ( tFilter.m_sName );
		if ( iAttrIndex<0 )
			continue;

		const AttributeHeader_i * pHeader = GetHeader ( tFilter.m_sName );
		if ( pHeader )
		{
			Analyzer_i * pAnalyzer = CreateAnalyzer ( tFilter, !!pMatchingBlocks );
			if ( pAnalyzer )
			{
				pAnalyzer->Setup ( pMatchingBlocks, pHeader->GetNumDocs() );
				dAnalyzers.push_back(pAnalyzer);
				dDeletedFilters.push_back ( (int)i );
			}
		}
	}

	return dAnalyzers;
}


std::vector<BlockIterator_i *> Columnar_c::TryToCreatePrefilter ( const std::vector<HeaderWithLocator_t> & dHeaders, SharedBlocks_c pMatchingBlocks ) const
{
	if ( !pMatchingBlocks )
		return {};

	std::unique_ptr<BlockIterator_c> pBlockIterator ( new BlockIterator_c );
	if ( !pBlockIterator->Setup ( dHeaders, pMatchingBlocks ) )
		pBlockIterator.reset();

	return { pBlockIterator.release() };
}


const AttributeHeader_i * Columnar_c::GetHeader ( const std::string & sName ) const
{
	const auto & tFound = m_hHeaders.find(sName);
	return tFound==m_hHeaders.end() ? nullptr : tFound->second.first;
}


bool Columnar_c::LoadHeaders ( FileReader_c & tReader, int iNumAttrs, std::string & sError )
{
	m_dHeaders.resize(iNumAttrs);

	for ( size_t i = 0; i < m_dHeaders.size(); i++ )
	{
		AttrType_e eType = AttrType_e ( tReader.Read_uint32() );
		std::unique_ptr<AttributeHeader_i> pHeader ( CreateAttributeHeader ( eType, m_uTotalDocs, sError ) );
		if ( !pHeader )
			return false;

		if ( !pHeader->Load ( tReader, sError ) )
			return false;

		m_dHeaders[i] = std::move(pHeader);
		m_hHeaders.insert ( { m_dHeaders[i]->GetName(), { m_dHeaders[i].get(), (int)i } } );
		tReader.Seek ( tReader.Read_uint64() );
	}

	return true;
}

} // namespace columnar


columnar::Columnar_i * CreateColumnarStorageReader ( const std::string & sFilename, uint32_t uTotalDocs, std::string & sError )
{
	std::unique_ptr<columnar::Columnar_c> pColumnar ( new columnar::Columnar_c ( sFilename, uTotalDocs ) );
	if ( !pColumnar->Setup(sError) )
		return nullptr;

	return pColumnar.release();
}


void CheckColumnarStorage ( const std::string & sFilename, uint32_t uNumRows, columnar::Reporter_fn & fnError, columnar::Reporter_fn & fnProgress )
{
	columnar::CheckStorage ( sFilename, uNumRows, fnError, fnProgress );
}


int GetColumnarLibVersion()
{
	return columnar::LIB_VERSION;
}


extern const char * LIB_VERSION;
const char * GetColumnarLibVersionStr()
{
	return LIB_VERSION;
}


int GetColumnarStorageVersion()
{
	return columnar::STORAGE_VERSION;
}

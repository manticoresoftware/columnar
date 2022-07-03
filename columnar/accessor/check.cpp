// Copyright (c) 2021-2022, Manticore Software LTD (https://manticoresearch.com)
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

#include "check.h"
#include "accessorint.h"
#include "accessorbool.h"
#include "accessorstr.h"
#include "accessormva.h"

namespace columnar
{

using namespace util;
using namespace common;

Checker_c::Checker_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader, Reporter_fn & fnProgress, Reporter_fn & fnError )
	: m_tHeader ( tHeader )
	, m_pReader ( pReader )
	, m_fnProgress ( fnProgress )
	, m_fnError ( fnError )
{}


bool Checker_c::Check()
{
	if ( !m_tHeader.GetNumDocs() )
		return true;

	m_fnProgress("\n");

	for ( uint32_t tRowID=0; tRowID < m_tHeader.GetNumDocs(); tRowID++ )
	{
		uint32_t uBlockId = RowId2BlockId(tRowID);
		if ( uBlockId==m_uBlockId )
			continue;

		m_pReader->Seek ( m_tHeader.GetBlockOffset(uBlockId) );
		if ( !CheckBlockHeader(uBlockId) )
			return false;

		m_uChecked += m_tHeader.GetNumDocs(uBlockId);
		m_fnProgress ( FormatStr ( "\r\tchecked %u/%u docs", m_uChecked, m_tHeader.GetNumDocs() ).c_str() );

		m_uBlockId = uBlockId;
	}

	m_fnProgress("\n\tok\n");

	return true;
}

//////////////////////////////////////////////////////////////////////////

class StorageChecker_c
{
public:
						StorageChecker_c ( const std::string & sFilename, uint32_t uTotalDocs, Reporter_fn & fnError, Reporter_fn & fnProgress );

	bool				Check();

private:
	const std::string & m_sFilename;
	uint32_t			m_uTotalDocs = 0;
	Reporter_fn &		m_fnError;
	Reporter_fn &		m_fnProgress;
	FileReader_c		m_tReader;
	std::vector<std::unique_ptr<AttributeHeader_i>>	m_dHeaders;

	bool				CheckHeaders ( int iNumAttrs );
	Checker_i *			CreateChecker ( const AttributeHeader_i & tHeader ) const;
};


StorageChecker_c::StorageChecker_c ( const std::string & sFilename, uint32_t uTotalDocs, Reporter_fn & fnError, Reporter_fn & fnProgress )
	: m_sFilename ( sFilename )
	, m_uTotalDocs ( uTotalDocs )
	, m_fnError ( fnError )
	, m_fnProgress ( fnProgress )
{}


bool StorageChecker_c::Check()
{
	std::string sError;
	if ( !m_tReader.Open ( m_sFilename, sError ) )
	{
		m_fnError ( sError.c_str() );
		return false;
	}

	uint32_t uStorageVersion = m_tReader.Read_uint32();
	if ( uStorageVersion!=STORAGE_VERSION )
	{
		m_fnError ( FormatStr ( "Unable to load columnar storage: %s is v.%d, binary is v.%d", m_sFilename.c_str(), uStorageVersion, STORAGE_VERSION ).c_str() );
		return false;
	}

	int iNumAttrs = (int)m_tReader.Read_uint32();
	if ( iNumAttrs && !CheckHeaders(iNumAttrs) )
		return false;

	// headers are ok and loaded, time to run checks
	for ( const auto & i : m_dHeaders )
	{
		m_fnProgress ( FormatStr ( "\tchecking attribute '%s'...", i->GetName().c_str() ).c_str() );

		std::unique_ptr<Checker_i> pChecker ( CreateChecker(*i) );
		if ( !pChecker )
			return false;

		if ( !pChecker->Check() )
			return false;
	}

	if ( m_tReader.IsError() )
	{
		m_fnError ( m_tReader.GetError().c_str() );
		return false;
	}

	return true;
}


Checker_i * StorageChecker_c::CreateChecker ( const AttributeHeader_i & tHeader ) const
{
 	std::unique_ptr<FileReader_c> pReader ( new FileReader_c ( m_tReader.GetFD() ) );

	switch ( tHeader.GetType() )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
	case AttrType_e::FLOAT:
	case AttrType_e::INT64:
		return CreateCheckerInt ( tHeader, pReader.release(), m_fnProgress, m_fnError );

	case AttrType_e::BOOLEAN:
		return CreateCheckerBool ( tHeader, pReader.release(), m_fnProgress, m_fnError );

	case AttrType_e::STRING:
		return CreateCheckerStr ( tHeader, pReader.release(), m_fnProgress, m_fnError );

	case AttrType_e::UINT32SET:
	case AttrType_e::INT64SET:
		return CreateCheckerMva ( tHeader, pReader.release(), m_fnProgress, m_fnError );

	default:
		m_fnError ( FormatStr ( "Unsupported header type: %d", (int)tHeader.GetType() ).c_str() );
		return nullptr;
	}
}


bool StorageChecker_c::CheckHeaders ( int iNumAttrs )
{
	m_dHeaders.resize(iNumAttrs);
	int64_t iFileSize = m_tReader.GetFileSize();

	for ( size_t i = 0; i < m_dHeaders.size(); i++ )
	{
		AttrType_e eType = AttrType_e ( m_tReader.Read_uint32() );
		if ( eType>=AttrType_e::TOTAL )
		{
			m_fnError ( FormatStr ( "Unknown attribute type in header: %u", to_underlying(eType) ).c_str() );
			return false;
		}

		std::string sError;
		std::unique_ptr<AttributeHeader_i> pHeader ( CreateAttributeHeader ( eType, m_uTotalDocs, sError ) );
		if ( !pHeader )
		{
			m_fnError ( sError.c_str() );
			return false;
		}

		int64_t iHeaderPos = m_tReader.GetPos();
		if ( !pHeader->Check ( m_tReader, m_fnError ) )
			return false;

		// header passed checks, safe to load it now
		m_tReader.Seek ( iHeaderPos );
		if ( !pHeader->Load ( m_tReader, sError ) )
		{
			m_fnError ( sError.c_str() );
			return false;
		}

		m_dHeaders[i] = std::move(pHeader);

		int64_t iNextOffset = (int64_t)m_tReader.Read_uint64();
		if ( iNextOffset<0 || iNextOffset>=iFileSize )
		{
			m_fnError ( FormatStr ( "Offset points beyond EOF: %lld; EOF at %lld", iNextOffset, iFileSize ).c_str() );
			return false;

		}

		m_tReader.Seek(iNextOffset);
	}

	return true;
}

/////////////////////////////////////////////////////////////////////

bool CheckString ( FileReader_c & tReader, int iMinLength, int iMaxLength, const std::string & sMessage, Reporter_fn & fnError )
{
	int iLen = (int)tReader.Read_uint32();
	if ( iLen<iMinLength || iLen>iMaxLength )
	{
		fnError ( FormatStr ( "String length out of bounds: %d", iLen ).c_str() );
		return false;
	}

	tReader.Seek ( tReader.GetPos()+iLen );

	return true;
}


bool CheckInt32 ( FileReader_c & tReader, int iMin, int iMax, const std::string & sMessage, Reporter_fn & fnError )
{
	int iValue = 0;
	return CheckInt32 ( tReader, iMin, iMax, sMessage, iValue, fnError );
}


bool CheckInt32 ( FileReader_c & tReader, int iMin, int iMax, const std::string & sMessage, int & iValue, Reporter_fn & fnError )
{
	iValue = (int)tReader.Read_uint32();
	if ( iValue<iMin || iValue>iMax )
	{
		fnError ( FormatStr ( "%s out of bounds: %d", sMessage.c_str(), iValue ).c_str() );
		return false;
	}

	return true;
}


bool CheckInt32Packed ( FileReader_c & tReader, int iMin, int iMax, const std::string & sMessage, int & iValue, Reporter_fn & fnError )
{
	iValue = (int)tReader.Unpack_uint32();
	if ( iValue<iMin || iValue>iMax )
	{
		fnError ( FormatStr ( "%s out of bounds: %d", sMessage.c_str(), iValue ).c_str() );
		return false;
	}

	return true;
}


bool CheckInt32Packed ( FileReader_c & tReader, int iMin, int iMax, const std::string & sMessage, Reporter_fn & fnError )
{
	int iValue = 0;
	return CheckInt32Packed ( tReader, iMin, iMax, sMessage, iValue, fnError );
}


bool CheckInt64 ( FileReader_c & tReader, int64_t iMin, int64_t iMax, const std::string & sMessage, int64_t & iValue, Reporter_fn & fnError )
{
	iValue = (int)tReader.Read_uint64();
	if ( iValue<iMin || iValue>iMax )
	{
		fnError ( FormatStr ( "%s out of bounds: %lld", sMessage.c_str(), iValue ).c_str() );
		return false;
	}

	return true;
}


bool CheckInt64 ( FileReader_c & tReader, int64_t iMin, int64_t iMax, const std::string & sMessage, Reporter_fn & fnError )
{
	int64_t iValue = 0;
	return CheckInt64 ( tReader, iMin, iMax, sMessage, iValue, fnError );
}

/////////////////////////////////////////////////////////////////////

void CheckStorage ( const std::string & sFilename, uint32_t uNumRows, Reporter_fn & fnError, Reporter_fn & fnProgress )
{
	StorageChecker_c tChecker ( sFilename, uNumRows, fnError, fnProgress );
	tChecker.Check();
}

} // namespace columnar

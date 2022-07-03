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

#include "builder.h"
#include "buildertraits.h"
#include "builderstr.h"
#include "builderbool.h"
#include "builderint.h"
#include "buildermva.h"

#include <memory>
#include <algorithm>

namespace columnar
{

using namespace util;
using namespace common;


class Builder_c final : public Builder_i
{
public:
	bool	Setup ( const Settings_t & tSettings, const Schema_t & tSchema, const std::string & sFile, std::string & sError );
	void	SetAttr ( int iAttr, int64_t tAttr ) final;
	void	SetAttr ( int iAttr, const uint8_t * pData, int iLength ) final;
	void	SetAttr ( int iAttr, const int64_t * pData, int iLength ) final;
	bool	Done ( std::string & sError ) final;

private:
	std::string	m_sFile;
	std::vector<std::vector<std::shared_ptr<Packer_i>>> m_dPackers;
	std::vector<std::shared_ptr<Packer_i>> m_dFlatPackers;

	bool	WriteHeaders ( FileWriter_c & tWriter, std::string & sError );
	bool	WriteBodies ( std::string & sError );
	void	Cleanup();
};


bool Builder_c::Setup ( const Settings_t & tSettings, const Schema_t & tSchema, const std::string & sFile, std::string & sError )
{
	m_sFile = sFile;

	int iPackers = 0;

	for ( const auto & i : tSchema )
	{
		std::vector<std::shared_ptr<Packer_i>> dPackers;

		switch ( i.m_eType )
		{
		case AttrType_e::UINT32:
		case AttrType_e::TIMESTAMP:
			dPackers.push_back ( std::shared_ptr<Packer_i> ( CreatePackerUint32 ( tSettings, i.m_sName ) ) );
			break;

		case AttrType_e::INT64:
			dPackers.push_back ( std::shared_ptr<Packer_i> ( CreatePackerInt64 ( tSettings, i.m_sName ) ) );
			break;

		case AttrType_e::BOOLEAN:
			dPackers.push_back ( std::shared_ptr<Packer_i> ( CreatePackerBool ( tSettings, i.m_sName ) ) );
			break;

		case AttrType_e::FLOAT:
			dPackers.push_back ( std::shared_ptr<Packer_i> ( CreatePackerFloat ( tSettings, i.m_sName ) ) );
			break;

		case AttrType_e::STRING:
			if ( i.m_fnCalcHash )
				dPackers.push_back ( std::shared_ptr<Packer_i> ( CreatePackerHash ( tSettings, GenerateHashAttrName ( i.m_sName ), i.m_fnCalcHash ) ) );

			dPackers.push_back ( std::shared_ptr<Packer_i> ( CreatePackerStr ( tSettings, i.m_sName ) ) );
			break;

		case AttrType_e::UINT32SET:
			dPackers.push_back ( std::shared_ptr<Packer_i> ( CreatePackerMva32 ( tSettings, i.m_sName ) ) );
			break;

		case AttrType_e::INT64SET:
			dPackers.push_back ( std::shared_ptr<Packer_i> ( CreatePackerMva64 ( tSettings, i.m_sName ) ) );
			break;

		default:
			break;
		}

		if ( !dPackers.empty() )
		{
			for ( auto & i : dPackers )
			{
				std::string sFilename = FormatStr ( "%s.%d", sFile.c_str(), iPackers++ );
				if ( !i->Setup ( sFilename, sError ) )
					return false;
			}

			m_dPackers.push_back ( std::move(dPackers) );
		}
		else
		{
			sError = FormatStr ( "unable to store attribute '%s' in columnar store", i.m_sName.c_str() );
			return false;
		}
	}

	for ( auto & i : m_dPackers )
		for ( auto & j : i )
			m_dFlatPackers.push_back(j);

	return true;
}


void Builder_c::SetAttr ( int iAttr, int64_t tAttr )
{
	for ( auto & i : m_dPackers[iAttr] )
		i->AddDoc(tAttr);
}


void Builder_c::SetAttr ( int iAttr, const uint8_t * pData, int iLength )
{
	for ( auto & i : m_dPackers[iAttr] )
		i->AddDoc ( pData, iLength );
}


void Builder_c::SetAttr ( int iAttr, const int64_t * pData, int iLength )
{
	for ( auto & i : m_dPackers[iAttr] )
		i->AddDoc ( pData, iLength );
}


bool Builder_c::WriteHeaders ( FileWriter_c & tWriter, std::string & sError )
{
	tWriter.Write_uint32 ( STORAGE_VERSION );
	tWriter.Write_uint32 ( (uint32_t)m_dFlatPackers.size() );
	for ( size_t i=0; i < m_dFlatPackers.size(); i++ )
	{
		auto & pPacker = m_dFlatPackers[i];
		if ( !pPacker->WriteHeader ( tWriter, sError ) )
			return false;

		int64_t tNextOffset = i<m_dFlatPackers.size()-1 ? tWriter.GetPos() : 0;
		tWriter.Write_uint64 ( tNextOffset + sizeof(int64_t) );
	}

	return true;
}


bool Builder_c::WriteBodies ( std::string & sError )
{
	return std::all_of ( m_dFlatPackers.cbegin(), m_dFlatPackers.cend(), [this, &sError]( auto & i ){ return i->WriteBody ( m_sFile, sError ); } );
}


void Builder_c::Cleanup()
{
	std::for_each ( m_dFlatPackers.cbegin(), m_dFlatPackers.cend(), []( auto & i ){ return i->Cleanup(); } );
}


bool Builder_c::Done ( std::string & sError )
{
	std::for_each ( m_dFlatPackers.cbegin(), m_dFlatPackers.cend(), []( auto & i ){ i->Done(); } );

	// [N][header0][offset_of_header1]...[body0]...

	{
		FileWriter_c tWriter;
		if ( !tWriter.Open ( m_sFile, sError ) )
			return false;

		if ( !WriteHeaders ( tWriter, sError) )
			return false;

		int64_t tBodyOffset = tWriter.GetPos();
		for ( auto & i : m_dFlatPackers )
		{
			i->CorrectOffset ( tWriter, tBodyOffset );
			tBodyOffset += i->GetBodySize();
		}
	}

	if ( !WriteBodies(sError) )
		return false;

	Cleanup();

	return true;
}


bool CheckSubblockSize ( int iSubblockSize, std::string & sError )
{
	const int MIN_SUBBLOCK_SIZE = 128;

	if ( iSubblockSize < MIN_SUBBLOCK_SIZE )
	{
		FormatStr ( "Subblock sizes less than %d are not supported (%d specified)", MIN_SUBBLOCK_SIZE, iSubblockSize );
		return false;
	}

	if ( iSubblockSize & ( MIN_SUBBLOCK_SIZE-1 ) )
	{
		FormatStr ( "Subblock size should be a multiple of %d (%d specified)", MIN_SUBBLOCK_SIZE, iSubblockSize );
		return false;
	}

	return true;
}

} // namespace columnar


columnar::Builder_i * CreateColumnarBuilder ( const columnar::Settings_t & tSettings, const columnar::Schema_t & tSchema, const std::string & sFile, std::string & sError )
{
	if ( !columnar::CheckSubblockSize ( tSettings.m_iSubblockSize, sError ) )
		return nullptr;

	std::unique_ptr<columnar::Builder_c> pBuilder ( new columnar::Builder_c );
	if ( !pBuilder->Setup ( tSettings, tSchema, sFile, sError ) )
		return nullptr;

	return pBuilder.release();
}
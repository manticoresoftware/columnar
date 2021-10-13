// Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
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

class Builder_c final : public Builder_i
{
public:
	bool	Setup ( const Settings_t & tSettings, const Schema_t & tSchema, const std::string & sFile, std::string & sError );
	void	SetAttr ( int iAttr, int64_t tAttr ) final;
	void	SetAttr ( int iAttr, const uint8_t * pData, int iLength ) final;
	void	SetAttr ( int iAttr, const int64_t * pData, int iLength ) final;
	bool	Done ( std::string & sError ) final;

private:
	std::string m_sFile;
	std::vector<std::shared_ptr<Packer_i>> m_dPackers;

	bool	WriteHeaders ( FileWriter_c & tWriter, std::string & sError );
	bool	WriteBodies ( std::string & sError );
	void	Cleanup();
};


bool Builder_c::Setup ( const Settings_t & tSettings, const Schema_t & tSchema, const std::string & sFile, std::string & sError )
{
	m_sFile = sFile;

	for ( const auto & i : tSchema )
	{
		std::shared_ptr<Packer_i> pPacker;
		switch ( i.m_eType )
		{
		case AttrType_e::UINT32:
		case AttrType_e::TIMESTAMP:
			pPacker = std::shared_ptr<Packer_i> ( CreatePackerUint32 ( tSettings, i.m_sName ) );
			break;

		case AttrType_e::INT64:
			pPacker = std::shared_ptr<Packer_i> ( CreatePackerUint64 ( tSettings, i.m_sName ) );
			break;

		case AttrType_e::BOOLEAN:
			pPacker = std::shared_ptr<Packer_i> ( CreatePackerBool ( tSettings, i.m_sName ) );
			break;

		case AttrType_e::FLOAT:
			pPacker = std::shared_ptr<Packer_i> ( CreatePackerFloat ( tSettings, i.m_sName ) );
			break;

		case AttrType_e::STRING:
			pPacker = std::shared_ptr<Packer_i> ( CreatePackerStr ( tSettings, i.m_sName, i.m_fnCalcHash ) );
			break;

		case AttrType_e::UINT32SET:
			pPacker = std::shared_ptr<Packer_i> ( CreatePackerMva32 ( tSettings, i.m_sName ) );
			break;

		case AttrType_e::INT64SET:
			pPacker = std::shared_ptr<Packer_i> ( CreatePackerMva64 ( tSettings, i.m_sName ) );
			break;

		default:
			break;
		}

		if ( pPacker )
		{
			std::string sFilename = FormatStr ( "%s.%zu", sFile.c_str(), m_dPackers.size() );
			if ( pPacker->Setup ( sFilename, sError ) )
				m_dPackers.push_back(pPacker);
			else
				return false;
		}
		else
		{
			sError = FormatStr ( "unable to store attribute '%s' in columnar store", i.m_sName.c_str() );
			return false;
		}
	}

	return true;
}


void Builder_c::SetAttr ( int iAttr, int64_t tAttr )
{
	m_dPackers[iAttr]->AddDoc(tAttr);
}


void Builder_c::SetAttr ( int iAttr, const uint8_t * pData, int iLength )
{
	m_dPackers[iAttr]->AddDoc ( pData, iLength );
}


void Builder_c::SetAttr ( int iAttr, const int64_t * pData, int iLength )
{
	m_dPackers[iAttr]->AddDoc ( pData, iLength );
}


bool Builder_c::WriteHeaders ( FileWriter_c & tWriter, std::string & sError )
{
	tWriter.Write_uint32 ( STORAGE_VERSION );
	tWriter.Write_uint32 ( (uint32_t)m_dPackers.size() );
	for ( size_t i=0; i < m_dPackers.size(); i++ )
	{
		auto & pPacker = m_dPackers[i];
		if ( !pPacker->WriteHeader ( tWriter, sError ) )
			return false;

		int64_t tNextOffset = i<m_dPackers.size()-1 ? tWriter.GetPos() : 0;
		tWriter.Write_uint64 ( tNextOffset + sizeof(int64_t) );
	}

	return true;
}


bool Builder_c::WriteBodies ( std::string & sError )
{
	return std::all_of ( m_dPackers.cbegin(), m_dPackers.cend(), [this, &sError]( auto & i ){ return i->WriteBody ( m_sFile, sError ); } );
}


void Builder_c::Cleanup()
{
	std::for_each ( m_dPackers.cbegin(), m_dPackers.cend(), []( auto & i ){ return i->Cleanup(); } );
}


bool Builder_c::Done ( std::string & sError )
{
	std::for_each ( m_dPackers.cbegin(), m_dPackers.cend(), []( auto & i ){ i->Done(); } );

	// [N][header0][offset_of_header1]...[body0]...

	{
		FileWriter_c tWriter;
		if ( !tWriter.Open ( m_sFile, sError ) )
			return false;

		if ( !WriteHeaders ( tWriter, sError) )
			return false;

		int64_t tBodyOffset = tWriter.GetPos();
		for ( auto & i : m_dPackers )
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
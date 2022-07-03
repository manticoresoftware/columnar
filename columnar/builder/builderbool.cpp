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

#include "builderbool.h"
#include "buildertraits.h"
#include "builderminmax.h"

namespace columnar
{

using namespace util;
using namespace common;

class AttributeHeaderBuilder_Bool_c : public AttributeHeaderBuilder_c
{
	using BASE = AttributeHeaderBuilder_c;

public:
	MinMaxBuilder_T<uint8_t> m_tMinMax;

			AttributeHeaderBuilder_Bool_c ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType );

	bool	Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError );
};


AttributeHeaderBuilder_Bool_c::AttributeHeaderBuilder_Bool_c ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType )
	: BASE ( tSettings, sName, eType )
	, m_tMinMax ( tSettings )
{}


bool AttributeHeaderBuilder_Bool_c::Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError )
{
	if ( !BASE::Save ( tWriter, tBaseOffset, sError ) )
		return false;

	return m_tMinMax.Save ( tWriter, sError );
}

//////////////////////////////////////////////////////////////////////////


class Packer_Bool_c : public PackerTraits_T<AttributeHeaderBuilder_Bool_c>
{
public:
	using BASE = PackerTraits_T<AttributeHeaderBuilder_Bool_c>;
	using BASE::m_tWriter;
	using BASE::m_tHeader;

						Packer_Bool_c ( const Settings_t & tSettings, const std::string & sName );

	void				AddDoc ( int64_t tAttr ) override;
	void				AddDoc ( const uint8_t * pData, int iLength ) override;
	void				AddDoc ( const int64_t * pData, int iLength ) override;
	void				Flush() override;

protected:
	bool					m_bFirst = true;
	bool					m_bConst = true;
	bool					m_bConstValue = false;
	std::vector<bool>		m_dCollected;
	std::vector<uint32_t>	m_dValues;
	std::vector<uint32_t>	m_dPacked;


	void				AnalyzeCollected ( int64_t tAttr );

	BoolPacking_e		ChoosePacking() const;

	void				WritePacked_Const();
	void				WritePacked_Bitmap();

	void				WriteToFile ( BoolPacking_e ePacking );
};


Packer_Bool_c::Packer_Bool_c ( const Settings_t & tSettings, const std::string & sName )
	: BASE ( tSettings, sName, AttrType_e::BOOLEAN )
{
	assert ( !( tSettings.m_iSubblockSize & 127 ) );
	m_dValues.resize ( tSettings.m_iSubblockSize );
	m_dPacked.resize ( tSettings.m_iSubblockSize >> 5 );
}


void Packer_Bool_c::AddDoc ( int64_t tAttr )
{
	if ( m_dCollected.size()==DOCS_PER_BLOCK )
		Flush();

	AnalyzeCollected(tAttr);
	m_dCollected.push_back ( !!tAttr );
}


void Packer_Bool_c::AddDoc ( const uint8_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to bool packer" );
}


void Packer_Bool_c::AddDoc ( const int64_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending MVA to bool packer" );
}


void Packer_Bool_c::AnalyzeCollected ( int64_t tAttr )
{
	bool bValue = !!tAttr;

	if ( m_bFirst )
	{
		m_bConstValue = bValue;
		m_bFirst = false;
	}

	if ( m_bConstValue!=bValue )
		m_bConst = false;

	m_tHeader.m_tMinMax.Add ( bValue ? 1 : 0 );
}


BoolPacking_e Packer_Bool_c::ChoosePacking() const
{
	if ( m_bConst )
		return BoolPacking_e::CONST;

	return BoolPacking_e::BITMAP;
}


void Packer_Bool_c::WriteToFile ( BoolPacking_e ePacking )
{
	m_tWriter.Pack_uint32 ( to_underlying(ePacking) );

	switch ( ePacking )
	{
	case BoolPacking_e::CONST:
		WritePacked_Const();
		break;

	case BoolPacking_e::BITMAP:
		WritePacked_Bitmap();
		break;

	default:
		assert ( 0 && "Unknown packing" );
		break;
	}
}


void Packer_Bool_c::Flush()
{
	if ( m_dCollected.empty() )
		return;

	m_tHeader.AddBlock ( m_tWriter.GetPos() );

	WriteToFile ( ChoosePacking() );

	m_dCollected.resize(0);
	m_bFirst = true;
	m_bConst = true;
	m_bConstValue = false;
}


void Packer_Bool_c::WritePacked_Const()
{
	m_tWriter.Write_uint8 ( m_bConstValue ? 1 : 0 );
}


void Packer_Bool_c::WritePacked_Bitmap()
{
	const int iSubblockSize = m_tHeader.GetSettings().m_iSubblockSize;

	int iId = 0;
	for ( size_t i=0; i < m_dCollected.size(); i++ )
	{
		m_dValues[iId++] = m_dCollected[i] ? 1 : 0;
		if ( iId==iSubblockSize )
		{
			BitPack ( m_dValues, m_dPacked, 1 );
			m_tWriter.Write ( (uint8_t*)m_dPacked.data(), m_dPacked.size()*sizeof(m_dPacked[0]) );
			iId = 0;
		}
	}

	if ( iId )
	{
		// zero out unused values
		memset ( m_dValues.data()+iId, 0, (m_dValues.size()-iId)*sizeof(m_dValues[0]) );
		BitPack ( m_dValues, m_dPacked, 1 );
		m_tWriter.Write ( (uint8_t*)m_dPacked.data(), m_dPacked.size()*sizeof(m_dPacked[0]) );
	}
}

//////////////////////////////////////////////////////////////////////////

Packer_i * CreatePackerBool ( const Settings_t & tSettings, const std::string & sName )
{
	return new Packer_Bool_c ( tSettings, sName );
}

} // namespace columnar
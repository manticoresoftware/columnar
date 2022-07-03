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

#include "builderstr.h"
#include "buildertraits.h"
#include "builderminmax.h"

#include "memory"
#include <unordered_map>
#include <algorithm>

namespace columnar
{

using namespace util;
using namespace common;


class AttributeHeaderBuilder_String_c : public AttributeHeaderBuilder_c
{
	using BASE = AttributeHeaderBuilder_c;

public:
	MinMaxBuilder_T<uint32_t> m_tMinMax;

			AttributeHeaderBuilder_String_c ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType );

	bool	Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError );
};


AttributeHeaderBuilder_String_c::AttributeHeaderBuilder_String_c ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType )
	: BASE ( tSettings, sName, eType )
	, m_tMinMax ( tSettings )
{}


bool AttributeHeaderBuilder_String_c::Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError )
{
	if ( !BASE::Save ( tWriter, tBaseOffset, sError ) )
		return false;

	if ( !m_tMinMax.Save ( tWriter, sError ) )
		return false;

	return !tWriter.IsError();
}


class Packer_String_c : public PackerTraits_T<AttributeHeaderBuilder_String_c>
{
	using BASE = PackerTraits_T<AttributeHeaderBuilder_String_c>;

public:
							Packer_String_c ( const Settings_t & tSettings, const std::string & sName );

	void					AddDoc ( int64_t tAttr ) final;
	void					AddDoc ( const uint8_t * pData, int iLength ) final;
	void					AddDoc ( const int64_t * pData, int iLength ) final;

protected:
	std::unique_ptr<IntCodec_i>	m_pCodec;

	std::vector<std::string>	m_dCollected;
	std::unordered_map<std::string, int> m_hUnique;
	std::vector<std::string>	m_dUniques;
	std::vector<uint64_t>		m_dOffsets;

	// used by table encoding
	std::vector<uint32_t>	m_dTableLengths;
	std::vector<uint32_t>	m_dTableIndexes;

	std::vector<uint32_t>	m_dUncompressed32;
	std::vector<uint64_t>	m_dUncompressed;
	std::vector<uint32_t>	m_dCompressed;

	int						m_iUniques = 0;
	int						m_iConstLength = -1;

	std::vector<uint8_t>	m_dTmpBuffer;
	std::vector<uint8_t>	m_dTmpBuffer2;
	std::vector<uint64_t>	m_dTmpLengths;

	void					Flush() override;
	StrPacking_e			ChoosePacking() const;
	void					AnalyzeCollected ( const uint8_t * pData, int iLength );
	void					WriteToFile ( StrPacking_e ePacking );

	void					WritePacked_Const();
	void					WritePacked_ConstLen();
	void					WritePacked_Table();
	void					WritePacked_Generic();

	void					WriteOffsets();
};


Packer_String_c::Packer_String_c ( const Settings_t & tSettings, const std::string & sName )
	: BASE ( tSettings, sName, AttrType_e::STRING )
	, m_pCodec ( CreateIntCodec ( tSettings.m_sCompressionUINT32, tSettings.m_sCompressionUINT64 ) )
{
	m_dTableIndexes.resize ( tSettings.m_iSubblockSize );
}


void Packer_String_c::AddDoc ( int64_t tAttr )
{
	assert ( 0 && "INTERNAL ERROR: sending integers to string packer" );
}


void Packer_String_c::AddDoc ( const uint8_t * pData, int iLength )
{
	if ( m_dCollected.size()==DOCS_PER_BLOCK )
		Flush();

	AnalyzeCollected ( pData, iLength );
	m_dCollected.push_back ( std::string ( (const char*)pData, iLength ) );
}


void Packer_String_c::AddDoc ( const int64_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending MVA to string packer" );
}


void Packer_String_c::Flush()
{
	if ( m_dCollected.empty() )
		return;

	m_tHeader.AddBlock ( m_tWriter.GetPos() );

	WriteToFile ( ChoosePacking() );

	m_dCollected.resize(0);

	m_iUniques = 0;
	m_hUnique.clear();
	m_iConstLength = -1;
}


void Packer_String_c::AnalyzeCollected ( const uint8_t * pData, int iLength )
{
	if ( !m_iUniques )
		m_iConstLength = iLength;

	if ( iLength!=m_iConstLength )
		m_iConstLength = -1;

	// if we've got over 256 uniques, no point in further checks
	if ( m_iUniques<256 )
	{
		std::string sStr ( (const char*)pData, iLength );
		
		if ( !m_hUnique.count(sStr) )
		{
			m_hUnique.insert ( { sStr, 0 } );
			m_iUniques++;
		}
	}

	m_tHeader.m_tMinMax.Add(iLength);
}


StrPacking_e Packer_String_c::ChoosePacking() const
{
	if ( m_iUniques==1 )
		return StrPacking_e::CONST;

	if ( m_iUniques<256 )
		return StrPacking_e::TABLE;

	if ( m_iConstLength!=-1 )
		return StrPacking_e::CONSTLEN;

	return StrPacking_e::GENERIC;
}


void Packer_String_c::WriteToFile ( StrPacking_e ePacking )
{
	m_tWriter.Pack_uint32 ( to_underlying(ePacking) );

	switch ( ePacking )
	{
	case StrPacking_e::CONST:
		WritePacked_Const();
		break;

	case StrPacking_e::TABLE:
		WritePacked_Table();
		break;

	case StrPacking_e::CONSTLEN:
		WritePacked_ConstLen();
		break;

	case StrPacking_e::GENERIC:
		WritePacked_Generic();
		break;

	default:
		assert ( 0 && "Unknown packing" );
		break;
	}
}


void Packer_String_c::WritePacked_Const()
{
	assert ( m_iUniques==1 );

	m_tWriter.Pack_uint32(m_iConstLength);
	m_tWriter.Write ( (const uint8_t*)m_dCollected[0].c_str(), m_dCollected[0].length() );
}


void Packer_String_c::WritePacked_Table()
{
	assert ( m_iUniques<256 );

	m_dUniques.resize(0);
	for ( const auto & i : m_hUnique )
		m_dUniques.push_back ( i.first );

	std::sort ( m_dUniques.begin(), m_dUniques.end(), []( const auto & a, const auto & b ) { return a.length()<b.length(); } );

	for ( size_t i = 0; i < m_dUniques.size(); i++ )
	{
		auto tFound = m_hUnique.find ( m_dUniques[i] );
		assert ( tFound!=m_hUnique.end() );
		tFound->second = (int)i;
	}

	m_dTableLengths.resize ( m_dUniques.size() );
	for ( size_t i = 0; i < m_dUniques.size(); i++ )
		m_dTableLengths[i] = (uint32_t)m_dUniques[i].length();

	// write the table
	m_tWriter.Write_uint8 ( (uint8_t)m_dUniques.size() );
	WriteValues_Delta_PFOR ( Span_T<uint32_t>(m_dTableLengths), m_dUncompressed32, m_dCompressed, BASE::m_tWriter, m_pCodec.get() );

	for ( const auto & i : m_dUniques )
		m_tWriter.Write ( (const uint8_t*)i.c_str(), i.length() );

	WriteTableOrdinals ( m_dUniques, m_hUnique, m_dCollected, m_dTableIndexes, m_dCompressed, m_tHeader.GetSettings().m_iSubblockSize, m_tWriter );
}


void Packer_String_c::WritePacked_ConstLen()
{
	assert ( m_iConstLength>=0 );
	m_tWriter.Pack_uint32 ( m_iConstLength );

	for ( const auto & i : m_dCollected )
		m_tWriter.Write ( (const uint8_t*)i.c_str(), i.length() );
}


void Packer_String_c::WritePacked_Generic()
{
	int iSubblockSize = m_tHeader.GetSettings().m_iSubblockSize;
	int iBlocks = ( (int)m_dCollected.size() + iSubblockSize - 1 ) / iSubblockSize;

	m_dOffsets.resize(iBlocks);
	m_dTmpBuffer.resize(0);

	MemWriter_c tMemWriter ( m_dTmpBuffer );

	int iBlockStart = 0;
	for ( int iBlock=0; iBlock < (int)m_dOffsets.size(); iBlock++ )
	{
		int iBlockValues = GetSubblockSize ( iBlock, iBlocks, (int)m_dCollected.size(), iSubblockSize );
		m_dOffsets[iBlock] = tMemWriter.GetPos();

		// write lengths
		m_dTmpLengths.resize(iBlockValues);
		for ( int i = 0; i<iBlockValues; i++ )
			m_dTmpLengths[i] = m_dCollected[iBlockStart+i].size();

		WriteValues_PFOR ( Span_T<uint64_t>(m_dTmpLengths), m_dUncompressed, m_dCompressed, tMemWriter, m_pCodec.get(), true );

		// write bodies
		for ( int i = 0; i<iBlockValues; i++ )
		{
			const std::string & sCollected = m_dCollected[iBlockStart+i];
			tMemWriter.Write ( (const uint8_t*)sCollected.c_str(), sCollected.length() );
		}

		iBlockStart += iBlockValues;
	}

	WriteOffsets();

	m_tWriter.Write ( m_dTmpBuffer.data(), m_dTmpBuffer.size()*sizeof ( m_dTmpBuffer[0] ) );
}


void Packer_String_c::WriteOffsets()
{
	assert ( !m_dOffsets[0] );

	// write sub-block sizes to a in-memory buffer
	m_dTmpBuffer2.resize(0);
	MemWriter_c tMemWriterOffsets(m_dTmpBuffer2);

	WriteValues_Delta_PFOR ( Span_T<uint64_t>(m_dOffsets), m_dUncompressed, m_dCompressed, tMemWriterOffsets, m_pCodec.get() );

	// write compressed offsets
	m_tWriter.Write ( m_dTmpBuffer2.data(), m_dTmpBuffer2.size()*sizeof ( m_dTmpBuffer2[0] ) );
}

//////////////////////////////////////////////////////////////////////////

Packer_i * CreatePackerStr ( const Settings_t & tSettings, const std::string & sName )
{
	return new Packer_String_c ( tSettings, sName );
}

} // namespace columnar
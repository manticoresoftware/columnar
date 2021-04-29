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

#include "builderint.h"
#include "buildertraits.h"
#include "builderminmax.h"

#include <unordered_map>
#include <algorithm>

namespace columnar
{

template <typename T>
class AttributeHeaderBuilder_Int_T : public AttributeHeaderBuilder_c
{
	using BASE = AttributeHeaderBuilder_c;

public:
	MinMaxBuilder_T<T>	m_tMinMax;

			AttributeHeaderBuilder_Int_T ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType );

	bool	Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError );
};

template <typename T>
AttributeHeaderBuilder_Int_T<T>::AttributeHeaderBuilder_Int_T ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType )
	: BASE ( tSettings, sName, eType )
	, m_tMinMax ( tSettings )
{}

template <typename T>
bool AttributeHeaderBuilder_Int_T<T>::Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError )
{
	if ( !BASE::Save ( tWriter, tBaseOffset, sError ) )
		return false;

	return m_tMinMax.Save ( tWriter, sError );
}

//////////////////////////////////////////////////////////////////////////

class AttributeHeaderBuilder_Float_c : public AttributeHeaderBuilder_c
{
	using BASE = AttributeHeaderBuilder_c;

public:
	MinMaxBuilder_T<float>	m_tMinMax;

			AttributeHeaderBuilder_Float_c ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType );

	bool	Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError );
};


AttributeHeaderBuilder_Float_c::AttributeHeaderBuilder_Float_c ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType )
	: BASE ( tSettings, sName, eType )
	, m_tMinMax ( tSettings )
{}


bool AttributeHeaderBuilder_Float_c::Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError )
{
	if ( !BASE::Save ( tWriter, tBaseOffset, sError ) )
		return false;

	return m_tMinMax.Save ( tWriter, sError );
}

//////////////////////////////////////////////////////////////////////////

template <typename T, typename HEADER>
class Packer_Int_T : public PackerTraits_T<HEADER>
{
public:
	using BASE = PackerTraits_T<HEADER>;
	using BASE::m_tWriter;
	using BASE::m_tHeader;

						Packer_Int_T ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType );

	void				AddDoc ( int64_t tAttr ) override;
	void				AddDoc ( const uint8_t * pData, int iLength ) override;
	void				AddDoc ( const int64_t * pData, int iLength ) override;
	void				Flush() override;

private:
	T						m_tMin = T(0);
	T						m_tMax = T(0);
	T						m_tPrevValue = T(0);

	std::unordered_map<T,int> m_hUnique { DOCS_PER_BLOCK };
	std::vector<T>			m_dUniques;
	int						m_iUniques = 0;
	std::vector<uint32_t>	m_dTableIndexes;
	std::vector<uint32_t>	m_dTablePacked;

	bool					m_bMonoAsc = true;
	bool					m_bMonoDesc = true;
	std::vector<uint8_t>	m_dTmpBuffer;
	std::vector<T>			m_dCollected;

	std::unique_ptr<IntCodec_i>	m_pCodec;
	std::vector<uint32_t>	m_dCompressed;
	std::vector<T>			m_dUncompressed;
	std::vector<uint32_t>	m_dUncompressed32;
	std::vector<uint8_t>	m_dTmpBuffer2;
	std::vector<uint32_t>	m_dSubblockSizes;

	void				AnalyzeCollected ( int64_t tAttr );
	IntPacking_e		ChoosePacking() const;
	void				WriteToFile ( IntPacking_e ePacking );

	void				WritePacked_Const();
	void				WritePacked_Table();

	template <typename U, typename WRITER>
	void				WriteSubblock_Delta ( const Span_T<U> & dSubblockValues, WRITER & tWriter, std::vector<U> & dTmp, bool bWriteFlag );

	template <typename WRITESUBBLOCK>
	void				WritePacked_PFOR ( IntPacking_e ePacking, WRITESUBBLOCK && fnWriteSubblock );
};

template <typename T, typename HEADER>
Packer_Int_T<T,HEADER>::Packer_Int_T ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType )
	: BASE ( tSettings, sName, eType )
	, m_pCodec ( CreateIntCodec ( tSettings.m_sCompressionUINT32, tSettings.m_sCompressionUINT64 ) )
{
	assert ( tSettings.m_iSubblockSize==128 );
	m_dTableIndexes.resize ( tSettings.m_iSubblockSize );
}

template <typename T, typename HEADER>
void Packer_Int_T<T,HEADER>::AddDoc ( int64_t tAttr )
{
	if ( m_dCollected.size()==DOCS_PER_BLOCK )
		Flush();

	AnalyzeCollected(tAttr);
	m_dCollected.push_back ( (T)tAttr );
}

template <typename T, typename HEADER>
void Packer_Int_T<T,HEADER>::AddDoc ( const uint8_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to integer packer" );
}

template <typename T, typename HEADER>
void Packer_Int_T<T,HEADER>::AddDoc ( const int64_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending MVA to integer packer" );
}

template <typename T, typename HEADER>
void Packer_Int_T<T,HEADER>::AnalyzeCollected ( int64_t tAttr )
{
	T tValue = (T)tAttr;

	if ( !m_iUniques )
	{
		m_tMin = tValue;
		m_tMax = tValue;
	}
	else
	{
		m_tMin = std::min ( m_tMin, tValue );
		m_tMax = std::max ( m_tMax, tValue );

		m_bMonoAsc  &= tValue>=m_tPrevValue;
		m_bMonoDesc &= tValue<=m_tPrevValue;
	}

	// if we've got over 256 uniques, no point in further checks
	if ( m_iUniques<256 && !m_hUnique.count(tValue) )
	{
		m_hUnique.insert ( { tValue, 0 } );
		m_iUniques++;
	}

	m_tHeader.m_tMinMax.Add(tValue);

	m_tPrevValue = tValue;
}

template <typename T, typename HEADER>
IntPacking_e Packer_Int_T<T,HEADER>::ChoosePacking() const
{
	if ( m_iUniques==1 )
		return IntPacking_e::CONST;

	if ( m_iUniques<256 )
		return IntPacking_e::TABLE;

	if ( m_bMonoAsc || m_bMonoDesc )
		return IntPacking_e::DELTA;

	return IntPacking_e::GENERIC;
}

template <typename T, typename HEADER>
void Packer_Int_T<T,HEADER>::WriteToFile ( IntPacking_e ePacking )
{
	m_tWriter.Pack_uint32 ( to_underlying(ePacking) );

	switch ( ePacking )
	{
	case IntPacking_e::CONST:
		WritePacked_Const();
		break;

	case IntPacking_e::TABLE:
		WritePacked_Table();
		break;

	case IntPacking_e::DELTA:
		WritePacked_PFOR ( ePacking, [this]( const Span_T<T> & dSubblockValues, MemWriter_c & tWriter )
			{ WriteSubblock_Delta ( dSubblockValues, tWriter, m_dUncompressed, true ); }
		);
		break;

	case IntPacking_e::GENERIC:
		WritePacked_PFOR ( ePacking, [this]( const Span_T<T> & dSubblockValues, MemWriter_c & tWriter )
			{ WriteValues_PFOR ( dSubblockValues, m_dUncompressed, m_dCompressed, tWriter, m_pCodec.get(), false ); }
		);
		break;

	default:
		assert ( 0 && "Unknown packing" );
		break;
	}
}

template <typename T, typename HEADER>
void Packer_Int_T<T,HEADER>::Flush()
{
	if ( m_dCollected.empty() )
		return;

	m_tHeader.AddBlock ( m_tWriter.GetPos() );

	WriteToFile ( ChoosePacking() );

	m_dCollected.resize(0);
	m_hUnique.clear();
	m_tPrevValue = 0;
	m_iUniques = 0;
	m_bMonoAsc = m_bMonoDesc = true;
}

template <typename T, typename HEADER>
void Packer_Int_T<T,HEADER>::WritePacked_Const()
{
	m_tWriter.Pack_uint64 ( (uint64_t)m_dCollected[0] );
}

template <typename T, typename HEADER>
void Packer_Int_T<T,HEADER>::WritePacked_Table()
{
	assert ( m_iUniques<256 );

	m_dUniques.resize(0);
	for ( const auto & i : m_hUnique )
		m_dUniques.push_back ( i.first );

	std::sort ( m_dUniques.begin(), m_dUniques.end() );

	for ( size_t i = 0; i < m_dUniques.size(); i++ )
	{
		auto tFound = m_hUnique.find ( m_dUniques[i] );
		assert ( tFound!=m_hUnique.end() );
		tFound->second = (int)i;
	}

	// write the table
	m_tWriter.Write_uint8 ( (uint8_t)m_dUniques.size() );
	WriteValues_Delta_PFOR ( Span_T<T>(m_dUniques), m_dUncompressed, m_dCompressed, m_tWriter, m_pCodec.get() );
	WriteTableOrdinals ( m_dUniques, m_hUnique, m_dCollected, m_dTableIndexes, m_dTablePacked, m_tWriter );
}

template <typename T, typename HEADER>
template <typename U, typename WRITER>
void Packer_Int_T<T,HEADER>::WriteSubblock_Delta ( const Span_T<U> & dSubblockValues, WRITER & tWriter, std::vector<U> & dTmp, bool bWriteFlag )
{
	dTmp.resize ( dSubblockValues.size() );
	memcpy ( dTmp.data(), dSubblockValues.data(), dSubblockValues.size()*sizeof(dSubblockValues[0]) );
	ComputeDeltas ( dTmp.data(), (int)dTmp.size(), bWriteFlag ? m_bMonoAsc : true );

	if ( bWriteFlag )
		tWriter.Write_uint8 ( m_bMonoAsc ? to_underlying ( IntDeltaPacking_e::DELTA_ASC ) : to_underlying ( IntDeltaPacking_e::DELTA_DESC ) );

	tWriter.Pack_uint64 ( dTmp[0] );
	dTmp[0]=0;

	m_pCodec->Encode ( dTmp, m_dCompressed );

	tWriter.Write ( (uint8_t*)m_dCompressed.data(), m_dCompressed.size()*sizeof ( m_dCompressed[0] ) );
}

template <typename T, typename HEADER>
template <typename WRITESUBBLOCK>
void Packer_Int_T<T,HEADER>::WritePacked_PFOR ( IntPacking_e ePacking, WRITESUBBLOCK && fnWriteSubblock )
{
	int iSubblockSize = m_tHeader.GetSettings().m_iSubblockSize;
	int iBlocks = ( (int)m_dCollected.size() + iSubblockSize - 1 ) / iSubblockSize;

	m_dSubblockSizes.resize(iBlocks);

	m_dTmpBuffer.resize(0);
	MemWriter_c tMemWriter(m_dTmpBuffer);

	int iBlockStart = 0;
	for ( int iBlock=0; iBlock < (int)m_dSubblockSizes.size(); iBlock++ )
	{
		int iBlockValues = GetSubblockSize ( iBlock, iBlocks, (int)m_dCollected.size(), iSubblockSize );

		int64_t iSubblockStart = tMemWriter.GetPos();
		fnWriteSubblock ( Span_T<T> ( &m_dCollected[iBlockStart], iBlockValues ), tMemWriter );
		m_dSubblockSizes[iBlock] = uint32_t ( tMemWriter.GetPos()-iSubblockStart );

		iBlockStart += iBlockValues;
	}

	// write sub-block sizes to a in-memory buffer
	m_dTmpBuffer2.resize(0);
	MemWriter_c tMemWriterSizes(m_dTmpBuffer2);

	// note that these are 32-bit uints
	ComputeInverseDeltas ( m_dSubblockSizes, true );
	WriteSubblock_Delta ( Span_T<uint32_t>(m_dSubblockSizes), tMemWriterSizes, m_dUncompressed32, false );

	m_tWriter.Pack_uint32 ( (uint32_t)m_dTmpBuffer2.size() );

	// write compressed sub-block lengths
	m_tWriter.Write ( m_dTmpBuffer2.data(), m_dTmpBuffer2.size()*sizeof ( m_dTmpBuffer2[0] ) );

	// write the compressed sub-blocks
	m_tWriter.Write ( m_dTmpBuffer.data(), m_dTmpBuffer.size()*sizeof ( m_dTmpBuffer[0] ) );
}


//////////////////////////////////////////////////////////////////////////

class Packer_Float_c : public Packer_Int_T<uint32_t, AttributeHeaderBuilder_Float_c>
{
	using BASE = Packer_Int_T<uint32_t, AttributeHeaderBuilder_Float_c>;

public:
	Packer_Float_c ( const Settings_t & tSettings, const std::string & sName ) : BASE ( tSettings, sName, AttrType_e::FLOAT ) {}
};

//////////////////////////////////////////////////////////////////////////

Packer_i * CreatePackerUint32 ( const Settings_t & tSettings, const std::string & sName )
{
	return new Packer_Int_T<uint32_t,AttributeHeaderBuilder_Int_T<uint32_t>> ( tSettings, sName, AttrType_e::UINT32 );
}


Packer_i * CreatePackerUint64 ( const Settings_t & tSettings, const std::string & sName )
{
	return new Packer_Int_T<uint64_t,AttributeHeaderBuilder_Int_T<uint64_t>> ( tSettings, sName, AttrType_e::INT64 );
}


Packer_i * CreatePackerFloat ( const Settings_t & tSettings, const std::string & sName )
{
	return new Packer_Float_c ( tSettings, sName );
}

} // namespace columnar
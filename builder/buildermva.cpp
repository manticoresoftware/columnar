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

#include "buildermva.h"
#include "buildertraits.h"
#include "builderminmax.h"

#include <unordered_map>

namespace columnar
{

template <typename T>
class AttributeHeaderBuilder_MVA_T : public AttributeHeaderBuilder_c
{
	using BASE = AttributeHeaderBuilder_c;

public:
	MinMaxBuilder_T<T>	m_tMinMax;

			AttributeHeaderBuilder_MVA_T ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType );

	bool	Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError );
};

template <typename T>
AttributeHeaderBuilder_MVA_T<T>::AttributeHeaderBuilder_MVA_T ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType )
	: BASE ( tSettings, sName, eType )
	, m_tMinMax ( tSettings )
{}

template <typename T>
bool AttributeHeaderBuilder_MVA_T<T>::Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError )
{
	if ( !BASE::Save ( tWriter, tBaseOffset, sError ) )
		return false;

	return m_tMinMax.Save ( tWriter, sError );
}

//////////////////////////////////////////////////////////////////////////

template<typename T>
struct HashFunc_Vec_T
{
	std::size_t operator() ( const std::vector<T> & dKey ) const
	{
		std::size_t uRes = dKey.size();
		for ( auto i : dKey )
			uRes ^= i + 0x9e3779b9 + (uRes << 6) + (uRes >> 2);

		return uRes;
	}
};


template <typename T>
class Packer_MVA_T : public PackerTraits_T<AttributeHeaderBuilder_MVA_T<T>>
{
	using BASE = PackerTraits_T<AttributeHeaderBuilder_MVA_T<T>>;

public:
					Packer_MVA_T ( const Settings_t & tSettings, const std::string & sName, AttrType_e eAttr );

protected:
	void			AddDoc ( int64_t tAttr ) final;
	void			AddDoc ( const uint8_t * pData, int iLength ) final;
	void			AddDoc ( const int64_t * pData, int iLength ) final;
	void			Flush() final;

	void			AnalyzeCollected ( const int64_t * pData, int iLength );
	MvaPacking_e	ChoosePacking() const;
	void			WriteToFile ( MvaPacking_e ePacking );

private:
	std::vector<uint32_t>		m_dCollectedLengths;
	std::vector<T>				m_dCollectedValues;
	std::vector<uint32_t>		m_dUncompressed32;
	std::vector<T>				m_dUncompressed;
	std::vector<uint32_t>		m_dCompressed;
	std::unique_ptr<IntCodec_i>	m_pCodec;

	std::vector<uint8_t>		m_dTmpBuffer;
	std::vector<uint8_t>		m_dTmpBuffer2;
	std::vector<uint32_t>		m_dSubblockSizes;

	// temp arrays for table encoding
	std::vector<uint32_t>		m_dTableLengths;
	std::vector<T>				m_dTableValues;
	std::vector<T>				m_dKey;
	std::vector<uint32_t>		m_dTableIndexes;
	std::vector<uint32_t>		m_dTablePacked;

	std::unordered_map<std::vector<T>, int, HashFunc_Vec_T<T>> m_hUnique;
	int				m_iUniques = 0;
	int				m_iConstLength = -1;

	void			WritePacked_Const();
	void			WritePacked_ConstLen();
	void			WritePacked_Table();
	void			WritePacked_DeltaPFOR ( bool bWriteLengths );

	void			WriteSubblockSizes();
	void			PrepareValues ( Span_T<T> & dValues, const Span_T<uint32_t> & dLengths );
};

template <typename T>
Packer_MVA_T<T>::Packer_MVA_T ( const Settings_t & tSettings, const std::string & sName, AttrType_e eAttr )
	: BASE ( tSettings, sName, eAttr )
	, m_pCodec ( CreateIntCodec ( tSettings.m_sCompressionUINT32, tSettings.m_sCompressionUINT64 ) )
{
	assert ( tSettings.m_iSubblockSize==128 );
	m_dTableIndexes.resize ( tSettings.m_iSubblockSize );
}

template <typename T>
void Packer_MVA_T<T>::AddDoc ( int64_t tAttr )
{
	assert ( 0 && "INTERNAL ERROR: sending integers to MVA packer" );
}

template <typename T>
void Packer_MVA_T<T>::AddDoc ( const uint8_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending strings to MVA packer" );
}

template <typename T>
void Packer_MVA_T<T>::AddDoc ( const int64_t * pData, int iLength )
{
	if ( m_dCollectedLengths.size()==DOCS_PER_BLOCK )
		Flush();

	AnalyzeCollected ( pData, iLength );

	m_dCollectedLengths.push_back(iLength);
	for ( int i = 0; i < iLength; i++ )
		m_dCollectedValues.push_back ( to_type<T> ( pData[i] ) );

	BASE::m_tHeader.m_tMinMax.Add ( pData, iLength );
}

template <typename T>
void Packer_MVA_T<T>::AnalyzeCollected ( const int64_t * pData, int iLength )
{
	if ( !m_iUniques )
		m_iConstLength = iLength;

	if ( iLength!=m_iConstLength )
		m_iConstLength = -1;

	// if we've got over 256 uniques, no point in further checks
	if ( m_iUniques<256 )
	{
		std::vector<T> dAdd(iLength);
		for ( int i=0; i < iLength; i++ )
			dAdd[i] = (T)pData[i];

		if ( !m_hUnique.count(dAdd) )
		{
			m_hUnique.insert ( { dAdd, 0 } );
			m_iUniques++;
		}
	}
}

template <typename T>
MvaPacking_e Packer_MVA_T<T>::ChoosePacking() const
{
	if ( m_iUniques==1 )
		return MvaPacking_e::CONST;

	if ( m_iUniques<256 )
		return MvaPacking_e::TABLE;

	if ( m_iConstLength!=-1 )
		return MvaPacking_e::CONSTLEN;

	return MvaPacking_e::DELTA_PFOR;
}

template <typename T>
void Packer_MVA_T<T>::Flush()
{
	if ( m_dCollectedLengths.empty() )
		return;

	BASE::m_tHeader.AddBlock ( BASE::m_tWriter.GetPos() );

	WriteToFile ( ChoosePacking() );

	m_dCollectedLengths.resize(0);
	m_dCollectedValues.resize(0);

	m_iConstLength = -1;
	m_iUniques = 0;
	m_hUnique.clear();
}

template <typename T>
void Packer_MVA_T<T>::WriteToFile ( MvaPacking_e ePacking )
{
	BASE::m_tWriter.Pack_uint32 ( to_underlying(ePacking) );

	switch ( ePacking )
	{
	case MvaPacking_e::CONST:
		WritePacked_Const();
		break;

	case MvaPacking_e::CONSTLEN:
		WritePacked_ConstLen();
		break;

	case MvaPacking_e::TABLE:
		WritePacked_Table();
		break;

	case MvaPacking_e::DELTA_PFOR:
		WritePacked_DeltaPFOR(true);
		break;

	default:
		assert ( 0 && "Unknown packing" );
		break;
	}
}

template <typename T>
void Packer_MVA_T<T>::WritePacked_Const()
{
	assert ( m_iUniques==1 );

	Span_T<uint32_t> dLengths ( m_dCollectedLengths.data(), 1 );
	Span_T<T> dValues ( m_dCollectedValues.data(), dLengths[0] );
	PrepareValues ( dValues, dLengths );
	WriteValues_PFOR ( dValues, m_dUncompressed, m_dCompressed, BASE::m_tWriter, m_pCodec.get(), true );
}

template <typename T>
void Packer_MVA_T<T>::WritePacked_ConstLen()
{
	assert ( m_iConstLength>=0 );
	BASE::m_tWriter.Pack_uint32 ( (uint32_t)m_iConstLength );
	WritePacked_DeltaPFOR(false);
}

template <typename T>
void Packer_MVA_T<T>::WritePacked_Table()
{
	assert ( m_iUniques<256 );

	m_dTableLengths.resize(0);
	m_dTableValues.resize(0);

	int iTableEntryId = 0;
	for ( auto & i : m_hUnique )
	{
		const auto & dMVA = i.first;
		i.second = iTableEntryId++;
		int iNumValues = (int)dMVA.size();
		m_dTableLengths.push_back(iNumValues);

		size_t uOldSize = m_dTableValues.size();
		m_dTableValues.resize ( uOldSize + dMVA.size() );
		T * pNew = &m_dTableValues[uOldSize];
		memcpy ( pNew, dMVA.data(), dMVA.size()*sizeof ( dMVA[0] ) );
	}

	WriteValues_PFOR ( Span_T<uint32_t>(m_dTableLengths), m_dUncompressed32, m_dCompressed, BASE::m_tWriter, m_pCodec.get(), true );

	Span_T<T> dTableValues ( m_dTableValues );
	PrepareValues ( dTableValues, m_dTableLengths );
	WriteValues_PFOR ( dTableValues, m_dUncompressed, m_dCompressed, BASE::m_tWriter, m_pCodec.get(), true );

	// write the ordinals
	int iBits = CalcNumBits ( m_hUnique.size() );
	m_dTablePacked.resize ( ( m_dTableIndexes.size()*iBits + 31 ) >> 5 );

	uint32_t uOffset = 0;
	int iId = 0;
	for ( auto i : m_dCollectedLengths )
	{
		m_dKey.resize(i);
		memcpy ( m_dKey.data(), &m_dCollectedValues[uOffset], i*sizeof(T) );
		auto tFound = m_hUnique.find(m_dKey);
		assert ( tFound != m_hUnique.end() );
		assert ( tFound->second < 256 );

		m_dTableIndexes[iId++] = tFound->second;
		if ( iId==128 )
		{
			BitPack128 ( m_dTableIndexes, m_dTablePacked, iBits );
			BASE::m_tWriter.Write ( (uint8_t*)m_dTablePacked.data(), m_dTablePacked.size()*sizeof(m_dTablePacked[0]) );
			iId = 0;
		}

		uOffset += i;
	}

	if ( iId )
	{
		// zero out unused values
		memset ( m_dTableIndexes.data()+iId, 0, (m_dTableIndexes.size()-iId)*sizeof(m_dTableIndexes[0]) );
		BitPack128 ( m_dTableIndexes, m_dTablePacked, iBits );
		BASE::m_tWriter.Write ( (uint8_t*)m_dTablePacked.data(), m_dTablePacked.size()*sizeof(m_dTablePacked[0]) );
	}
}

template <typename T>
void Packer_MVA_T<T>::WritePacked_DeltaPFOR ( bool bWriteLengths )
{
	int iSubblockSize = BASE::m_tHeader.GetSettings().m_iSubblockSizeMva;
	int iBlocks = ( (int)m_dCollectedLengths.size() + iSubblockSize - 1 ) / iSubblockSize;

	m_dSubblockSizes.resize(iBlocks);

	m_dTmpBuffer.resize(0);
	MemWriter_c tMemWriter ( m_dTmpBuffer );

	int iBlockStart = 0;
	uint32_t uTotalValues = 0;
	for ( int iBlock=0; iBlock < (int)m_dSubblockSizes.size(); iBlock++ )
	{
		int iBlockValues = GetSubblockSize ( iBlock, iBlocks, (int)m_dCollectedLengths.size(), iSubblockSize );
		int64_t tSubblockStart = tMemWriter.GetPos();

		Span_T<uint32_t> dLengths ( &m_dCollectedLengths[iBlockStart], iBlockValues );

		uint32_t uNumValues = 0;
		if ( bWriteLengths )
		{
			WriteValues_PFOR ( dLengths, m_dUncompressed32, m_dCompressed, tMemWriter, m_pCodec.get(), true );
			for ( auto i : dLengths )
				uNumValues += i;
		}
		else
			uNumValues = m_iConstLength*iBlockValues;

		// write bodies	
		Span_T<T> dValuesToWrite ( &m_dCollectedValues[uTotalValues], uNumValues );
		PrepareValues ( dValuesToWrite, dLengths );
		WriteValues_PFOR ( dValuesToWrite, m_dUncompressed, m_dCompressed, tMemWriter, m_pCodec.get(), false );

		m_dSubblockSizes[iBlock] = uint32_t ( tMemWriter.GetPos()-tSubblockStart );

		iBlockStart += iBlockValues;
		uTotalValues += uNumValues;
	}

	WriteSubblockSizes();

	BASE::m_tWriter.Write ( m_dTmpBuffer.data(), m_dTmpBuffer.size()*sizeof ( m_dTmpBuffer[0] ) );
}

template <typename T>
void Packer_MVA_T<T>::PrepareValues ( Span_T<T> & dValues, const Span_T<uint32_t> & dLengths )
{
	// MVAs come in ascending mini-sequences, like 0-10-50  20-30-50, 5-6-20
	// let's delta-encode each sequence (if sequence is 0 or 1 values, do nothing)
	uint32_t uOffset = 0;
	for ( auto i : dLengths )
	{
		if ( i>1 )
			ComputeDeltas ( &dValues[uOffset], i, true );

		uOffset+=i;
	}
}

template <typename T>
void Packer_MVA_T<T>::WriteSubblockSizes()
{
	// write cumulative sub-block sizes to a in-memory buffer
	m_dTmpBuffer2.resize(0);
	MemWriter_c tMemWriter(m_dTmpBuffer2);

	ComputeInverseDeltas ( m_dSubblockSizes, true );
	WriteValues_Delta_PFOR ( Span_T<uint32_t>(m_dSubblockSizes), m_dUncompressed32, m_dCompressed, tMemWriter, m_pCodec.get() );

	// write compressed offsets
	BASE::m_tWriter.Write ( m_dTmpBuffer2.data(), m_dTmpBuffer2.size()*sizeof ( m_dTmpBuffer2[0] ) );
}

//////////////////////////////////////////////////////////////////////////

Packer_i * CreatePackerMva32 ( const Settings_t & tSettings, const std::string & sName )
{
	return new Packer_MVA_T<uint32_t> ( tSettings, sName, AttrType_e::UINT32SET );
}


Packer_i * CreatePackerMva64 ( const Settings_t & tSettings, const std::string & sName )
{
	return new Packer_MVA_T<uint64_t> ( tSettings, sName, AttrType_e::INT64SET );
}

} // namespace columnar
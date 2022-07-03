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

#include "attributeheader.h"
#include "buildertraits.h"
#include "reader.h"
#include "check.h"

namespace columnar
{

using namespace util;
using namespace common;


template <typename T>
class MinMax_T
{
public:
	using Element_t = std::pair<T,T>;

	inline int			GetNumLevels() const					{ return (int)m_dTreeLevels.size(); }
	inline int			GetNumBlocks ( int iLevel ) const		{ return m_dTreeLevels[iLevel].first; }
	inline Element_t	Get ( int iLevel, int iBlock ) const	{ return m_dTreeLevels[iLevel].second[iBlock]; }

	bool				Load ( FileReader_c & tReader, std::string & sError );
	bool				Check ( FileReader_c & tReader, Reporter_fn & fnError );

private:
	using TreeLevel_t = std::pair<int,Element_t*>;
	std::unique_ptr<Element_t[]>	m_pMinMaxTree;
	std::unique_ptr<TreeLevel_t[]>	m_pTreeLevels;
	Span_T<Element_t>				m_dMinMaxTree;
	Span_T<TreeLevel_t>				m_dTreeLevels;

	void				LoadTreeLevels ( FileReader_c & tReader );
};


template <typename T>
bool MinMax_T<T>::Load ( FileReader_c & tReader, std::string & sError )
{
	int iTreeLevels = tReader.Unpack_uint32();
	m_pTreeLevels = std::unique_ptr<TreeLevel_t[]> ( new TreeLevel_t[iTreeLevels] );
	m_dTreeLevels = Span_T<TreeLevel_t>( m_pTreeLevels.get(), iTreeLevels );

	int iTreeElements = 0;
	for ( auto & i : m_dTreeLevels )
	{
		i.first = tReader.Unpack_uint32();
		iTreeElements += i.first;
	}

	m_pMinMaxTree = std::unique_ptr<Element_t[]> ( new Element_t[iTreeElements] );
	m_dMinMaxTree = Span_T<Element_t> ( m_pMinMaxTree.get(), iTreeElements );

	LoadTreeLevels(tReader);

	if ( iTreeElements )
	{
		int iCumulativeBlocks = 0;
		for ( auto & i : m_dTreeLevels )
		{
			i.second = &m_dMinMaxTree[iCumulativeBlocks];
			iCumulativeBlocks += i.first;
		}
	}

	if ( tReader.IsError() )
	{
		sError = tReader.GetError();
		return false;
	}

	return true;
}

template <typename T>
bool MinMax_T<T>::Check ( FileReader_c & tReader, Reporter_fn & fnError )
{
	int iTreeLevels = 0;
	if ( !CheckInt32Packed ( tReader, 0, 128, "Number of minmax tree levels", iTreeLevels, fnError ) ) return false;

	int iTotalElements = 0;
	int iLastElements = 0;
	for ( int i = 0; i < iTreeLevels; i++ )
	{
		int iElementsOnLevel = (int)tReader.Unpack_uint32();
		if ( iElementsOnLevel < iLastElements )
		{
			fnError ( "Decreasing number of elements on minmax tree levels" );
			return false;
		}

		iLastElements = iElementsOnLevel;
		iTotalElements += iElementsOnLevel;
	}

	// fixme: maybe add minmax tree verification (opposite of construction process)
	for ( int i = 0; i < iTotalElements; i++ )
	{
		tReader.Unpack_uint64();
		tReader.Unpack_uint64();
	}

	return true;
}

template <typename T>
void MinMax_T<T>::LoadTreeLevels ( FileReader_c & tReader )
{
	for ( auto & i : m_dMinMaxTree )
	{
		i.first = (T)tReader.Unpack_uint64();
		i.second = i.first + (T)tReader.Unpack_uint64();
		assert ( i.first<=i.second );
	}
}

template <>
void MinMax_T<uint8_t>::LoadTreeLevels ( FileReader_c & tReader )
{
	for ( auto & i : m_dMinMaxTree )
	{
		uint8_t uPacked = tReader.Read_uint8();
		i.first = ( uPacked >> 1 ) & 1;
		i.second = uPacked & 1;
		assert ( i.first<=i.second );
		assert ( i.second<2 );
	}
}

template <>
void MinMax_T<float>::LoadTreeLevels ( FileReader_c & tReader )
{
	for ( auto & i :m_dMinMaxTree )
	{
		i.first = UintToFloat ( tReader.Unpack_uint32() );
		i.second = UintToFloat ( tReader.Unpack_uint32() );
		assert ( i.first<=i.second );
	}
}

//////////////////////////////////////////////////////////////////////////
class AttributeHeader_c : public AttributeHeader_i, public Settings_t
{
public:
							AttributeHeader_c ( AttrType_e eType, uint32_t uTotalDocs );

	const std::string &		GetName() const override { return m_sName; }
	AttrType_e				GetType() const override { return m_eType; }
	const Settings_t &		GetSettings() const override { return m_tSettings; }

	uint32_t				GetNumDocs() const override { return m_uTotalDocs; }
	int						GetNumBlocks() const override { return (int)m_dBlocks.size(); }
	uint32_t				GetNumDocs ( int iBlock ) const override ;
	uint64_t				GetBlockOffset ( int iBlock ) const override { return m_dBlocks[iBlock]; }

	int						GetNumMinMaxLevels() const override { return 0; }
	int						GetNumMinMaxBlocks ( int iLevel ) const override { return 0; }
	std::pair<int64_t,int64_t> GetMinMax ( int iLevel, int iBlock ) const override { return {0, 0}; }

	bool					Load ( FileReader_c & tReader, std::string & sError ) override;
	bool					Check ( FileReader_c & tReader, Reporter_fn & fnError ) override;

private:
	std::string				m_sName;
	AttrType_e				m_eType = AttrType_e::NONE;
	uint32_t				m_uTotalDocs = 0;
	Settings_t				m_tSettings;

	std::vector<uint64_t>	m_dBlocks{0};
};


AttributeHeader_c::AttributeHeader_c ( AttrType_e eType, uint32_t uTotalDocs )
	: m_eType ( eType )
	, m_uTotalDocs ( uTotalDocs )
{}


uint32_t AttributeHeader_c::GetNumDocs ( int iBlock ) const
{
	if ( iBlock==m_dBlocks.size()-1 )
	{
		uint32_t uLeftover = m_uTotalDocs & (DOCS_PER_BLOCK-1);
		return uLeftover ? uLeftover : DOCS_PER_BLOCK;
	}

	return DOCS_PER_BLOCK;
}


bool AttributeHeader_c::Load ( FileReader_c & tReader, std::string & sError )
{
	m_tSettings.Load(tReader);

	m_sName = tReader.Read_string();
	uint64_t uOffset = tReader.Read_uint64();

	m_dBlocks.resize ( tReader.Unpack_uint32() );

	if ( !m_dBlocks.empty() )
		m_dBlocks[0] = uOffset;

	for ( size_t i=1; i < m_dBlocks.size(); i++ )
		m_dBlocks[i] = tReader.Unpack_uint64() + m_dBlocks[i-1];

	if ( tReader.IsError() )
	{
		sError = tReader.GetError();
		return false;
	}

	return true;
}


bool AttributeHeader_c::Check ( FileReader_c & tReader, Reporter_fn & fnError )
{
	int iBlocks = 0;
	int64_t iOffset = 0;
	int64_t iFileSize = tReader.GetFileSize();
	if ( !m_tSettings.Check ( tReader, fnError ) ) return false;
	if ( !CheckString ( tReader, 0, 1024, "Attribute name", fnError ) ) return false;
	if ( !CheckInt64 ( tReader, 0, iFileSize, "Header offset", iOffset, fnError ) ) return false;
	if ( !CheckInt32Packed ( tReader, 0, int ( m_uTotalDocs/65536 )+1, "Number of blocks", iBlocks, fnError ) ) return false;

	for ( int i = 0; i < iBlocks-1; i++ )
	{
		iOffset += (int64_t)tReader.Unpack_uint64();
		if ( iOffset<0 || iOffset>iFileSize )
		{
			fnError ( FormatStr ( "Block offset out of bounds: %lld", iOffset ).c_str() );
			return false;
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////

template <typename T>
class AttributeHeader_Int_T : public AttributeHeader_c
{
	using BASE = AttributeHeader_c;
	using BASE::AttributeHeader_c;

public:
	int				GetNumMinMaxLevels() const override					{ return m_tMinMax.GetNumLevels(); }
	int				GetNumMinMaxBlocks ( int iLevel ) const override	{ return m_tMinMax.GetNumBlocks(iLevel); }
	std::pair<int64_t,int64_t> GetMinMax ( int iLevel, int iBlock ) const override;

	bool			Load ( FileReader_c & tReader, std::string & sError ) override;
	bool			Check ( FileReader_c & tReader, Reporter_fn & fnError ) override;

private:
	MinMax_T<T>		m_tMinMax;
};

template <typename T>
bool AttributeHeader_Int_T<T>::Load ( FileReader_c & tReader, std::string & sError )
{
	if ( !BASE::Load ( tReader, sError ) )
		return false;

	return m_tMinMax.Load ( tReader, sError );
}

template <typename T>
bool AttributeHeader_Int_T<T>::Check ( FileReader_c & tReader, Reporter_fn & fnError )
{
	if ( !BASE::Check ( tReader, fnError ) )
		return false;

	return m_tMinMax.Check ( tReader, fnError );
}

template <typename T>
std::pair<int64_t,int64_t> AttributeHeader_Int_T<T>::GetMinMax ( int iLevel, int iBlock ) const
{
	auto tMinMax = m_tMinMax.Get ( iLevel, iBlock );
	return { tMinMax.first, tMinMax.second };
}

template <>
std::pair<int64_t,int64_t> AttributeHeader_Int_T<float>::GetMinMax ( int iLevel, int iBlock ) const
{
	auto tMinMax = m_tMinMax.Get ( iLevel, iBlock );
	return { FloatToUint ( tMinMax.first ), FloatToUint ( tMinMax.second ) };
}

//////////////////////////////////////////////////////////////////////////

AttributeHeader_i * CreateAttributeHeader ( AttrType_e eType, uint32_t uTotalDocs, std::string & sError )
{
	switch ( eType )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
		return new AttributeHeader_Int_T<uint32_t> ( eType, uTotalDocs );

	case AttrType_e::INT64:
		return new AttributeHeader_Int_T<int64_t> ( eType, uTotalDocs );

	case AttrType_e::UINT64:
		return new AttributeHeader_Int_T<uint64_t> ( eType, uTotalDocs );

	case AttrType_e::BOOLEAN:
		return new AttributeHeader_Int_T<uint8_t> ( eType, uTotalDocs );

	case AttrType_e::FLOAT:
		return new AttributeHeader_Int_T<float> ( eType, uTotalDocs );

	case AttrType_e::STRING:
		return new AttributeHeader_Int_T<uint32_t> (eType, uTotalDocs);

	case AttrType_e::UINT32SET:
		return new AttributeHeader_Int_T<uint32_t> ( eType, uTotalDocs );

	case AttrType_e::INT64SET:
		return new AttributeHeader_Int_T<int64_t> ( eType, uTotalDocs );

	default:
		sError = "unknown data type";
		return nullptr;
	}
}

} // namespace columnar

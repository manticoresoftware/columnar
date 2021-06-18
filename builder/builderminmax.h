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

#pragma once

#include "attributeheader.h"

namespace columnar
{

template<typename T>
class MinMaxBuilder_T
{
public:
				MinMaxBuilder_T ( const Settings_t & tSettings );

	void		Add ( int64_t tValue );
	void		Add ( const int64_t * pValues, int iNumValues );
	bool		Save ( FileWriter_c & tWriter, std::string & sError );

private:
	const Settings_t & m_tSettings;

	using TreeLevel_t = std::vector<std::pair<T,T>>;
	std::vector<TreeLevel_t> m_dTreeLevels;
	int			m_iCollected = 0;
	bool		m_bHaveNonEmpty = false;
	T			m_tMin;
	T			m_tMax;

	void		Flush();

	inline bool	SaveTreeLevels ( FileWriter_c & tWriter ) const;
};

template<typename T>
MinMaxBuilder_T<T>::MinMaxBuilder_T ( const Settings_t & tSettings )
	: m_tSettings ( tSettings )
{
#ifndef NDEBUG
	int iSubblockSize = tSettings.m_iSubblockSize;
	int iLeafSize = tSettings.m_iMinMaxLeafSize;
	assert ( ( iSubblockSize & (iSubblockSize - 1) ) == 0 );
	assert ( ( iLeafSize & (iLeafSize - 1) ) == 0 );
	assert ( iLeafSize>=iSubblockSize );
#endif

	m_dTreeLevels.resize(1);
}

template<typename T>
void MinMaxBuilder_T<T>::Add ( int64_t tValue )
{
	if ( m_iCollected==m_tSettings.m_iMinMaxLeafSize )
		Flush();

	T tConverted = to_type<T>(tValue);

	if ( !m_iCollected )
	{
		m_tMin = tConverted;
		m_tMax = tConverted;
	}
	else
	{
		m_tMin = std::min ( m_tMin, tConverted );
		m_tMax = std::max ( m_tMax, tConverted );
	}

	m_bHaveNonEmpty = true;
	m_iCollected++;
}

template<typename T>
void MinMaxBuilder_T<T>::Add ( const int64_t * pValues, int iNumValues )
{
	if ( m_iCollected==m_tSettings.m_iMinMaxLeafSize )
		Flush();

	if ( !iNumValues )
	{
		m_iCollected++;
		return;
	}

	T tMin, tMax;
	for ( int i = 0; i < iNumValues; i++ )
	{
		T tConverted = to_type<T>(pValues[i]);
		if ( i )
		{
			tMin = std::min ( tMin, tConverted );
			tMax = std::max ( tMax, tConverted );
		}
		else
			tMin = tMax = tConverted;
	}

	if ( !m_bHaveNonEmpty )
	{
		m_tMin = tMin;
		m_tMax = tMax;
	}
	else
	{
		m_tMin = std::min ( m_tMin, tMin );
		m_tMax = std::max ( m_tMax, tMax );
	}

	m_bHaveNonEmpty = true;
	m_iCollected++;
}

template<typename T>
void MinMaxBuilder_T<T>::Flush()
{
	if ( !m_iCollected )
		return;

	// fixme! this will give false positives for queries like ANY()>=0
	if ( !m_bHaveNonEmpty )
	{
		m_tMin = (T)0;
		m_tMax = (T)0;
	}

	m_dTreeLevels[0].push_back ( { m_tMin, m_tMax } );
	m_iCollected=0;
	m_bHaveNonEmpty = false;
}

template<typename T>
bool MinMaxBuilder_T<T>::Save ( FileWriter_c & tWriter, std::string & sError )
{
	Flush();

	do
	{
		m_dTreeLevels.push_back ( TreeLevel_t() );
		auto & dNewBlocks = m_dTreeLevels.back();
		auto & dBlocks = m_dTreeLevels[m_dTreeLevels.size()-2];

		for ( int i = 0; i<dBlocks.size(); i+=2 )
		{
			dNewBlocks.emplace_back();
			std::pair<T,T> & tMinMax = dNewBlocks.back();
			if ( i+1<dBlocks.size() )
			{
				tMinMax.first = std::min ( dBlocks[i].first, dBlocks[i+1].first );
				tMinMax.second = std::max ( dBlocks[i].second, dBlocks[i+1].second );
			}
			else
				tMinMax = dBlocks[i];
		}
	}
	while ( m_dTreeLevels.back().size()>1 );

	// now save the tree
	tWriter.Pack_uint32 ( (uint32_t)m_dTreeLevels.size() );
	for ( int i = (int)m_dTreeLevels.size()-1; i>=0; i-- )
		tWriter.Pack_uint32 ( (uint32_t)m_dTreeLevels[i].size() );

	return SaveTreeLevels(tWriter);
}

template<typename T>
inline bool MinMaxBuilder_T<T>::SaveTreeLevels ( FileWriter_c & tWriter ) const
{
	for ( int i = (int)m_dTreeLevels.size()-1; i>=0; i-- )
		for ( auto & tMinMax : m_dTreeLevels[i] )
		{
			tWriter.Pack_uint64 ( (uint64_t)tMinMax.first );
			tWriter.Pack_uint64 ( uint64_t ( tMinMax.second-tMinMax.first ) );
		}

	return !tWriter.IsError();
}

template<>
inline bool MinMaxBuilder_T<uint8_t>::SaveTreeLevels ( FileWriter_c & tWriter ) const
{
	for ( int i = (int)m_dTreeLevels.size()-1; i>=0; i-- )
		for ( auto & tMinMax : m_dTreeLevels[i] )
		{
			assert ( tMinMax.first<2 && tMinMax.second<2 );
			tWriter.Write_uint8 ( ( tMinMax.first << 1 ) | tMinMax.second );
		}

	return !tWriter.IsError();
}

template<>
inline bool MinMaxBuilder_T<float>::SaveTreeLevels ( FileWriter_c & tWriter ) const
{
	for ( int i = (int)m_dTreeLevels.size()-1; i>=0; i-- )
		for ( auto & tMinMax : m_dTreeLevels[i] )
		{
			tWriter.Pack_uint32 ( FloatToUint ( tMinMax.first ) );
			tWriter.Pack_uint32 ( FloatToUint ( tMinMax.second ) );
		}

	return !tWriter.IsError();
}

} // namespace columnar

// Copyright (c) 2023-2025, Manticore Software LTD (https://manticoresearch.com)
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

#include "iterator.h"
#include "knn.h"

#include <algorithm>

namespace knn
{

using namespace util;

class RowidIteratorKNN_c : public Iterator_i
{
public:
			RowidIteratorKNN_c ( KNNIndex_i & tIndex, const Span_T<float> & dData, int64_t iResults, int iEf, KNNFilter_i * pFilter );

	bool	HintRowID ( uint32_t tRowID ) override;
	bool	GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) override;
	int64_t	GetNumProcessed() const override			{ return m_iIndex; }
	void	SetCutoff ( int iCutoff ) override			{}
	bool	WasCutoffHit() const override				{ return false; }
	void	AddDesc ( std::vector<common::IteratorDesc_t> & dDesc ) const override {}

	Span_T<const DocDist_t> GetData() const override	{ return Span_T<const DocDist_t> ( m_dCollected.data(), m_dCollected.size() ); }

private:
	static const int DOCS_PER_CHUNK = 1000;

	std::vector<uint32_t>	m_dRowIDs;
	std::vector<DocDist_t>	m_dCollected;
	std::vector<uint8_t>	m_dQuantized;
	int						m_iIndex = 0;
};


RowidIteratorKNN_c::RowidIteratorKNN_c ( KNNIndex_i & tIndex, const Span_T<float> & dData, int64_t iResults, int iEf, KNNFilter_i * pFilter )
{
	tIndex.Search ( m_dCollected, dData, iResults, iEf, m_dQuantized, pFilter );
	std::sort ( m_dCollected.begin(), m_dCollected.end(), []( const auto & a, const auto & b ) { return a.m_tRowID<b.m_tRowID; } );
	m_dRowIDs.resize(DOCS_PER_CHUNK);
}


bool RowidIteratorKNN_c::HintRowID ( uint32_t tRowID )
{
	if ( m_iIndex>=(int)m_dCollected.size() )
		return false;

	auto tEnd = m_dCollected.end();
	auto tFound = std::lower_bound ( m_dCollected.begin() + m_iIndex, tEnd, tRowID, []( auto & tEntry, uint32_t tValue ){ return tEntry.m_tRowID < tValue; } );
	if ( tFound==tEnd )
	{
		m_iIndex = (int)m_dCollected.size();
		return false;
	}

	m_iIndex = tFound - m_dCollected.begin();
	return true;
}


bool RowidIteratorKNN_c::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	int iCollected = std::max ( std::min ( int(m_dCollected.size()) - m_iIndex, DOCS_PER_CHUNK ), 0 );
	DocDist_t * pStart = m_dCollected.data() + m_iIndex;
	DocDist_t * pMax = pStart + iCollected;
	m_iIndex += iCollected;

	if ( pStart==pMax )
		return false;

	DocDist_t * pDoc = pStart;
	uint32_t * pRowID = m_dRowIDs.data();
	while ( pDoc<pMax )
	{
		*pRowID = pDoc->m_tRowID;
		pRowID++;
		pDoc++;
	}

	dRowIdBlock = Span_T<uint32_t> ( m_dRowIDs.data(), pRowID-m_dRowIDs.data() );
	return true;
}

/////////////////////////////////////////////////////////////////////

Iterator_i * CreateIterator ( KNNIndex_i & tIndex, const util::Span_T<float> & dData, int64_t iResults, int iEf, KNNFilter_i * pFilter )
{
	return new RowidIteratorKNN_c ( tIndex, dData, iResults, iEf, pFilter );
}

} // namespace knn

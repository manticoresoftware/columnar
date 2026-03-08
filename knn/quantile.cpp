// Copyright (c) 2026, Manticore Software LTD (https://manticoresearch.com)
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

#include "quantile.h"

#include <algorithm>
#include <cassert>

namespace knn
{

P2QuantileEstimator_c::P2QuantileEstimator_c ( double fQuantile )
	: m_fQuantile ( fQuantile )
{
	assert ( m_fQuantile >= 0.0 && m_fQuantile <= 1.0 );
	Reset();
}


void P2QuantileEstimator_c::Reset()
{
	m_iCount = 0;
	std::fill ( m_dQ, m_dQ + NUM_MARKERS, 0.0 );
	std::fill ( m_dN, m_dN + NUM_MARKERS, 0.0 );
	std::fill ( m_dNP, m_dNP + NUM_MARKERS, 0.0 );
	std::fill ( m_dDN, m_dDN + NUM_MARKERS, 0.0 );

	m_dDN[0] = 0.0;
	m_dDN[1] = m_fQuantile / 2.0;
	m_dDN[2] = m_fQuantile;
	m_dDN[3] = (1.0 + m_fQuantile) / 2.0;
	m_dDN[4] = 1.0;
}


void P2QuantileEstimator_c::InitializeMarkers ( double fValue )
{
	m_dQ[m_iCount-1] = fValue;
	if ( m_iCount<NUM_MARKERS )
		return;

	std::sort ( m_dQ, m_dQ+NUM_MARKERS );

	for ( int i = 0; i < NUM_MARKERS; i++ )
	{
		m_dN[i] = i+1;
		m_dNP[i] = 1 + (NUM_MARKERS-1) * m_dDN[i];
	}
}

////////////////////////////////////////////////////////////////////////////////
MP2QuantileEstimator_c::MP2QuantileEstimator_c ( size_t uWindowSize, double fQuantile )
	: m_uWindowSize ( uWindowSize )
	, m_fInvWindowSize ( 1.0 / uWindowSize )
	, m_tEstimator ( fQuantile )
{
	assert(m_uWindowSize);
}


void MP2QuantileEstimator_c::Reset()
{
	m_uCount = 0;
	m_uCountInWindow = 0;
	m_fPrevWindowEstimation = 0.0;
	m_tEstimator.Reset();
}

} // namespace knn

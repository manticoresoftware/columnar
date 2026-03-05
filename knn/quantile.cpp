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


void P2QuantileEstimator_c::Insert ( double fValue )
{
	m_iCount++;

	if ( m_iCount<=NUM_MARKERS )
	{
		InitializeMarkers(fValue);
		return;
	}

	int iMarker = FindMarker(fValue);
	IncrementPositions(iMarker);
	AdjustMarkerHeights();
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


int P2QuantileEstimator_c::FindMarker ( double fValue )
{
	if ( fValue < m_dQ[0] )
	{
		m_dQ[0] = fValue;
		return 0;
	}

	if ( fValue >= m_dQ[NUM_MARKERS-1] )
	{
		m_dQ[NUM_MARKERS-1] = fValue;
		return NUM_MARKERS-2;
	}

	for ( int i = 0; i < NUM_MARKERS-1; i++ )
		if ( m_dQ[i] <= fValue && fValue < m_dQ[i+1] )
			return i;

	assert ( 0 && "Unable to find marker" );
	return 0;
}


void P2QuantileEstimator_c::IncrementPositions ( int iMarker )
{
	for ( int i = iMarker + 1; i < NUM_MARKERS; i++ )
		m_dN[i]++;

	for ( int i = 0; i < NUM_MARKERS; i++ )
		m_dNP[i] += m_dDN[i];
}


void P2QuantileEstimator_c::AdjustMarkerHeights()
{
	for ( int i = 1; i < NUM_MARKERS - 1; i++ )
	{
		double fD = m_dNP[i] - m_dN[i];
		if ( ( fD >= 1.0 && m_dN[i+1] - m_dN[i] > 1.0 ) || ( fD <= -1.0 && m_dN[i-1] - m_dN[i] < -1.0 ) )
		{
			int iSign = ( fD >= 1.0 ) ? 1 : -1;
			double fNewQ = m_dQ[i] + ( iSign / ( m_dN[i+1] - m_dN[i-1] ) ) *
				( ( m_dN[i] - m_dN[i-1] + iSign ) * ( m_dQ[i+1] - m_dQ[i] ) / ( m_dN[i+1] - m_dN[i] ) +
				  ( m_dN[i+1] - m_dN[i] - iSign ) * ( m_dQ[i] - m_dQ[i-1] ) / ( m_dN[i] - m_dN[i-1] ) );

			if ( fNewQ > m_dQ[i-1] && fNewQ < m_dQ[i+1] )
				m_dQ[i] = fNewQ;
			else
				m_dQ[i] = m_dQ[i] + iSign * ( m_dQ[i+iSign] - m_dQ[i] ) / ( m_dN[i+iSign] - m_dN[i] );

			m_dN[i] += iSign;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
MP2QuantileEstimator_c::MP2QuantileEstimator_c ( size_t uWindowSize, double fQuantile )
	: m_uWindowSize ( uWindowSize )
	, m_tEstimator ( fQuantile )
{
	assert(m_uWindowSize);
}


void MP2QuantileEstimator_c::Reset()
{
	m_uCount = 0;
	m_fPrevWindowEstimation = 0.0;
	m_tEstimator.Reset();
}


void MP2QuantileEstimator_c::Insert ( double value )
{
	m_tEstimator.Insert(value);
	m_uCount++;
	if ( !(m_uCount % m_uWindowSize) )
	{
		m_fPrevWindowEstimation = m_tEstimator.Get();
		m_tEstimator.Reset();
	}
}


double MP2QuantileEstimator_c::Get() const
{
	if ( !m_uCount )
		return 0.0;

	if ( m_uCount < m_uWindowSize )
		return m_tEstimator.Get();

	// exactly at a window boundary: current window was just reset and is empty
	if ( !(m_uCount % m_uWindowSize) )
		return m_fPrevWindowEstimation;

	double fEstimation1 = m_fPrevWindowEstimation;
	double fEstimation2 = m_tEstimator.Get();
	double fW2 = double( m_uCount % m_uWindowSize ) / m_uWindowSize;
	double fW1 = 1.0 - fW2;
	return fW1*fEstimation1 + fW2*fEstimation2;
}

} // namespace knn

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

#pragma once

#include "util/util.h"

namespace knn
{

class P2QuantileEstimator_c
{
public:
			P2QuantileEstimator_c ( double fQuantile );

	void	Reset();

	FORCE_INLINE void Insert ( double fValue )
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

	FORCE_INLINE bool	Ready() const	{ return m_iCount>=NUM_MARKERS; }
	FORCE_INLINE double	Get() const		{ return m_dQ[2]; }

private:
	static const int NUM_MARKERS = 5;

	double	m_dQ[NUM_MARKERS] = {};
	double	m_dN[NUM_MARKERS] = {};
	double	m_dNP[NUM_MARKERS] = {};
	double	m_dDN[NUM_MARKERS] = {};
	double	m_fQuantile = 0.0;
	int		m_iCount = 0;

	void	InitializeMarkers ( double fValue );
	FORCE_INLINE int FindMarker ( double fValue )
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

	FORCE_INLINE void IncrementPositions ( int iMarker )
	{
		for ( int i = iMarker + 1; i < NUM_MARKERS; i++ )
			m_dN[i]++;

		for ( int i = 0; i < NUM_MARKERS; i++ )
			m_dNP[i] += m_dDN[i];
	}

	FORCE_INLINE void AdjustMarkerHeights()
	{
		for ( int i = 1; i < NUM_MARKERS - 1; i++ )
		{
			double fD = m_dNP[i] - m_dN[i];
			if ( ( fD >= 1.0 && m_dN[i+1] - m_dN[i] > 1.0 ) || ( fD <= -1.0 && m_dN[i-1] - m_dN[i] < -1.0 ) )
			{
				int iSign = ( fD >= 1.0 ) ? 1 : -1;
				double fN10 = m_dN[i+1] - m_dN[i];
				double fN01 = m_dN[i] - m_dN[i-1];
				double fNewQ = m_dQ[i] + ( iSign / ( fN10 + fN01 ) ) *
					( ( fN01 + iSign ) * ( m_dQ[i+1] - m_dQ[i] ) / fN10 +
					  ( fN10 - iSign ) * ( m_dQ[i] - m_dQ[i-1] ) / fN01 );

				if ( fNewQ > m_dQ[i-1] && fNewQ < m_dQ[i+1] )
					m_dQ[i] = fNewQ;
				else
					m_dQ[i] += iSign * ( m_dQ[i+iSign] - m_dQ[i] ) / ( iSign > 0 ? fN10 : fN01 );

				m_dN[i] += iSign;
			}
		}
	}

};

class MP2QuantileEstimator_c
{
public:
			MP2QuantileEstimator_c ( size_t uWindowSize, double fQuantile );

	void	Reset();

	FORCE_INLINE void Insert ( double value )
	{
		m_tEstimator.Insert(value);
		m_uCount++;
		m_uCountInWindow++;

		if ( m_uCountInWindow==m_uWindowSize )
		{
			m_fPrevWindowEstimation = m_tEstimator.Get();
			m_tEstimator.Reset();
			m_uCountInWindow = 0;
		}
	}

	FORCE_INLINE double Get() const
	{
		if ( !m_uCount )
			return 0.0;

		if ( m_uCount < m_uWindowSize )
			return m_tEstimator.Get();

		// exactly at a window boundary: current window was just reset and is empty
		if ( !m_uCountInWindow )
			return m_fPrevWindowEstimation;

		double fEstimation1 = m_fPrevWindowEstimation;
		double fEstimation2 = m_tEstimator.Get();
		double fW2 = double(m_uCountInWindow) * m_fInvWindowSize;
		double fW1 = 1.0 - fW2;
		return fW1*fEstimation1 + fW2*fEstimation2;
	}

private:
	size_t  m_uWindowSize = 0;
	size_t  m_uCount = 0;
	size_t  m_uCountInWindow = 0;
	double  m_fInvWindowSize = 0.0;
	double  m_fPrevWindowEstimation = 0.0;
	P2QuantileEstimator_c m_tEstimator;
};

} // namespace knn

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
	void	Insert ( double fValue );
	bool	Ready() const			{ return m_iCount>=NUM_MARKERS; }
	FORCE_INLINE double Get() const	{ return m_dQ[2]; }

private:
	static const int NUM_MARKERS = 5;

	double	m_dQ[NUM_MARKERS] = {};
	double	m_dN[NUM_MARKERS] = {};
	double	m_dNP[NUM_MARKERS] = {};
	double	m_dDN[NUM_MARKERS] = {};
	double	m_fQuantile = 0.0;
	int		m_iCount = 0;

	void	InitializeMarkers ( double fValue );
	int		FindMarker ( double fValue );
	void	IncrementPositions ( int iMarker );
	void	AdjustMarkerHeights();
};

class MP2QuantileEstimator_c
{
public:
			MP2QuantileEstimator_c ( size_t uWindowSize, double fQuantile );

	void	Reset();
	void	Insert ( double value );
	double	Get() const;

private:
	size_t  m_uWindowSize = 0;
	size_t  m_uCount = 0;
	double  m_fPrevWindowEstimation = 0.0;
	P2QuantileEstimator_c m_tEstimator;
};

} // namespace knn

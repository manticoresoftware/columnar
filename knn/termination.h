// Copyright (c) 2026, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
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

#include "knn.h"
#include "quantile.h"
namespace knn
{

class TerminationQuantile_c
{
public:
			TerminationQuantile_c ( double fThresholdQuantile = DEFAULT_THRESHOLD_QUANTILE );

	void	reset();
	FORCE_INLINE void onDistanceScored()     { m_iScored++; }
	FORCE_INLINE void onCandidateCollected() { m_iCollected++; }
	bool	shouldTerminate ( size_t ef, size_t currentSize );

protected:
	static constexpr double DEFAULT_THRESHOLD_QUANTILE = 0.15;
	static constexpr size_t THRESHOLD_WINDOW_SIZE = 64;

	int     m_iCollected = 0;
	int     m_iPrevCollected = 0;
	int     m_iScored = 0;
	int		m_iBadRounds = 0;
	MP2QuantileEstimator_c  m_tThresholdQuantile;
};

class TerminationQuantileL2_c : public TerminationQuantile_c
{
public:
		TerminationQuantileL2_c() : TerminationQuantile_c ( L2_THRESHOLD_QUANTILE ) {}

protected:
	static constexpr double L2_THRESHOLD_QUANTILE = 0.11;
};

} // namespace knn

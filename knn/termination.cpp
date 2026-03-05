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
//

#include "termination.h"

#include <algorithm>

using namespace util;

namespace knn
{
static FORCE_INLINE int CalcPatience ( size_t ef )
{
	if ( ef <= 200 )
		return 8;

	if ( ef <= 800 )
		return 7;

	if ( ef <= 1000 )
		return 6;

	if ( ef <= 3000 )
		return 5;

	if ( ef <= 6000 )
		return 4;

	return 3;
}


TerminationQuantile_c::TerminationQuantile_c ( double fThresholdQuantile )
	: m_tThresholdQuantile ( THRESHOLD_WINDOW_SIZE, fThresholdQuantile )
{}

void TerminationQuantile_c::reset()
{
	m_iCollected = 0;
	m_iPrevCollected = 0;
	m_iScored = 0;
	m_tThresholdQuantile.Reset();
}


bool TerminationQuantile_c::shouldTerminate ( size_t ef, size_t currentSize )
{
	if ( currentSize < ef )
	{
		// ignore warm-up while the ef heap is still filling. The signal is only meaningful once
		// we are measuring replacements against a saturated frontier.
		m_iPrevCollected = m_iCollected;
		m_iScored = 0;
		m_iBadRounds = 0;
		return false;
	}

	double fDiscoveryRate = double( m_iCollected - m_iPrevCollected ) / ( 1e-9 + double(m_iScored) );
	bool bBadRound = fDiscoveryRate < m_tThresholdQuantile.Get();

	if ( bBadRound )
		m_iBadRounds++;
	else
		m_iBadRounds = 0;

	m_tThresholdQuantile.Insert ( fDiscoveryRate );

	m_iPrevCollected = m_iCollected;
	m_iScored = 0;

	return m_iBadRounds >= CalcPatience(ef);
}

} // namespace knn

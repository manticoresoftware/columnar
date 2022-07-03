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

#include "filter.h"
#include "util/util.h"

namespace common
{

using namespace util;

void FixupFilterSettings ( Filter_t & tFilter, AttrType_e eAttrType )
{
	switch ( eAttrType )
	{
	case AttrType_e::UINT32:
	case AttrType_e::UINT32SET:
	case AttrType_e::TIMESTAMP:
		// clamp to min and max values from a wider type
		if ( tFilter.m_iMinValue<0 )
		{
			tFilter.m_iMinValue = 0;
			tFilter.m_bLeftClosed = true;
		}

		if ( tFilter.m_iMaxValue>UINT_MAX )
		{
			tFilter.m_iMaxValue = UINT_MAX;
			tFilter.m_bRightClosed = true;
		}
		break;

	case AttrType_e::FLOAT:
		// this is basically the same stuff we do when we create filters, but we don't have access to previously modified filter settings
		// that's why we need to do it all over again
		if ( tFilter.m_eType==FilterType_e::VALUES && tFilter.m_dValues.size()==1 )
		{
			tFilter.m_eType = FilterType_e::FLOATRANGE;
			tFilter.m_fMinValue = tFilter.m_fMaxValue = (float)tFilter.m_dValues[0];
		}

		if ( tFilter.m_eType==FilterType_e::RANGE )
		{
			tFilter.m_eType = FilterType_e::FLOATRANGE;
			tFilter.m_fMinValue = (float)tFilter.m_iMinValue;
			tFilter.m_fMaxValue = (float)tFilter.m_iMaxValue;
		}
		break;

	default:
		break;
	}
}


std::string GenerateHashAttrName ( const std::string & sAttr )
{
	return FormatStr ( "$%s_HASH", sAttr.c_str() );
}


Filter_t StringFilterToHashFilter ( const Filter_t & tFilter, bool bGenerateName )
{
	assert ( tFilter.m_eType==FilterType_e::STRINGS );
	Filter_t tRes;

	tRes.m_eType = FilterType_e::VALUES;
	tRes.m_bExclude = tFilter.m_bExclude;
	tRes.m_sName = bGenerateName ?  GenerateHashAttrName ( tFilter.m_sName ) : tFilter.m_sName;

	for ( const auto & i : tFilter.m_dStringValues )
		tRes.m_dValues.push_back ( i.empty() ? 0 : tFilter.m_fnCalcStrHash ( i.data(), i.size(), STR_HASH_SEED ) );

	return tRes;
}

} // namespace common

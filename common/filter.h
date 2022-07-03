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

#pragma once

#include "schema.h"
#include <limits>

namespace common
{

enum class FilterType_e
{
	NONE,
	VALUES,
	RANGE,
	FLOATRANGE,
	STRINGS
};


enum class MvaAggr_e
{
	NONE,
	ALL,
	ANY
};

using StringCmp_fn = int (*) ( std::pair<const uint8_t *, int> tStrA, std::pair<const uint8_t *, int> tStrB, bool bPacked );

struct Filter_t
{
	std::string				m_sName;
	bool					m_bExclude = false;
	FilterType_e			m_eType = FilterType_e::NONE;
	MvaAggr_e				m_eMvaAggr = MvaAggr_e::NONE;
	int64_t					m_iMinValue = 0;
	int64_t					m_iMaxValue = 0;
	float					m_fMinValue = 0.0f;
	float					m_fMaxValue = 0.0f;
	bool					m_bLeftUnbounded = false;
	bool					m_bRightUnbounded = false;
	bool					m_bLeftClosed = true;
	bool					m_bRightClosed = true;

	StringHash_fn			m_fnCalcStrHash = nullptr;
	StringCmp_fn			m_fnStrCmp = nullptr;

	std::vector<int64_t>	m_dValues;
	std::vector<std::vector<uint8_t>> m_dStringValues;
};

struct RowidRange_t
{
	uint32_t m_uMin { std::numeric_limits<uint32_t>::min() };
	uint32_t m_uMax{ std::numeric_limits<uint32_t>::max() };
};

void		FixupFilterSettings ( Filter_t & tFilter, AttrType_e eAttrType );
Filter_t	StringFilterToHashFilter ( const Filter_t & tFilter, bool bGenerateName );
std::string	GenerateHashAttrName ( const std::string & sAttr );

} // namespace common
